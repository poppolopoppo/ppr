// ------------------------------------------------------------------
// shared world-lighting probe schedule and atlas geometry
// ------------------------------------------------------------------
// The fixed four-frame quarter-probe schedule, probe-grid geometry, derived
// trace directions and coarse interval atlas addressing. Coarse transport and
// fine reconstruction include this one definition so a retained probe's
// direction identity cannot drift between the two stages.
#ifndef PPR_WORLD_LIGHTING_SCHEDULE_SLANG
#define PPR_WORLD_LIGHTING_SCHEDULE_SLANG

#include "world_lighting_codec.slang"

static const uint kCascadeLevels = 5u;
static const uint kCascadeEdge = 1024u;
static const uint kCascadeLevelRecords = 1048576u;
static const uint kCascadeDomainEdge = 512u;
static const uint kCascadeTerminalDirections = 16384u;
static const float kCascadeTwoPi = 6.28318530717958647692;

// ------------------------------------------------------------------
// fixed four-frame quarter-probe schedule
// ------------------------------------------------------------------
// The group phase is a pure uint32 hash of the row-major 2x2 group and level;
// the accepted frame index is carried as a checked low/high pair so a probe
// never-visited in the current world is distinguished from a wrapped counter.
uint cascadeMix32(uint value) {
    value = (value ^ (value >> 16u)) * 0x7feb352du;
    value = (value ^ (value >> 15u)) * 0x846ca68bu;
    return value ^ (value >> 16u);
}

uint cascadeSpacing(uint level) {
    return 2u << level;
}

uint cascadeDirections(uint level) {
    return 4u << (2u * level);
}

uint cascadeProbeEdge(uint level) {
    return kCascadeEdge / cascadeSpacing(level);
}

uint cascadeGroupEdge(uint level) {
    return cascadeProbeEdge(level) / 2u;
}

uint cascadeGroupPhase(uint2 group, uint level) {
    return cascadeMix32(group.x + cascadeGroupEdge(level) * group.y + 0x9e3779b9u * level) & 3u;
}

uint cascadeSelectedProbe(uint frame_low, uint phase) {
    return (frame_low + phase) & 3u;
}

struct CascadeProbeFrame {
    uint m_age;
    uint m_trace_low;
    uint m_trace_high;
    bool m_valid;
};

// age = ((n&3) + h + 4 - k) mod 4 with the uint64 subtraction formed as a
// borrow chain: validity is established before the trace frame is used, and no
// unsigned wrap is ever interpreted as an earlier frame.
CascadeProbeFrame cascadeProbeFrame(uint frame_low, uint frame_high, uint phase, uint local) {
    uint age = ((frame_low & 3u) + phase + 4u - local) & 3u;
    uint borrow = frame_low < age ? 1u : 0u;

    CascadeProbeFrame result;
    result.m_age = age;
    result.m_valid = frame_high != 0u || frame_low >= age;
    result.m_trace_low = frame_low - age;
    result.m_trace_high = frame_high - borrow;
    return result;
}

// Golden-ratio fixed-point phase: the 23-bit fraction is exactly representable
// in binary32, so successive frames keep distinct angular samples.
float cascadePhaseFraction(uint trace_low) {
    uint phase_word = trace_low * 0x9e3779b9u;
    return (float(phase_word >> 9u) + 0.5) / 8388608.0;
}

float2 cascadeProbeDirection(uint phi, uint directions, uint trace_low) {
    float theta = kCascadeTwoPi * (float(phi) + cascadePhaseFraction(trace_low)) / float(directions);
    return float2(cos(theta), sin(theta));
}

uint cascadeRecordIndex(uint level, uint2 probe, uint phi) {
    uint spacing = cascadeSpacing(level);
    uint atlas_x = probe.x * spacing + phi % spacing;
    uint atlas_y = probe.y * spacing + phi / spacing;
    return (level - 1u) * kCascadeLevelRecords + atlas_y * kCascadeEdge + atlas_x;
}

float cascadeIntervalStart(uint level) {
    return 0.512 * float(1u << (2u * level - 1u));
}

Interval cascadeMerge(Interval near_interval, Interval far_interval) {
    Interval result;
    result.m_radiance = near_interval.m_radiance + near_interval.m_transmittance * far_interval.m_radiance;
    result.m_transmittance = near_interval.m_transmittance * far_interval.m_transmittance;
    return result;
}

#endif
