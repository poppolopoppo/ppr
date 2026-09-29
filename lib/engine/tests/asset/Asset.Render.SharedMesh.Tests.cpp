module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import std;

// Shared-mesh join proofs: two instances share one two-prim mesh with distinct
// nodes (the second carries a material override) and join through the
// mesh-major table to the right bags/materials/worlds. Runs under the shared
// render scope (the asset/render parent in Asset.Tests.cpp owns the single
// SharedGpu acquire).
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace SharedMesh {
        [[nodiscard]] mesh::StaticMeshVertex sharedQuadVert_(const float x, const float y) {
            mesh::StaticMeshVertex vert{};
            vert.m_position[0] = x;
            vert.m_position[1] = y;
            vert.m_position[2] = 0.0f;
            vert.m_normal[0] = 0.0f;
            vert.m_normal[1] = 0.0f;
            vert.m_normal[2] = 1.0f;
            vert.m_texcoord[0] = x + 0.5f;
            vert.m_texcoord[1] = y + 0.5f;
            vert.m_tangent[0] = 1.0f;
            vert.m_tangent[3] = -1.0f;
            vert.m_color[0] = 1.0f;
            vert.m_color[1] = 1.0f;
            vert.m_color[2] = 1.0f;
            vert.m_color[3] = 1.0f;
            return vert;
        }

        [[nodiscard]] Expected<image::ImageAsset> sharedSolidImage_(const u8 r, const u8 g, const u8 b) {
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(16u);
            if (const std::error_code err = storage.materialize()) [[unlikely]] {
                return std::unexpected{err};
            }
            Expected<mem::MutableBufferView> destination = storage.getMutableData();
            if (not destination.has_value()) [[unlikely]] {
                return std::unexpected{destination.error()};
            }

            for (std::size_t i = 0u; i < 16u; i += 4u) {
                (*destination)[i] = static_cast<std::byte>(r);
                (*destination)[i + 1u] = static_cast<std::byte>(g);
                (*destination)[i + 2u] = static_cast<std::byte>(b);
                (*destination)[i + 3u] = std::byte{255};
            }

            mem::SharedBuffer frozen{};
            if (const std::error_code err = storage.moveToShared(&frozen)) [[unlikely]] {
                return std::unexpected{err};
            }

            image::ImageAsset picture{};
            picture.m_width = 2u;
            picture.m_height = 2u;
            picture.m_storage = frozen;
            picture.m_subresources.push_back(image::ImageSubresource{
                .m_view = frozen.subspan(0u, 16u),
                .m_row_pitch = 8u,
                .m_slice_pitch = 16u,
            });
            return picture;
        }

        [[nodiscard]] math::Transform sharedPlace_(const float x) {
            math::Transform place = math::Transform::identity();
            place.m_translate.x = x;
            return place;
        }

        struct SharedScene {
            mesh::SceneAsset m_scene;
            Array<image::ImageAsset> m_images;
        };

        // One two-prim quad mesh, two materials, two nodes at distinct
        // translations, two instances sharing the mesh — the second with a
        // material override to material 1.
        [[nodiscard]] Expected<SharedScene> twoInstanceSharedMesh_() {
            Expected<image::ImageAsset> image = sharedSolidImage_(40u, 120u, 200u);
            if (not image.has_value()) [[unlikely]] {
                return std::unexpected{image.error()};
            }

            SharedScene out{};
            mesh::StaticMeshAsset mesh_asset{};
            mesh_asset.m_vertices.push_back(sharedQuadVert_(-0.5f, -0.5f));
            mesh_asset.m_vertices.push_back(sharedQuadVert_(0.5f, -0.5f));
            mesh_asset.m_vertices.push_back(sharedQuadVert_(0.5f, 0.5f));
            mesh_asset.m_vertices.push_back(sharedQuadVert_(-0.5f, 0.5f));
            for (const u32 index: {0u, 1u, 2u, 0u, 2u, 3u}) {
                mesh_asset.m_indices.push_back(index);
            }
            mesh_asset.m_primitives.push_back(mesh::MeshPrimitiveRange{
                .m_start = 0u, .m_count = 3u, .m_base = 0, .m_material = mesh::MaterialAssetId{0u}
            });
            mesh_asset.m_primitives.push_back(mesh::MeshPrimitiveRange{
                .m_start = 3u, .m_count = 3u, .m_base = 0, .m_material = mesh::MaterialAssetId{1u}
            });
            mesh_asset.m_flags = mesh::EMeshAttribute::position | mesh::EMeshAttribute::normal |
                                 mesh::EMeshAttribute::texcoord | mesh::EMeshAttribute::tangent | mesh::EMeshAttribute::color;

            mesh::MaterialAsset first_material{};
            first_material.m_base_color = float4{1.0f, 0.0f, 0.0f, 1.0f};
            first_material.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            first_material.m_base_color_map.m_texcoord = mesh::UvSetId{0u};
            mesh::MaterialAsset second_material{};
            second_material.m_base_color = float4{0.0f, 1.0f, 0.0f, 1.0f};
            second_material.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            second_material.m_base_color_map.m_texcoord = mesh::UvSetId{0u};

            const math::Transform place_a = sharedPlace_(0.0f);
            const math::Transform place_b = sharedPlace_(2.0f);

            out.m_scene.m_meshes.push_back(std::move(mesh_asset));
            out.m_scene.m_materials.push_back(first_material);
            out.m_scene.m_materials.push_back(second_material);
            out.m_scene.m_images.push_back(mesh::ImageRef{});
            out.m_scene.m_nodes.push_back(mesh::SceneNodeAsset{
                .m_local = place_a,
                .m_world = place_a,
                .m_parent = none_v,
            });
            out.m_scene.m_nodes.push_back(mesh::SceneNodeAsset{
                .m_local = place_b,
                .m_world = place_b,
                .m_parent = none_v,
            });
            out.m_scene.m_instances.push_back(mesh::SceneInstance{
                .m_mesh = mesh::MeshAssetId{0u},
                .m_node = mesh::NodeId{0u},
            });
            out.m_scene.m_instances.push_back(mesh::SceneInstance{
                .m_mesh = mesh::MeshAssetId{0u},
                .m_node = mesh::NodeId{1u},
                .m_material_override = mesh::MaterialAssetId{1u},
            });
            out.m_images.push_back(*image);
            return out;
        }

        // Both instances share the single mesh: the join table resolves each
        // (instance, slot) to the right bag, the prim material without an
        // override and the override material with one, and both node worlds
        // submit cleanly.
        PPR_UNIT_TEST (shared_mesh_instances_join_distinct_bags_materials_worlds) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<SharedScene> built = twoInstanceSharedMesh_();
            PPR_TEST_ASSERT(built.has_value());
            const mesh::SceneAsset &scene = built->m_scene;
            PPR_TEST_ASSERT(scene.m_instances.size() == 2u);

            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(scene, built->m_images);
            PPR_TEST_ASSERT(uploaded.has_value());

            PPR_TEST_ASSERT(uploaded->m_mesh_prim_base.size() == scene.m_meshes.size());
            PPR_TEST_ASSERT(uploaded->m_mesh_prim_base.size() == 1u);
            PPR_TEST_ASSERT(uploaded->m_mesh_prim_base[0] == 0u);
            PPR_TEST_ASSERT(uploaded->m_primitives.size() == 2u);
            PPR_TEST_ASSERT(uploaded->m_materials.size() == 2u);

            for (std::size_t slot = 0u; slot < 2u; ++slot) {
                const Expected<TrianglePass::JoinedInstancePrim> joined =
                        TrianglePass::joinInstancePrim(scene, *uploaded, scene.m_instances[0], slot);
                PPR_TEST_ASSERT(joined.has_value());
                PPR_TEST_ASSERT(joined->m_bag == uploaded->m_primitives[slot].m_bag);
                PPR_TEST_ASSERT(joined->m_material == uploaded->m_materials[slot]);
            }

            for (std::size_t slot = 0u; slot < 2u; ++slot) {
                const Expected<TrianglePass::JoinedInstancePrim> joined =
                        TrianglePass::joinInstancePrim(scene, *uploaded, scene.m_instances[1], slot);
                PPR_TEST_ASSERT(joined.has_value());
                PPR_TEST_ASSERT(joined->m_bag == uploaded->m_primitives[slot].m_bag);
                PPR_TEST_ASSERT(joined->m_material == uploaded->m_materials[1]);
            }

            PPR_TEST_ASSERT(scene.m_nodes[0].m_world.m_translate.x != scene.m_nodes[1].m_world.m_translate.x);
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const float4x4 world = scene.m_nodes[instance.m_node].m_world.toMatrix();
                const mesh::StaticMeshAsset &mesh_asset = scene.m_meshes[instance.m_mesh];
                for (std::size_t slot = 0u; slot < mesh_asset.m_primitives.size(); ++slot) {
                    const Expected<TrianglePass::JoinedInstancePrim> joined =
                            TrianglePass::joinInstancePrim(scene, *uploaded, instance, slot);
                    PPR_TEST_ASSERT(joined.has_value());
                    PPR_TEST_ASSERT(not pass.submitInstance(joined->m_bag, joined->m_material, world));
                }
            }
            pass.clearInstances();

            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
        };

        // Fail-closed join: unknown mesh, out-of-range slot, and unknown
        // material override all report invalid_argument.
        PPR_UNIT_TEST (shared_mesh_join_mismatch_fails_closed) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<SharedScene> built = twoInstanceSharedMesh_();
            PPR_TEST_ASSERT(built.has_value());
            const mesh::SceneAsset &scene = built->m_scene;

            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(scene, built->m_images);
            PPR_TEST_ASSERT(uploaded.has_value());

            const std::error_code kInvalid = std::make_error_code(std::errc::invalid_argument);

            mesh::SceneInstance bad_mesh = scene.m_instances[0];
            bad_mesh.m_mesh = mesh::MeshAssetId{7u};
            const Expected<TrianglePass::JoinedInstancePrim> bad_mesh_join =
                    TrianglePass::joinInstancePrim(scene, *uploaded, bad_mesh, 0u);
            PPR_TEST_ASSERT(not bad_mesh_join.has_value());
            PPR_TEST_ASSERT(bad_mesh_join.error() == kInvalid);

            const Expected<TrianglePass::JoinedInstancePrim> bad_slot_join =
                    TrianglePass::joinInstancePrim(scene, *uploaded, scene.m_instances[0], 2u);
            PPR_TEST_ASSERT(not bad_slot_join.has_value());
            PPR_TEST_ASSERT(bad_slot_join.error() == kInvalid);

            mesh::SceneInstance bad_override = scene.m_instances[0];
            bad_override.m_material_override = mesh::MaterialAssetId{9u};
            const Expected<TrianglePass::JoinedInstancePrim> bad_override_join =
                    TrianglePass::joinInstancePrim(scene, *uploaded, bad_override, 0u);
            PPR_TEST_ASSERT(not bad_override_join.has_value());
            PPR_TEST_ASSERT(bad_override_join.error() == kInvalid);

            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
        };
    } // namespace SharedMesh
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest sharedmesh = UnitTest::Named("sharedmesh") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::SharedMesh::shared_mesh_instances_join_distinct_bags_materials_worlds,
            detail::SharedMesh::shared_mesh_join_mismatch_fails_closed,
        });
    };

    const UnitTest &renderSharedMeshTests() noexcept {
        return sharedmesh;
    }
} // namespace pP::tests
