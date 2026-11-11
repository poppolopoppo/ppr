// ------------------------------------------------------------------
// continuously scheduled coarse radiance cascades L5..L1
// ------------------------------------------------------------------
// One quarter-probe 512x512 dispatch per level updates a single 2x2-group probe
// selected from the fixed four-frame schedule and all of its directions. Level
// l=1..5 has probe spacing 2<<l, 4<<(2*l) directions and a 1024-square logical
// interval atlas; L0 is the fine CH field, so no raw L0 interval store exists.
// L5 integrates four terminal sub-sectors per stored sector to the actual map
// exit and closes the path with the analytic sky; lower levels trace the near
// interval to the parent probe and merge the four retained parent sectors.
#include "world_lighting_schedule.slang"
#include "numeric_status.slang"
#include "world_lighting_sky.slang"
#include "world_lighting_trace.slang"

struct CascadeFrame {
    uint m_level;
    uint m_frame_low;
    uint m_frame_high;
    int m_sky_hour;
    uint m_status_record;
    uint m_reserved[3];
};

ConstantBuffer<CascadeFrame> g_cascade;
RWStructuredBuffer<uint2> g_cascades;

PackedInterval cascadePack(Interval value, uint invocation) {
    uint category = intervalPackCategory(value);
    if (category != 0u) {
        numericRecordFailure(
            g_cascade.m_status_record, category == 1u ? kNumericInvalidState : kNumericRangeError, invocation);
        PackedInterval zero;
        zero.m_rg = 0u;
        zero.m_bt = 0u;
        return zero;
    }

    return packInterval(value);
}

// ------------------------------------------------------------------
// terminal level L5
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(16, 16, 1)]
void updateTerminal(uint2 pixel : SV_DispatchThreadID) {
    uint level = kCascadeLevels;
    uint spacing = cascadeSpacing(level);
    if (pixel.x >= kCascadeDomainEdge || pixel.y >= kCascadeDomainEdge) return;

    uint2 group = pixel / spacing;
    uint phi = (pixel.y % spacing) * spacing + pixel.x % spacing;
    uint invocation = pixel.y * kCascadeDomainEdge + pixel.x;

    uint phase = cascadeGroupPhase(group, level);
    uint selected = cascadeSelectedProbe(g_cascade.m_frame_low, phase);
    CascadeProbeFrame frame =
        cascadeProbeFrame(g_cascade.m_frame_low, g_cascade.m_frame_high, phase, selected);
    if (!frame.m_valid) return;

    uint2 probe = group * 2u + uint2(selected % 2u, selected / 2u);
    float2 center = (float2(probe) + 0.5) * float(spacing);
    float start_distance = cascadeIntervalStart(level);

    float3 radiance = float3(0.0);
    for (uint sub = 0u; sub < 4u; ++sub) {
        uint sector = 4u * phi + sub;
        float2 direction = cascadeProbeDirection(sector, kCascadeTerminalDirections, frame.m_trace_low);
        float2 start = center + direction * start_distance;
        // 2*1024 cells past the start bounds every interior forward exit; the
        // shared tracer slab-clips to the [0,1024) map, so this is the actual
        // forward map exit rather than a premature finite 1048.576-cell end.
        float2 end = start + direction * (2.0 * float(kCascadeEdge));
        Interval interval = traceSegmentDetailed(start, end, invocation, g_cascade.m_status_record).m_interval;

        // A clear segment (T=1) is the terminal sky; a hit or a contained
        // numeric failure returns its cached radiance. Every terminal path has
        // zero remaining transmittance because the sky closes the path.
        radiance += interval.m_transmittance == 1.0
                        ? worldLightingSkySectorMean(sector, kCascadeTerminalDirections, g_cascade.m_sky_hour)
                        : interval.m_radiance;
    }

    Interval result;
    result.m_radiance = radiance * 0.25;
    result.m_transmittance = 0.0;
    PackedInterval packed = cascadePack(result, invocation);
    g_cascades[cascadeRecordIndex(level, probe, phi)] = uint2(packed.m_rg, packed.m_bt);
}

