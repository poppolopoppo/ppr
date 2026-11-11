// ------------------------------------------------------------------
// test-only quarter-probe schedule probe
// ------------------------------------------------------------------
// Reads the real shader-side schedule from world_lighting_cascade.slang for a
// batch of (group, level, local probe) queries and reports the selected probe,
// derived age, actual trace frame, validity, phase fraction and direction.
// Only diagnostics consume this program; no runtime pass loads it.
#include "../world_lighting_cascade.slang"

struct ScheduleProbeFrame {
    uint m_count;
    uint m_reserved[3];
};

struct ScheduleQuery {
    uint m_group_x;
    uint m_group_y;
    uint m_level;
    uint m_local;
    uint m_phi;
    uint m_reserved[3];
};

ConstantBuffer<ScheduleProbeFrame> g_schedule_probe;
StructuredBuffer<ScheduleQuery> g_schedule_queries;
RWStructuredBuffer<uint> g_schedule_results;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeSchedule(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_schedule_probe.m_count) return;

    ScheduleQuery query = g_schedule_queries[id.x];
    uint2 group = uint2(query.m_group_x, query.m_group_y);
    uint phase = cascadeGroupPhase(group, query.m_level);
    uint selected = cascadeSelectedProbe(g_cascade.m_frame_low, phase);
    CascadeProbeFrame frame =
        cascadeProbeFrame(g_cascade.m_frame_low, g_cascade.m_frame_high, phase, query.m_local);

    uint base = id.x * 8u;
    g_schedule_results[base + 0u] = phase;
    g_schedule_results[base + 1u] = selected;
    g_schedule_results[base + 2u] = frame.m_age;
    g_schedule_results[base + 3u] = frame.m_trace_low;
    g_schedule_results[base + 4u] = frame.m_trace_high;
    g_schedule_results[base + 5u] = frame.m_valid ? 1u : 0u;
    g_schedule_results[base + 6u] = asuint(cascadePhaseFraction(frame.m_trace_low));
    g_schedule_results[base + 7u] = asuint(
        kCascadeTwoPi * (float(query.m_phi) + cascadePhaseFraction(frame.m_trace_low)) /
        float(cascadeDirections(query.m_level)));
}
