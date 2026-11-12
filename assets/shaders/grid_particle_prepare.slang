#include "grid_particle_native.slang"

// Immutable cold row (24 bytes); the pack shifts volume into a physical radius.
struct ReadonlyRow { float mass, volume, material, r, g, b; };

// Renderer-owned 24-byte presentation row (plan §3 "Instance layout"). CPU twin:
// pP::ParticleGpu in lib/engine/app/renderer/App.Renderer.GridPass.cppm; the
// draw-side mirror is ParticleGpu in grid_tiles.slang. Position and radius stay
// float32 world metres: the float ulp at the 1365 m map corner is 4.1e-3 px at
// span64 where an absolute half2 position would be 8.44 px. Only the material
// word, the tint and the motion pair are quantised.
struct PresentationRow {
    float2 position;                  // 0:  world metres
    float radius;                     // 8:  metres
    uint material_flags_temperature;  // 12: material 12 | class 4 << 12 | binary16 K << 16
    uint color_rgb9e5;                // 16: RGB9E5 palette tint; alpha not stored
    half2 motion;                     // 20: native cells per solver step
};

// Class nibble of material_flags_temperature. It sits beside the material id
// because a gas row and a particle can share one material id. Bits 2..3 are
// reserved; solid versus liquid is selected by the material's own surface flags.
static const uint kParticleClassGas = 1u;
static const uint kParticleClassOptical = 2u;
static const uint kParticleMaterialMask = 0xfffu;
static const uint kParticleMaterialLimit = 4096u;
// Presentation clamp for the motion proxy. Not a solver limit.
static const float kParticleMotionLimit = 64.0;

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

// Shared-exponent RGB9E5, the same codec as the host `lighting::packRGB9E5`:
// mantissas in bits 0..26, exponent in 27..31, nearest/ties-up rounding. The
// source is the palette's RGBA8 tint, so every channel is finite and in [0,1]
// and the rejection branch is unreachable for valid content; it still resolves
// to a neutral tint so a future non-8-bit source can never place a NaN in the
// row or fail the frame.
uint packParticleTint(float3 rgb) {
    if (!all(isfinite(rgb)) || any(rgb < 0.0) || any(rgb > 65408.0))
        rgb = float3(1.0);
    if (all(rgb == 0.0))
        return 0u;
    for (uint exponent=0u;exponent<=31u;++exponent) {
        float3 scaled=rgb*exp2(float(int(24)-int(exponent)));
        uint3 mantissa=uint3(floor(scaled+0.5));
        if (!any(mantissa>uint3(511u)))
            return mantissa.x|(mantissa.y<<9u)|(mantissa.z<<18u)|(exponent<<27u);
    }

    return 0u;
}

