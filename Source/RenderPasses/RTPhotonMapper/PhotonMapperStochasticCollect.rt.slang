#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Scene.Material.ShadingUtils;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Materials.StandardMaterial;
//import Experimental.Scene.Material.MaterialHelpers;
import Rendering.Lights.LightHelpers;


cbuffer PerFrame
{
    uint gFrameCount;       // Frame count since scene was loaded.
    float gCausticRadius;   // Radius for the caustic photons
    float gGlobalRadius;    // Radius for the global photons
}

cbuffer CB
{
    float gEmissiveScale; // Scale for the emissive part
    bool gCollectGlobalPhotons;
    bool gCollectCausticPhotons;
};

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewWorld;
Texture2D<float4> gThp;
Texture2D<float4> gEmissive;


// Outputs
RWTexture2D<float4> gPhotonImage;

//Acceleration Structure
RaytracingAccelerationStructure gPhotonAS;

//Helper Structs
struct PhotonInfo
{
    float3 dir;
    float faceNTheta;
    float3 flux;
    float faceNPhi;
};

 //Photon Buffers
Texture2D<float4> gPhotonFlux[2];
Texture2D<float4> gPhotonDir[2];
StructuredBuffer<AABB> gPhotonAABB[2];

// Static configuration based on defines set from the host.
static const uint kInfoTexHeight = INFO_TEXTURE_HEIGHT;
static const bool kUsePhotonFaceNormal = PHOTON_FACE_NORMAL;

static const float kRayTMin = RAY_TMIN;
static const float kRayTMax = RAY_TMAX;

/** Payload for ray (16 * X B).
*/
struct RayData
{
    uint counter;                   //Counter for photons this pixel
    uint photonIdx[NUM_PHOTONS];    //Num Photons, variable length. Should be 3 + 4*X for best fit.

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
};

struct SphereAttribs
{
    float2 pad;
};

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}

[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    const uint primIndex = PrimitiveIndex();

    uint idx = rayData.counter;
    rayData.counter++;
    
    //If photon list is full we get a random index between 0 and number of elements (rayData.counter) to stochastically insert
    if (idx >= NUM_PHOTONS)
    {
        idx = uint(sampleNext1D(rayData.sg) * rayData.counter);
    }
    //Insert if index is within maximum list size
    if (idx < NUM_PHOTONS)
        rayData.photonIdx[idx] = primIndex;
}

//Checks if the ray start point is inside the sphere.
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    radiusTest = radiusTest * radiusTest;
    float radiusTestF = radiusTest.x + radiusTest.y + radiusTest.z;
    if (radiusTestF < radius * radius)
        return true;
    return false;
}

[shader("intersection")]
void intersection()
{
    //Check for Sphere intersection
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();

    //Reject hits if face normal of the surfaces is not the same
    if (kUsePhotonFaceNormal)
    {
        const uint2 index2D = uint2(primIndex / kInfoTexHeight, primIndex % kInfoTexHeight);
        const float theta = gPhotonFlux[InstanceIndex()][index2D].w;   
        const float phi = gPhotonDir[InstanceIndex()][index2D].w;
        float sinTheta = sin(theta);
        float3 photonFaceN = float3(cos(phi) * sinTheta, cos(theta), sin(phi) * sinTheta);
        if(dot(WorldRayDirection(), photonFaceN) < 0.9f)    //Face N is stored in WorldRayDirection
            return;
    }
    
    //Get AABB buffer. Instance Index 0 is causic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gCausticRadius : gGlobalRadius;
    
    //Sphere test  
    bool tHit = hitSphere(photonAABB.center(), radius, origin);

    //Attribs are needed by the api. Because we have no need for additional attributes it is empty
    SphereAttribs attribs;
    attribs.pad = float2(0);

    //Report the hit if the point is in the sphere
    if (tHit)
    {
        ReportHit(RayTCurrent(), 0, attribs);
    }
}

//Lighting Calculation for each photon in the list. This is done seperatly for caustic and global photons
float3 photonContribution(in const ShadingData sd, in IBSDF bsdf, inout RayData rayData, bool isCaustic)
{
    //Get the size of the list to loop through. 
    uint maxIdx = min(rayData.counter, NUM_PHOTONS);
    float3 radiance = float3(0);

    //Return when there is no element
    if (maxIdx == 0)
        return radiance;

    //loop through the list and accumulate photons
    for (uint i = 0; i < maxIdx; i++)
    {
        //Get Photon info
        uint photonIdx = rayData.photonIdx[i];
        uint2 photonIdx2D = uint2(photonIdx / kInfoTexHeight, photonIdx % kInfoTexHeight);
        uint instanceIndex = isCaustic ? 0 : 1;
        
        float3 photonFlux = gPhotonFlux[instanceIndex][photonIdx2D].xyz;
        float3 photonDir = gPhotonDir[instanceIndex][photonIdx2D].xyz;

        //BSDF and contribution
        float3 f_r = bsdf.eval(sd, -photonDir, rayData.sg);
       
        radiance += f_r * photonFlux;
    }

    //Weight output radiance with number of photons for this pixel.
    //The weight equals 1 if there is less or equal than the maximum list size in the photon array.
    return radiance * (float(rayData.counter) / float(maxIdx));
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    float4 thpMatID = gThp[launchIndex];
    float3 viewW = -gViewWorld[launchIndex].xyz;

    //Prepare payload
    RayData rayData;
    rayData.counter = 0;
    rayData.sg = SampleGenerator(launchIndex, gFrameCount);
    
    const HitInfo hit = HitInfo(gVBuffer[launchIndex]);
    bool valid = hit.isValid(); //Check if the ray is valid 

    //Get vertex data for the world position and shading data
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    //Get shading data and bsdf
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -viewW, lod);
    adjustShadingNormal(sd, v);
    let bsdf = gScene.materials.getBSDF(sd, lod);

    float3 faceN = dot(-viewW, sd.faceN) > 0 ? sd.faceN : -sd.faceN; //Flip face normal so that it points to the camera (or view direction)
    
    RayDesc ray;
    ray.Origin = v.posW;
    ray.TMin = RAY_TMIN;
    ray.TMax = RAY_TMAX;
    ray.Direction = faceN;     //Take face normal as direction for face normal rejection. Saves space in payload

    //Closest hit shader can be skipped
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;
    float3 radiance = float3(0);
    
    //It is faster to trace two times in the different instance mask because of divergence
    if (gCollectCausticPhotons && valid)
    {
        TraceRay(gPhotonAS, rayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float3 radiancePhotons = photonContribution(sd, bsdf, rayData, true);
        float w = 1 / (M_PI * gCausticRadius * gCausticRadius);
        radiance += w * radiancePhotons;
        rayData.counter = 0;
    }
        
    if (gCollectGlobalPhotons && valid)
    {
        TraceRay(gPhotonAS, rayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float3 radiancePhotons = photonContribution(sd, bsdf, rayData, false);
        float w = 1 / (M_PI * gGlobalRadius * gGlobalRadius);
        radiance += w * radiancePhotons;
    }

    //Accommodate for the path taken by the path traced V buffer
    radiance *= thpMatID.xyz;

    //Add emission
    float3 pixEmission = gEmissive[launchIndex].xyz;
    radiance += pixEmission * thpMatID.xyz;
    
    //Accumulate the image
    if (gFrameCount > 0 )
    {
        float3 last = gPhotonImage[launchIndex].xyz;
        float frameCountF = float(gFrameCount);
        last *= frameCountF;
        radiance += last;
        radiance /= frameCountF + 1.0;
    }
    
    gPhotonImage[launchIndex] = float4(radiance, 1);
}
