#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Scene.Camera.CameraData;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.GeometryHelpers;
import Scene.Material.ShadingUtils;
import Rendering.Lights.LightHelpers;



cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was loaded.
};

cbuffer CB
{
    uint gMaxRecursion;     //Maximum length a path can travel
    float gSpecularRougnessCutoff; //cutoff for when a hit counts as diffuse
    bool gAdjustShadingNormals; //Adjusts the shading normals if activated
    bool gUseAlphaTest;     //uses alpha test if activated
    
    bool gUseRandomPixelPosCamera;  //Uses a randomly generated pixel offset for camera direction sampeling instead of fixed jitter
    uint3 _pad;
};

// Outputs
RWTexture2D<PackedHitInfo> gVBuffer;
RWTexture2D<float4> gViewWorld;
RWTexture2D<float4> gThp;
RWTexture2D<float4> gEmissive;

//Optional
RWTexture2D<float> gLinDepth;
RWTexture2D<float2> gMVec;

#define is_valid(name) (is_valid_##name != 0)

static const bool kComputeDepthOfField = COMPUTE_DEPTH_OF_FIELD;

/** Payload for scatter ray (64B).
*/
struct RayData
{
    float3 thp; ///< Current path throughput. This is updated at each path vertex.
    bool terminated; ///< Set to true when path is terminated.
    float3 origin; ///< Next path segment origin.
    uint pathLength; ///<Length of the path
    float3 direction; ///< Next path segment direction.
    float distance;
    
    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).

    /** Create ray payload with default parameters.
    */
    __init(SampleGenerator sg){
        this.thp = float3(1.0);
        this.terminated = false; 
        this.origin = float3(0); 
        this.distance = 0.f; 
        this.direction = float3(0); 
        this.pathLength = 0;
        this.sg = sg;
    }
};

/** Computes a camera ray for a given pixel assuming a thin-lens camera model with a random instead of a fixed jitter. Based on computeRandomRayThinlens()
        The camera jitter is taken into account to compute the sample position on the image plane.
        \param[in] pixel Pixel coordinates with origin in top-left.
        \param[in] frameDim Image plane dimensions in pixels.
        \param[in] u Uniform 2D sample. Used for pixel pos.
        \param[in] v Uniform 2D sample. Used for thin lens sample.
        \param[in] data Camera data.
        \return Returns the camera ray.
    */
Ray cameraComputeRandomRayThinlens(uint2 pixel, uint2 frameDim, float2 u , float2 v, in const CameraData data)
{
    Ray ray;

    // Sample position in screen space in [0,1] with origin at the top-left corner.
    // The camera jitter offsets the sample by +-0.5 pixels from the pixel center.
    float2 p = (pixel + u) / frameDim;
    float2 ndc = float2(2, -2) * p + float2(-1, 1);

    // Compute the normalized ray direction assuming a thin-lens camera.
    ray.origin = data.posW;
    ray.dir = ndc.x * data.cameraU + ndc.y * data.cameraV + data.cameraW;
    float2 apertureSample = sample_disk(v); // Sample lies in the unit disk [-1,1]^2
    float3 rayTarget = ray.origin + ray.dir;
    ray.origin += data.apertureRadius * (apertureSample.x * normalize(data.cameraU) + apertureSample.y * normalize(data.cameraV));
    ray.dir = normalize(rayTarget - ray.origin);

    float invCos = 1.f / dot(normalize(data.cameraW), ray.dir);
    ray.tMin = data.nearZ * invCos;
    ray.tMax = data.farZ * invCos;

    return ray;
}

void writeHit(uint2 pixel, float3 rayDir, float3 thp, float3 emissive, const in HitInfo hit,const float linZ )
{
    gVBuffer[pixel] = hit.getData();
    gViewWorld[pixel] = float4(rayDir, 1);
    gThp[pixel] = float4(thp, 1);
    gEmissive[pixel] = float4(emissive, 1);

    //If depth in valid write it
    if (is_valid(gLinDepth)) gLinDepth[pixel] = linZ;
}

void writeMiss(uint2 pixel)
{
    gVBuffer[pixel] = { };

    if (is_valid(gLinDepth)) gLinDepth[pixel] = 0.f;
    if (is_valid(gMVec)) gMVec[pixel] = { };
}

//TODO: Add other miss colors, as throughput then has to be respected
[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    rayData.terminated = true;
    writeMiss(launchIndex);
}

//add anyHit shader for alpha test
[shader("anyhit")]
void anyHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    if (gUseAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}