// ------------------------------------------------------------------
// lower levels L4..L1
// ------------------------------------------------------------------
[shader("compute")]
[numthreads(16, 16, 1)]
void updateCascade(uint2 pixel : SV_DispatchThreadID) {
    uint level = g_cascade.m_level;
    if (level < 1u || level >= kCascadeLevels) return;

    uint spacing = cascadeSpacing(level);
    if (pixel.x >= kCascadeDomainEdge || pixel.y >= kCascadeDomainEdge) return;

    uint2 group = pixel / spacing;
    uint phi = (pixel.y % spacing) * spacing + pixel.x % spacing;
    uint invocation = pixel.y * kCascadeDomainEdge + pixel.x;

    uint phase = cascadeGroupPhase(group, level);
    uint selected = cascadeSelectedProbe(g_cascade.m_frame_low, phase);
    CascadeProbeFrame frame =
        cascadeProbeFrame(g_cascade.m_frame_low, g_cascade.m_frame_high, phase, selected);
    if (!frame.m_valid) return;

    uint2 probe = group * 2u + uint2(selected % 2u, selected / 2u);
    float2 center = (float2(probe) + 0.5) * float(spacing);
    float2 direction = cascadeProbeDirection(phi, cascadeDirections(level), frame.m_trace_low);
    float2 start = center + direction * cascadeIntervalStart(level);
    float end_distance = cascadeIntervalStart(level + 1u);

    // Bilinear parent gather. Every index is clamped before its center, group,
    // validity, trace frame or record address is derived; a clamped edge tap
    // keeps its ordinary weight, whose sum stays one.
    uint parent_level = level + 1u;
    uint parent_spacing = cascadeSpacing(parent_level);
    uint parent_edge = cascadeProbeEdge(parent_level);
    float2 relative = center / float(parent_spacing) - 0.5;
    int2 base = int2(floor(relative));
    float2 fraction = relative - float2(base);
    float weights[4] = {
        (1.0 - fraction.x) * (1.0 - fraction.y), fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y, fraction.x * fraction.y};
    int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};

    Interval accumulated;
    accumulated.m_radiance = float3(0.0);
    accumulated.m_transmittance = 0.0;

    for (uint tap = 0u; tap < 4u; ++tap) {
        float weight = weights[tap];
        if (weight <= 0.0) continue;

        int2 parent = clamp(base + offsets[tap], int2(0), int2(int(parent_edge) - 1));
        float2 parent_center = (float2(parent) + 0.5) * float(parent_spacing);
        Interval near_interval = traceSegmentDetailed(
                                     start, parent_center + direction * end_distance, invocation,
                                     g_cascade.m_status_record)
                                     .m_interval;

        uint2 parent_group = uint2(parent) / 2u;
        uint parent_local = (uint(parent.x) % 2u) + 2u * (uint(parent.y) % 2u);
        CascadeProbeFrame parent_frame = cascadeProbeFrame(
            g_cascade.m_frame_low, g_cascade.m_frame_high, cascadeGroupPhase(parent_group, parent_level),
            parent_local);

        // Retained parent sectors are already merged far intervals: no new
        // parent ray is traced and the stored sector identity stays fixed. A
        // never-initialized parent contributes the cold-start zero radiance and
        // zero transmittance, not sky through untraced geometry.
        Interval merged;
        merged.m_radiance = float3(0.0);
        merged.m_transmittance = 0.0;
        for (uint sub = 0u; sub < 4u; ++sub) {
            Interval far_interval;
            far_interval.m_radiance = float3(0.0);
            far_interval.m_transmittance = 0.0;
            if (parent_frame.m_valid) {
                uint2 record = g_cascades[cascadeRecordIndex(parent_level, uint2(parent), 4u * phi + sub)];
                PackedInterval packed;
                packed.m_rg = record.x;
                packed.m_bt = record.y;
                far_interval = unpackInterval(packed);
            }

            Interval merged_sector = cascadeMerge(near_interval, far_interval);
            merged.m_radiance += merged_sector.m_radiance;
            merged.m_transmittance += merged_sector.m_transmittance;
        }

        accumulated.m_radiance += merged.m_radiance * (0.25 * weight);
        accumulated.m_transmittance += merged.m_transmittance * (0.25 * weight);
    }

    PackedInterval packed = cascadePack(accumulated, invocation);
    g_cascades[cascadeRecordIndex(level, probe, phi)] = uint2(packed.m_rg, packed.m_bt);
}
