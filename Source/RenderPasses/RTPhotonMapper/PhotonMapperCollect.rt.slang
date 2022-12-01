#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
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
    float4 dir;
    float4 flux;
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

/** Payload for ray (48B).
*/
struct RayData
{
    float3 radiance;                ///< Accumulated outgoing radiance from path.
    uint pad;                       ///< free space.
    PackedHitInfo packedHitInfo;    ///< Hit info from vBuffer; Up to 16B

    SampleGenerator sg;             ///< Per-ray state for the sample generator (up to 16B).
  
    __init(){
        this.radiance = float3(0);
        this.pad = 0;
    }
};

struct SphereAttribs
{
    float2 pad;
};

//Load in shading data from hit and direction
ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    adjustShadingNormal(sd, v);
    
    return sd;
}

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    //Empty. Is needed for compilation
}


[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{    
    const uint primIndex = PrimitiveIndex();
    const uint2 primIndex2D = uint2(primIndex / kInfoTexHeight, primIndex % kInfoTexHeight);

    //Get caustic or global photon depending on Instance Index. (0 is caustic; 1 is global)
    PhotonInfo photon;
    photon.flux = gPhotonFlux[InstanceIndex()][primIndex2D];
    photon.dir = gPhotonDir[InstanceIndex()][primIndex2D];
        
    //Get hit data from payload
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    let lod = ExplicitLodTextureSampler(0.f);
    //World Direction is set to the viewDir we get from the vBuffer
    ShadingData sd = loadShadingData(hit, WorldRayDirection(), lod);

    //Do face normal test if enabled
    if(kUsePhotonFaceNormal){
        //Sperical to cartesian
        float sinTheta = sin(photon.flux.w);
        float3 photonFaceN = float3(cos(photon.dir.w) * sinTheta, cos(photon.flux.w), sin(photon.dir.w) * sinTheta);
        photonFaceN = normalize(photonFaceN);
        float3 faceN = dot(-WorldRayDirection(), sd.faceN) > 0 ? sd.faceN : -sd.faceN;  //Correct orientation of faceN so that it points to the camera
        //Compaire normals with a small margin of error due to data loss for the photon face normal
        if(dot(faceN, photonFaceN) < 0.9f)
            return;
    }

    //Photon contribution
    let bsdf = gScene.materials.getBSDF(sd, lod);   
    float3 f_r = bsdf.eval(sd, -photon.dir.xyz, rayData.sg);
    rayData.radiance += f_r * photon.flux.xyz;
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
    //Get start point and prim index
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();

    //Get Photon AABB. Instance Index 0 is caustic; 1 is global
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gCausticRadius : gGlobalRadius;
    
    //Check for Sphere intersection
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


[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float4 thp = gThp[launchIndex];

    //Prepare payload
    RayData rayData = RayData();
    rayData.packedHitInfo = gVBuffer[launchIndex];
    rayData.sg = SampleGenerator(launchIndex, gFrameCount);
    
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    bool valid = hit.isValid(); //Check if the ray is valid 

    //Get vertex data for the world position
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);

    //Ray description 
    RayDesc ray;
    ray.Origin = v.posW;
    ray.TMin = RAY_TMIN;
    ray.TMax = RAY_TMAX;
    ray.Direction = -gViewWorld[launchIndex].xyz;    //we take the view dir as direction to save payload space

    //Closest hit shader can be skipped
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;
    float3 radiance = float3(0);
    
    //It is faster to trace two times in the different instance mask because of divergence
    if (gCollectCausticPhotons && valid)
    {
        TraceRay(gPhotonAS, rayFlags, 1 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float w = 1 / (M_PI * gCausticRadius * gCausticRadius);
        radiance += w * rayData.radiance;
        rayData.radiance = 0.0;
    }
        
    if (gCollectGlobalPhotons && valid)
    {
        TraceRay(gPhotonAS, rayFlags, 2 /* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
        float w = 1 / (M_PI * gGlobalRadius * gGlobalRadius);
        radiance += w * rayData.radiance;
    }

    //Accommodate for the path taken by the path traced V buffer
    radiance *= thp.xyz;

    //Add emission
    float3 pixEmission = gEmissive[launchIndex].xyz;
    radiance += pixEmission * thp.xyz;
    
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
