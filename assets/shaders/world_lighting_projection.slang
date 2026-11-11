// ------------------------------------------------------------------
// deterministic categorical native-cell projection
// ------------------------------------------------------------------
// One resolved word per native cell. Every candidate is reduced through two
// row-major uint32 scratch grids with 32-bit atomic minima only: best_key holds
// the ordered distance/class key, best_row the stable-ID metadata row rank that
// owns the winning key. Dispatch order and GPU wave layout never select the
// winner, and no winning payload races with the index reduction.
#include "grid_particle_native.slang"
#include "numeric_status.slang"
#include "world_lighting_codec.slang"

struct ProjectionFrame {
    float origin;
    float edge;
    uint count;
    uint movingCount;
    uint materialCount;
    uint statusRecord;
    uint reserved0;
    uint reserved1;
};

ConstantBuffer<ProjectionFrame> g_projection;
StructuredBuffer<MetadataRow> g_metadata;
StructuredBuffer<NumericalRow> g_numerical;
StructuredBuffer<TerrainVisual> g_visuals;
StructuredBuffer<uint> g_terrain;
RWStructuredBuffer<uint> g_best_key;
RWStructuredBuffer<uint> g_best_row;
RWStructuredBuffer<uint> g_resolved;

static const uint kProjectionStatusClear = 0u;
static const uint kProjectionStatusKeys = 1u;
static const uint kProjectionStatusRows = 2u;
static const uint kProjectionStatusCells = 3u;

// Candidate state: 0 accepted, 1 rejected (nonfinite, out-of-domain or
// otherwise invalid), 4 inactive (an ordinary skip, not a failure). The
// projection has no representational range: word 1 stays UINT32_MAX here and is
// reserved for later source/transport stages that encode bounded records.
static const uint kProjectionCandidateInactive = 4u;

struct ProjectionCandidate {
    uint cell;
    uint key;
    uint status;
};

// The single native-cell coordinate, distance-key and classification helper.
// Both reductions call it so their comparison operands are bit-identical:
// (native - (cell + 0.5)) squared per axis with the same ordered multiply/add,
// canonicalized zero, and the transparent receive-only class bit above the
// positive binary32 distance bits. The exact closed maximum endpoint belongs to
// the last cell; a genuinely outside or nonfinite position is rejected, never
// clamped into the scene.
ProjectionCandidate projectionCandidate(
    MetadataRow metadata, NumericalRow hot, bool sourceValid, ProjectionFrame frame) {
    ProjectionCandidate result = {0u, 0u, 0u};
    if (!sourceValid) {
        result.status = 1u;
        return result;
    }

    float enabled = hot.carried[13];
    if (!isfinite(enabled)) {
        result.status = 1u;
        return result;
    }

    if (enabled <= 0.0) {
        result.status = kProjectionCandidateInactive;
        return result;
    }

    float2 native = float2(hot.positionX, hot.positionY) - frame.origin;
    if (!all(isfinite(native))) {
        result.status = 1u;
        return result;
    }

    uint material = metadata.materialTemperature & 0xffffu;
    if (material == 0u || material >= frame.materialCount) {
        result.status = 1u;
        return result;
    }

    if (any(native < 0.0) || any(native > frame.edge)) {
        result.status = 1u;
        return result;
    }

    uint edge = uint(frame.edge);
    uint2 cell = min(uint2(floor(min(native, float2(frame.edge)))), uint2(edge - 1u));
    float2 delta = native - (float2(cell) + 0.5);
    float distance2 = delta.x * delta.x + delta.y * delta.y;
    if (distance2 == 0.0) distance2 = 0.0;

    TerrainVisual visual = g_visuals[material];
    bool receiveOnly = (visual.surfaceFlags & 1u) != 0u && visual.emissiveStrength == 0.0;

    result.cell = cell.y * edge + cell.x;
    result.key = (receiveOnly ? 0x80000000u : 0u) | asuint(distance2);
    return result;
}

// ------------------------------------------------------------------
// scratch and status clear
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(256, 1, 1)]
void clearWinners(uint3 id: SV_DispatchThreadID) {
    uint cells = uint(g_projection.edge) * uint(g_projection.edge);
    if (id.x < cells) {
        g_best_key[id.x] = 0xffffffffu;
        g_best_row[id.x] = 0xffffffffu;
    }

    if (id.x < kNumericStatusRecords * kNumericStatusWords) {
        uint word = id.x % kNumericStatusWords;
        g_numeric_status[id.x] = word < 2u ? kNoNumericIndex : 0u;
    }
}

// ------------------------------------------------------------------
// ordered key and row reductions
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(256, 1, 1)]
void reduceKeys(uint3 id: SV_DispatchThreadID) {
    if (id.x >= g_projection.count) return;

    MetadataRow metadata = g_metadata[id.x];
    // Gas is not a categorical candidate: it never nominates a cell.
    if (metadata.sourceRow == 0xffffffffu) return;

    bool sourceValid = metadata.sourceRow < g_projection.movingCount;
    NumericalRow hot = (NumericalRow)0;
    if (sourceValid) hot = g_numerical[metadata.sourceRow];

    ProjectionCandidate candidate = projectionCandidate(metadata, hot, sourceValid, g_projection);
    if (candidate.status == kProjectionCandidateInactive) return;
    if (candidate.status != 0u) {
        // Every rejected candidate (nonfinite, out-of-domain, invalid material
        // or source) is an invalid-state failure. There is no projection range
        // error; the second minimum is reserved for later representational
        // overflow.
        numericRecordFailure(g_projection.statusRecord, kNumericInvalidState, id.x);
        return;
    }

    // Existing terrain material takes precedence in its cell.
    if (g_terrain[candidate.cell] != 0u) return;

    InterlockedMin(g_best_key[candidate.cell], candidate.key);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void reduceRows(uint3 id: SV_DispatchThreadID) {
    if (id.x >= g_projection.count) return;

    MetadataRow metadata = g_metadata[id.x];
    if (metadata.sourceRow == 0xffffffffu) return;

    bool sourceValid = metadata.sourceRow < g_projection.movingCount;
    NumericalRow hot = (NumericalRow)0;
    if (sourceValid) hot = g_numerical[metadata.sourceRow];

    ProjectionCandidate candidate = projectionCandidate(metadata, hot, sourceValid, g_projection);
    if (candidate.status == kProjectionCandidateInactive) return;
    // Rejected candidates were already reported by reduceKeys; they own no key.
    if (candidate.status != 0u) return;
    if (g_terrain[candidate.cell] != 0u) return;

    // Only the completed winning key admits a row, so the metadata-row rank
    // resolves exact binary32 distance ties by stable ID and no payload write
    // races with the key reduction.
    if (candidate.key != g_best_key[candidate.cell]) return;

    InterlockedMin(g_best_row[candidate.cell], id.x);
}

// ------------------------------------------------------------------
// full-domain resolve
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(256, 1, 1)]
void resolveCells(uint3 id: SV_DispatchThreadID) {
    uint cells = uint(g_projection.edge) * uint(g_projection.edge);
    if (id.x >= cells) return;

    // Every cell is overwritten: terrain, the selected particle's packed
    // material/temperature word, or the all-zero empty cell.
    uint resolved = g_terrain[id.x];
    if (resolved == 0u) {
        uint row = g_best_row[id.x];
        if (row != 0xffffffffu) {
            uint word = g_metadata[row].materialTemperature;
            if ((word & 0xffffu) != 0u) resolved = word;
        }
    }

    g_resolved[id.x] = resolved;
}
