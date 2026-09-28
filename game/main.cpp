#include "pP/Macros.h"

import engine.core;
import engine.image;
import engine.math;
import engine.mesh;
import engine.rhi;
import engine.app;
import std;

import imgui_internal;

namespace demo {
    using namespace pP;
    PPR_DEFINE_LOG_CATEGORY(Demo, info, none);

    class TurboLarbin : public ApplicationEditor {
        struct AssetSpec final {
            const char *m_pack;
            const char *m_format;
            const char *m_file;
        };

        enum class ESceneZone : u8 {
            shell,
            circulation,
            services,
            habitat,
            nature,
        };

        struct Placement final {
            ESceneZone m_zone;
            std::size_t m_asset;
            float m_position[3];
            float m_scale;
            u8 m_quarter_turns;
        };

        static constexpr AssetSpec kAssetSpecs[]{
            {"Space Station Kit", "GLB format", "floor.glb"},
            {"Space Station Kit", "GLB format", "wall.glb"},
            {"Space Station Kit", "GLB format", "wall-window.glb"},
            {"Space Station Kit", "GLB format", "wall-door-center.glb"},
            {"Space Station Kit", "GLB format", "door-single.glb"},
            {"Space Station Kit", "GLB format", "stairs.glb"},
            {"Space Station Kit", "GLB format", "pipe.glb"},
            {"Space Station Kit", "GLB format", "bed-single.glb"},
            {"Space Station Kit", "GLB format", "container-tall.glb"},
            {"Space Station Kit", "GLB format", "table.glb"},
            {"Factory Kit", "GLB format", "catwalk-straight.glb"},
            {"Factory Kit", "GLB format", "machine.glb"},
            {"Factory Kit", "GLB format", "conveyor.glb"},
            {"Factory Kit", "GLB format", "pipe-large-valve.glb"},
            {"Prototype Kit", "GLB format", "ladder.glb"},
            {"Food Kit", "GLB format", "bread.glb"},
        };
        static_assert(std::size(kAssetSpecs) == 16u);

