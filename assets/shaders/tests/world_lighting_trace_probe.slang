// Test-only segment probe for the shared world-lighting tracer. Each thread
// traces one parameterized finite segment through the real traceSegment helper
// and publishes its hit cell plus decoded (RGB radiance, transmittance).
#include "../world_lighting_trace.slang"

struct TraceProbeFrame {
    uint m_count;
    uint m_reserved[3];
};

ConstantBuffer<TraceProbeFrame> g_trace_probe;
StructuredBuffer<float4> g_trace_segments;
RWStructuredBuffer<uint> g_trace_out;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeSegments(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_trace_probe.m_count) return;

    float4 segment = g_trace_segments[id.x];
    SegmentHit result = traceSegmentDetailed(segment.xy, segment.zw, id.x, 0u);

    uint base = id.x * 5u;
    g_trace_out[base + 0u] = result.m_cell;
    g_trace_out[base + 1u] = asuint(result.m_interval.m_radiance.x);
    g_trace_out[base + 2u] = asuint(result.m_interval.m_radiance.y);
    g_trace_out[base + 3u] = asuint(result.m_interval.m_radiance.z);
    g_trace_out[base + 4u] = asuint(result.m_interval.m_transmittance);
}
