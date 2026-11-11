// ------------------------------------------------------------------
// full-domain visibility masks and jump table
// ------------------------------------------------------------------
// Two dispatches rebuild the reference's tiled OR reduction. buildVisibility
// owns one independent 32x32 native-cell tile per group and writes all three
// levels from 160 bytes of group-shared storage; buildJump selects the largest
// aligned empty square per native cell with at most three uint2 loads. The
// resolved-cell buffer and the authored palette classification are the same
// inputs source preparation consumes.
#include "world_lighting_codec.slang"

static const uint kVisibilityEdge = 1024u;
static const uint kLevel0Offset = 0u;
static const uint kLevel1Offset = 16384u;
static const uint kLevel2Offset = 20480u;
static const uint kLevel0Pitch = 128u;
static const uint kLevel1Pitch = 64u;
static const uint kLevel2Pitch = 32u;

StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<TerrainVisual> g_visuals;
RWStructuredBuffer<uint2> g_visibility;

// Jump storage stays an R8_UINT texel bound as uint: no filtering, no mips and
// no normalized conversion.
[format("r8ui")] RWTexture2D<uint> g_jump;

// 160 bytes per group: two arrays of 16 atomic level-0 words (128 bytes) and
// four level-1 uint2 masks (32 bytes).
groupshared uint sLevel0Low[16];
groupshared uint sLevel0High[16];
groupshared uint2 sLevel1[4];

// ------------------------------------------------------------------
// fixed 2x2 bit reduction
// ------------------------------------------------------------------

// One 8x8 mask to its packed 4x4 parent: horizontal OR by one, vertical OR by
// eight, select even rows, then compact each surviving four-bit row. The four
// 4x4 results are placed into an 8x8 parent at bit x + 8*y.
uint compactQuadrant(uint2 mask) {
    uint horizontalLow = (mask.x | (mask.x >> 1u)) & 0x55555555u;
    uint horizontalHigh = (mask.y | (mask.y >> 1u)) & 0x55555555u;

    uint verticalLow = horizontalLow | (horizontalLow >> 8u);
    uint verticalHigh = horizontalHigh | (horizontalHigh >> 8u);

    uint evenLow = verticalLow & 0x00ff00ffu;
    uint evenHigh = verticalHigh & 0x00ff00ffu;

    uint compactLow = (evenLow | (evenLow >> 1u)) & 0x33333333u;
    compactLow = (compactLow | (compactLow >> 2u)) & 0x0f0f0f0fu;
    uint compactHigh = (evenHigh | (evenHigh >> 1u)) & 0x33333333u;
    compactHigh = (compactHigh | (compactHigh >> 2u)) & 0x0f0f0f0fu;

    return (compactLow & 0x0000000fu) | ((compactLow >> 12u) & 0x000000f0u) |
           ((compactHigh & 0x0000000fu) << 8u) | (((compactHigh >> 12u) & 0x000000f0u) << 8u);
}

uint2 reduceQuadrants(uint2 q00, uint2 q10, uint2 q01, uint2 q11) {
    uint a00 = compactQuadrant(q00);
    uint a10 = compactQuadrant(q10);
    uint a01 = compactQuadrant(q01);
    uint a11 = compactQuadrant(q11);

    uint2 parent = uint2(0u, 0u);
    for (uint row = 0u; row < 4u; ++row) {
        parent.x |= ((a00 >> (4u * row)) & 0x0fu) << (8u * row);
        parent.x |= ((a10 >> (4u * row)) & 0x0fu) << (8u * row + 4u);
        parent.y |= ((a01 >> (4u * row)) & 0x0fu) << (8u * row);
        parent.y |= ((a11 >> (4u * row)) & 0x0fu) << (8u * row + 4u);
    }

    return parent;
}

bool maskBit(uint2 mask, uint bit) {
    return (((bit < 32u ? mask.x : mask.y) >> (bit & 31u)) & 1u) != 0u;
}

