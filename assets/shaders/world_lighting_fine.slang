// ------------------------------------------------------------------
// fine CH reconstruction and in-place temporal accumulation
// ------------------------------------------------------------------
// 512x512 probes, one 32-byte record each. Every invocation gathers the four
// clamped L1 probes around its fine centre, builds the 64 nominal L1
// connectors, traces only the near connector segment against the current
// resolved occupancy and cached source, merges each connector with the L1
// probe's retained interval at its own last trace direction, and fits one
// signed circular harmonic field.
//
// The update reads only its own prior record and writes all 32 bytes of that
// same record. There is no history copy and no neighbour read: source
// preparation has already consumed the pre-update field, and scene readers run
// after these writes.
#include "world_lighting_trace.slang"
#include "world_lighting_schedule.slang"

// The in-place fine field. It is also the binding the CH include samples in
// angularMean, so it must be declared before that include.
RWStructuredBuffer<PackedCH> g_fine_ch;

#include "world_lighting_ch.slang"

struct FineFrame {
    uint m_frame_low;
    uint m_frame_high;
    uint m_fine_edge;
    uint m_status_record;
    float m_current_weight;
    uint m_first_field;
    uint m_reserved0;
    uint m_reserved1;
};

ConstantBuffer<FineFrame> g_fine;
StructuredBuffer<uint2> g_cascades;

static const float kFineOriginOffset = 0.001;
static const float kFineConnectorHalfExtent = 4.0;

// ------------------------------------------------------------------
// per-probe schedule and connector geometry
// ------------------------------------------------------------------

// L1 probe frame. The probe index is already clamped to the L1 domain by the
// caller before this derives its group, validity or trace frame.
CascadeProbeFrame fineProbeFrame(uint2 probe) {
    uint2 group = probe / 2u;
    uint local = (probe.x % 2u) + 2u * (probe.y % 2u);
    uint phase = cascadeGroupPhase(group, 1u);
    return cascadeProbeFrame(g_fine.m_frame_low, g_fine.m_frame_high, phase, local);
}

PackedCH fineZeroCH() {
    PackedCH result;
    result.m_dc = 0u;
    result.m_scale = 0.0;
    result.m_ac[0] = 0u;
    result.m_ac[1] = 0u;
    result.m_ac[2] = 0u;
    result.m_ac[3] = 0u;
    result.m_ac[4] = 0u;
    result.m_ac[5] = 0u;
    return result;
}

// Blend the nine decoded FP32 coefficients, then encode exactly once. The
// first field bypasses the blend under cold start. Negative AC is preserved;
// coefficients are never clamped and DC is never renormalized. A nonencodable
// result returns zero containment and reports its category to the caller.
PackedCH fineEncodeCH(
    CH reconstructed, PackedCH previous, bool first_field, float current_weight, out uint category) {
    CH blended = reconstructed;
    if (!first_field) {
        CH prior = unpackCH(previous);
        for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
            blended.m_coefficients[coefficient] =
                prior.m_coefficients[coefficient] +
                (reconstructed.m_coefficients[coefficient] - prior.m_coefficients[coefficient]) * current_weight;
        }
    }

    category = chPackCategory(blended);
    if (category != 0u) return fineZeroCH();
    return packCH(blended);
}

