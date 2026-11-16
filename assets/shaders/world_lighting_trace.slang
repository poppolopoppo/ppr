// ------------------------------------------------------------------
// common slab-clipped segment tracer
// ------------------------------------------------------------------
// One helper traces every near, coarse and fine finite segment. It reads the
// resolved categorical classification, the R8 jump table and the cached
// per-cell source; it never consults camera state or a retained history field.
// The caller owns the fine connector's explicit 0.001-cell origin offset.
#include "world_lighting_codec.slang"
#include "numeric_status.slang"

static const uint kTraceEdge = 1024u;
static const float kTraceFar = 3.402823466e+38;

StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<TerrainVisual> g_visuals;
[format("r8ui")] Texture2D<uint> g_jump;
// One RGB9E5 outgoing-radiance word per native cell. Source preparation owns
// the producer; tracing only decodes the cache.
StructuredBuffer<uint> g_cell_source;

// One traced segment's decoded interval and the hit native cell (0xFFFFFFFF on
// a miss or numeric failure). Production tracing uses
// traceSegmentDetailed(start, end, owning_invocation, status_record) so a
// traversal failure reports the coarse/fine invocation that owns it.
struct SegmentHit {
    Interval m_interval;
    uint m_cell;
};

// ------------------------------------------------------------------
// containment and boundary helpers
// ------------------------------------------------------------------

// A numeric failure is not a clear interval: the status record reports the
// owning invocation and the returned value is the finite zero result.
SegmentHit traceFailure(uint invocation, uint status_record) {
    numericRecordFailure(status_record, kNumericInvalidState, invocation);

    SegmentHit failure;
    failure.m_interval.m_radiance = float3(0.0, 0.0, 0.0);
    failure.m_interval.m_transmittance = 0.0;
    failure.m_cell = 0xffffffffu;
    return failure;
}

SegmentHit traceClear() {
    SegmentHit clear;
    clear.m_interval.m_radiance = float3(0.0, 0.0, 0.0);
    clear.m_interval.m_transmittance = 1.0;
    clear.m_cell = 0xffffffffu;
    return clear;
}

// An exact grid boundary selects the cell entered by the ray direction; a
// parallel component keeps the upper (floor) cell.
int2 traceEnteredCell(float2 position, float2 direction) {
    int2 cell = int2(floor(position));
    if (position.x == float(cell.x) && direction.x < 0.0) --cell.x;
    if (position.y == float(cell.y) && direction.y < 0.0) --cell.y;
    return cell;
}

// The cell index along one axis at the point just after parameter `next`. Face
// parameters are rebuilt from the original ray, and a crossing that rounds onto
// a cell or aligned-block boundary advances with the ray direction. The selected
// cell therefore always has its next forward face strictly ahead, which keeps a
// corner crossing from reporting a spurious no-progress failure. The face
// parameter grows by exactly one cell per 1/|delta|, so the crossed-cell count
// is estimated directly rather than stepping a near-parallel axis.
int traceAdvanceAxis(float start, float delta, int cell, float next) {
    if (delta == 0.0) return cell;

    int step = delta > 0.0 ? 1 : -1;
    float face = delta > 0.0 ? (float(cell + 1) - start) / delta : (float(cell) - start) / delta;
    if (face > next) return cell;

    float advance = (next - face) * abs(delta);
    int crossed = int(floor(advance)) + 1;
    if (crossed < 1) crossed = 1;

    cell += step * crossed;
    face = delta > 0.0 ? (float(cell + 1) - start) / delta : (float(cell) - start) / delta;
    while (face <= next) {
        cell += step;
        face = delta > 0.0 ? (float(cell + 1) - start) / delta : (float(cell) - start) / delta;
    }

    return cell;
}

// ------------------------------------------------------------------
// probe self-occlusion
// ------------------------------------------------------------------

