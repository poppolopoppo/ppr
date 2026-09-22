module;
#include "pP/Macros.h"
export module engine.mesh:types;

import engine.core;
import engine.math;

import std;

// CPU-side static mesh vocabulary (docs/plans/asset-pipeline.md §2.3).
// Verbatim left-handed glTF copy: the Mango fork pre-converts RH→LH, so the
// converter below bit-copies positions/normals (x,y,-z), negates tangent w
// (P0c: verbatim fork w inverts bitangent handedness), falls back to in-fork
// MikkTSpace regen when TANGENT is absent, and keeps file-order indices and
// V-down UVs without further transforms.

export namespace pP::mesh {
    // Index-ID vocabulary (§2.1): every index into SceneAsset storage is a
    // Numeric<u32, Tag> alias. Raw u32/u64 indices are banned at API boundaries.
    struct ImageAssetIdTag final {
    };

    using ImageAssetId = Numeric<u32, ImageAssetIdTag>;

    struct MaterialAssetIdTag final {
    };

    using MaterialAssetId = Numeric<u32, MaterialAssetIdTag>;

    struct MeshAssetIdTag final {
    };

    using MeshAssetId = Numeric<u32, MeshAssetIdTag>;

    struct NodeIdTag final {
    };

    using NodeId = Numeric<u32, NodeIdTag>;

    struct UvSetIdTag final {
    };

    using UvSetId = Numeric<u32, UvSetIdTag>;

    inline constexpr ImageAssetId kInvalidImage{0xFFFFFFFFu};
    inline constexpr MaterialAssetId kInvalidMaterial{0xFFFFFFFFu};
    inline constexpr NodeId kInvalidNode{0xFFFFFFFFu};

    static_assert(std::is_standard_layout_v<ImageAssetId>);
    static_assert(sizeof(ImageAssetId) == 4u);
    static_assert(std::is_standard_layout_v<MaterialAssetId>);
    static_assert(sizeof(MaterialAssetId) == 4u);
    static_assert(std::is_standard_layout_v<MeshAssetId>);
    static_assert(sizeof(MeshAssetId) == 4u);
    static_assert(std::is_standard_layout_v<NodeId>);
    static_assert(sizeof(NodeId) == 4u);
    static_assert(std::is_standard_layout_v<UvSetId>);
    static_assert(sizeof(UvSetId) == 4u);

    // Plain-float-array DTO (P1 interim vertex verdict): mango Vector members
    // carry user-provided copy/dtor, so they can never be trivially copyable —
    // which §2.4 upload + §3 bitwise-copy require. No math lives here;
    // conversion happens at the §3 boundary in :convert via load/store helpers.
    struct StaticMeshVertex {
        float m_position[3];
        float m_normal[3];
        float m_texcoord[2];
        float m_tangent[4];
        float m_color[4];
    };

    static_assert(std::is_trivially_copyable_v<StaticMeshVertex>);
    static_assert(std::is_standard_layout_v<StaticMeshVertex>);
    static_assert(sizeof(StaticMeshVertex) == 64u);
    static_assert(alignof(StaticMeshVertex) == 4u);
    static_assert(PPR_OFFSETOF(StaticMeshVertex, m_position) == 0u);
    static_assert(PPR_OFFSETOF(StaticMeshVertex, m_normal) == 12u);
    static_assert(PPR_OFFSETOF(StaticMeshVertex, m_texcoord) == 24u);
    static_assert(PPR_OFFSETOF(StaticMeshVertex, m_tangent) == 32u);
    static_assert(PPR_OFFSETOF(StaticMeshVertex, m_color) == 48u);

    // PPR-owned typed flags. No joints/weights bits exist in MVP: joint data
    // present in the source is rejected with function_not_supported, never stored.
    enum class EMeshAttribute : u32 {
        none = 0u,
        position = 0x1u,
        normal = 0x2u,
        texcoord = 0x4u,
        tangent = 0x8u,
        color = 0x10u,
    };

    [[nodiscard]] constexpr EMeshAttribute operator|(const EMeshAttribute lhs, const EMeshAttribute rhs) noexcept {
        return static_cast<EMeshAttribute>(enumOrd(lhs) | enumOrd(rhs));
    }

    constexpr EMeshAttribute &operator|=(EMeshAttribute &lhs, const EMeshAttribute rhs) noexcept {
        return lhs = lhs | rhs;
    }

