// ------------------------------------------------------------------
// one local complete-query retry and bounded generation counters
// ------------------------------------------------------------------

// Four independent scalar words: attempted, qualified, unresolved, bound-error.
// Host checked-u64 bounds cap ALL queries in the generation at UINT32_MAX;
// each word is incremented at most once per query, so no atomic can wrap.
RWStructuredBuffer<uint> g_terrain_precision_counts;

void terrainRequireRte64() {
    spirv_asm {
        OpCapability RoundingModeRTE;
        OpExecutionMode __entryPoint RoundingModeRTE 64;
    };
}

bool terrainRefinementEligible(uint status, uint initialStatus) {
    uint reasons = kTerrainReasonClip | kTerrainReasonOwnership | kTerrainReasonDda |
        kTerrainReasonRoots | kTerrainReasonOrder;
    uint allowed = kTerrainNumericAmbiguity | reasons | kTerrainOrderSiteMask;
    return initialStatus == 0 && (status & 0xffu) == kTerrainNumericAmbiguity &&
        (status & reasons) != 0 && (status & ~allowed) == 0;
}

OpticalResult terrainSettleRefinement(OpticalResult first, OpticalResult refined) {
    uint previous;
    InterlockedAdd(g_terrain_precision_counts[0], 1u, previous);
    if (refined.status == 0) {
        InterlockedAdd(g_terrain_precision_counts[1], 1u, previous);
        return refined;
    }

    InterlockedAdd(g_terrain_precision_counts[2], 1u, previous);
    if ((refined.status & kTerrainReasonArithmetic) != 0)
        InterlockedAdd(g_terrain_precision_counts[3], 1u, previous);

    // An unresolved retry never clears the first failure or replaces its value.
    first.status |= refined.status;
    return first;
}
