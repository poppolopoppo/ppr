#include "grid_particle_native.slang"

// Continuous GPU circle presentation; no terrain bins or occupancy geometry.
struct ReadonlyRow { float mass, volume, material, r, g, b; };
struct PresentationRow {
    float2 position;
    float radius;
    uint material;
    float4 color;
};
// Same 96-byte GridTerrainVisual palette; circle packing consumes only ID/tint.
// Retained carrier words preserve the full authored transport append stride.
struct MaterialColor { uint material, rgba; uint carried[22]; };
struct PrepareFrame {
    float4 planes[6];
    float cellMetres, origin;
    uint count, movingCount;
    uint blockCount, censusWorldEdge, padding1, padding2;
};
ConstantBuffer<PrepareFrame> g_particle_prepare;
StructuredBuffer<NumericalRow> g_numerical;
StructuredBuffer<ReadonlyRow> g_readonly;
StructuredBuffer<MetadataRow> g_metadata;
StructuredBuffer<MaterialColor> g_particle_palette;
RWStructuredBuffer<PresentationRow> g_packed;
RWStructuredBuffer<uint> g_ranks;
RWStructuredBuffer<uint> g_block_counts;
RWStructuredBuffer<uint> g_block_prefix;
RWStructuredBuffer<uint> g_particle_ids;
RWStructuredBuffer<uint> g_particle_args;

// ------------------------------------------------------------------
// physical circle packing with the committed hot/cold row identity
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(256,1,1)]
void packParticles(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_particle_prepare.count) return;

    MetadataRow metadata=g_metadata[id.x];
    uint material=metadata.materialTemperature & 0xffffu;
    PresentationRow particle;
    particle.material=material;
    if (metadata.sourceRow!=0xffffffffu) {
        NumericalRow hot=g_numerical[metadata.sourceRow];
        ReadonlyRow cold=g_readonly[metadata.sourceRow];
        particle.position=(float2(hot.positionX,hot.positionY)-g_particle_prepare.origin)*g_particle_prepare.cellMetres;
        // Cold volume is in cell^2*reference-thickness units. Its disk area
        // is volume/thickness, so physical radius is h*sqrt(volume/pi).
        particle.radius=g_particle_prepare.cellMetres*sqrt(cold.volume/3.141592653589793);
        particle.color=float4(cold.r,cold.g,cold.b,1.0);
    } else {
        particle.position=float2(metadata.gasX,metadata.gasY);
        particle.radius=metadata.radius;
        uint rgba=g_particle_palette[material].rgba;
        particle.color=float4(float3((rgba>>24u)&255u,(rgba>>16u)&255u,(rgba>>8u)&255u)/255.0,1.0);
    }

    g_packed[id.x]=particle;
}

// ------------------------------------------------------------------
// capture-only committed native-cell census, before any visual compaction
// ------------------------------------------------------------------
struct CensusRow {
    uint idLow, idHigh, material, status;
    uint cellX, cellY;
};
RWStructuredBuffer<CensusRow> g_particle_census;

[shader("compute")]
[numthreads(256,1,1)]
void projectParticleCensus(uint3 id:SV_DispatchThreadID) {
    if (id.x >= g_particle_prepare.count) return;

    MetadataRow metadata = g_metadata[id.x];
    CensusRow row;
    row.idLow = metadata.idLow;
    row.idHigh = metadata.idHigh;
    row.material = metadata.materialTemperature & 0xffffu;
    // Existing palette word20 is the explicitly authored transport tag. This
    // reports declared vacuum, never infers optics from solver class/opacity.
    row.status = row.material == 0u || g_particle_palette[row.material].carried[18] == 0x101u ? 8u : 0u;
    row.cellX = row.cellY = 0xffffffffu;
    bool moving=metadata.sourceRow!=0xffffffffu;
    bool sourceValid=!moving || metadata.sourceRow<g_particle_prepare.movingCount;
    NumericalRow hot=(NumericalRow)0;
    if (moving && sourceValid) hot=g_numerical[metadata.sourceRow];

    ParticleNativeCell cell=particleNativeCell(metadata,float2(hot.positionX,hot.positionY),hot.carried[13],
        sourceValid,g_particle_prepare.cellMetres,g_particle_prepare.origin,float2(0),
        uint2(g_particle_prepare.censusWorldEdge));
    row.status|=cell.status;row.cellX=cell.x;row.cellY=cell.y;

    g_particle_census[id.x] = row;
}

// ------------------------------------------------------------------
// stable per-particle frustum compaction and indirect count
// ------------------------------------------------------------------
groupshared uint g_scan[256];

void scanLane(uint lane, uint value) {
    g_scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < 256u; offset *= 2u) {
        uint prior = lane >= offset ? g_scan[lane - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[lane] += prior;
        GroupMemoryBarrierWithGroupSync();
    }
}

bool visible(PresentationRow particle) {
    // Enclose the disk billboard quad corners; retired solid/liquid envelopes are
    // no longer drawn, so their wider support cannot hide a live gas disk.
    float radius=particle.radius*1.5;
    for (uint plane=0u;plane<6u;++plane) {
        float4 p=g_particle_prepare.planes[plane];
        if (dot(float4(particle.position,0,1),p)+radius*length(p.xyz)<0) return false;
    }

    return true;
}

[shader("compute")]
[numthreads(256,1,1)]
void rankParticles(uint3 id:SV_DispatchThreadID,uint3 local:SV_GroupThreadID,uint3 group:SV_GroupID) {
    bool selected=id.x<g_particle_prepare.count ? visible(g_packed[id.x]) : false;
    scanLane(local.x,selected ? 1u : 0u);
    if (id.x<g_particle_prepare.count) g_ranks[id.x]=(g_scan[local.x]-(selected ? 1u : 0u))|(selected ? 0x80000000u : 0u);
    if (local.x==255u) g_block_counts[group.x]=g_scan[255u];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void scanParticleBlocks(uint3 local : SV_GroupThreadID) {
    // At most 4096 block totals for the admitted 1M population. All 256 lanes
    // participate; each lane visits at most 16 values, independent of N.
    uint prefixes[16];
    uint sum = 0u;
    for (uint item = 0u; item < 16u; ++item) {
        uint block = local.x * 16u + item;
        prefixes[item] = sum;
        if (block < g_particle_prepare.blockCount) sum += g_block_counts[block];
    }

    scanLane(local.x, sum);
    uint base = g_scan[local.x] - sum;
    for (uint item = 0u; item < 16u; ++item) {
        uint block = local.x * 16u + item;
        if (block < g_particle_prepare.blockCount) g_block_prefix[block] = base + prefixes[item];
    }

    if (local.x == 255u) g_block_prefix[g_particle_prepare.blockCount] = g_scan[255u];
}

[shader("compute")]
[numthreads(256,1,1)]
void scatterParticles(uint3 id:SV_DispatchThreadID) {
    if (id.x==0u) {
        g_particle_args[0]=6u;
        g_particle_args[1]=g_block_prefix[g_particle_prepare.blockCount];
        g_particle_args[2]=0u;
        g_particle_args[3]=0u;
    }

    if (id.x>=g_particle_prepare.count) return;

    uint rank=g_ranks[id.x];
    if ((rank&0x80000000u)!=0u) {
        uint before=g_block_prefix[id.x/256u]+(rank&0x7fffffffu);
        g_particle_ids[before]=id.x;
    }
}
