// ------------------------------------------------------------------
// cascade world light field: persistent radiance + temporal update ABI
// ------------------------------------------------------------------
// Shared scalar ABI for the temporal world light field. One cascade hierarchy:
// level 0 = 2-cell spacing / 4 directions; level l = 2^(l+1) spacing /
// 4^(l+1) directions. Each level stores radiance for its own interval. A
// radiance sample is 8 bytes: RGB radiance plus a scalar transmittance, stored
// as four 16-bit halves. Per-direction sample counts are one u32 per sample
// (saturating at 255). The two radiance buffers ping-pong (previous/current).
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
    uint pad0, pad1, pad2;
    WorldLightLevelGpu levels[kWorldLightMaxLevels];
};

// Per-dispatch update constants: which level, and the within-level sample
// index this dispatch's thread 0 owns.
struct WorldLightFieldDispatch {
    uint level;
    uint sampleBegin;
    uint pad0, pad1;
};

// Per-dispatch reset constants: which level, the dirty probe-rectangle origin
// and width. One thread resets one probe's directions.
struct WorldLightFieldReset {
    uint level;
    uint probeX0, probeY0;
    uint rectWidth;
};

// 8-byte radiance sample: RGB (half) + scalar transmittance (half).
struct WorldLightRadiance {
    half radianceR, radianceG, radianceB;
    half transmittance;
};

ConstantBuffer<WorldLightFieldConstants> g_world_light_field;
ConstantBuffer<WorldLightFieldDispatch> g_world_light_dispatch;
ConstantBuffer<WorldLightFieldReset> g_world_light_reset;

StructuredBuffer<WorldLightMaterial> g_world_light_materials;

RWStructuredBuffer<WorldLightRadiance> g_world_light_radiance_prev;
RWStructuredBuffer<WorldLightRadiance> g_world_light_radiance_cur;
RWStructuredBuffer<uint> g_world_light_state; // u32 per direction sample (count)

#endif
