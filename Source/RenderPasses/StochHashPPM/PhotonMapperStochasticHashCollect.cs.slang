#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Scene.Material.ShadingUtils;
import Rendering.Materials.StandardMaterial;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;

import PhotonMapperStochasticHashFunctions;

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
    float gCausticRadius; // Radius for the caustic photons
    float gGlobalRadius; // Radius for the global photons
    float gCausticHashScaleFactor; //Hash scale factor for caustic hash cells
    float gGlobalHashScaleFactor;
}

cbuffer CB
{
    float gEmissiveScale; // Scale for the emissive part
    bool gCollectGlobalPhotons;
    bool gCollectCausticPhotons;
    uint gBucketYExtent; // Y Extent of bucket for 2D index calc
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


 //Internal Buffer Structs
Texture2D<float4> gHashBucketPos[2];
Texture2D<float4> gHashBucketDir[2];
Texture2D<float4> gHashBucketFlux[2];
StructuredBuffer<uint> gHashCounter[2];


// Static configuration based on defines set from the host.
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
    
static const uint kInfoTexHeight = INFO_TEXTURE_HEIGHT;
static const uint kNumBuckets = NUM_BUCKETS; //Total number of buckets in 2^x
static const bool kUsePhotonFaceNormal = PHOTON_FACE_NORMAL;


//Checks if the ray start point is inside the sphere. 0 is returned if it is not in sphere and 1 if it is
bool hitSphere(const float3 center, const float radius, const float3 p)
{
    float3 radiusTest = p - center;
    radiusTest = radiusTest * radiusTest;
    float radiusTestF = radiusTest.x + radiusTest.y + radiusTest.z;
    if (radiusTestF < radius * radius)
        return true;
    return false;
}

ShadingData loadShadingData(const HitInfo hit, const float3 rayDir, const ITextureSampler lod)
{
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    adjustShadingNormal(sd, v);
    
    return sd;
}

float3 photonContribution(in ShadingData sd, uint hash , inout SampleGenerator sg ,bool isCaustic)
{
    //get caustic or global photon
    float radius = isCaustic ? gCausticRadius : gGlobalRadius;
    uint mapIdx = isCaustic ? 0 : 1;
    uint2 texIdx = uint2(hash / gBucketYExtent, hash % gBucketYExtent);
    float4 photonPos = gHashBucketPos[mapIdx][texIdx];
    float4 photonDir = gHashBucketDir[mapIdx][texIdx];
    float4 photonFlux = gHashBucketFlux[mapIdx][texIdx];
    uint photonCount = gHashCounter[mapIdx][hash];

    //Do face normal test if enabled
    if(kUsePhotonFaceNormal){
        //Sperical to cartesian
        float sinTheta = sin(photonFlux.w);
        float3 photonFaceN = float3(cos(photonDir.w) * sinTheta, cos(photonFlux.w), sin(photonDir.w) * sinTheta);
        photonFaceN = normalize(photonFaceN);
        float3 faceN = dot(sd.V, sd.faceN) > 0 ? sd.faceN : -sd.faceN;
        //Dot product has to be negative (View dir points to surface)
        if(dot(faceN, photonFaceN) < 0.9f)
            return float3(0);
    }
    
    //Radius test
    if (!hitSphere(photonPos.xyz, radius, sd.posW) || photonCount == 0)
        return float3(0);
    
    
    let lod = ExplicitLodTextureSampler(0.f);
    let bsdf = gScene.materials.getBSDF(sd, lod);
            
    float3 f_r = bsdf.eval(sd, -photonDir.xyz,sg);
     
    return f_r * (photonFlux.xyz * photonCount);
}

float3 collectPhotons(in HitInfo hitInfo, in float3 dirVec, uint2 launchIndex, bool isCaustic)
{
    float3 radiance = float3(0);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = loadShadingData(hitInfo, dirVec, lod);
    
    SampleGenerator sg = SampleGenerator(launchIndex, gFrameCount);
    
    float radius = isCaustic ? gCausticRadius : gGlobalRadius;
    float scale = isCaustic ? gCausticHashScaleFactor : gGlobalHashScaleFactor;
    int3 gridCenter = int3(floor(sd.posW * scale));
    int gridRadius = int(ceil(radius * scale));
    //Loop over whole hash grid

    for (int z = gridCenter.z - gridRadius; z <= gridCenter.z + gridRadius; z++){
        for (int y = gridCenter.y - gridRadius; y <= gridCenter.y + gridRadius; y++){
            for (int x = gridCenter.x - gridRadius; x <= gridCenter.x + gridRadius; x++)
            {
                uint b = hash(int3(x, y, z)) & (kNumBuckets - 1);
                radiance += photonContribution(sd, b, sg ,isCaustic);        
            }
        }
    }
    return radiance;
}

[numthreads(16, 16, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 Gid : SV_GroupID, uint2 GTid : SV_GroupThreadID, uint GI : SV_GroupIndex)
{
    float3 viewVec = -gViewWorld[DTid].xyz;
    float4 thp = gThp[DTid];
    PackedHitInfo packedHitInfo = gVBuffer[DTid];
    const HitInfo hit = HitInfo(packedHitInfo);
    bool valid = hit.isValid(); //Check if the ray is valid
    float3 radiance = float3(0);


    if (gCollectGlobalPhotons && valid)
    {
        float3 globalRadiance = collectPhotons(hit, viewVec,DTid , false);
        float w = 1 / (M_PI * gGlobalRadius * gGlobalRadius); //make this a constant
        radiance += w * globalRadiance;
    }
    
    if (gCollectCausticPhotons && valid)
    {
        float3 causticRadiance = collectPhotons(hit, viewVec, DTid, true);
        float w = 1 / (M_PI * gCausticRadius * gCausticRadius); //make this a constant
        radiance += w * causticRadiance;
    }
    
    radiance *= thp.xyz;   //Add throughput for path

    //Add emission
    float3 pixEmission = gEmissive[DTid].xyz;
    radiance += pixEmission * thp.xyz;

     //Accumulate the image (Put in accumulate pass ? )
    if (gFrameCount > 0)
    {
        float3 last = gPhotonImage[DTid].xyz;
        float frameCountF = float(gFrameCount);
        last *= frameCountF;
        radiance += last;
        radiance /= frameCountF + 1.0;
    }
    
    gPhotonImage[DTid] = float4(radiance, 1);
}
