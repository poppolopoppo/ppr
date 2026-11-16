// ------------------------------------------------------------------
// test-only fine CH reconstruction probes
// ------------------------------------------------------------------
// Calls the real fine reconstruction helpers with synthetic samples and
// queries the actual CH include. Only diagnostics consume this program; no
// runtime pass loads it.
#include "../world_lighting_fine.slang"

struct CHProbeFrame {
    uint m_count;
    uint m_reserved[3];
};

struct CHProbeCase {
    uint m_sample_offset;
    uint m_sample_count;
    float m_current_weight;
    uint m_first_field;
    uint m_previous[8];
};

struct CHProbeSample {
    float3 m_radiance;
    float m_theta;
    float2 m_basis;
    float m_weight;
    float m_reserved;
};

ConstantBuffer<CHProbeFrame> g_ch_probe;
StructuredBuffer<CHProbeCase> g_ch_cases;
StructuredBuffer<CHProbeSample> g_ch_samples;
RWStructuredBuffer<uint> g_ch_results;

// Words written per case: the theta reference fit, then the direction fit. Each
// fit reports its packed record (8 words), its encode report, and its finished
// coefficients before packing (27 words). Both fits run in this one invocation
// over the same sample list, so the comparison cannot depend on a different
// sample schedule, weight set or accumulation order.
static const uint kCHProbeFitWords = 8u + 1u + 27u;
static const uint kCHProbeCaseWords = 2u * kCHProbeFitWords;

void writeCHProbeFit(uint base, PackedCH packed, uint report, CH fit) {
    g_ch_results[base + 0u] = packed.m_dc;
    g_ch_results[base + 1u] = asuint(packed.m_scale);
    for (uint word = 0u; word < 6u; ++word) g_ch_results[base + 2u + word] = packed.m_ac[word];
    g_ch_results[base + 8u] = report;
    for (uint coefficient = 0u; coefficient < 9u; ++coefficient) {
        uint offset = base + 9u + coefficient * 3u;
        g_ch_results[offset + 0u] = asuint(fit.m_coefficients[coefficient].x);
        g_ch_results[offset + 1u] = asuint(fit.m_coefficients[coefficient].y);
        g_ch_results[offset + 2u] = asuint(fit.m_coefficients[coefficient].z);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void probeCH(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_ch_probe.m_count) return;

    CHProbeCase probe = g_ch_cases[id.x];
    CHAccumulator theta_accumulator = chAccumulator();
    CHAccumulator direction_accumulator = chAccumulator();
    for (uint index = 0u; index < probe.m_sample_count; ++index) {
        CHProbeSample sample = g_ch_samples[probe.m_sample_offset + index];
        chAccumulate(theta_accumulator, sample.m_radiance, sample.m_theta, sample.m_weight);
        chAccumulateDirection(direction_accumulator, sample.m_radiance, sample.m_basis, sample.m_weight);
    }

    PackedCH previous;
    previous.m_dc = probe.m_previous[0];
    previous.m_scale = asfloat(probe.m_previous[1]);
    for (uint word = 0u; word < 6u; ++word) previous.m_ac[word] = probe.m_previous[2u + word];

    CH theta_fit;
    CH direction_fit;
    bool theta_finished = chFinish(theta_accumulator, theta_fit);
    bool direction_finished = chFinish(direction_accumulator, direction_fit);

    PackedCH theta_packed = fineZeroCH();
    PackedCH direction_packed = fineZeroCH();
    uint theta_report = 0u;
    uint direction_report = 0u;
    if (!theta_finished) {
        theta_report = 1u;
    } else {
        uint category = 0u;
        theta_packed = fineEncodeCH(theta_fit, previous, probe.m_first_field != 0u, probe.m_current_weight, category);
        theta_report = category == 0u ? 0u : (category == 1u ? 2u : 3u);
    }
    if (!direction_finished) {
        direction_report = 1u;
    } else {
        uint category = 0u;
        direction_packed =
            fineEncodeCH(direction_fit, previous, probe.m_first_field != 0u, probe.m_current_weight, category);
        direction_report = category == 0u ? 0u : (category == 1u ? 2u : 3u);
    }

    uint base = id.x * kCHProbeCaseWords;
    writeCHProbeFit(base, theta_packed, theta_report, theta_fit);
    writeCHProbeFit(base + kCHProbeFitWords, direction_packed, direction_report, direction_fit);
}

// ------------------------------------------------------------------
// L1 probe schedule identity
// ------------------------------------------------------------------

struct L1ProbeQuery {
    uint m_probe_x;
    uint m_probe_y;
    uint m_phi;
    uint m_reserved;
};

ConstantBuffer<CHProbeFrame> g_l1_probe;
StructuredBuffer<L1ProbeQuery> g_l1_queries;
RWStructuredBuffer<uint> g_l1_results;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeL1Frames(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_l1_probe.m_count) return;

    L1ProbeQuery query = g_l1_queries[id.x];
    uint2 probe = uint2(query.m_probe_x, query.m_probe_y);
    CascadeProbeFrame frame = fineProbeFrame(probe);
    uint trace_low = frame.m_valid ? frame.m_trace_low : g_fine.m_frame_low;
    float2 direction = cascadeProbeDirection(query.m_phi, cascadeDirections(1u), trace_low);

    uint base = id.x * 7u;
    g_l1_results[base + 0u] = cascadeGroupPhase(probe / 2u, 1u);
    g_l1_results[base + 1u] = frame.m_valid ? 1u : 0u;
    g_l1_results[base + 2u] = frame.m_trace_low;
    g_l1_results[base + 3u] = frame.m_trace_high;
    g_l1_results[base + 4u] = asuint(direction.x);
    g_l1_results[base + 5u] = asuint(direction.y);
    g_l1_results[base + 6u] = frame.m_age;
}

// ------------------------------------------------------------------
// field angular mean
// ------------------------------------------------------------------

ConstantBuffer<CHProbeFrame> g_mean_probe;
StructuredBuffer<float4> g_mean_positions;
RWStructuredBuffer<float4> g_mean_results;

[shader("compute")]
[numthreads(64, 1, 1)]
void probeAngularMean(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_mean_probe.m_count) return;

    float4 position = g_mean_positions[id.x];
    g_mean_results[id.x] = float4(angularMean(position.xy), 0.0);
}
