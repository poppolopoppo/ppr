// ------------------------------------------------------------------
// cascade world light field: persistent radiance + temporal update ABI
// ------------------------------------------------------------------
// Shared scalar ABI for the temporal world light field. One cascade hierarchy:
// level 0 = 2-cell spacing / 4 directions; level l = 2^(l+1) spacing /
// 4^(l+1) directions. Non-coarsest levels store the per-parent-corner connector
// fold; level 0 holds the fully merged field and shading reads it alone. A
// radiance sample is 8 bytes: RGB radiance plus a scalar transmittance, stored as
// four 16-bit halves. Per-direction counts are one u32 per sample, incremented
// saturating as diagnostic visit metadata (never a mean weight); a zero count is
// the dirty/fresh sentinel and the persistent invalidation backlog, so a reset
// clears counts only and the last published radiance/transmittance remains
// visible until that sample is traced again. The two radiance buffers
// ping-pong (previous/current).
#ifndef PPR_WORLD_LIGHT_FIELD_SLANG
#define PPR_WORLD_LIGHT_FIELD_SLANG

#include "world_light_source.slang"

static const uint kWorldLightMaxLevels = 8u;

// One cascade level, matched field-for-field by the C++ WorldLightLevelGpu.
// probeOffset/sampleOffset index the shared per-probe and per-sample regions.
struct WorldLightLevelGpu {
    uint spacing, directions;
    uint probeOffset, sampleOffset;
    uint probesX, probesY;
    float nearM, farM;
};

// Field constants, matched field-for-field by the C++ WorldLightFieldConstants.
// Scalar pad fields avoid Slang ConstantBuffer array-stride padding.
struct WorldLightFieldConstants {
    float originX, originY;
    uint width, height;
    uint levelCount;
    float cellMetres;
    uint sourceRevision;
    uint frame;
    uint budget;
    float frameDelta;
    uint pad0, pad1;
    WorldLightLevelGpu levels[kWorldLightMaxLevels];

    // Angular sky at the coarsest level: solar hour (negative disables it) and
    // the reference sky-intensity scale.
    float skyHour, skyIntensity;
};

// Ordinary group dispatch: which level, the raw padded-domain group cursor
// begin/count, and the persistent four-sweep phase. Each visited 2x2 packed
// group writes one stored direction sample; padding groups are charged but
// write nothing.
struct WorldLightFieldDispatch {
    uint level;
    uint groupBegin;
    uint groupCount;
    uint phase;
};

// Dirty reset dispatch: which level and the dirty probe rectangle (half-open
// [probeX0, probeX0+rectWidth) x [probeY0, probeY0+rectHeight)). One thread per
// direction sample in row-major probe order clears counts only; published
// radiance stays until the sample is traced again.
struct WorldLightFieldReset {
    uint level;
    uint probeX0, probeY0;
    uint rectWidth, rectHeight;
};

// Dirty update dispatch: which level, the dirty probe rectangle origin/width,
// and how many direction samples (in row-major probe/direction order) this
// dispatch reprocesses. One thread owns one direction sample.
struct WorldLightFieldProbeUpdate {
    uint level;
    uint probeX0, probeY0;
    uint rectWidth;
    uint sampleCount;
};

// 8-byte radiance sample: RGB (half) + scalar transmittance (half).
struct WorldLightRadiance {
    half radianceR, radianceG, radianceB;
    half transmittance;
};

ConstantBuffer<WorldLightFieldConstants> g_world_light_field;
ConstantBuffer<WorldLightFieldDispatch> g_world_light_dispatch;
ConstantBuffer<WorldLightFieldReset> g_world_light_reset;
ConstantBuffer<WorldLightFieldProbeUpdate> g_world_light_probe_update;

StructuredBuffer<WorldLightMaterial> g_world_light_materials;

RWStructuredBuffer<WorldLightRadiance> g_world_light_radiance_prev;
RWStructuredBuffer<WorldLightRadiance> g_world_light_radiance_cur;
RWStructuredBuffer<uint> g_world_light_state; // u32 per direction sample (count)

#endif
