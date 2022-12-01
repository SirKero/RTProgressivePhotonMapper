#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Scene.Material.ShadingUtils;
import Utils.Sampling.SampleGenerator;
import Rendering.Materials.StandardMaterial;
import Rendering.Lights.LightHelpers;

import PhotonMapperHashFunctions;

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
    uint gQuadProbeIt;  //Max num of quadratic probe iterations
    bool gEnableStochasicGathering; //Enable stochastic collection
    float gCollectProbability; //collection probability
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
struct PhotonBucket
{
    uint size;
    int cell;
    uint2 pad;
    uint photonIdx[NUM_PHOTONS_PER_BUCKET];
};

struct PhotonInfo
{
    float4 dir;
    float4 flux;
};

 //Internal Buffer Structs
StructuredBuffer<PhotonBucket> gGlobalHashBucket;
StructuredBuffer<PhotonBucket> gCausticHashBucket;

RWTexture2D<float4> gCausticPos;
RWTexture2D<float4> gCausticFlux;
RWTexture2D<float4> gCausticDir;
RWTexture2D<float4> gGlobalPos;
RWTexture2D<float4> gGlobalFlux;
RWTexture2D<float4> gGlobalDir;


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

float3 photonContribution(in ShadingData sd,in const IBSDF bsdf, uint photonIndex, inout SampleGenerator sg , bool isCaustic)
{
    const uint2 photonIndex2D = uint2(photonIndex / kInfoTexHeight, photonIndex % kInfoTexHeight);
    //get caustic or global photon
    float radius = isCaustic ? gCausticRadius : gGlobalRadius;
    PhotonInfo photon;
    float3 photonPos;
     //Instance 0 is always the caustic buffer
    if (isCaustic)
    {
        photonPos = gCausticPos[photonIndex2D].xyz;
        photon.flux = gCausticFlux[photonIndex2D];
        photon.dir = gCausticDir[photonIndex2D];
    }
    else
    {
        photonPos = gGlobalPos[photonIndex2D].xyz;
        photon.flux = gGlobalFlux[photonIndex2D];
        photon.dir = gGlobalDir[photonIndex2D];
    }

    //Do face normal test if enabled
    if(kUsePhotonFaceNormal){
        //Sperical to cartesian
        float sinTheta = sin(photon.flux.w);
        float3 photonFaceN = float3(cos(photon.dir.w) * sinTheta, cos(photon.flux.w), sin(photon.dir.w) * sinTheta);
        photonFaceN = normalize(photonFaceN);
        float3 faceN = dot(sd.V, sd.faceN) > 0 ? sd.faceN : -sd.faceN;
        //Dot product has to be negative (View dir points to surface)
        if(dot(faceN, photonFaceN) < 0.9f)
            return float3(0);
    }
    
    //Radius test
    if (!hitSphere(photonPos, radius, sd.posW))
        return float3(0);
                
    float3 f_r = bsdf.eval(sd, -photon.dir.xyz,sg);
     
    return f_r * photon.flux.xyz;
}

//returns geometrically distributed random number generator
//param[in] p: probability
//param[in] u: uniform distributed random number
uint step(float p, float u)
{
    return uint(floor(log(u) / log(1.f - p)));
}

float3 collectPhotons(in HitInfo hitInfo, in float3 dirVec, uint2 launchIndex,bool isCaustic)
{
    float3 radiance = float3(0);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = loadShadingData(hitInfo, dirVec, lod);
    
    const IBSDF bsdf = gScene.materials.getBSDF(sd, lod);
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
                uint d = 0;
                uint bucketSize = 0;
                bool validBucket = false;
                //Quadratic Probe with an maximum
                for (uint i = 0; i < gQuadProbeIt; i++)
                {
                    bucketSize = isCaustic ? gCausticHashBucket[b].size : gGlobalHashBucket[b].size;
                    int bucketCell = isCaustic ? gCausticHashBucket[b].cell : gGlobalHashBucket[b].cell;
                    int cellXY = (x << 16) | (y & 0xFFFF);
                    if (cellXY == 0)
                        cellXY = 0xFFFFFFFF; //avoid zero cell index
                    //Stop on empty bucket
                    if (bucketSize == 0)
                        break;
                    //If cell is the same collect all photons and stop loop for this cell at the end
                    if (bucketCell == cellXY)
                    {
                        validBucket = true;
                        break;  //Stop for this cell
                    }
                    
                    //quadratic probe next bucket
                    ++d;
                    b = (b + ((d + d * d) >> 1)) & (kNumBuckets - 1);
                }

                //If cell is the same collect all photons and stop loop for this cell at the end
                if (validBucket)
                {
                    uint photonCellIt = min(bucketSize, NUM_PHOTONS_PER_BUCKET);
                    float3 cellRadiance = float3(0);
                    float u = gEnableStochasicGathering ? sampleNext1D(sg) :0.0;
                    //Guarantee that at least 1 photon is collected per cell 
                    uint startIdx = gEnableStochasicGathering ? min(step(gCollectProbability, u), photonCellIt-1) : 0;
                    uint collectedPhotons = 0;
                    for (uint idx = startIdx; idx < photonCellIt; idx++)
                    {
                        uint photonIdx = isCaustic ? gCausticHashBucket[b].photonIdx[idx] : gGlobalHashBucket[b].photonIdx[idx];
                        cellRadiance += photonContribution(sd, bsdf, photonIdx, sg ,isCaustic);
                        //add a stochasic step on top i if enabled
                        if (gEnableStochasicGathering)
                        {
                            u = sampleNext1D(sg);
                            idx += step(gCollectProbability, u);
                        }
                        collectedPhotons++;
                    }
                    radiance += collectedPhotons > 0 ? cellRadiance * (bucketSize / collectedPhotons) : 0.0;
                }
                
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
        float3 globalRadiance = collectPhotons(hit, viewVec, DTid, false);
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