// ------------------------------------------------------------------
// one fine probe
// ------------------------------------------------------------------
bool fineReconstructProbe(uint2 probe, out CH result) {
    float2 fine_center = (float2(probe) + 0.5) * 2.0;
    float2 relative = fine_center * 0.25 - 0.5;

    // The clamped L1 index is derived before its centre, group, validity, trace
    // frame or storage address. Repeated edge taps keep their ordinary bilinear
    // weights, whose sum remains one.
    int2 base = int2(floor(relative));
    float2 fraction = relative - float2(base);
    int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
    int2 last = int2(int(cascadeProbeEdge(1u)) - 1);
    uint directions = cascadeDirections(1u);
    float direction_weight = kChTwoPi / float(directions);
    uint invocation = probe.y * g_fine.m_fine_edge + probe.x;

    float2 box_min = fine_center - kFineConnectorHalfExtent;
    float2 box_max = fine_center + kFineConnectorHalfExtent;

    // The probe corner is shared by four native cells, which are the probe's
    // own surface. The near trace traverses that 2x2 block instead of
    // terminating on it, so a wall sources rho*incident rather than its own
    // outgoing radiance. The coarse and cascade traces keep the ordinary rule.
    int2 own_block_min = int2(fine_center) - 1;
    int2 own_block_max = int2(fine_center);

    // A block that reaches the world edge has the map exit, not another cell,
    // beyond it. Excluding it would replace the terminal boundary with a
    // missing direction and make a uniform field non-uniform at the domain
    // edge, so only an interior block is excluded and the outermost probe ring
    // keeps the ordinary first-hit rule.
    int trace_edge = int(kTraceEdge);
    if (own_block_min.x < 1 || own_block_min.y < 1 || own_block_max.x >= trace_edge - 1 ||
        own_block_max.y >= trace_edge - 1) {
        own_block_min = int2(-1, -1);
        own_block_max = int2(-1, -1);
    }

    CHAccumulator accumulator = chAccumulator();
    for (uint tap = 0u; tap < 4u; ++tap) {
        int2 offset = offsets[tap];
        float tap_weight =
            (offset.x == 0 ? 1.0 - fraction.x : fraction.x) * (offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        if (tap_weight <= 0.0) continue;

        int2 l1_probe = clamp(base + offset, int2(0), last);
        uint2 l1 = uint2(l1_probe);
        CascadeProbeFrame frame = fineProbeFrame(l1);
        // A never-initialized tap has no stored direction identity: its near
        // connector uses the current frame, and its far interval stays zero.
        uint trace_low = frame.m_valid ? frame.m_trace_low : g_fine.m_frame_low;
        float2 center = (float2(l1) + 0.5) * float(cascadeSpacing(1u));

        for (uint phi = 0u; phi < directions; ++phi) {
            float2 direction = cascadeProbeDirection(phi, directions, trace_low);

            // Exit the connector box [f-4, f+4] from the L1 centre. The basis
            // direction is the actual connector f->e, never the L1 schedule
            // direction, except for an exactly zero connector length.
            float2 slip = float2(kTraceFar, kTraceFar);
            if (direction.x > 0.0) slip.x = (box_max.x - center.x) / direction.x;
            else if (direction.x < 0.0) slip.x = (box_min.x - center.x) / direction.x;
            if (direction.y > 0.0) slip.y = (box_max.y - center.y) / direction.y;
            else if (direction.y < 0.0) slip.y = (box_min.y - center.y) / direction.y;

            float exit_parameter = min(slip.x, slip.y);
            float2 exit_point = center + direction * exit_parameter;
            float2 connector = exit_point - fine_center;
            float connector_length = length(connector);
            float2 basis = connector_length == 0.0 ? direction : connector / connector_length;

            // Near segment against the current resolved occupancy and cached
            // source. The probe's own/neighbour block never terminates it. The
            // far interval is the L1 probe's retained transport for this
            // direction; a never-initialized interval is (0,0) and cannot
            // suppress a current near hit.
            SegmentHit near_hit = traceSegmentDetailed(
                fine_center + basis * kFineOriginOffset, exit_point, invocation, g_fine.m_status_record,
                own_block_min, own_block_max);

            Interval far_interval;
            far_interval.m_radiance = float3(0.0);
            far_interval.m_transmittance = 0.0;
            if (frame.m_valid) {
                uint2 record = g_cascades[cascadeRecordIndex(1u, l1, phi)];
                PackedInterval packed;
                packed.m_rg = record.x;
                packed.m_bt = record.y;
                far_interval = unpackInterval(packed);
            }

            Interval merged = cascadeMerge(near_hit.m_interval, far_interval);
            chAccumulate(
                accumulator, merged.m_radiance, atan2(basis.y, basis.x), tap_weight * direction_weight);
        }
    }

    return chFinish(accumulator, result);
}

// ------------------------------------------------------------------
// full field
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(8, 8, 1)]
void reconstructFine(uint2 probe : SV_DispatchThreadID) {
    if (probe.x >= g_fine.m_fine_edge || probe.y >= g_fine.m_fine_edge) return;

    uint index = probe.y * g_fine.m_fine_edge + probe.x;
    CH reconstructed;
    bool valid = fineReconstructProbe(probe, reconstructed);

    // Own prior record only: no neighbour is read during this dispatch.
    PackedCH previous = g_fine_ch[index];
    if (!valid) {
        numericRecordFailure(g_fine.m_status_record, kNumericInvalidState, index);
        g_fine_ch[index] = fineZeroCH();
        return;
    }

    uint category = 0u;
    PackedCH packed = fineEncodeCH(
        reconstructed, previous, g_fine.m_first_field != 0u, g_fine.m_current_weight, category);
    if (category != 0u) {
        numericRecordFailure(
            g_fine.m_status_record, category == 1u ? kNumericInvalidState : kNumericRangeError, index);
        packed = fineZeroCH();
    }

    // Every invocation writes all 32 bytes of its own record.
    g_fine_ch[index] = packed;
}
