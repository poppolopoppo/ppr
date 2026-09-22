// Bindless static-mesh pass (docs/plans/asset-pipeline.md §6): PURE
// StructuredBuffer fetch. The CPU StaticMeshVertex stride is 64 B
// (plain-float arrays, 12+12+8+16+16); BagVertex mirrors it exactly.
// Texture descriptors reside in one StructuredBuffer heap. Heap slot 0 is the
// reserved 1x1 white fallback; host material slots start at 1. kNoTexture
// resolves to slot 0, while normal-map absence still takes its unperturbed path.
struct BagVertex {
    float3 m_position;
    float3 m_normal;
    float2 m_uv;
    float4 m_tangent;
    float4 m_color;
};

// CPU GpuMaterial mirror: 5x16 B rows (float rows + uint rows), row-major.
// m_textures holds bindless slots (albedo, mr, normal, emissive);
// 0xFFFFFFFF selects the factor fallback. Occlusion shares the composited mr
// texture (ORM: R=occlusion-or-1, G=rough, B=metal).
struct GpuMaterial {
    float4 m_base_color;
    float4 m_emissive_metallic;
    float4 m_rough_alpha_occl_nscale;
    uint4 m_textures;
    uint m_texcoord;
    uint m_flags;
    uint2 m_pad;
};

// Per-instance push as VERTEX entry-point params (root constants), NOT
// globals: slang-rhi d3b6/abb4 misplaces any ConstantBuffer nested beside
// $Globals ordinary data (emits b2 for b1), so global scope keeps exactly ONE
// CBV (g_frame) and ZERO ordinary data. PushScalars is scalars-only (20 B,
// exact both sides); the matrix rides its own param (64 B, exact).
struct PushScalars {
    uint m_vb_offset;
    uint m_ib_start;
    uint m_index_count;
    uint m_material;
    int m_base_vertex;
};

struct FrameConstants {
    float4x4 m_view;
    float4x4 m_projection;
    float4x4 m_view_projection;
    float4x4 m_inverse_view_projection;
    float4 m_camera_position;
    float4 m_viewport_size;
};

ConstantBuffer<FrameConstants> g_frame;
StructuredBuffer<BagVertex> g_vertices;
StructuredBuffer<uint> g_indices;
StructuredBuffer<GpuMaterial> g_materials;

static const uint kNoTexture = 0xFFFFFFFF;
static const uint kFallbackTextureSlot = 0u;

uint resolveTextureSlot(uint slot) {
    return slot == kNoTexture ? kFallbackTextureSlot : slot;
}

struct BindlessVertexOutput {
    float4 position : SV_Position;
    float3 world : WORLDPOS;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    nointerpolation uint material : MATERIAL;
};

[shader("vertex")]
BindlessVertexOutput vertexMain(
    uint sv_vertex_id : SV_VertexID, uniform float4x4 g_model, uniform PushScalars g_push) {
    int bag_index = int(g_indices[g_push.m_ib_start + sv_vertex_id])
        + int(g_push.m_vb_offset)
        + g_push.m_base_vertex;
    BagVertex v = g_vertices[bag_index];
    BindlessVertexOutput output;
    // Row-vector convention (mirrors triangle.slang): translation in row 3,
    // positive clip.w for +Z view-space points.
    float4 world = mul(float4(v.m_position, 1.0), g_model);
    output.position = mul(world, g_frame.m_view_projection);
    output.world = world.xyz;
    output.uv = v.m_uv;
    output.normal = mul(float4(v.m_normal, 0.0), g_model).xyz;
    output.tangent = float4(mul(float4(v.m_tangent.xyz, 0.0), g_model).xyz, v.m_tangent.w);
    output.material = g_push.m_material;
    return output;
}

// The texture heap and shared sampler ride FRAGMENT entry-point params for the
// same $Globals reason: global scope stays free of ordinary data.
[shader("fragment")]
float4 fragmentMain(
    BindlessVertexOutput input,
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    GpuMaterial mat = g_materials[input.material];
    float4 albedo = mat.m_base_color;
    albedo *= nonuniform(g_textures[resolveTextureSlot(mat.m_textures.x)]).Sample(g_sampler, input.uv);
    // Opaque + mask only (plan §7): blend is rejected at pack time.
    if ((mat.m_flags & 0x3u) == 1u && albedo.a < mat.m_rough_alpha_occl_nscale.y) {
        discard;
    }
    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent.xyz);
    float3 bitangent = cross(normal, tangent) * input.tangent.w;
    if (mat.m_textures.z != kNoTexture) {
        float3 texel = nonuniform(g_textures[resolveTextureSlot(mat.m_textures.z)]).Sample(g_sampler, input.uv).rgb * 2.0 - 1.0;
        normal = normalize(tangent * texel.x + bitangent * texel.y + normal * texel.z);
    }
    float metallic = mat.m_emissive_metallic.w;
    float roughness = mat.m_rough_alpha_occl_nscale.x;
    float occlusion = mat.m_rough_alpha_occl_nscale.z;
    float3 mr = nonuniform(g_textures[resolveTextureSlot(mat.m_textures.y)]).Sample(g_sampler, input.uv).rgb;
    occlusion *= mr.x;
    roughness *= mr.y;
    metallic *= mr.z;
    float3 emissive = mat.m_emissive_metallic.rgb;
    emissive *= nonuniform(g_textures[resolveTextureSlot(mat.m_textures.w)]).Sample(g_sampler, input.uv).rgb;
    float3 light_dir = normalize(float3(-0.35, 0.55, 0.76));
    float diffuse = saturate(dot(normal, light_dir));
    float3 view_dir = normalize(g_frame.m_camera_position.xyz - input.world);
    float specular = pow(saturate(dot(normal, normalize(light_dir + view_dir))), lerp(64.0, 8.0, roughness));
    float3 lit = albedo.rgb * (0.35 + 0.65 * diffuse) * occlusion * (1.0 - 0.5 * metallic)
        + specular * (1.0 - roughness) * 0.25
        + emissive;
    return float4(lit, albedo.a);
}