// True when the native cell belongs to the probe's own/neighbour 2x2 block.
// The fine near segment starts on the probe corner shared by those four cells,
// so an occupied block cell is the probe's own surface. It is an opaque black
// self-occluder: the near trace terminates on it with zero radiance and zero
// transmittance, which also suppresses the far interval. Vacuum cells in the
// block never match the occupancy test and stay transparent, and the rule is
// direction-symmetric with no "starting cell" and no DDA tie-breaking. The
// generic and coarse callers pass the disabled (-1, -1) bounds, which address
// no cell because a traced cell is always inside [0, kTraceEdge).
bool traceInOwnBlock(int2 cell, int2 own_block_min, int2 own_block_max) {
    return cell.x >= own_block_min.x && cell.x <= own_block_max.x && cell.y >= own_block_min.y &&
           cell.y <= own_block_max.y;
}

// ------------------------------------------------------------------
// finite segment traversal
// ------------------------------------------------------------------
SegmentHit traceSegmentDetailed(
    float2 start, float2 end, uint invocation, uint status_record, int2 own_block_min, int2 own_block_max) {
    if (!all(isfinite(start)) || !all(isfinite(end))) return traceFailure(invocation, status_record);

    float2 delta = end - start;
    if (!all(isfinite(delta))) return traceFailure(invocation, status_record);

    // A zero-length segment is a point, not a positive-length intersection.
    if (delta.x == 0.0 && delta.y == 0.0) return traceClear();

    // Slab-clip to [0, 1024) in both axes. A zero direction component is a
    // parallel slab: no epsilon perturbation and no 0*infinity.
    //
    // A segment whose two endpoints are both inside the domain is already
    // wholly inside it. Every axis then has min(lower, upper) < 0 and
    // max(lower, upper) > 1, so the exact clip leaves the (0, 1) defaults
    // untouched and its per-axis divisions and boundary tests are skipped. The
    // result is bit-identical; only the discarded work is removed.
    bool interior = all(start >= float2(0.0)) && all(start < float2(float(kTraceEdge))) &&
                    all(end >= float2(0.0)) && all(end < float2(float(kTraceEdge)));
    float enter = 0.0;
    float exit = 1.0;
    if (!interior) {
        if (delta.x == 0.0) {
            if (start.x < 0.0 || start.x >= float(kTraceEdge)) return traceClear();
        } else {
            float lower = (0.0 - start.x) / delta.x;
            float upper = (float(kTraceEdge) - start.x) / delta.x;
            enter = max(enter, min(lower, upper));
            exit = min(exit, max(lower, upper));
        }

        if (delta.y == 0.0) {
            if (start.y < 0.0 || start.y >= float(kTraceEdge)) return traceClear();
        } else {
            float lower = (0.0 - start.y) / delta.y;
            float upper = (float(kTraceEdge) - start.y) / delta.y;
            enter = max(enter, min(lower, upper));
            exit = min(exit, max(lower, upper));
        }
    }

    if (!(exit > enter)) return traceClear();

    float2 clipped = start + enter * delta;
    int2 cell = traceEnteredCell(clipped, delta);
    if (cell.x < 0 || cell.y < 0 || cell.x >= int(kTraceEdge) || cell.y >= int(kTraceEdge)) return traceClear();

    float previous = enter;
    while (true) {
        uint cell_index = uint(cell.y) * kTraceEdge + uint(cell.x);
        uint resolved = g_resolved_cells[cell_index];
        uint material = resolved & 0xffffu;
        TerrainVisual visual = g_visuals[material];
        bool occupied = resolvedCellOccupied(resolved, visual);

        uint jump = g_jump[cell];
        if (jump == 0u || occupied) {
            if (occupied && traceInOwnBlock(cell, own_block_min, own_block_max)) {
                // The probe's own surface is an opaque self-occluder. The black
                // terminal interval closes both the current cell's outgoing
                // radiance and, through zero transmittance, the far interval,
                // so no light can cross the wall it belongs to.
                SegmentHit occluder;
                occluder.m_interval.m_radiance = float3(0.0, 0.0, 0.0);
                occluder.m_interval.m_transmittance = 0.0;
                occluder.m_cell = cell_index;
                return occluder;
            }

            // Decoded current source with zero transmittance, independent of
            // source brightness (a black source still stops the path).
            SegmentHit hit;
            hit.m_interval.m_radiance = unpackRGB9E5(g_cell_source[cell_index]);
            hit.m_interval.m_transmittance = 0.0;
            hit.m_cell = cell_index;
            return hit;
        }

        // Next face parameters are rebuilt from the original ray. A positive
        // jump advances only to the next exiting face of its aligned block,
        // never by the jump value as unconstrained travel.
        float next_x = kTraceFar;
        float next_y = kTraceFar;
        if (jump <= 1u) {
            if (delta.x > 0.0) next_x = (float(cell.x + 1) - start.x) / delta.x;
            else if (delta.x < 0.0) next_x = (float(cell.x) - start.x) / delta.x;
            if (delta.y > 0.0) next_y = (float(cell.y + 1) - start.y) / delta.y;
            else if (delta.y < 0.0) next_y = (float(cell.y) - start.y) / delta.y;
        } else {
            int size = int(jump);
            int2 base = (cell / size) * size;
            if (delta.x > 0.0) next_x = (float(base.x + size) - start.x) / delta.x;
            else if (delta.x < 0.0) next_x = (float(base.x) - start.x) / delta.x;
            if (delta.y > 0.0) next_y = (float(base.y + size) - start.y) / delta.y;
            else if (delta.y < 0.0) next_y = (float(base.y) - start.y) / delta.y;
        }

        // A nonfinite face parameter is a numeric failure, not the finite
        // "no boundary on this axis" sentinel. Checking before min() keeps a
        // pathological overflowing division from degrading into a clear
        // interval through the sentinel value.
        if (!isfinite(next_x) || !isfinite(next_y)) return traceFailure(invocation, status_record);

        float next = min(next_x, next_y);
        int2 entered = cell;
        if (jump <= 1u) {
            entered.x = traceAdvanceAxis(start.x, delta.x, cell.x, next);
            entered.y = traceAdvanceAxis(start.y, delta.y, cell.y, next);
        } else {
            // A jump crossing an aligned block face advances the crossed axis
            // exactly; the other coordinate is the cell containing the ray just
            // after the crossing, which may differ from the cell the ray
            // occupied when the jump began.
            int size = int(jump);
            int2 base = (cell / size) * size;
            if (next_x < next_y) {
                entered.x = delta.x > 0.0 ? base.x + size : base.x - 1;
                entered.y = traceAdvanceAxis(start.y, delta.y, cell.y, next);
            } else if (next_y < next_x) {
                entered.y = delta.y > 0.0 ? base.y + size : base.y - 1;
                entered.x = traceAdvanceAxis(start.x, delta.x, cell.x, next);
            } else {
                entered.x = delta.x > 0.0 ? base.x + size : base.x - 1;
                entered.y = delta.y > 0.0 ? base.y + size : base.y - 1;
            }
        }

        // Nonfinite arithmetic or failure to make geometric progress is a
        // numeric failure, never a clear interval or a step-budget miss. A
        // crossing at the clipped end (including t=1) belongs to the following
        // segment and is excluded.
        if (!isfinite(next) || next <= previous) return traceFailure(invocation, status_record);
        if (next >= exit) return traceClear();

        if (entered.x < 0 || entered.y < 0 || entered.x >= int(kTraceEdge) || entered.y >= int(kTraceEdge)) {
            return traceClear();
        }

        previous = next;
        cell = entered;
    }
}

// The ordinary segment: generic, coarse and diagnostic callers keep the plain
// first-hit rule with no own-block self-occlusion.
SegmentHit traceSegmentDetailed(float2 start, float2 end, uint invocation, uint status_record) {
    return traceSegmentDetailed(start, end, invocation, status_record, int2(-1, -1), int2(-1, -1));
}

// The shared helper: the caller that owns numeric diagnostics uses the
// detailed form with its own invocation index and status record.
Interval traceSegment(float2 start, float2 end) {
    return traceSegmentDetailed(start, end, 0u, 0u).m_interval;
}
