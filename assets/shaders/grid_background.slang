#include "grid_background_world.slang"
#include "world_lighting_codec.slang"
#include "grid_background_coverage.slang"

// ------------------------------------------------------------------
// background frame and lighting bindings
// ------------------------------------------------------------------
// The CH helper expects the fine-field binding to exist before it is included.
StructuredBuffer<PackedCH> g_fine_ch;
#include "world_lighting_ch.slang"

// Same 336-byte per-slot frame mirror as GridPass::GridFrame. Row vectors,
// +Y-up world coordinates and [0,1] depth; no backend-specific transform.
struct BackgroundLayer {
    uint m_texture_index;
    uint m_order;
    uint m_color_rgba;
    float m_repeat_period_m;
    float m_parallax_factor;
    uint m_reserved0;
    uint m_reserved1;
    uint m_reserved2;
};
struct GridFrame {
    float4x4 m_view_projection;
    uint m_use_visible_ids;
    float m_cell_size_m;
    uint m_surface_mode;
    uint m_boundary_material_id;
    float4x4 m_inverse_view_projection;
    float4 m_viewport;
    float2 m_camera_origin;
    uint m_has_background;
    // The four bytes after m_has_background are padding: the layer array is
    // 16-byte aligned on both sides of the mirror.
    BackgroundLayer m_layers[4];
    float m_camera_world_x;
    float m_camera_world_y;
    float m_camera_world_z;
    float m_camera_forward_x;
    float m_camera_forward_y;
    float m_camera_forward_z;
    uint m_camera_ortho;
    uint m_has_resolved_cells;
};
ConstantBuffer<GridFrame> g_grid_frame;
Texture2D<float4> g_backdrop;
struct BackgroundVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
BackgroundVertexOutput vertexBackgroundMain(uint vertex : SV_VertexID) {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    BackgroundVertexOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 linearTint(uint rgba) {
    float4 color = float4((rgba >> 24u) & 255u, (rgba >> 16u) & 255u,
        (rgba >> 8u) & 255u, rgba & 255u) / 255.0;
    color.rgb = select(color.rgb <= 0.04045, color.rgb / 12.92,
        pow((color.rgb + 0.055) / 1.055, float3(2.4)));
    return color;
}

// Cubic B-spline reconstruction uses four hardware bilinear taps instead of
// sixteen point taps. The shared wrap sampler keeps the support continuous
// across repeated texture boundaries, including negative world coordinates.
float4 sampleBackground(DescriptorHandle<Texture2D<float4>> texture, SamplerState.Handle sampler,
    float2 uv, float2 dx, float2 dy) {
    uint width, height;
    nonuniform(texture).GetDimensions(width, height);
    float2 extent = float2(width, height);
    float footprint = max(length(dx * extent), length(dy * extent));
    if (footprint >= 1.0) {
        return nonuniform(texture).SampleGrad(sampler, uv, dx, dy);
    }

    float2 pixel = uv * extent - 0.5;
    float2 base = floor(pixel);
    float2 f = pixel - base;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 left_weight = w0 + w1;
    float2 right_weight = w2 + w3;
    float2 left_uv = (base - 0.5 + w1 / left_weight) / extent;
    float2 right_uv = (base + 1.5 + w3 / right_weight) / extent;

    float4 lower = lerp(
        nonuniform(texture).SampleLevel(sampler, left_uv, 0.0),
        nonuniform(texture).SampleLevel(sampler, float2(right_uv.x, left_uv.y), 0.0), right_weight.x);
    float4 upper = lerp(
        nonuniform(texture).SampleLevel(sampler, float2(left_uv.x, right_uv.y), 0.0),
        nonuniform(texture).SampleLevel(sampler, right_uv, 0.0), right_weight.x);
    float4 cubic = lerp(lower, upper, right_weight.y);
    if (footprint <= 0.75) {
        return cubic;
    }

    // Fade back to the existing gradient/mip filter as the texture approaches
    // minification, avoiding a sudden reconstruction change at one texel/pixel.
    float4 filtered = nonuniform(texture).SampleGrad(sampler, uv, dx, dy);
    return lerp(cubic, filtered, smoothstep(0.75, 1.0, footprint));
}

// ------------------------------------------------------------------
// layer receivers
// ------------------------------------------------------------------
// Each layer samples its own CH position: the receiving-plane point shifted
// by at most one native cell along the camera displacement, softened by a
// sixteen-cell length. The shifted position is a CH sample location only; the
// authored texture UV stays unshifted. A shifted receiver outside the fine
// field's [0, 1024*h)^2 domain has zero reflected radiance for that layer.
float3 backgroundLayerRadiance(float3 linear_rgb, float2 world, float2 camera, float cell, float parallax) {
    float2 displacement = (world - camera) / cell;
    float depth_weight = 1.0 - clamp(parallax, 0.0, 1.0);
    float2 offset_cells = depth_weight * displacement / sqrt(256.0 + dot(displacement, displacement));
    float2 shifted_cells = world / cell + offset_cells;
    // A receiver outside the fine field clamps to the field's own edge instead of
    // returning black: above the world that edge is open sky, so the background
    // layer standing in for the sky is lit by the sky integral the cascades
    // already solved rather than by nothing.
    return linear_rgb * angularMean(clamp(shifted_cells, float2(0.0), float2(1023.0)));
}

float4 shadeBackground(BackgroundVertexOutput input, uint lightingSample,
    StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    SamplerState.Handle g_sampler) {
    const bool lit = g_grid_frame.m_has_resolved_cells != 0u && g_grid_frame.m_camera_ortho != 0u;
    float2 world;
    if (lit) {
        // A lit view never substitutes the near-plane fallback: an unvalidated
        // receiving point has no field position to sample and stays black.
        if (!backgroundReceivingPointAt(input.uv, g_grid_frame.m_inverse_view_projection, world)) {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
    } else {
        world = backgroundWorldAt(input.uv, g_grid_frame.m_inverse_view_projection);
    }

    float2 dx = ddx(world), dy = ddy(world);

    // Only native scene samples reject coverage. Capture gradients first so
    // surviving parallax layers retain their original texture footprint.
    // The single-sample optical bake always passes lightingSample == 0.
    if (lightingSample != 0u && backgroundCoveredByOpaqueTerrain(world / g_grid_frame.m_cell_size_m)) discard;

    if (lit) {
        float2 world_cells = world / g_grid_frame.m_cell_size_m;
        if (any(world_cells < 0.0) || any(world_cells >= 1024.0)) return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 camera_world = float2(g_grid_frame.m_camera_world_x, g_grid_frame.m_camera_world_y);
    const float cell = g_grid_frame.m_cell_size_m;
    float3 result = float3(0.0);
    [unroll]
    for (uint index = 0u; index < 4u; ++index) {
        BackgroundLayer layer = g_grid_frame.m_layers[index];
        float2 uv = (world - g_grid_frame.m_camera_origin * (1.0 - layer.m_parallax_factor)) / layer.m_repeat_period_m;
        float4 texel = sampleBackground(g_textures[layer.m_texture_index], g_sampler,
            uv, dx / layer.m_repeat_period_m, dy / layer.m_repeat_period_m);
        float4 tint = linearTint(layer.m_color_rgba);
        float alpha = index == 0u ? 1.0 : saturate(texel.a * tint.a);
        float3 radiance = texel.rgb * tint.rgb;
        if (lit) {
            radiance = backgroundLayerRadiance(radiance, world, camera_world, cell, layer.m_parallax_factor);
        }
        result = lerp(result, radiance, alpha);
    }
    return float4(result, 1.0);
}

[shader("fragment")]
float4 fragmentBackgroundMain(BackgroundVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    return shadeBackground(input, 0u, g_textures, g_sampler);
}

// Direct scene composition at the admitted native sample locations. The cached
// single-sample optical backdrop remains the legacy/refraction path only.
[shader("fragment")]
float4 fragmentBackgroundSpatial(BackgroundVertexOutput input, uint sample_index : SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    input.uv = EvaluateAttributeAtSample(input.uv, sample_index);
    return shadeBackground(input, sample_index + 1u, g_textures, g_sampler);
}

struct BackgroundReactiveOutput {
    float4 color : SV_Target0;
    float reactive : SV_Target1;
};

[shader("fragment")]
BackgroundReactiveOutput fragmentBackgroundReactive(BackgroundVertexOutput input, uint sample_index : SV_SampleIndex,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) {
    input.uv = EvaluateAttributeAtSample(input.uv, sample_index);
    BackgroundReactiveOutput output;
    output.color=shadeBackground(input,sample_index+1u,g_textures,g_sampler);
    output.reactive=0;
    return output;
}

[shader("fragment")]
float4 fragmentBackgroundCopy(BackgroundVertexOutput input,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 extent = max(g_grid_frame.m_viewport.zw, float2(1.0));
    float2 uv = clamp(input.uv, 0.5 / extent, 1.0 - 0.5 / extent);
    return g_backdrop.SampleLevel(g_sampler, uv, 0.0);
}