[shader("compute")]
[numthreads(256,1,1)]
void packParticles(uint3 id:SV_DispatchThreadID) {
    if (id.x>=g_particle_prepare.count) return;

    MetadataRow metadata=g_metadata[id.x];
    uint material=metadata.materialTemperature & 0xffffu;

    // A rejected or disabled row is written all-zero and never enters the
    // compaction: zero radius cannot pass visible(), so the indirect instance
    // count is the observable rejection count and no stale bits survive.
    PresentationRow particle=(PresentationRow)0;
    uint class_flags=0u;

    if (metadata.sourceRow!=0xffffffffu) {
        NumericalRow hot=g_numerical[metadata.sourceRow];
        ReadonlyRow cold=g_readonly[metadata.sourceRow];
        // Disabled rows are excluded at pack time, before the compaction count.
        if (!(hot.enabled>0.0)) {
            g_packed[id.x]=particle;
            return;
        }

        // The hot row is the authoritative live position. It is written in world
        // metres directly: one origin shift and one cell-to-metre scale, with
        // every binary32 quantum preserved (no cell rounding).
        float2 position=(float2(hot.positionX,hot.positionY)-g_particle_prepare.origin)*g_particle_prepare.cellMetres;
        // Cold volume is in cell^2*reference-thickness units. Its disk area
        // is volume/thickness, so physical radius is h*sqrt(volume/pi).
        float radius=g_particle_prepare.cellMetres*sqrt(cold.volume/3.141592653589793);
        if (!all(isfinite(position)) || !isfinite(radius) || radius<=0.0) {
            g_packed[id.x]=particle;
            return;
        }

        particle.position=position;
        particle.radius=radius;
        // Native cells per solver step, with no dt division: the per-step
        // displacement is the only motion the solver publishes. Non-finite
        // components become 0 and each component is clamped to the presentation
        // limit, so nothing unbounded reaches the vertex stage.
        float2 displacement=float2(hot.displacementX,hot.displacementY);
        float2 motion=float2(
            isfinite(displacement.x) ? clamp(displacement.x,-kParticleMotionLimit,kParticleMotionLimit) : 0.0,
            isfinite(displacement.y) ? clamp(displacement.y,-kParticleMotionLimit,kParticleMotionLimit) : 0.0);
        particle.motion=half2(half(motion.x),half(motion.y));
    } else {
        // Gas keeps its own path and range rule: metadata position and radius
        // are already metres, and a gas row has no solver displacement.
        particle.position=float2(metadata.gasX,metadata.gasY);
        particle.radius=metadata.radius;
        particle.motion=half2(half(0.0),half(0.0));
        class_flags|=kParticleClassGas;
        if (!all(isfinite(particle.position)) || !isfinite(particle.radius) || particle.radius<=0.0) {
            g_packed[id.x]=particle;
            return;
        }
    }

    // The material id rides the row in 12 bits. A larger id is rejected rather
    // than truncated into a different valid material.
    if (material>=kParticleMaterialLimit) {
        g_packed[id.x]=(PresentationRow)0;
        return;
    }

    // MaterialColor mirrors GridTerrainVisual: words 0/1 are material and rgba,
    // so carried[17] is m_surface_flags (byte 76) and carried[18] is
    // m_transport_model (byte 80). Bit 0 of the surface flags is
    // kGridSurfaceOpticalBlend on the CPU and kSurfaceOpticalBlend in
    // grid_tiles.slang.
    uint surface_flags=g_particle_palette[material].carried[17];
    if ((surface_flags&1u)!=0u)
        class_flags|=kParticleClassOptical;

    // Tint comes from the authored palette word for every row; the cold r/g/b
    // columns are not initialised by the app. Alpha is deliberately not stored:
    // the material's own alpha supplies opacity.
    uint rgba=g_particle_palette[material].rgba;
    particle.color_rgb9e5=packParticleTint(float3((rgba>>24u)&255u,(rgba>>16u)&255u,(rgba>>8u)&255u)/255.0);
    // The material is not stored in its canonical packCell position: material,
    // class and temperature share one word. Shading reconstructs
    // canonical = material | (temperature << 16).
    particle.material_flags_temperature=
        (material&kParticleMaterialMask)|(class_flags<<12u)|(metadata.materialTemperature&0xffff0000u);

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

    ParticleNativeCell cell=particleNativeCell(metadata,float2(hot.positionX,hot.positionY),hot.enabled,
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
    // A rejected pack writes an all-zero row, and no row may carry a non-finite
    // coordinate into a clip position. Rejecting both here means the compaction
    // count excludes them and the vertex stage never loads such a row.
    if (!all(isfinite(particle.position)) || !(particle.radius>0.0))
        return false;
    // Enclose the drawn support, not only the disk quad: liquid is a disk of
    // radius R = 2.0 r and the solid rounded square is inscribed in the same
    // circle, so both stay inside this shared 2.45 r cull margin (which also
    // covers the optional 1.2x stretch at 2.400 r).
    float radius=particle.radius*kParticleCullRadiusScale;
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
