module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.mesh;
import std;

namespace pP::tests::detail {
    namespace Mesh {
        // Mesh fixtures stage POST_BUILD next to the test executable; CTest
        // runs with WORKING_DIRECTORY == that directory (Ninja single-config),
        // so current_path()/"meshes" is the getContentDir()/"meshes" analogue.
        // P0c trap: Mango importScene concatenates dir+file VERBATIM, so dir
        // must carry a trailing separator (operator/ with "" appends one).
        [[nodiscard]] std::filesystem::path meshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] bool allResolvedInRange_(const pP::mesh::StaticMeshAsset &mesh_asset) noexcept {
            const i64 vert_count = static_cast<i64>(mesh_asset.m_verts.size());
            for (const pP::mesh::MeshPrimitiveRange &prim: mesh_asset.m_prims) {
                const u64 end = static_cast<u64>(prim.m_start) + static_cast<u64>(prim.m_count);
                if (end > mesh_asset.m_indices.size()) {
                    return false;
                }
                for (u64 k = 0u; k < static_cast<u64>(prim.m_count); ++k) {
                    const i64 resolved =
                            static_cast<i64>(mesh_asset.m_indices[prim.m_start + static_cast<u32>(k)])
                            + static_cast<i64>(prim.m_base);
                    if (resolved < 0
                        or resolved >= vert_count)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        PPR_UNIT_TEST (glb_embed_path_imports) {
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "textured_quad.glb");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_meshes.size() == 1u);
            PPR_TEST_ASSERT(not scene->m_instances.empty());

            const pP::mesh::StaticMeshAsset &mesh = scene->m_meshes.front();
            PPR_TEST_ASSERT(not mesh.m_verts.empty());
            PPR_TEST_ASSERT(mesh.m_indices.size() % 3u == 0u);
            PPR_TEST_ASSERT(not mesh.m_prims.empty());
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::position) == pP::mesh::EMeshAttribute::position);
            for (const pP::mesh::MeshPrimitiveRange &prim: mesh.m_prims) {
                PPR_TEST_ASSERT(*prim.m_material < scene->m_mats.size());
            }
            PPR_TEST_ASSERT(allResolvedInRange_(mesh));