// ------------------------------------------------------------------
// tile-local visibility construction
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(16, 16, 1)]
void buildVisibility(uint2 local : SV_GroupThreadID, uint2 group : SV_GroupID) {
    // First 4x4 threads clear both atomic level-0 arrays.
    if (local.x < 4u && local.y < 4u) {
        uint mask = local.x + 4u * local.y;
        sLevel0Low[mask] = 0u;
        sLevel0High[mask] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Each thread reads four quadrant cells, so the 256 threads cover the
    // 32x32 tile exactly once with x-adjacent accesses.
    uint2 tile = group * 32u;
    uint2 quadrants[4] = {uint2(0u, 0u), uint2(16u, 0u), uint2(0u, 16u), uint2(16u, 16u)};
    for (uint index = 0u; index < 4u; ++index) {
        uint2 q = local + quadrants[index];
        uint2 cell = tile + q;
        uint resolved = g_resolved_cells[cell.y * kVisibilityEdge + cell.x];
        uint material = resolved & 0xffffu;
        TerrainVisual visual = g_visuals[material];
        if (!resolvedCellOccupied(resolved, visual)) continue;

        uint bit = (q.x & 7u) + 8u * (q.y & 7u);
        uint mask = (q.x >> 3u) + 4u * (q.y >> 3u);
        if (bit < 32u) InterlockedOr(sLevel0Low[mask], 1u << bit);
        else InterlockedOr(sLevel0High[mask], 1u << (bit - 32u));
    }
    GroupMemoryBarrierWithGroupSync();

    // Level 0: one record per 8x8 mask.
    if (local.x < 4u && local.y < 4u) {
        uint mask = local.x + 4u * local.y;
        uint2 record = group * 4u + local;
        g_visibility[kLevel0Offset + record.y * kLevel0Pitch + record.x] =
            uint2(sLevel0Low[mask], sLevel0High[mask]);
    }

    // Level 1: each of the first 2x2 threads reduces a disjoint 2x2 block of
    // level-0 masks into a separate 32-byte shared array.
    if (local.x < 2u && local.y < 2u) {
        uint x = 2u * local.x;
        uint y = 2u * local.y;
        uint2 q00 = uint2(sLevel0Low[x + 4u * y], sLevel0High[x + 4u * y]);
        uint2 q10 = uint2(sLevel0Low[x + 1u + 4u * y], sLevel0High[x + 1u + 4u * y]);
        uint2 q01 = uint2(sLevel0Low[x + 4u * (y + 1u)], sLevel0High[x + 4u * (y + 1u)]);
        uint2 q11 = uint2(sLevel0Low[x + 1u + 4u * (y + 1u)], sLevel0High[x + 1u + 4u * (y + 1u)]);

        uint2 parent = reduceQuadrants(q00, q10, q01, q11);
        uint2 record = group * 2u + local;
        g_visibility[kLevel1Offset + record.y * kLevel1Pitch + record.x] = parent;
        sLevel1[local.x + 2u * local.y] = parent;
    }
    GroupMemoryBarrierWithGroupSync();

    // Level 2: thread (0,0) applies the same reduction to the four separate
    // level-1 masks, so no level-0 shared slot is overwritten.
    if (local.x == 0u && local.y == 0u) {
        uint2 parent = reduceQuadrants(sLevel1[0u], sLevel1[1u], sLevel1[2u], sLevel1[3u]);
        g_visibility[kLevel2Offset + group.y * kLevel2Pitch + group.x] = parent;
    }
}

// ------------------------------------------------------------------
// per-cell jump selection
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(16, 16, 1)]
void buildJump(uint2 cell : SV_DispatchThreadID) {
    if (cell.x >= kVisibilityEdge || cell.y >= kVisibilityEdge) return;

    uint resolved = g_resolved_cells[cell.y * kVisibilityEdge + cell.x];
    uint material = resolved & 0xffffu;
    TerrainVisual visual = g_visuals[material];

    uint jump = 0u;
    if (!resolvedCellOccupied(resolved, visual)) {
        // Largest aligned empty square containing this cell, in the reference's
        // priority order. Each promoted mask is a superset of the previous one.
        jump = 1u;

        uint2 level1_cell = cell >> 1u;
        uint2 level1_mask = level1_cell >> 3u;
        uint2 mask1 = g_visibility[kLevel1Offset + level1_mask.y * kLevel1Pitch + level1_mask.x];
        if (!maskBit(mask1, (level1_cell.x & 7u) + 8u * (level1_cell.y & 7u))) jump = 2u;

        uint2 level2_cell = cell >> 2u;
        uint2 level2_mask = level2_cell >> 3u;
        uint2 mask2 = g_visibility[kLevel2Offset + level2_mask.y * kLevel2Pitch + level2_mask.x];
        if (!maskBit(mask2, (level2_cell.x & 7u) + 8u * (level2_cell.y & 7u))) jump = 4u;

        uint2 level0_mask = cell >> 3u;
        uint2 mask0 = g_visibility[kLevel0Offset + level0_mask.y * kLevel0Pitch + level0_mask.x];
        if (mask0.x == 0u && mask0.y == 0u) jump = 8u;
        if (mask1.x == 0u && mask1.y == 0u) jump = 16u;
        if (mask2.x == 0u && mask2.y == 0u) jump = 32u;
    }

    g_jump[cell] = jump;
}
