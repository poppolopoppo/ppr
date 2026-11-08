#ifndef PPR_GRID_SCREEN_CONTACT_TYPES_SLANG
#define PPR_GRID_SCREEN_CONTACT_TYPES_SLANG

// ------------------------------------------------------------------
// default-off centre-density backwall pilot,32-byte constant protocol
// ------------------------------------------------------------------

struct GridScreenContactParameters {
    uint width,height,left,top;
    uint countX,countY,enabled,stride;
};

struct GridScreenContactResult { float r,g,b; uint status; };

ConstantBuffer<GridScreenContactParameters> g_screen_contact;
StructuredBuffer<GridScreenContactResult> g_screen_incident;

uint2 screenBlockCentre(uint2 block) {
    uint stride=g_screen_contact.stride;
    uint2 first=block*stride;
    uint2 end=min(first+stride,uint2(g_screen_contact.width,g_screen_contact.height));
    return (first+end)/2u;
}

uint screenBlockIndex(uint2 block) {
    return block.y*g_screen_contact.countX+block.x;
}

uint screenNeighbourBit(int2 offset) {
    return uint((offset.y+1)*3+offset.x+1);
}

// A nearby receiver contributes only when the current mask contains an open
// path to its centre. Missing weights leave the unmodified world field.
float3 screenBlockDelta(uint2 block,int2 offset,uint neighbours) {
    if (any(offset!=0) && (neighbours & (1u<<screenNeighbourBit(offset)))==0u) return 0;
    GridScreenContactResult row=g_screen_incident[2u*screenBlockIndex(uint2(int2(block)+offset))];
    return float3(row.r,row.g,row.b);
}

float3 gridScreenCorrection(uint2 pixel) {
    uint stride=g_screen_contact.stride;
    uint2 block=pixel/stride;
    uint base=2u*screenBlockIndex(block);
    GridScreenContactResult first=g_screen_incident[base], second=g_screen_incident[base+1u];
    uint2 local=pixel-block*stride;
    uint lane=local.y*8u+local.x;
    uint mask=lane<32u ? first.status:second.status;
    if ((mask & (1u<<(lane%32u)))==0u) return 0;

    uint2 centre=screenBlockCentre(block);
    int2 offset=int2(pixel.x<centre.x?-1:1,pixel.y<centre.y?-1:1);
    int2 next=clamp(int2(block)+offset,int2(0),int2(g_screen_contact.countX,g_screen_contact.countY)-1);
    offset=next-int2(block);
    float2 span=abs(float2(screenBlockCentre(uint2(next)))-float2(centre));
    float2 weight=saturate(abs(float2(pixel)-float2(centre))/max(span,float2(1)));
    uint neighbours=uint(second.r);
    float3 top=lerp(float3(first.r,first.g,first.b),screenBlockDelta(block,int2(offset.x,0),neighbours),weight.x);
    float3 bottom=lerp(screenBlockDelta(block,int2(0,offset.y),neighbours),screenBlockDelta(block,offset,neighbours),weight.x);
    return lerp(top,bottom,weight.y);
}

// Both the cached1x and direct4x backwall load the SAME centre-density result.
// Unexpected invalid rows are conspicuous failed-pilot diagnostics, never a
// silent world-only/black fallback. This is not general product error reporting.
bool gridScreenIncident(uint2 pixel,out float3 incident) {
    incident=0;
    if (g_screen_contact.enabled==0u || any(pixel<uint2(g_screen_contact.left,g_screen_contact.top))) return false;
    uint2 local=pixel-uint2(g_screen_contact.left,g_screen_contact.top);
    if (local.x>=g_screen_contact.countX || local.y>=g_screen_contact.countY) return false;

    GridScreenContactResult row=g_screen_incident[local.y*g_screen_contact.countX+local.x];
    incident=row.status!=0u ? float3(64,0,64) : max(float3(row.r,row.g,row.b),float3(0));
    return true;
}
#endif
