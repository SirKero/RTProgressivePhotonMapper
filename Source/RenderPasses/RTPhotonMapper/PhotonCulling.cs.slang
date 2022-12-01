#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Rendering.Materials.StandardMaterial;
import Rendering.Lights.LightHelpers;

import PhotonCullingHash;

cbuffer PerFrame
{
    float gHashScaleFactor;    //Scale factor calculated from photon radius
    uint gHashSize;            //Size of the hash
    uint gYExtend;
    float gProjTest;            //Factor for the projection test 
}

static const bool kUseProjMatrixCulling = CULLING_USE_PROJECTION;

Texture2D<PackedHitInfo> gVBuffer;

RWTexture2D<uint> gHashBuffer;

void getArrayOfCells(float3 position, out int3 outCells[8]){
    float3 cell = (position.xyz * gHashScaleFactor);
    float3 cellFloor = floor(cell);
    cell = abs(cell - cellFloor);
    //Get offset direction
    int3 offsetCell;
    offsetCell.x = cell.x < 0.5 ? -1 : 1;
    offsetCell.y = cell.y < 0.5 ? -1 : 1;
    offsetCell.z = cell.z < 0.5 ? -1 : 1;

    //Fill the cell array
    outCells[0] = int3(cellFloor);
    outCells[1] = outCells[0] + int3(offsetCell.x, 0 ,0);
    outCells[2] = outCells[0] + int3(offsetCell.x, offsetCell.y ,0);
    outCells[3] = outCells[0] + int3(offsetCell.x, offsetCell.y ,offsetCell.z);
    outCells[4] = outCells[0] + int3(offsetCell.x, 0 ,offsetCell.z);
    outCells[5] = outCells[0] + int3(0, offsetCell.y ,0);
    outCells[6] = outCells[0] + int3(0, offsetCell.y ,offsetCell.z);
    outCells[7] = outCells[0] + int3(0, 0 ,offsetCell.z);
}

[numthreads(16, 16, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 Gid : SV_GroupID, uint2 GTid : SV_GroupThreadID, uint GI : SV_GroupIndex)
{
    uint2 texIndex = DTid;
    

    const HitInfo hit = HitInfo(gVBuffer[DTid]);

    if (!hit.isValid())
        return;
    
    const float4 pos = float4(gScene.getVertexData(hit.getTriangleHit()).posW, 1);

    if(!kUseProjMatrixCulling){
        //Insert one for every hash where a photon can be
        int3 cells[8];
        getArrayOfCells(pos.xyz, cells);
                
        [unroll]
        for(uint i = 0; i<8 ; i++){
            uint h = hash(cells[i]) & (gHashSize - 1);
            uint2 hIdx = uint2(h % gYExtend, h / gYExtend);
            gHashBuffer[hIdx] = 1;  //it does not matter if multiple threads write to one hash
        }
    }
    else{
        float4 projPos = mul(pos, gScene.camera.getViewProj());
        projPos /= projPos.w;
    
        //insert box if it is outside of perspective camera
        if (any(abs(projPos.xy) > gProjTest) || projPos.z > 1.f || projPos.z < 0.f)
        {
            int3 cells[8];
            getArrayOfCells(pos.xyz, cells);

            [unroll]
            for(uint i = 0; i<8 ; i++){
                uint h = hash(cells[i]) & (gHashSize - 1);
                uint2 hIdx = uint2(h % gYExtend, h / gYExtend);
                gHashBuffer[hIdx] = 1;  //it does not matter if multiple threads write to one hash
            }
        }
    }
}
