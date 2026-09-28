module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.mesh;
import std;

namespace pP::tests::detail {
    namespace UvNormal {
        // Mesh fixtures stage POST_BUILD next to the test executable; CTest
        // runs with WORKING_DIRECTORY == that directory (Ninja single-config),
        // so current_path()/"meshes" is the getContentDir()/"meshes" analogue.
        // Path trap: Mango importScene concatenates dir+file VERBATIM, so dir
        // must carry a trailing separator (operator/ with "" appends one).
        [[nodiscard]] std::filesystem::path meshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        void discardExpectedFailureLog_(const Log::Entry &) noexcept {
        }

        class ExpectedFailureLogGuard final {
            Log::Policy m_previous;

        public:
            ExpectedFailureLogGuard() noexcept
                : m_previous(Log::setWriterPolicy(discardExpectedFailureLog_)) {
            }

            ~ExpectedFailureLogGuard() noexcept {
                // The logger drains asynchronously: flush under the discard
                // policy so no queued error entry can leak past the restore
                // and fail a later test (or the root) nondeterministically.
                std::ignore = Log::flush(true);
                Log::setWriterPolicy(m_previous);
            }
        };

        // KHR_texture_transform bake: the unanimous scene transform
        // lands in m_texcoord at convert, and the stored per-slot transforms
        // reset to identity so no later stage can double-apply them.
        PPR_UNIT_TEST(uv_transform_bakes_into_texcoords) {
            const Expected<mesh::SceneAsset> raw =
                    mesh::importAndConvert(meshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(raw.has_value());
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "uv_transform_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_materials.size() == 1u);
            const mesh::MaterialAsset &material = scene->m_materials.front();
            PPR_TEST_ASSERT(material.m_base_color_map.enabled());
            PPR_TEST_ASSERT(material.m_base_color_map.m_texcoord == pP::mesh::UvSetId{0u});
            PPR_TEST_ASSERT(material.m_base_color_map.m_transform.m_scale.x == 1.0f);
            PPR_TEST_ASSERT(material.m_base_color_map.m_transform.m_scale.y == 1.0f);
            PPR_TEST_ASSERT(material.m_base_color_map.m_transform.m_offset.x == 0.0f);
            PPR_TEST_ASSERT(material.m_base_color_map.m_transform.m_offset.y == 0.0f);
            PPR_TEST_ASSERT(material.m_base_color_map.m_transform.m_rotation == 0.0f);

            // Same geometry: baked UVs follow the spec formula from raw UVs.
            const mesh::StaticMeshAsset &raw_mesh = raw->m_meshes.front();
            const mesh::StaticMeshAsset &baked_mesh = scene->m_meshes.front();
            PPR_TEST_ASSERT(baked_mesh.m_vertices.size() == raw_mesh.m_vertices.size());
            const float rotation_c = std::cos(0.5f);
            const float rotation_s = std::sin(0.5f);
            u32 origin_count = 0u;
            for (std::size_t i = 0u; i < baked_mesh.m_vertices.size(); ++i) {
                const float u = raw_mesh.m_vertices[i].m_texcoord[0];
                const float v = raw_mesh.m_vertices[i].m_texcoord[1];
                const float want_u = rotation_c * 2.0f * u - rotation_s * 0.5f * v + 0.25f;
                const float want_v = rotation_s * 2.0f * u + rotation_c * 0.5f * v + 0.5f;
                PPR_TEST_ASSERT(std::abs(baked_mesh.m_vertices[i].m_texcoord[0] - want_u) < 1e-5f);
                PPR_TEST_ASSERT(std::abs(baked_mesh.m_vertices[i].m_texcoord[1] - want_v) < 1e-5f);
                if (u == 0.0f and v == 0.0f) {
                    // Exact spot check: zero UVs bake to exactly the offset.
                    PPR_TEST_ASSERT(baked_mesh.m_vertices[i].m_texcoord[0] == 0.25f);
                    PPR_TEST_ASSERT(baked_mesh.m_vertices[i].m_texcoord[1] == 0.5f);
                    ++origin_count;
                }
            }
            PPR_TEST_ASSERT(origin_count > 0u);
        };

        // Alternate TEXCOORD_n sets: the per-slot selector flows convert into
        // pack. The single-channel verts keep the authored set-0 data verbatim
        // (Mango imports TEXCOORD_0 only), so set-1 fetch stays deferred and
        // the pack agreement rule still guards mixed sets.
        PPR_UNIT_TEST(second_texcoord_set_flows_to_pack) {
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "second_set_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_materials.size() == 1u);
            const mesh::MaterialAsset &material = scene->m_materials.front();
            PPR_TEST_ASSERT(material.m_base_color_map.enabled());
            PPR_TEST_ASSERT(material.m_base_color_map.m_texcoord == pP::mesh::UvSetId{1u});

            const Expected<mesh::SceneAsset> box =
                    mesh::importAndConvert(meshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(box.has_value());
            const mesh::StaticMeshAsset &box_mesh = box->m_meshes.front();
            const mesh::StaticMeshAsset &second_mesh = scene->m_meshes.front();
            PPR_TEST_ASSERT(second_mesh.m_vertices.size() == box_mesh.m_vertices.size());
            for (std::size_t i = 0u; i < second_mesh.m_vertices.size(); ++i) {
                PPR_TEST_ASSERT(second_mesh.m_vertices[i].m_texcoord[0] == box_mesh.m_vertices[i].m_texcoord[0]);
                PPR_TEST_ASSERT(second_mesh.m_vertices[i].m_texcoord[1] == box_mesh.m_vertices[i].m_texcoord[1]);
            }

            // Pack carries the selector to the GPU struct (CPU-only, no device).
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                material, {none_v, none_v, none_v, none_v});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT(*gpu->m_texcoord == 1u);
        };

        // Mixed sets with identity transforms convert cleanly: only authored
        // non-identity divergence is deferred, never silent agreement.
        PPR_UNIT_TEST(mixed_identity_sets_convert_cleanly) {
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "mixed_set_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_materials.size() == 1u);
            const mesh::MaterialAsset &material = scene->m_materials.front();
            PPR_TEST_ASSERT(material.m_base_color_map.enabled());
            PPR_TEST_ASSERT(material.m_base_color_map.m_texcoord == pP::mesh::UvSetId{0u});
            PPR_TEST_ASSERT(material.m_normal_map.enabled());
            PPR_TEST_ASSERT(material.m_normal_map.m_texcoord == pP::mesh::UvSetId{1u});
        };

        // Divergent authored transforms cannot share the single UV channel:
        // deferred with function_not_supported, never silently mis-sampled.
        PPR_UNIT_TEST(divergent_uv_transforms_rejected) {
            ExpectedFailureLogGuard expected_failure_log;
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "divergent_transform_box.gltf");
            PPR_TEST_ASSERT(not scene.has_value());
            PPR_TEST_ASSERT(scene.error() == std::errc::function_not_supported);
        };

