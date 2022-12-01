#include "Scene/SceneDefines.slangh"


import Scene.Raytracing;
import Scene.Intersection;
import Scene.Camera.CameraData;

cbuffer PerFrame
{
    float gGlobalRadius;
    float gCausticRadius;
}

cbuffer CB
{
    float3 gCamPosW;
    float3 gCameraU;
    float3 gCameraV;
    float3 gCameraW;
};

// Outputs
RWTexture2D<float4> gPhotonImage;

//AABB buffers
StructuredBuffer<AABB> gPhotonAABB[2];

//Acceleration Structure
RaytracingAccelerationStructure gPhotonAS;


/** Payload for ray (16B).
*/
struct RayData
{
    float distance;
    float3 color;
};

struct SphereAttribs
{
    float2 pad;
};

[shader("miss")]
void miss(inout RayData rayData : SV_RayPayload)
{
    // Nothing happens here. Just here for completions sake
}

[shader("closesthit")]
void closestHit(inout RayData rayData : SV_RayPayload, SphereAttribs attribs : SV_IntersectionAttributes)
{
    rayData.color = InstanceIndex() == 0 ? float3(1, 0.4, 0.4) : float3(0.4, 1 , 0.4);
}

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

//Look at https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-sphere-intersection
[shader("intersection")]
void intersection()
{
    SphereAttribs attribs;
    const uint primIndex = PrimitiveIndex();
    AABB photonAABB = gPhotonAABB[InstanceIndex()][primIndex];
    float radius = InstanceIndex() == 0 ? gCausticRadius : gGlobalRadius;

    float3 L = photonAABB.center() - ObjectRayOrigin();
    float tca = dot(L, ObjectRayDirection());
    float d2 = dot(L,L) - tca * tca;
    float rad2 = radius * radius;
    if(d2 > rad2) return;
    float thc = sqrt(rad2 - d2);
    float t0 = tca-thc;

    ReportHit(t0, 0, attribs);

}

[shader("raygeneration")]
void rayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    //prepare payload
    RayData rayData;
    rayData.distance = 100000000.f;
    rayData.color = float3(0);
    
    Ray debugCamRay;
    //Pinhole Camera model
    debugCamRay.origin = gCamPosW;
    float2 p = (launchIndex + float2(0.5f, 0.5f)) / launchDim;
    float2 ndc = float2(2, -2) * p + float2(-1, 1);
    // Compute the non-normalized ray direction assuming a pinhole camera.
    float3 dir = ndc.x * gCameraU + ndc.y * gCameraV + gCameraW;
    debugCamRay.dir = normalize(dir);

    
    RayDesc ray;
    ray.Origin = debugCamRay.origin;
    ray.TMin = 0.0000001f;
    ray.TMax = 10000000.f;
    ray.Direction = debugCamRay.dir;
    
    TraceRay(gPhotonAS, 0, 0xFF/* instanceInclusionMask */, 0 /* hitIdx */, 0 /* rayType count */, 0 /* missIdx */, ray, rayData);
      
    gPhotonImage[launchIndex] = float4(rayData.color, 1);
}
