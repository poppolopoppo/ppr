// Same 304-byte per-slot frame mirror as GridPass::GridFrame. Row vectors,
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
    uint m_padding;
    BackgroundLayer m_layers[4];
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

[shader("fragment")]
float4 fragmentBackgroundMain(BackgroundVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 a = mul(float4(ndc, 0.0, 1.0), g_grid_frame.m_inverse_view_projection);
    float4 b = mul(float4(ndc, 1.0, 1.0), g_grid_frame.m_inverse_view_projection);
    float3 near_point = a.xyz / a.w;
    float3 far_point = b.xyz / b.w;
    float3 direction = far_point - near_point;
    float t = abs(direction.z) > 1.0e-8 ? -near_point.z / direction.z : 0.0;
    float2 world = (near_point + direction * t).xy;
    float2 dx = ddx(world), dy = ddy(world);
    float3 result = float3(0.0);
    [unroll]
    for (uint index = 0u; index < 4u; ++index) {
        BackgroundLayer layer = g_grid_frame.m_layers[index];
        float2 uv = (world - g_grid_frame.m_camera_origin * (1.0 - layer.m_parallax_factor)) / layer.m_repeat_period_m;
        float4 texel = nonuniform(g_textures[layer.m_texture_index]).SampleGrad(
            g_sampler, uv, dx / layer.m_repeat_period_m, dy / layer.m_repeat_period_m);
        float4 tint = linearTint(layer.m_color_rgba);
        float alpha = index == 0u ? 1.0 : saturate(texel.a * tint.a);
        result = lerp(result, texel.rgb * tint.rgb, alpha);
    }
    return float4(result, 1.0);
}

[shader("fragment")]
float4 fragmentBackgroundCopy(BackgroundVertexOutput input,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    float2 extent = max(g_grid_frame.m_viewport.zw, float2(1.0));
    float2 uv = clamp(input.uv, 0.5 / extent, 1.0 - 0.5 / extent);
    return float4(g_backdrop.SampleLevel(g_sampler, uv, 0.0).rgb, 1.0);
}
