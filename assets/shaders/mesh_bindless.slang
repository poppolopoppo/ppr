// Bindless static-mesh pass (docs/plans/asset-pipeline.md §6): PURE
// StructuredBuffer fetch. The CPU StaticMeshVertex stride is 64 B
// (plain-float arrays, 12+12+8+16+16); BagVertex mirrors it exactly.
// Texture descriptors reside in one StructuredBuffer heap. Heap slot 0 is the
// reserved 1x1 white fallback; kNoTexture resolves to slot 0, so albedo, ORM,
// and emissive all multiply by 1.0 rather than 0.0 when a map is absent.
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

struct FrameConstants {
    // Only m_view_projection and m_camera_position are read by this shader.
    // m_view, m_projection, m_inverse_view_projection, and m_viewport_size are
    // uploaded by uploadFrameConstants_ and locked by a CPU sizeof == 288
    // assertion, so they stay even though nothing samples them here.
    float4x4 m_view;
    float4x4 m_projection;
    float4x4 m_view_projection;
    float4x4 m_inverse_view_projection;
    float4 m_camera_position;
    float4 m_viewport_size;
};

// CPU mirror: 96 B. Field order, widths, signedness, and the m_pad that rounds
// the element to a 16 B multiple are asserted on the CPU side.
// Payload indices: C++ groups by (variant, bucket, vb_offset, ib_start, count,
// base_vertex) and lays each group out contiguously, so a draw's instances are
// base..base+count-1. Bags are never content-deduped, so a vb_offset/ib_start
// pair always identifies exactly one bag region.
struct InstancePayload {
    float4x4 m_model;
    uint m_vb_offset;
    uint m_ib_start;
    uint m_index_count;
    uint m_material;
    int m_base_vertex;
    uint3 m_pad;
};

// Global scope carries NO ordinary data: a ConstantBuffer nested beside $Globals
// ordinary data is misbound (D3D12SerializeRootSignature then reports the
// generated globalParams b0 overlapping g_frame b1), which fails program
// creation. Per-draw ordinary values must therefore arrive as entry-point
// `uniform` parameters, as g_payload_base does below.
ConstantBuffer<FrameConstants> g_frame;
StructuredBuffer<BagVertex> g_vertices;
StructuredBuffer<uint> g_indices;
// One slot per pack call, never content-deduped; material slot 0 is a real
// material, not a fallback, so a released slot is tombstoned with kNoTexture in
// all four m_textures (a zeroed struct would alias four real materials).
StructuredBuffer<GpuMaterial> g_materials;
StructuredBuffer<InstancePayload> g_payloads;

static const uint kNoTexture = 0xFFFFFFFF;
static const uint kFallbackTextureSlot = 0u;
// Contract: the descriptor heap holds rhi::kBindlessTextureBudget entries, slot 0
// is the reserved white fallback, and host slots start at 1. The heap buffer is
// created without an init payload, so a never-written slot reads back a garbage
// handle and an out-of-range structured read returns 0, i.e. a null descriptor.
static const uint kTextureBudget = 4096u;

// Policy: an out-of-bounds slot falls back to slot 0, by explicit decision, so a
// bad index degrades to defined factor-only rendering instead of aliasing the
// last heap slot. Unreachable today - host slots are bump-allocated below the
// budget - so this is defence in depth, not a live path.
uint resolveTextureSlot(uint slot) {
    if (slot == kNoTexture || slot >= kTextureBudget) {
        return kFallbackTextureSlot;
    }
    return slot;
}

struct BindlessVertexOutput {
    float4 position : SV_Position;
    float3 world : WORLDPOS;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    nointerpolation uint material : MATERIAL;
};