bool generateNewRay(const in ShadingData sd, const in IBSDF bsdf, float3 rayOrigin, inout BSDFSample bsdfSample, inout RayData rayData)
{
    // Sample material.
    if (bsdf.sample(sd, rayData.sg, bsdfSample, true /*Importance Sampeling*/))
    {
        rayData.origin = rayOrigin;
        rayData.direction = bsdfSample.wo;
        if (bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = computeRayOrigin(sd.posW, dot(sd.faceN, bsdfSample.wo) <= 0.f ? -sd.faceN : sd.faceN);
        }
        
        rayData.thp *= bsdfSample.weight;
        return any(rayData.thp > 0.f);
    }

    return false;
}

[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    uint2 launchIndex = DispatchRaysIndex().xy;
   // Evaluate Falcor's material parameters at the hit point.
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);

    //Calc Motion Vector if it is valid on first valid hit
    if(is_valid(gMVec) && rayData.pathLength == 0 && !gUseRandomPixelPosCamera){
        float3 prevPosW = gScene.getPrevPosW(triangleHit);
        float2 pixelPos = DispatchRaysIndex().xy + float2(0.5f, 0.5f);
        float4 prevPosH = mul(float4(prevPosW, 1.f), gScene.camera.data.prevViewProjMatNoJitter);
        float2 motionVec = calcMotionVector(pixelPos, prevPosH, DispatchRaysDimensions().xy);
        // Remove camera jitter from motion vector 
        motionVec += float2(gScene.camera.data.jitterX, -gScene.camera.data.jitterY); //JitterX is subtracted, JitterY is added in Cam jitter calc
        
        gMVec[launchIndex] = motionVec;
    }

    rayData.pathLength++;
    
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayData.direction, lod);

    
    if (gAdjustShadingNormals)
    {
        adjustShadingNormal(sd, v);
    }
    
    float3 thpStart = rayData.thp;
    rayData.distance += distance(ObjectRayOrigin(), sd.posW);   //accumulate ray distance
    
    BSDFSample bsdfSample;
    let bsdf = gScene.materials.getBSDF(sd, lod);
        
    if (!generateNewRay(sd, bsdf, sd.computeNewRayOrigin(), bsdfSample, rayData))
    {
        rayData.distance = -1.f;
        rayData.terminated = true;
        return;
    }

    //get material properties
    let bsdfProperties = bsdf.getProperties(sd);

    bool isDiffuse = bsdfProperties.roughness > gSpecularRougnessCutoff;
       
    //store hit if lobe is diffuse
    if ((bsdfSample.isLobe(LobeType::DiffuseReflection) || (bsdfSample.isLobe(LobeType::Reflection) && isDiffuse)) && (!rayData.terminated || isDiffuse))
    {        
        writeHit(launchIndex, -WorldRayDirection(), thpStart, bsdfProperties.emission * bsdfSample.weight, HitInfo(triangleHit), rayData.distance);
        
        rayData.terminated = true;
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    float3 outColor = float3(0);
    float3 throughput = float3(1);
    RayData rayData = RayData(SampleGenerator(launchIndex, gFrameCount)); //lazy init for now
    Ray camRay;
    if (gUseRandomPixelPosCamera)
    {
        float2 rndJitter = sampleNext2D(rayData.sg);
        camRay = cameraComputeRandomRayThinlens(launchIndex, launchDim, rndJitter, sampleNext2D(rayData.sg), gScene.camera.data);
    }
    else
    {
        camRay = gScene.camera.computeRayThinlens(launchIndex, launchDim, sampleNext2D(rayData.sg)); //get jittered ray
    }
   
    uint rayFlags = 0;
    RayDesc ray;
    ray.Origin = camRay.origin;
    ray.Direction = camRay.dir;
    ray.TMax = camRay.tMax;
    ray.TMin = camRay.tMin;

    rayData.origin = ray.Origin;
    rayData.direction = ray.Direction;
    
    //Trace recursive
    for (uint i = 0; i < gMaxRecursion; i++)
    {
        TraceRay(gScene.rtAccel, 0, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);

        //If ray is invalid return
        if (rayData.terminated){
            if(rayData.distance < 0)
                writeMiss(launchIndex);
            break;
        }
           
        //Update ray for next iteration
        ray.Origin = rayData.origin;
        ray.Direction = rayData.direction;
        if(i==0){
            ray.TMin = 0;    //Change tMin for transparent hits
        }
    }
}
