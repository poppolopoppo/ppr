// ------------------------------------------------------------------
// retained world-lighting inspection overlay
// ------------------------------------------------------------------
// One fullscreen diagnostic draw in the C stage, after post-processing and
// before native UI. Every view decodes already-published data with the captured
// camera; nothing here writes world, CH, cascade, source or history state.
//
// Radiance views use the fixed inspection mapping max(RGB,0)/(1+max(RGB,0)).
// It is an inspection display curve, never a scene exposure decision.
#include "world_lighting_codec.slang"

// Mirrored by WorldLightingPass's InspectionStageFrame.
struct InspectFrame {
    float4x4 m_inverse_view_projection;
    float4 m_viewport; // width, height, 1/width, 1/height
    uint m_diagnostic;
    uint m_cascade_level;
    uint m_encode_srgb;
    float m_cells_per_metre;
};
ConstantBuffer<InspectFrame> g_inspect;

// The CH helper expects the fine-field binding to exist before it is included.
StructuredBuffer<PackedCH> g_fine_ch;
#include "world_lighting_ch.slang"
#include "world_lighting_schedule.slang"

StructuredBuffer<uint> g_resolved_cells;
StructuredBuffer<TerrainVisual> g_visuals;
StructuredBuffer<uint> g_cell_source;
StructuredBuffer<uint2> g_cascades;
[format("r8ui")] Texture2D<uint> g_jump;

static const uint kInspectEdge = 1024u;
static const uint kDiagnosticProbes = 1u;
static const uint kDiagnosticSource = 2u;
static const uint kDiagnosticOccupancy = 3u;
static const uint kDiagnosticJump = 4u;
static const uint kDiagnosticCascade = 5u;

struct InspectVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
InspectVertexOutput vertexInspect(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    InspectVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

// ------------------------------------------------------------------
// display mapping
// ------------------------------------------------------------------

float3 inspectionCurve(float3 radiance) {
    float3 positive = max(radiance, float3(0.0));
    float peak = max(positive.x, max(positive.y, positive.z));
    return positive / (1.0 + peak);
}

float3 inspectionSrgb(float3 display_linear) {
    return select(display_linear <= 0.0031308, display_linear * 12.92,
        1.055 * pow(max(display_linear, float3(0.0)), float3(1.0 / 2.4)) - 0.055);
}

// Distinct jump classes: 0 occupied/none, then 1, 2, 4, 8, 16, 32.
float3 jumpColor(uint jump) {
    if (jump == 0u) return float3(0.0, 0.0, 0.0);
    if (jump == 1u) return float3(0.80, 0.08, 0.08);
    if (jump == 2u) return float3(0.95, 0.45, 0.05);
    if (jump == 4u) return float3(0.92, 0.92, 0.08);
    if (jump == 8u) return float3(0.10, 0.80, 0.20);
    if (jump == 16u) return float3(0.10, 0.35, 0.95);
    if (jump == 32u) return float3(0.85, 0.15, 0.85);
    return float3(1.0, 0.0, 1.0); // Unreachable stored class: loudly invalid.
}

// ------------------------------------------------------------------
// world reconstruction
// ------------------------------------------------------------------

// The world plane is z = 0. Each window pixel unprojects through the captured
// camera and intersects that plane, so cell selection matches the presented
// framing rather than the UI coordinate scale.
float3 worldOnPlane(float2 uv) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 near_h = mul(float4(ndc, 0.0, 1.0), g_inspect.m_inverse_view_projection);
    float4 far_h = mul(float4(ndc, 1.0, 1.0), g_inspect.m_inverse_view_projection);
    float3 near_point = near_h.xyz / near_h.w;
    float3 far_point = far_h.xyz / far_h.w;
    float3 direction = far_point - near_point;
    if (abs(direction.z) > 1.0e-12) {
        return near_point + direction * (-near_point.z / direction.z);
    }
    return near_point;
}

bool cellInside(int2 cell) {
    return cell.x >= 0 && cell.y >= 0 && cell.x < int(kInspectEdge) && cell.y < int(kInspectEdge);
}

uint cellIndex(int2 cell) {
    return uint(cell.y) * kInspectEdge + uint(cell.x);
}