    [[nodiscard]] constexpr EMeshAttribute operator&(const EMeshAttribute lhs, const EMeshAttribute rhs) noexcept {
        return static_cast<EMeshAttribute>(enumOrd(lhs) & enumOrd(rhs));
    }

    // Draw range with the stored Mango base honoured verbatim: append-path
    // primitives carry base=0 with remapped global indices, direct-glTF
    // primitives carry base=vert_count with file-local indices. Resolved index
    // is always (index + base), so both conventions draw correctly.
    struct MeshPrimitiveRange {
        u32 m_start;
        u32 m_count;
        i32 m_base;
        MaterialAssetId m_material;
    };

    static_assert(std::is_trivially_copyable_v<MeshPrimitiveRange>);
    static_assert(sizeof(MeshPrimitiveRange) == 16u);

    struct StaticMeshAsset {
        Array<StaticMeshVertex> m_verts;
        Array<u32> m_indices;
        Array<MeshPrimitiveRange> m_prims;
        Box m_bounds;
        EMeshAttribute m_flags;
    };

    struct UvTransformAsset {
        float2 m_scale{1.0f, 1.0f};
        float2 m_offset{0.0f, 0.0f};
        float m_rotation = 0.0f;
    };

    struct MaterialImageSlot {
        ImageAssetId m_image = kInvalidImage;
        UvSetId m_texcoord{};
        UvTransformAsset m_transform;

        [[nodiscard]] constexpr bool enabled() const noexcept { return m_image != kInvalidImage; }
    };

    enum class AlphaMode : u8 {
        opaque,
        mask,
        blend,
    };

    // MVP carries only the metallic-roughness core. Deferred (nothing consumes
    // them): clearcoat/sheen/anisotropy. Not carried: per-slot swizzle (implied
    // by semantic) and per-slot color space (base_color/emissive→sRGB, rest→linear).
    struct MaterialAsset {
        float4 m_base_color{1.0f, 1.0f, 1.0f, 1.0f};
        float m_metallic = 1.0f;
        float m_roughness = 1.0f;
        float3 m_emissive{0.0f, 0.0f, 0.0f};
        float m_alpha_cutoff = 0.5f;
        float m_occlusion_strength = 1.0f;
        float m_normal_scale = 1.0f;
        AlphaMode m_alpha_mode = AlphaMode::opaque;
        bool m_twosided = false;
        MaterialImageSlot m_base_color_map;
        MaterialImageSlot m_metallic_map;
        MaterialImageSlot m_roughness_map;
        MaterialImageSlot m_normal_map;
        MaterialImageSlot m_occlusion_map;
        MaterialImageSlot m_emissive_map;
    };

    // Local duplicate of the image reference (mesh→image type dependency is
    // rejected): the app-side pack step joins ImageRef → decodeTo_*.
    // Embed rule: Mango embed bytes are cloned (UniqueBuffer::clone →
    // moveToShared) while the Mango Scene lives; file refs are mapped with
    // SharedBuffer::mapFile, keeping m_rel_path for pack-step re-resolution.
    // After importAndConvert returns, nothing of Mango outlives the call.
    struct ImageRef {
        std::string m_name;
        std::string m_ext;
        std::string m_rel_path;
        mem::SharedBuffer m_bytes;
        bool m_is_file = true;
    };

    struct SceneNodeAsset {
        NodeId m_parent = kInvalidNode;
        float4x4 m_local{float4x4::identity()};
        float4x4 m_world{float4x4::identity()};
    };

    struct SceneInstance {
        MeshAssetId m_mesh;
        NodeId m_node;
        MaterialAssetId m_materialOverride = kInvalidMaterial;
    };

    // SceneAsset owns meshes + materials + image refs + nodes; instances
    // reference them by typed ID. m_materialOverride stays kInvalidMaterial:
    // glTF binds materials per primitive, never per instance.
    struct SceneAsset {
        Array<StaticMeshAsset> m_meshes;
        Array<MaterialAsset> m_mats;
        Array<ImageRef> m_images;
        Array<SceneNodeAsset> m_nodes;
        Array<SceneInstance> m_instances;
    };

    enum class errc : int {
        invalid_argument = 1,
        function_not_supported,
        import_failed,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;
    [[nodiscard]] std::error_code make_error_code(errc err) noexcept;
}

export template<>
struct std::is_error_code_enum<pP::mesh::errc> : true_type {
};