// g_payload_base is an entry-point `uniform` (not a global) because global-scope
// ordinary data makes D3D12SerializeRootSignature fail with an overlapping
// globalParams/g_frame CBV. C++ binds it by the same flat
// cursor["g_payload_base"].setData(&base, sizeof(base)) used for the former
// g_model/g_push entry-point uniforms; it is plain data, so it needs no
// getDereferenced (unlike g_frame).
//
// SV_InstanceID is draw-local on D3D12, SPIR-V, and Metal alike: the CPU's
// startInstanceLocation never reaches the shader, so a bare SV_InstanceID would
// make every instanced draw read payload 0. C++ therefore encodes
// startInstanceLocation = 0 and sets this 4-byte base to the payload index of
// the draw group's first instance. Plain u32 addition matches the payload index
// domain exactly; the sum is a draw-local offset, never a resource index.
[shader("vertex")]
BindlessVertexOutput vertexIndirectMain(
    uint sv_vertex_id : SV_VertexID,
    uint sv_instance_id : SV_InstanceID,
    uniform uint g_payload_base) {
    InstancePayload payload = g_payloads[g_payload_base + sv_instance_id];
    int bag_index = int(g_indices[payload.m_ib_start + sv_vertex_id])
        + int(payload.m_vb_offset)
        + payload.m_base_vertex;
    BagVertex v = g_vertices[bag_index];
    BindlessVertexOutput output;
    // Row-vector convention (mirrors the CPU Mango transform): translation in
    // row 3, positive clip.w for +Z view-space points.
    float4 world = mul(float4(v.m_position, 1.0), payload.m_model);
    output.position = mul(world, g_frame.m_view_projection);
    output.world = world.xyz;
    output.uv = v.m_uv;
    // Contract: m_model is a rigid transform or a uniform scale. Rotating a
    // direction with mul() instead of the inverse-transpose is exact only in
    // that case; non-uniform scale would need the normal matrix, which would
    // change this payload's 96 B size and its CPU-side assertions.
    output.normal = mul(float4(v.m_normal, 0.0), payload.m_model).xyz;
    output.tangent = float4(mul(float4(v.m_tangent.xyz, 0.0), payload.m_model).xyz, v.m_tangent.w);
    output.material = payload.m_material;
    return output;
}

// The texture heap and shared sampler ride FRAGMENT entry-point params, for the
// same reason g_payload_base is a vertex entry-point param: global scope stays
// free of ordinary data. Heap element indices carry nonuniform() because they
// are DescriptorHandles; the material read does not, and the reasons differ.
[shader("fragment")]
float4 fragmentMain(
    BindlessVertexOutput input,
    // Texture slots ARE content-deduped and refcounted, so one heap slot can
    // serve many loads; heap slot 0 is the reserved white fallback.
    uniform StructuredBuffer<DescriptorHandle<Texture2D<float4>>> g_textures,
    uniform SamplerState.Handle g_sampler) : SV_Target {
    // Plain index on purpose. The heap element reads below need nonuniform()
    // because they are DescriptorHandles; that annotation does not extend to a
    // classic SRV, and the matching NonUniformResourceIndex does not exist in
    // the fragment stage for the metal and wgsl targets (verified with the
    // vendored slangc 2026.17.1). A StructuredBuffer tolerates a divergent
    // index on every backend, so this stays unannotated rather than portable-
    // only on D3D12/SPIR-V. Do not "fix" this to match the heap reads.
    GpuMaterial mat = g_materials[input.material];
    float4 albedo = mat.m_base_color;
    albedo *= nonuniform(g_textures[resolveTextureSlot(mat.m_textures.x)]).Sample(g_sampler, input.uv);
    // Opaque + mask only: blend is rejected at pack time.
    if ((mat.m_flags & 0x3u) == 1u && albedo.a < mat.m_rough_alpha_occl_nscale.y) {
        discard;
    }
    float3 normal = normalize(input.normal);
    // A zero-length tangent would make normalize() emit NaN and poison the
    // cross() below; fall back to the geometric normal instead of the bitangent.
    float3 tangent = normal;
    if (length(input.tangent.xyz) > 0.0) {
        tangent = normalize(input.tangent.xyz);
    }
    float3 bitangent = cross(normal, tangent) * input.tangent.w;
    if (mat.m_textures.z != kNoTexture) {
        float3 texel = nonuniform(g_textures[resolveTextureSlot(mat.m_textures.z)]).Sample(g_sampler, input.uv).rgb * 2.0 - 1.0;
        normal = normalize(tangent * texel.x + bitangent * texel.y + normal * texel.z);
    }
    // ORM convention: R is occlusion (1 when absent), G roughness, B metallic.
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