        // Kepler-9 Cutaway Waystation: a 20-unit shell establishes the
        // footprint while the entrance, stairs, ladder, catwalk, and raised
        // habitat remain concentrated around the inherited central camera.
        static constexpr std::array<Placement, 53u> kHabitatLayout{
            {
                {ESceneZone::shell, 1u, {-9.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 2u, {-7.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {-5.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 2u, {-3.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {-1.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 2u, {1.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {3.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 2u, {5.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {7.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {9.0f, 0.0f, 15.0f}, 2.0f, 0u},
                {ESceneZone::shell, 1u, {-9.0f, 0.0f, 8.0f}, 2.0f, 1u},
                {ESceneZone::shell, 1u, {-9.0f, 0.0f, 11.5f}, 2.0f, 1u},
                {ESceneZone::shell, 2u, {-9.0f, 0.0f, 15.0f}, 2.0f, 1u},
                {ESceneZone::shell, 1u, {9.0f, 0.0f, 8.0f}, 2.0f, 1u},
                {ESceneZone::shell, 1u, {9.0f, 0.0f, 11.5f}, 2.0f, 1u},
                {ESceneZone::shell, 2u, {9.0f, 0.0f, 15.0f}, 2.0f, 1u},
                {ESceneZone::shell, 3u, {0.0f, 0.0f, 6.0f}, 1.5f, 0u},
                {ESceneZone::shell, 4u, {0.0f, 0.0f, 6.2f}, 1.5f, 0u},
                {ESceneZone::shell, 0u, {-8.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {-6.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {-4.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {-2.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {0.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {2.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {4.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {6.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::shell, 0u, {8.0f, -0.3f, 8.0f}, 2.0f, 0u},
                {ESceneZone::circulation, 5u, {-1.5f, 0.0f, 6.5f}, 1.0f, 0u},
                {ESceneZone::circulation, 5u, {-0.5f, 0.3f, 6.5f}, 1.0f, 0u},
                {ESceneZone::circulation, 5u, {0.5f, 0.6f, 6.5f}, 1.0f, 0u},
                {ESceneZone::circulation, 5u, {1.5f, 0.9f, 6.5f}, 1.0f, 0u},
                {ESceneZone::circulation, 14u, {2.2f, 0.9f, 6.8f}, 1.8f, 0u},
                {ESceneZone::circulation, 10u, {-1.0f, 1.8f, 6.8f}, 1.5f, 1u},
                {ESceneZone::circulation, 10u, {1.0f, 1.8f, 6.8f}, 1.5f, 1u},
                {ESceneZone::circulation, 10u, {3.0f, 1.8f, 6.8f}, 1.5f, 1u},
                {ESceneZone::circulation, 10u, {5.0f, 1.8f, 6.8f}, 1.5f, 1u},
                {ESceneZone::circulation, 0u, {0.0f, 1.8f, 7.0f}, 2.0f, 0u},
                {ESceneZone::circulation, 0u, {2.0f, 1.8f, 7.0f}, 2.0f, 0u},
                {ESceneZone::circulation, 0u, {4.0f, 1.8f, 7.0f}, 2.0f, 0u},
                {ESceneZone::services, 11u, {-2.5f, 0.0f, 6.0f}, 1.2f, 0u},
                {ESceneZone::services, 12u, {0.0f, 0.0f, 6.2f}, 1.5f, 0u},
                {ESceneZone::services, 12u, {-2.0f, 0.0f, 9.0f}, 2.0f, 0u},
                {ESceneZone::services, 12u, {2.0f, 0.0f, 9.0f}, 2.0f, 0u},
                {ESceneZone::services, 13u, {-3.5f, 0.9f, 9.0f}, 1.2f, 0u},
                {ESceneZone::services, 8u, {-6.0f, 0.0f, 8.0f}, 1.5f, 0u},
                {ESceneZone::services, 8u, {6.0f, 0.0f, 8.0f}, 1.5f, 0u},
                {ESceneZone::services, 6u, {-5.0f, 2.3f, 14.6f}, 1.5f, 0u},
                {ESceneZone::services, 6u, {-2.5f, 2.3f, 14.6f}, 1.5f, 0u},
                {ESceneZone::services, 6u, {0.0f, 2.3f, 14.6f}, 1.5f, 0u},
                {ESceneZone::services, 6u, {2.5f, 2.3f, 14.6f}, 1.5f, 0u},
                {ESceneZone::habitat, 7u, {0.5f, 2.1f, 7.3f}, 1.2f, 0u},
                {ESceneZone::habitat, 9u, {-0.2f, 2.1f, 7.3f}, 1.2f, 0u},
                {ESceneZone::habitat, 15u, {-0.2f, 2.58f, 7.3f}, 1.0f, 0u},
            }
        };

        [[nodiscard]] static float4x4 placementTransform(const Placement &placement) noexcept {
            const float scale_x = placement.m_scale;
            const float scale_y = placement.m_scale;
            const float scale_z = placement.m_scale;
            switch (placement.m_quarter_turns) {
                case 0u:
                    return float4x4{
                        float4{scale_x, 0.0f, 0.0f, 0.0f},
                        float4{0.0f, scale_y, 0.0f, 0.0f},
                        float4{0.0f, 0.0f, scale_z, 0.0f},
                        float4{placement.m_position[0], placement.m_position[1], placement.m_position[2], 1.0f},
                    };
                case 1u:
                    return float4x4{
                        float4{0.0f, 0.0f, scale_x, 0.0f},
                        float4{0.0f, scale_y, 0.0f, 0.0f},
                        float4{-scale_z, 0.0f, 0.0f, 0.0f},
                        float4{placement.m_position[0], placement.m_position[1], placement.m_position[2], 1.0f},
                    };
                case 2u:
                    return float4x4{
                        float4{-scale_x, 0.0f, 0.0f, 0.0f},
                        float4{0.0f, scale_y, 0.0f, 0.0f},
                        float4{0.0f, 0.0f, -scale_z, 0.0f},
                        float4{placement.m_position[0], placement.m_position[1], placement.m_position[2], 1.0f},
                    };
                case 3u:
                    return float4x4{
                        float4{0.0f, 0.0f, -scale_x, 0.0f},
                        float4{0.0f, scale_y, 0.0f, 0.0f},
                        float4{scale_z, 0.0f, 0.0f, 0.0f},
                        float4{placement.m_position[0], placement.m_position[1], placement.m_position[2], 1.0f},
                    };
                default:
                    return float4x4::identity();
            }
        }

    public:
        using super_t = ApplicationEditor;
        using super_t::super_t;

    protected:
        std::error_code initialize() override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::initialize());

            const std::filesystem::path asset_root = getContentDir().path() / "meshes" / "kenney_colony";
            PPR_LOG(Demo, info, "colony fixture loading", {
                {"root", asset_root.string()},
                {"candidates", static_cast<u32>(std::size(kAssetSpecs))},
                });

            for (const Placement &placement: kHabitatLayout) {
                if (placement.m_asset >= std::size(kAssetSpecs) or placement.m_scale <= 0.0f or
                    placement.m_quarter_turns > 3u) [[unlikely]] {
                    return make_error_code(std::errc::invalid_argument);
                }
            }

            struct SourceAsset final {
                mesh::SceneAsset m_scene{};
                Array<mem::SharedBuffer> m_bytes{};
                u32 m_mesh_base = 0u;
                u32 m_mat_base = 0u;
                u32 m_img_base = 0u;
                u32 m_node_base = 0u;
                bool m_included = false;
            };
            std::array<SourceAsset, std::size(kAssetSpecs)> sources{};

            for (const auto &[spec_index, spec]: std::ranges::views::enumerate(kAssetSpecs)) {
                const std::filesystem::path model_dir = asset_root / spec.m_pack / "Models" / spec.m_format / "";
                Expected<mesh::SceneAsset> scene = mesh::importAndConvert(model_dir, spec.m_file);
                if (not scene.has_value()) [[unlikely]] {
                    PPR_LOG(Demo, warning, "colony asset import failed", {
                        {"file", std::string{spec.m_file}},
                        {"pack", std::string{spec.m_pack}},
                        {"error", scene.error().message()},
                        });
                    continue;
                }

                for (std::size_t mesh_index = 0u; mesh_index < scene->m_meshes.size(); ++mesh_index) {
                    const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes[mesh_index];
                    const float3 bounds_center = mesh_asset.m_bounds.center();
                    const float3 bounds_size = mesh_asset.m_bounds.size();
                    PPR_LOG(Demo, info, "colony source bounds", {
                        {"file", std::string{spec.m_file}},
                        {"mesh", static_cast<u32>(mesh_index)},
                        {"center_x", bounds_center.x},
                        {"center_y", bounds_center.y},
                        {"center_z", bounds_center.z},
                        {"size_x", bounds_size.x},
                        {"size_y", bounds_size.y},
                        {"size_z", bounds_size.z},
                        });
                }

                Array<mem::SharedBuffer> source_bytes{};
                source_bytes.reserve(scene->m_images.size());
                bool decoded_all = true;
                for (const mesh::ImageRef &ref: scene->m_images) {
                    mem::SharedBuffer bytes{};
                    if (ref.m_is_file) {
                        Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(model_dir / ref.m_rel_path);
                        if (not mapped.has_value()) [[unlikely]] {
                            PPR_LOG(Demo, warning, "colony texture mapping failed", {
                                {"file", std::string{spec.m_file}},
                                {"texture", ref.m_rel_path},
                                {"error", mapped.error().message()},
                                });
                            decoded_all = false;
                            break;
                        }
                        bytes = *mapped;
                    } else {
                        bytes = ref.m_bytes;
                    }

                    if (not bytes.isValid()) [[unlikely]] {
                        PPR_LOG(Demo, warning, "colony texture bytes are invalid", {
                            {"file", std::string{spec.m_file}},
                            {"texture", ref.m_name},
                            });
                        decoded_all = false;
                        break;
                    }

                    Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                        bytes.getBufferData(), ref.m_ext,
                        image::ImageDecodeDesc{}, image::EImageUsage::color);

                    if (not decoded.has_value()) [[unlikely]] {
                        PPR_LOG(Demo, warning, "colony asset decode failed", {
                            {"file", std::string{spec.m_file}},
                            {"texture", ref.m_name},
                            {"error", decoded.error().message()},
                            });
                        decoded_all = false;
                        break;
                    }

                    source_bytes.push_back(bytes);
                }

                if (not decoded_all) {
                    continue;
                }

                PPR_LOG(Demo, info, "colony asset ready", {
                    {"file", std::string{spec.m_file}},
                    {"images", source_bytes.size()},
                    {"meshes", scene->m_meshes.size()},
                    });

                SourceAsset &source = sources[spec_index];
                source.m_scene = std::move(*scene);
                source.m_bytes = std::move(source_bytes);
                source.m_included = true;
            }

            mesh::SceneAsset merged{};

            std::size_t total_meshes = 0u;
            std::size_t total_materials = 0u;
            std::size_t total_images = 0u;
            std::size_t total_nodes = 0u;
            std::size_t total_expanded = 0u;
            for (const SourceAsset &source: sources) {
                if (not source.m_included) {
                    continue;
                }
                total_meshes += source.m_scene.m_meshes.size();
                total_materials += source.m_scene.m_materials.size();
                total_images += source.m_scene.m_images.size();
                total_nodes += source.m_scene.m_nodes.size();
            }
            for (const Placement &placement: kHabitatLayout) {
                const SourceAsset &source = sources[placement.m_asset];
                if (not source.m_included) {
                    continue;
                }
                total_expanded += source.m_scene.m_instances.size();
            }

            merged.m_meshes.reserve(total_meshes);
            merged.m_materials.reserve(total_materials);
            merged.m_images.reserve(total_images);
            merged.m_nodes.reserve(total_nodes + total_expanded);
            merged.m_instances.reserve(total_expanded);

            for (SourceAsset &source: sources) {
                if (not source.m_included) {
                    continue;
                }

                source.m_mesh_base = safe_narrowing(merged.m_meshes.size());
                source.m_mat_base = safe_narrowing(merged.m_materials.size());
                source.m_img_base = safe_narrowing(merged.m_images.size());
                source.m_node_base = safe_narrowing(merged.m_nodes.size());

                const u32 img_base = source.m_img_base;
                const u32 node_base = source.m_node_base;
                for (const mesh::StaticMeshAsset &src_mesh: source.m_scene.m_meshes) {
                    merged.m_meshes.push_back(src_mesh);
                    mesh::StaticMeshAsset &dst_mesh = merged.m_meshes[merged.m_meshes.size() - 1u];
                    for (mesh::MeshPrimitiveRange &primitive: dst_mesh.m_primitives) {
                        primitive.m_material =
                                mesh::MaterialAssetId{static_cast<u32>(*primitive.m_material + source.m_mat_base)};
                    }
                }
                for (const mesh::MaterialAsset &src_material: source.m_scene.m_materials) {
                    merged.m_materials.push_back(src_material);

                    mesh::MaterialAsset &dst_material = merged.m_materials[merged.m_materials.size() - 1u];
                    mesh::MaterialImageSlot *const slots[] = {
                        &dst_material.m_base_color_map,
                        &dst_material.m_metallic_map,
                        &dst_material.m_roughness_map,
                        &dst_material.m_normal_map,
                        &dst_material.m_occlusion_map,
                        &dst_material.m_emissive_map,
                    };

                    for (mesh::MaterialImageSlot *const slot: slots) {
                        if (slot->enabled()) {
                            slot->m_image = mesh::ImageAssetId{static_cast<u32>(*slot->m_image + img_base)};
                        }
                    }
                }

                for (std::size_t img_index = 0u; img_index < source.m_scene.m_images.size(); ++img_index) {
                    merged.m_images.push_back(source.m_scene.m_images[img_index]);
                    mesh::ImageRef &dst_ref = merged.m_images[merged.m_images.size() - 1u];
                    dst_ref.m_is_file = false;
                    dst_ref.m_bytes = source.m_bytes[img_index];
                }

                for (const mesh::SceneNodeAsset &src_node: source.m_scene.m_nodes) {
                    merged.m_nodes.push_back(src_node);
                    if (src_node.m_parent != none_v) {
                        mesh::SceneNodeAsset &dst_node = merged.m_nodes[merged.m_nodes.size() - 1u];
                        dst_node.m_parent = mesh::NodeId{static_cast<u32>(*src_node.m_parent + node_base)};
                    }
                }
            }

            u32 active_placements = 0u;
            std::array<u32, 5u> zone_counts{};
            std::size_t total_draws = 0u;
            for (const Placement &placement: kHabitatLayout) {
                SourceAsset &source = sources[placement.m_asset];
                if (not source.m_included) {
                    continue;
                }

                ++active_placements;
                ++zone_counts[enumOrd(placement.m_zone)];

                const float4x4 placement_matrix = placementTransform(placement);
                for (const mesh::SceneInstance &src_instance: source.m_scene.m_instances) {
                    if (src_instance.m_mesh >= source.m_scene.m_meshes.size() or
                        src_instance.m_node >= source.m_scene.m_nodes.size()) [[unlikely]] {
                        return make_error_code(std::errc::invalid_argument);
                    }

                    const mesh::SceneNodeAsset &src_node = source.m_scene.m_nodes[src_instance.m_node];
                    const float4x4 combined = placement_matrix * src_node.m_world.toMatrix();
                    const math::Transform baked = math::Transform::fromMatrix(combined);

                    const mesh::StaticMeshAsset &src_mesh = source.m_scene.m_meshes[src_instance.m_mesh];
                    const u32 shared_mesh = source.m_mesh_base + static_cast<u32>(*src_instance.m_mesh);
                    total_draws += src_mesh.m_primitives.size();

                    const u32 fresh_node = static_cast<u32>(merged.m_nodes.size());
                    merged.m_nodes.push_back(mesh::SceneNodeAsset{
                        .m_local = baked,
                        .m_world = baked,
                        .m_parent = none_v,
                    });

                    mesh::SceneInstance fresh_instance{
                        .m_mesh = mesh::MeshAssetId{shared_mesh},
                        .m_node = mesh::NodeId{fresh_node},
                    };
                    if (src_instance.m_material_override != none_v) {
                        fresh_instance.m_material_override = mesh::MaterialAssetId{
                            static_cast<u32>(*src_instance.m_material_override + source.m_mat_base)
                        };
                    }
                    merged.m_instances.push_back(fresh_instance);
                }
            }

            PPR_LOG(Demo, info, "colony fixture loaded", {
                {"placements", active_placements},
                {"candidates", static_cast<u32>(std::size(kAssetSpecs))},
                {"meshes", static_cast<u32>(merged.m_meshes.size())},
                {"instances", static_cast<u32>(merged.m_instances.size())},
                });

            if (merged.m_instances.empty()) [[unlikely]] {
                return make_error_code(std::errc::no_such_file_or_directory);
            }

            const std::error_code scene_err = super_t::setLoadedScene(asset_root, std::move(merged));
            if (scene_err) [[unlikely]] {
                PPR_LOG(Demo, warning, "colony scene upload failed", {
                    {"error", scene_err.message()},
                    });
                return scene_err;
            }

            PPR_LOG(Demo, info, "colony fixture submitted", {
                {"placements", kHabitatLayout.size()},
                {"active_placements", active_placements},
                {"draws", total_draws},
                {"shell", zone_counts[enumOrd(ESceneZone::shell)]},
                {"circulation", zone_counts[enumOrd(ESceneZone::circulation)]},
                {"services", zone_counts[enumOrd(ESceneZone::services)]},
                {"habitat", zone_counts[enumOrd(ESceneZone::habitat)]},
                {"nature", zone_counts[enumOrd(ESceneZone::nature)]},
                });

            return default_value_v;
        }
    };
}

int main(const int argc, char *argv[]) {
    demo::TurboLarbin app("ppr", std::span(&argv[0], pP::checked_cast<std::size_t>(argc)));
    const std::error_code err = app.run();
    return err.value();
}