            // GLB-embed path: one PNG cloned while the Mango Scene lives.
            PPR_TEST_ASSERT(scene->m_images.size() == 1u);
            const pP::mesh::ImageRef &image = scene->m_images.front();
            PPR_TEST_ASSERT(not image.m_is_file);
            PPR_TEST_ASSERT(not image.m_bytes.getBufferData().empty());
            PPR_TEST_ASSERT(image.m_bytes.isMaterialized());
        };

        PPR_UNIT_TEST (gltf_external_uri_path_imports) {
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_meshes.size() == 1u);

            const pP::mesh::StaticMeshAsset &mesh = scene->m_meshes.front();
            PPR_TEST_ASSERT(mesh.m_verts.size() == 24u);
            PPR_TEST_ASSERT(mesh.m_indices.size() == 36u);
            PPR_TEST_ASSERT(mesh.m_prims.size() == 1u);
            const pP::mesh::MeshPrimitiveRange &prim = mesh.m_prims.front();
            PPR_TEST_ASSERT(prim.m_start == 0u);
            PPR_TEST_ASSERT(prim.m_count == 36u);
            PPR_TEST_ASSERT(prim.m_material == pP::mesh::MaterialAssetId{0u});
            PPR_TEST_ASSERT(allResolvedInRange_(mesh));
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::position) == pP::mesh::EMeshAttribute::position);
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::normal) == pP::mesh::EMeshAttribute::normal);
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::texcoord) == pP::mesh::EMeshAttribute::texcoord);
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::tangent) == pP::mesh::EMeshAttribute::none);
            PPR_TEST_ASSERT(
                (mesh.m_flags & pP::mesh::EMeshAttribute::color) == pP::mesh::EMeshAttribute::none);

            // External-URI path: geometry + texture stay file refs.
            PPR_TEST_ASSERT(scene->m_images.size() == 1u);
            const pP::mesh::ImageRef &image = scene->m_images.front();
            PPR_TEST_ASSERT(image.m_is_file);
            PPR_TEST_ASSERT(image.m_rel_path == "textured_box.png");
            PPR_TEST_ASSERT(image.m_ext == ".png");
            PPR_TEST_ASSERT(not image.m_bytes.getBufferData().empty());

            PPR_TEST_ASSERT(scene->m_nodes.size() == 1u);
            PPR_TEST_ASSERT(scene->m_nodes.front().m_parent == pP::mesh::kInvalidNode);
            PPR_TEST_ASSERT(scene->m_instances.size() == 1u);
            PPR_TEST_ASSERT(scene->m_instances.front().m_mesh == pP::mesh::MeshAssetId{0u});
            PPR_TEST_ASSERT(scene->m_instances.front().m_node == pP::mesh::NodeId{0u});
            PPR_TEST_ASSERT(
                scene->m_instances.front().m_materialOverride == pP::mesh::kInvalidMaterial);
        };

        PPR_UNIT_TEST (verbatim_lh_box_values) {
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            const pP::mesh::StaticMeshAsset &mesh = scene->m_meshes.front();

            // Unit-cube corners verbatim: every component is exactly ±0.5.
            for (const pP::mesh::StaticMeshVertex &vert: mesh.m_verts) {
                PPR_TEST_ASSERT(vert.m_position[0] == 0.5f or vert.m_position[0] == -0.5f);
                PPR_TEST_ASSERT(vert.m_position[1] == 0.5f or vert.m_position[1] == -0.5f);
                PPR_TEST_ASSERT(vert.m_position[2] == 0.5f or vert.m_position[2] == -0.5f);
                // Axis-aligned unit normals, V-down UVs in {0, 1}.
                const float normal_len = vert.m_normal[0] * vert.m_normal[0]
                                         + vert.m_normal[1] * vert.m_normal[1] + vert.m_normal[2] * vert.m_normal[2];
                PPR_TEST_ASSERT(normal_len == 1.0f);
                PPR_TEST_ASSERT(vert.m_texcoord[0] == 0.0f or vert.m_texcoord[0] == 1.0f);
                PPR_TEST_ASSERT(vert.m_texcoord[1] == 0.0f or vert.m_texcoord[1] == 1.0f);
            }

            // First-vertex spot check against the decoded .bin (order kept,
            // RH→LH fork flip applied: (x, y, -z)).
            const pP::mesh::StaticMeshVertex &first = mesh.m_verts.front();
            PPR_TEST_ASSERT(first.m_position[0] == -0.5f);
            PPR_TEST_ASSERT(first.m_position[1] == -0.5f);
            PPR_TEST_ASSERT(first.m_position[2] == -0.5f);
            PPR_TEST_ASSERT(first.m_normal[0] == 0.0f);
            PPR_TEST_ASSERT(first.m_normal[1] == 0.0f);
            PPR_TEST_ASSERT(first.m_normal[2] == -1.0f);
            PPR_TEST_ASSERT(first.m_texcoord[0] == 0.0f);
            PPR_TEST_ASSERT(first.m_texcoord[1] == 0.0f);

            // First-face winding is file order (already CW-outside).
            PPR_TEST_ASSERT(mesh.m_indices[0u] == 0u);
            PPR_TEST_ASSERT(mesh.m_indices[1u] == 1u);
            PPR_TEST_ASSERT(mesh.m_indices[2u] == 2u);
        };

        PPR_UNIT_TEST (verbatim_lh_quad_values) {
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "textured_quad.glb");
            PPR_TEST_ASSERT(scene.has_value());
            const pP::mesh::StaticMeshAsset &mesh = scene->m_meshes.front();

            // XY-plane quad verbatim: 4 corners, z == 0 throughout.
            PPR_TEST_ASSERT(mesh.m_verts.size() == 4u);
            PPR_TEST_ASSERT(mesh.m_indices.size() == 6u);
            for (const pP::mesh::StaticMeshVertex &vert: mesh.m_verts) {
                PPR_TEST_ASSERT(vert.m_position[0] == 0.5f or vert.m_position[0] == -0.5f);
                PPR_TEST_ASSERT(vert.m_position[1] == 0.5f or vert.m_position[1] == -0.5f);
                PPR_TEST_ASSERT(vert.m_position[2] == 0.0f);
            }
            const pP::mesh::StaticMeshVertex &first = mesh.m_verts.front();
            PPR_TEST_ASSERT(first.m_position[0] == -0.5f);
            PPR_TEST_ASSERT(first.m_position[1] == -0.5f);
            PPR_TEST_ASSERT(first.m_position[2] == 0.0f);
        };

        PPR_UNIT_TEST (material_mr_split) {
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_mats.size() == 1u);
            const pP::mesh::MaterialAsset &material = scene->m_mats.front();

            // Factors verbatim from the glTF JSON.
            PPR_TEST_ASSERT(material.m_metallic == 0.0f);
            PPR_TEST_ASSERT(material.m_roughness > 0.9f - 1e-6f and material.m_roughness < 0.9f + 1e-6f);
            PPR_TEST_ASSERT(material.m_alpha_mode == pP::mesh::AlphaMode::opaque);
            PPR_TEST_ASSERT(material.m_alpha_cutoff == 0.5f);
            PPR_TEST_ASSERT(material.m_normal_scale == 1.0f);
            PPR_TEST_ASSERT(material.m_occlusion_strength == 1.0f);
            PPR_TEST_ASSERT(not material.m_twosided);

            // MR split: no shared ORM texture authored, so metallic and
            // roughness stay SEPARATE disabled slots (glTF sharing is the
            // special case); only base color is bound.
            PPR_TEST_ASSERT(material.m_base_color_map.enabled());
            PPR_TEST_ASSERT(material.m_base_color_map.m_image == pP::mesh::ImageAssetId{0u});
            PPR_TEST_ASSERT(material.m_base_color_map.m_texcoord == pP::mesh::UvSetId{0u});
            PPR_TEST_ASSERT(not material.m_metallic_map.enabled());
            PPR_TEST_ASSERT(not material.m_roughness_map.enabled());
            PPR_TEST_ASSERT(not material.m_normal_map.enabled());
            PPR_TEST_ASSERT(not material.m_occlusion_map.enabled());
            PPR_TEST_ASSERT(not material.m_emissive_map.enabled());

            // Defaults: slots start disabled, IDs start at set 0.
            const pP::mesh::MaterialImageSlot fresh;
            PPR_TEST_ASSERT(not fresh.enabled());
            PPR_TEST_ASSERT(fresh.m_image == pP::mesh::kInvalidImage);
        };

        PPR_UNIT_TEST (rejects_non_gltf_extension) {
            // OBJ/FBX are deferred entirely: rejected before any import.
            const Expected<pP::mesh::SceneAsset> obj = pP::mesh::importAndConvert(meshDir(), "mesh.obj");
            PPR_TEST_ASSERT(not obj.has_value());
            PPR_TEST_ASSERT(obj.error() == std::errc::invalid_argument);

            const Expected<pP::mesh::SceneAsset> fbx = pP::mesh::importAndConvert(meshDir(), "mesh.fbx");
            PPR_TEST_ASSERT(not fbx.has_value());
            PPR_TEST_ASSERT(fbx.error() == std::errc::invalid_argument);
        };

        PPR_UNIT_TEST (rejects_empty_and_missing) {
            const Expected<pP::mesh::SceneAsset> empty_dir = pP::mesh::importAndConvert({}, "textured_box.gltf");
            PPR_TEST_ASSERT(not empty_dir.has_value());
            PPR_TEST_ASSERT(empty_dir.error() == std::errc::invalid_argument);

            const Expected<pP::mesh::SceneAsset> empty_file =
                    pP::mesh::importAndConvert(meshDir(), "");
            PPR_TEST_ASSERT(not empty_file.has_value());
            PPR_TEST_ASSERT(empty_file.error() == std::errc::invalid_argument);

            const Expected<pP::mesh::SceneAsset> missing =
                    pP::mesh::importAndConvert(meshDir(), "does_not_exist.glb");
            PPR_TEST_ASSERT(not missing.has_value());
            PPR_TEST_ASSERT(
                missing.error() == pP::mesh::make_error_code(pP::mesh::errc::import_failed));
        };

        [[nodiscard]] std::filesystem::path malformedDir() {
            return std::filesystem::current_path() / "malformed_glb_tmp" / "";
        }

        [[nodiscard]] std::string readQuadGlbBytes() {
            std::ifstream file(meshDir() / "textured_quad.glb", std::ios::binary);
            if (not file) {
                return {};
            }
            return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        [[nodiscard]] bool writeScratchGlb(const std::string_view name, const std::string &bytes) {
            std::error_code err{};
            std::filesystem::create_directories(malformedDir(), err);
            if (err) {
                return false;
            }
            std::ofstream file(malformedDir() / name, std::ios::binary | std::ios::trunc);
            if (not file) {
                return false;
            }
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(file);
        }

        // Malformed GLBs must fail closed (Expected error, never a crash):
        // header-only truncation (body cut away), a JSON chunk length that
        // disagrees with the payload, and an images[] entry whose bufferView
        // no longer matches the bufferViews[] layout.
        PPR_UNIT_TEST (malformed_glb_truncated_header) {
            // 12-byte header only: magic + version + total, zero chunks.
            const std::string bytes{'g', 'l', 'T', 'F', 0x02, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00};
            PPR_TEST_ASSERT(writeScratchGlb("malformed_truncated.glb", bytes));
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(malformedDir(), "malformed_truncated.glb");
            PPR_TEST_ASSERT(not scene.has_value());
            std::error_code err{};
            std::filesystem::remove_all(std::filesystem::current_path() / "malformed_glb_tmp", err);
        };

        PPR_UNIT_TEST (malformed_glb_bad_chunk_length) {
            std::string bytes = readQuadGlbBytes();
            PPR_TEST_ASSERT(bytes.size() == 1660u);
            // JSON chunk length (file offset 12, little-endian 924) shrinks to
            // 8: the declared length disagrees with the JSON payload.
            bytes[12] = static_cast<char>(0x08);
            bytes[13] = static_cast<char>(0x00);
            bytes[14] = static_cast<char>(0x00);
            bytes[15] = static_cast<char>(0x00);
            PPR_TEST_ASSERT(writeScratchGlb("malformed_chunklen.glb", bytes));
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(malformedDir(), "malformed_chunklen.glb");
            PPR_TEST_ASSERT(not scene.has_value());
            std::error_code err{};
            std::filesystem::remove_all(std::filesystem::current_path() / "malformed_glb_tmp", err);
        };

        PPR_UNIT_TEST (malformed_glb_view_order_mismatch) {
            std::string bytes = readQuadGlbBytes();
            PPR_TEST_ASSERT(bytes.size() == 1660u);
            // images[0] claims bufferView 4 (the PNG); zero that view's
            // byteLength so bufferViews[] no longer delivers what images[]
            // references. Debugged live, not guessed: repointing at a valid
            // in-range geometry view imports garbage-bytes downstream (no
            // rejection — wrong test), and an out-of-range index fail-fasts
            // inside the mango fork (`asset.bufferViews[i]`, import_gltf:271 —
            // uncatchable, unusable here). A zero-length view is the mismatch
            // both sides survive: PPR skips the view and convertImage fails
            // closed, mango guards the empty range itself.
            // -2 JSON bytes: bump the JSON chunk length (offset 12: 924→922)
            // and the total length (offset 8: 1660→1658) to match.
            const std::string_view needle{R"("byteLength":565)"};
            const std::size_t at = bytes.find(needle);
            PPR_TEST_ASSERT(at != std::string::npos);
            bytes.replace(at, needle.size(), R"("byteLength":0)");
            bytes[12] = static_cast<char>(0x9Au);
            bytes[8] = static_cast<char>(0x7Au);
            bytes[9] = static_cast<char>(0x06u);
            PPR_TEST_ASSERT(writeScratchGlb("malformed_vieworder.glb", bytes));
            const Expected<pP::mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(malformedDir(), "malformed_vieworder.glb");
            PPR_TEST_ASSERT(not scene.has_value());
            std::error_code err{};
            std::filesystem::remove_all(std::filesystem::current_path() / "malformed_glb_tmp", err);
        };

        PPR_UNIT_TEST (errc_taxonomy) {
            PPR_TEST_ASSERT(std::string{pP::mesh::error_category().name()} == "mesh");
            PPR_TEST_ASSERT(
                pP::mesh::make_error_code(pP::mesh::errc::invalid_argument) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(
                pP::mesh::make_error_code(pP::mesh::errc::function_not_supported)
                == std::errc::function_not_supported);
            PPR_TEST_ASSERT(
                pP::mesh::make_error_code(pP::mesh::errc::import_failed).category()
                == pP::mesh::error_category());
        };

        PPR_UNIT_TEST (vertex_layout_64b) {
            // 64 B LH vertex (§2.3/§3): position + normal + uv + tangent + color.
            PPR_TEST_ASSERT(std::is_trivially_copyable_v<pP::mesh::StaticMeshVertex>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::StaticMeshVertex) == 64u);
            PPR_TEST_ASSERT(alignof(pP::mesh::StaticMeshVertex) == 4u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(pP::mesh::StaticMeshVertex, m_position) == 0u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(pP::mesh::StaticMeshVertex, m_normal) == 12u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(pP::mesh::StaticMeshVertex, m_texcoord) == 24u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(pP::mesh::StaticMeshVertex, m_tangent) == 32u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(pP::mesh::StaticMeshVertex, m_color) == 48u);

            PPR_TEST_ASSERT(std::is_trivially_copyable_v<pP::mesh::MeshPrimitiveRange>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::MeshPrimitiveRange) == 16u);
        };

        PPR_UNIT_TEST (id_layout_and_sentinels) {
            // Strong-index vocabulary (§2.1): layout + size + typed nones.
            PPR_TEST_ASSERT(std::is_standard_layout_v<pP::mesh::ImageAssetId>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::ImageAssetId) == 4u);
            PPR_TEST_ASSERT(std::is_standard_layout_v<pP::mesh::MaterialAssetId>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::MaterialAssetId) == 4u);
            PPR_TEST_ASSERT(std::is_standard_layout_v<pP::mesh::MeshAssetId>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::MeshAssetId) == 4u);
            PPR_TEST_ASSERT(std::is_standard_layout_v<pP::mesh::NodeId>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::NodeId) == 4u);
            PPR_TEST_ASSERT(std::is_standard_layout_v<pP::mesh::UvSetId>);
            PPR_TEST_ASSERT(sizeof(pP::mesh::UvSetId) == 4u);

            PPR_TEST_ASSERT(pP::mesh::kInvalidImage == pP::mesh::ImageAssetId{0xFFFFFFFFu});
            PPR_TEST_ASSERT(pP::mesh::kInvalidMaterial == pP::mesh::MaterialAssetId{0xFFFFFFFFu});
            PPR_TEST_ASSERT(pP::mesh::kInvalidNode == pP::mesh::NodeId{0xFFFFFFFFu});
        };
    } // namespace Mesh
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest mesh = UnitTest::Named("mesh") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Mesh::glb_embed_path_imports,
            detail::Mesh::gltf_external_uri_path_imports,
            detail::Mesh::verbatim_lh_box_values,
            detail::Mesh::verbatim_lh_quad_values,
            detail::Mesh::material_mr_split,
            detail::Mesh::rejects_non_gltf_extension,
            detail::Mesh::rejects_empty_and_missing,
            detail::Mesh::malformed_glb_truncated_header,
            detail::Mesh::malformed_glb_bad_chunk_length,
            detail::Mesh::malformed_glb_view_order_mismatch,
            detail::Mesh::errc_taxonomy,
            detail::Mesh::vertex_layout_64b,
            detail::Mesh::id_layout_and_sentinels,
        });
    };

    const UnitTest &meshTests() noexcept {
        return mesh;
    }
} // namespace pP::tests