        // A transform on a non-zero set has no channel to bake into: deferred.
        PPR_UNIT_TEST(transformed_nonzero_set_rejected) {
            ExpectedFailureLogGuard expected_failure_log;
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "transformed_set_box.gltf");
            PPR_TEST_ASSERT(not scene.has_value());
            PPR_TEST_ASSERT(scene.error() == std::errc::function_not_supported);
        };

        // Non-uniform node scales bake at convert (CPU, once): the scaled
        // instance gets a private mesh with positions moved by the world and
        // normals/tangents moved by its inverse-transpose, and its node world
        // resets to identity. Expectations derive from the unscaled twin's
        // converted values with a hand-rolled diagonal inverse-transpose, so
        // the convert path itself is what the test proves.
        PPR_UNIT_TEST(scaled_node_bakes_with_inverse_transpose) {
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "scaled_twin_tri.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_nodes.size() == 2u);
            PPR_TEST_ASSERT(scene->m_meshes.size() == 2u);
            PPR_TEST_ASSERT(scene->m_instances.size() == 2u);

            // Identity twin keeps the shared authored mesh verbatim.
            const mesh::SceneInstance &twin = scene->m_instances[0];
            PPR_TEST_ASSERT(twin.m_mesh == pP::mesh::MeshAssetId{0u});
            PPR_TEST_ASSERT(twin.m_node == pP::mesh::NodeId{0u});
            const mesh::StaticMeshAsset &twin_mesh = scene->m_meshes[0];
            PPR_TEST_ASSERT(twin_mesh.m_vertices.size() == 3u);
            PPR_TEST_ASSERT(twin_mesh.m_indices.size() == 3u);

            // Scaled instance owns the baked copy; its node world is identity.
            const mesh::SceneInstance &scaled = scene->m_instances[1];
            PPR_TEST_ASSERT(scaled.m_mesh == pP::mesh::MeshAssetId{1u});
            PPR_TEST_ASSERT(scaled.m_node == pP::mesh::NodeId{1u});
            const float4x4 &scaled_world = scene->m_nodes[1].m_world.toMatrix();
            PPR_TEST_ASSERT(scaled_world[0].x == 1.0f);
            PPR_TEST_ASSERT(scaled_world[1].y == 1.0f);
            PPR_TEST_ASSERT(scaled_world[2].z == 1.0f);
            PPR_TEST_ASSERT(scaled_world[3].x == 0.0f);
            PPR_TEST_ASSERT(scaled_world[3].y == 0.0f);
            PPR_TEST_ASSERT(scaled_world[3].z == 0.0f);

            const mesh::StaticMeshAsset &baked = scene->m_meshes[1];
            PPR_TEST_ASSERT(baked.m_vertices.size() == 3u);
            PPR_TEST_ASSERT(baked.m_flags == twin_mesh.m_flags);
            PPR_TEST_ASSERT(baked.m_primitives.size() == twin_mesh.m_primitives.size());
            PPR_TEST_ASSERT(baked.m_indices == twin_mesh.m_indices);

            // Node scale is exactly (2, 0.5, 1): positions bake exactly,
            // normals take the diagonal inverse-transpose (1/2, 2, 1),
            // tangents take the forward scale orthonormalized vs the normal.
            const float3 light = normalize(float3{-0.35f, 0.55f, 0.76f});
            for (std::size_t i = 0u; i < 3u; ++i) {
                const mesh::StaticMeshVertex &twin_vert = twin_mesh.m_vertices[i];
                const mesh::StaticMeshVertex &baked_vert = baked.m_vertices[i];
                PPR_TEST_ASSERT(baked_vert.m_position[0] == twin_vert.m_position[0] * 2.0f);
                PPR_TEST_ASSERT(baked_vert.m_position[1] == twin_vert.m_position[1] * 0.5f);
                PPR_TEST_ASSERT(baked_vert.m_position[2] == twin_vert.m_position[2]);

                const float twin_nx = twin_vert.m_normal[0];
                const float twin_ny = twin_vert.m_normal[1];
                const float twin_nz = twin_vert.m_normal[2];
                const float inv_nx = twin_nx / 2.0f;
                const float inv_ny = twin_ny * 2.0f;
                const float inv_nz = twin_nz;
                const float inv_len_sq = inv_nx * inv_nx + inv_ny * inv_ny + inv_nz * inv_nz;
                float want_nx = 0.0f;
                float want_ny = 0.0f;
                float want_nz = 0.0f;
                if (inv_len_sq < 1e-12f) {
                    // Degenerate authored normal: the inverse-transpose has no
                    // direction to preserve, so the bake saturates it to the
                    // deterministic +Y axis — the same fallback the tangent
                    // path's orthonormalize guard substitutes — never NaN.
                    PPR_TEST_ASSERT(baked_vert.m_normal[0] == 0.0f);
                    PPR_TEST_ASSERT(baked_vert.m_normal[1] == 1.0f);
                    PPR_TEST_ASSERT(baked_vert.m_normal[2] == 0.0f);
                } else {
                    const float inv_len = 1.0f / std::sqrt(inv_len_sq);
                    want_nx = inv_nx * inv_len;
                    want_ny = inv_ny * inv_len;
                    want_nz = inv_nz * inv_len;
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[0] - want_nx) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[1] - want_ny) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[2] - want_nz) < 1e-5f);

                    // Shading parity with the hand-transformed twin, and proof the
                    // correction is lighting-observable (naive mul differs).
                    const float shade_baked = baked_vert.m_normal[0] * light.x + baked_vert.m_normal[1] * light.y +
                                              baked_vert.m_normal[2] * light.z;
                    const float shade_hand = want_nx * light.x + want_ny * light.y + want_nz * light.z;
                    PPR_TEST_ASSERT(std::abs(shade_baked - shade_hand) < 1e-5f);
                    const float naive_len_sq =
                            twin_nx * twin_nx * 4.0f + twin_ny * twin_ny * 0.25f + twin_nz * twin_nz;
                    const float naive_inv_len = 1.0f / std::sqrt(naive_len_sq);
                    const float shade_naive = twin_nx * 2.0f * naive_inv_len * light.x +
                                              twin_ny * 0.5f * naive_inv_len * light.y + twin_nz * naive_inv_len * light.z;
                    PPR_TEST_ASSERT(std::abs(shade_baked - shade_naive) > 1e-3f);
                }

                // Tangents take the forward world scale, orthonormalized against
                // the baked normal (Gram-Schmidt); w is preserved.
                const float fwd_tx = twin_vert.m_tangent[0] * 2.0f;
                const float fwd_ty = twin_vert.m_tangent[1] * 0.5f;
                const float fwd_tz = twin_vert.m_tangent[2];
                const float fwd_len_sq = fwd_tx * fwd_tx + fwd_ty * fwd_ty + fwd_tz * fwd_tz;
                if (fwd_len_sq < 1e-12f) {
                    // Zero authored tangent orthonormalizes to a perpendicular
                    // of the baked normal, never to zero.
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[0] - 0.0f) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[1] + 0.4472136f) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[2] + 0.8944272f) < 1e-5f);
                } else if (inv_len_sq < 1e-12f) {
                    // Degenerate baked normal (v1): the orthonormalize guard
                    // substitutes +Y, killing the forward tangent, so the
                    // perpendicular fallback of (0, 1, 0) survives.
                    PPR_TEST_ASSERT(baked_vert.m_tangent[0] == 0.0f);
                    PPR_TEST_ASSERT(baked_vert.m_tangent[1] == 0.0f);
                    PPR_TEST_ASSERT(baked_vert.m_tangent[2] == -1.0f);
                } else {
                    const float ortho_d = fwd_tx * want_nx + fwd_ty * want_ny + fwd_tz * want_nz;
                    const float ortho_x = fwd_tx - ortho_d * want_nx;
                    const float ortho_y = fwd_ty - ortho_d * want_ny;
                    const float ortho_z = fwd_tz - ortho_d * want_nz;
                    const float ortho_len = 1.0f / std::sqrt(ortho_x * ortho_x + ortho_y * ortho_y + ortho_z * ortho_z);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[0] - ortho_x * ortho_len) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[1] - ortho_y * ortho_len) < 1e-5f);
                    PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[2] - ortho_z * ortho_len) < 1e-5f);
                    if (i == 0u) {
                        // Cross-check: hand-rolled forward-M + Gram-Schmidt.
                        PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[0] - 0.9759006f) < 1e-5f);
                        PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[1] + 0.1951801f) < 1e-5f);
                        PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[2] - 0.0975901f) < 1e-5f);
                    }
                }
                PPR_TEST_ASSERT(baked_vert.m_tangent[3] == twin_vert.m_tangent[3]);
            }

            // Baked bounds track baked positions: (2,0,0), (0,0.5,0), (0,0,-1).
            PPR_TEST_ASSERT(baked.m_bounds.corner[0].x == 0.0f);
            PPR_TEST_ASSERT(baked.m_bounds.corner[0].y == 0.0f);
            PPR_TEST_ASSERT(baked.m_bounds.corner[0].z == -1.0f);
            PPR_TEST_ASSERT(baked.m_bounds.corner[1].x == 2.0f);
            PPR_TEST_ASSERT(baked.m_bounds.corner[1].y == 0.5f);
            PPR_TEST_ASSERT(baked.m_bounds.corner[1].z == 0.0f);
        };

        // Mirrored (negative-determinant) worlds flip tangent-w so the
        // bitangent handedness survives the reflection.
        PPR_UNIT_TEST(mirrored_node_flips_tangent_w) {
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "mirror_twin_tri.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_meshes.size() == 2u);
            PPR_TEST_ASSERT(scene->m_instances.size() == 2u);
            const mesh::StaticMeshAsset &twin_mesh = scene->m_meshes.front();
            const mesh::StaticMeshAsset &baked = scene->m_meshes.back();
            PPR_TEST_ASSERT(baked.m_vertices.size() == 3u);

            // Scale is (-2, 0.5, 1): positions mirror, normals take the
            // diagonal inverse-transpose (-1/2, 2, 1), tangents take the
            // forward scale orthonormalized vs the normal, w negates.
            const mesh::StaticMeshVertex &twin_vert = twin_mesh.m_vertices[0];
            const mesh::StaticMeshVertex &baked_vert = baked.m_vertices[0];
            PPR_TEST_ASSERT(baked_vert.m_position[0] == twin_vert.m_position[0] * -2.0f);
            PPR_TEST_ASSERT(baked_vert.m_position[1] == twin_vert.m_position[1] * 0.5f);
            PPR_TEST_ASSERT(baked_vert.m_position[2] == twin_vert.m_position[2]);

            const float twin_nx = twin_vert.m_normal[0];
            const float twin_ny = twin_vert.m_normal[1];
            const float twin_nz = twin_vert.m_normal[2];
            const float inv_nx = twin_nx / -2.0f;
            const float inv_ny = twin_ny * 2.0f;
            const float inv_nz = twin_nz;
            const float inv_len = 1.0f / std::sqrt(inv_nx * inv_nx + inv_ny * inv_ny + inv_nz * inv_nz);
            const float bent_x = inv_nx * inv_len;
            const float bent_y = inv_ny * inv_len;
            const float bent_z = inv_nz * inv_len;
            PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[0] - bent_x) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[1] - bent_y) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_normal[2] - bent_z) < 1e-5f);

            const float fwd_tx = twin_vert.m_tangent[0] * -2.0f;
            const float fwd_ty = twin_vert.m_tangent[1] * 0.5f;
            const float fwd_tz = twin_vert.m_tangent[2];
            const float ortho_d = fwd_tx * bent_x + fwd_ty * bent_y + fwd_tz * bent_z;
            const float ortho_x = fwd_tx - ortho_d * bent_x;
            const float ortho_y = fwd_ty - ortho_d * bent_y;
            const float ortho_z = fwd_tz - ortho_d * bent_z;
            const float ortho_len = 1.0f / std::sqrt(ortho_x * ortho_x + ortho_y * ortho_y + ortho_z * ortho_z);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[0] - ortho_x * ortho_len) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[1] - ortho_y * ortho_len) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[2] - ortho_z * ortho_len) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[0] + 0.9759006f) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[1] + 0.1951801f) < 1e-5f);
            PPR_TEST_ASSERT(std::abs(baked_vert.m_tangent[2] - 0.0975901f) < 1e-5f);
            PPR_TEST_ASSERT(baked_vert.m_tangent[3] == -twin_vert.m_tangent[3]);
            PPR_TEST_ASSERT(baked_vert.m_tangent[3] == 1.0f);
        };

        // Singular worlds (collapsed basis) have no inverse-transpose: the
        // scene fails closed instead of baking NaN normals.
        PPR_UNIT_TEST(degenerate_node_world_rejected) {
            ExpectedFailureLogGuard expected_failure_log;
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(meshDir(), "degenerate_node_box.gltf");
            PPR_TEST_ASSERT(not scene.has_value());
            PPR_TEST_ASSERT(scene.error() == std::errc::invalid_argument);
        };

        // The per-instance bake honors the production mesh cap.
        PPR_UNIT_TEST(scaled_bake_respects_mesh_cap) {
            ExpectedFailureLogGuard expected_failure_log;
            mesh::MeshLimits tight = mesh::kDefaultMeshLimits;
            tight.m_max_meshes = 1u;
            const Expected<mesh::SceneAsset> scene =
                    pP::mesh::importAndConvert(meshDir(), "scaled_twin_tri.gltf", tight);
            PPR_TEST_ASSERT(not scene.has_value());
            PPR_TEST_ASSERT(scene.error() == std::errc::invalid_argument);
        };
    } // namespace UvNormal
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest mesh_uv = UnitTest::Named("uv") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::UvNormal::uv_transform_bakes_into_texcoords,
            detail::UvNormal::second_texcoord_set_flows_to_pack,
            detail::UvNormal::mixed_identity_sets_convert_cleanly,
            detail::UvNormal::divergent_uv_transforms_rejected,
            detail::UvNormal::transformed_nonzero_set_rejected,
            detail::UvNormal::scaled_node_bakes_with_inverse_transpose,
            detail::UvNormal::mirrored_node_flips_tangent_w,
            detail::UvNormal::degenerate_node_world_rejected,
            detail::UvNormal::scaled_bake_respects_mesh_cap,
        });
    };

    const UnitTest &meshUvTests() noexcept {
        return mesh_uv;
    }
} // namespace pP::tests