// ------------------------------------------------------------------
// cascade interval atlas
// ------------------------------------------------------------------
// Two equal panels show the selected level's 1024-square atlas: left is decoded
// radiance through the inspection curve, right is decoded transmittance. Atlas
// (x,y) is the per-probe direction sector address used by cascadeRecordIndex:
// probe xy is atlas/spacing and phi is (y%spacing)*spacing + x%spacing. The
// block lattice is that same probe/direction partition.
float3 cascadePanel(float2 pixel, float2 panel_origin, float2 panel_size, bool transmittance) {
    float2 local = (pixel - panel_origin) / panel_size;
    if (any(local < 0.0) || any(local >= 1.0)) {
        return float3(0.02, 0.02, 0.025);
    }

    uint level = clamp(g_inspect.m_cascade_level, 1u, kCascadeLevels);
    uint spacing = cascadeSpacing(level);
    uint2 atlas = uint2(clamp(local, float2(0.0), float2(0.9995)) * float(kCascadeEdge));
    uint index = (level - 1u) * kCascadeLevelRecords + atlas.y * kCascadeEdge + atlas.x;

    PackedInterval packed;
    packed.m_rg = g_cascades[index].x;
    packed.m_bt = g_cascades[index].y;
    Interval interval = unpackInterval(packed);

    float3 color = transmittance
        ? float3(interval.m_transmittance, interval.m_transmittance, interval.m_transmittance)
        : inspectionCurve(interval.m_radiance);

    // Probe/direction block lattice, sized in panel pixels.
    float2 in_block = frac(local * float(spacing));
    float2 distance = min(in_block, 1.0 - in_block);
    float2 footprint = fwidth(local * float(spacing));
    float edge = min(distance.x / max(footprint.x, 1.0e-6), distance.y / max(footprint.y, 1.0e-6));
    if (edge < 1.0) {
        color = lerp(color, float3(0.06), 0.6 * (1.0 - edge));
    }

    // Panel border.
    float border = min(min(local.x, 1.0 - local.x), min(local.y, 1.0 - local.y));
    float border_width = max(max(fwidth(local.x), fwidth(local.y)), 1.0e-6);
    if (border < border_width) {
        color = float3(0.85);
    }

    return color;
}

// ------------------------------------------------------------------
// diagnostic fragment
// ------------------------------------------------------------------

[shader("fragment")]
float4 fragmentInspect(InspectVertexOutput input) : SV_Target {
    float2 uv = saturate(input.uv);
    // World metres -> native gameplay cells, converted once for every view that
    // indexes published records. The cascade atlas is screen-addressed.
    float2 native = worldOnPlane(uv).xy * g_inspect.m_cells_per_metre;
    int2 cell = int2(floor(native));
    float3 color = float3(0.0);

    if (g_inspect.m_diagnostic == kDiagnosticCascade) {
        float2 size = float2(g_inspect.m_viewport.x, g_inspect.m_viewport.y);
        float2 pixel = uv * size;
        float side = min(size.x * 0.5, size.y);
        float2 panel = float2(side, side);
        float2 left_origin = float2((size.x * 0.5 - side) * 0.5, (size.y - side) * 0.5);
        float2 right_origin = float2(size.x * 0.5 + (size.x * 0.5 - side) * 0.5, (size.y - side) * 0.5);
        color = pixel.x < size.x * 0.5
                    ? cascadePanel(pixel, left_origin, panel, false)
                    : cascadePanel(pixel, right_origin, panel, true);
    } else if (cellInside(cell)) {
        uint index = cellIndex(cell);
        uint resolved = g_resolved_cells[index];

        if (g_inspect.m_diagnostic == kDiagnosticProbes) {
            color = inspectionCurve(angularMean(native));

            // Fine probes sit every two native cells at native coordinate 2p+1.
            // With fine = native*0.5, frac(fine) - 0.5 is zero at those probe
            // centres, so these lines mark the fine-probe sampling points
            // themselves; this is an inspection aid, not a scene feature. The
            // lattice keeps its true two-cell period and only fades: full
            // strength at >=6 window pixels of spacing, gone at <=3, so an
            // overview shows the underlying field instead of a dense pattern.
            float2 fine = native * 0.5;
            float2 distance = abs(frac(fine) - 0.5);
            float2 footprint = max(fwidth(fine), float2(1.0e-6));
            float spacing_px = 1.0 / max(footprint.x, footprint.y);
            float fade = smoothstep(3.0, 6.0, spacing_px);
            float edge = min(distance.x / footprint.x, distance.y / footprint.y);
            if (edge < 0.5) {
                color = lerp(color, float3(0.03), 0.55 * (1.0 - edge * 2.0) * fade);
            }
        } else if (g_inspect.m_diagnostic == kDiagnosticSource) {
            color = inspectionCurve(unpackRGB9E5(g_cell_source[index]));
        } else if (g_inspect.m_diagnostic == kDiagnosticOccupancy) {
            uint material = unpackCellMaterial(resolved);
            if (material != 0u) {
                TerrainVisual visual = g_visuals[material];
                bool receive_only =
                    (visual.surfaceFlags & kOpticalBlendSurfaceFlag) != 0u && visual.emissiveStrength == 0.0;
                color = receive_only ? float3(0.0, 1.0, 1.0) : float3(1.0, 1.0, 1.0);
            }
        } else if (g_inspect.m_diagnostic == kDiagnosticJump) {
            color = jumpColor(g_jump.Load(int3(cell, 0)));
        }
    }

    if (g_inspect.m_encode_srgb != 0u) {
        color = inspectionSrgb(color);
    }

    return float4(color, 1.0);
}
