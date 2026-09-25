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

        struct LoadedAsset final {
            mesh::SceneAsset m_scene{};
            Array<image::ImageAsset> m_images{};
            TrianglePass::UploadedScene m_uploaded{};
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

        static constexpr std::size_t kNoAsset = std::numeric_limits<std::size_t>::max();

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

            m_load_started = true;
            const std::filesystem::path asset_root = getContentDir().path() / "meshes" / "kenney_colony";
            m_asset_indices.resize(std::size(kAssetSpecs));
            std::ranges::fill(m_asset_indices, kNoAsset);
            PPR_LOG(Demo, info, "colony fixture loading", {
                {"root", asset_root.string()},
                {"candidates", static_cast<u32>(std::size(kAssetSpecs))},
                });

            for (std::size_t spec_index = 0u; spec_index < std::size(kAssetSpecs); ++spec_index) {
                const AssetSpec &spec = kAssetSpecs[spec_index];
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
                    PPR_LOG(Demo, info, "colony source bounds",
                        {
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

                Array<image::ImageAsset> images{};
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
                        bytes.getBufferData(), ref.m_ext, image::ImageDecodeDesc{}, image::ImageUsage::color);

                    if (not decoded.has_value()) [[unlikely]] {
                        PPR_LOG(Demo, warning, "colony asset decode failed", {
                            {"file", std::string{spec.m_file}},
                            {"texture", ref.m_name},
                            {"error", decoded.error().message()},
                            });
                        decoded_all = false;
                        break;
                    }

                    images.push_back(*decoded);
                }

                if (not decoded_all) {
                    continue;
                }

                Expected<TrianglePass::UploadedScene> uploaded = trianglePass().uploadScene(*scene, images);
                if (not uploaded.has_value()) [[unlikely]] {
                    PPR_LOG(Demo, warning, "colony asset upload failed", {
                        {"file", std::string{spec.m_file}},
                        {"error", uploaded.error().message()},
                        });
                    continue;
                }

                LoadedAsset entry{
                    .m_scene = std::move(*scene),
                    .m_images = std::move(images),
                    .m_uploaded = std::move(*uploaded),
                };

                PPR_LOG(Demo, info, "colony asset ready", {
                    {"file", std::string{spec.m_file}},
                    {"images", entry.m_images.size()},
                    {"primitives", entry.m_uploaded.m_prims.size()},
                    });

                m_asset_indices[spec_index] = m_assets.size();
                m_assets.push_back(std::move(entry));
            }

            m_load_succeeded = not m_assets.empty();

            PPR_LOG(Demo, info, "colony fixture loaded", {
                {"loaded", static_cast<u32>(m_assets.size())},
                {"candidates", static_cast<u32>(std::size(kAssetSpecs))},
                });

            if (not m_load_succeeded) [[unlikely]] {
                return make_error_code(std::errc::no_such_file_or_directory);
            }

            return default_value_v;
        }

        std::error_code update(const TimeSpan dt) override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::update(dt));

#if 0
            ImGui::ShowDemoWindow();
            ImGui::ShowDebugLogWindow();
#endif

            trianglePass().clearInstances();
            std::size_t submitted = 0u;
            std::size_t active_placements = 0u;
            std::array<u32, 5u> zone_counts{};
            for (const Placement &placement: kHabitatLayout) {
                if (placement.m_asset >= m_asset_indices.size()) [[unlikely]] {
                    return make_error_code(std::errc::invalid_argument);
                }
                if (placement.m_scale <= 0.0f or placement.m_quarter_turns > 3u) [[unlikely]] {
                    return make_error_code(std::errc::invalid_argument);
                }
                const std::size_t asset_index = m_asset_indices[placement.m_asset];
                if (asset_index == kNoAsset) {
                    continue;
                }
                ++active_placements;
                ++zone_counts[enumOrd(placement.m_zone)];
                const LoadedAsset &asset = m_assets[asset_index];
                const float4x4 placement_transform = placementTransform(placement);
                std::size_t primitive_cursor = 0u;
                for (const mesh::SceneInstance &instance: asset.m_scene.m_instances) {
                    const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                    const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                    if (mesh_index >= asset.m_scene.m_meshes.size() or node_index >= asset.m_scene.m_nodes.size())
                    [[unlikely]] {
                        return make_error_code(std::errc::invalid_argument);
                    }
                    const mesh::StaticMeshAsset &mesh_asset = asset.m_scene.m_meshes[mesh_index];
                    const float4x4 model = placement_transform * asset.m_scene.m_nodes[node_index].m_world;
                    for ([[maybe_unused]] const mesh::MeshPrimitiveRange &primitive: mesh_asset.m_prims) {
                        if (primitive_cursor >= asset.m_uploaded.m_prims.size()) [[unlikely]] {
                            return make_error_code(std::errc::invalid_argument);
                        }
                        const TrianglePass::UploadedPrimitive &source = asset.m_uploaded.m_prims[primitive_cursor++];
                        MaterialHandle material = source.m_material;
                        if (instance.m_material_override != mesh::kInvalidMaterial) {
                            const std::size_t material_index = static_cast<std::size_t>(*instance.m_material_override);
                            if (material_index >= asset.m_uploaded.m_materials.size()) [[unlikely]] {
                                return make_error_code(std::errc::invalid_argument);
                            }
                            material = asset.m_uploaded.m_materials[material_index];
                        }
                        PPR_RETURN_ERROR_ON_FAIL(Demo, trianglePass().submitInstance(source.m_bag, material, model));
                        ++submitted;
                    }
                }
            }
            if (not m_submission_logged) {
                m_submission_logged = true;
                PPR_LOG(Demo, info, "colony fixture submitted", {
                    {"placements", kHabitatLayout.size()},
                    {"active_placements", static_cast<u32>(active_placements)},
                    {"draws", submitted},
                    {"shell", zone_counts[enumOrd(ESceneZone::shell)]},
                    {"circulation", zone_counts[enumOrd(ESceneZone::circulation)]},
                    {"services", zone_counts[enumOrd(ESceneZone::services)]},
                    {"habitat", zone_counts[enumOrd(ESceneZone::habitat)]},
                    {"nature", zone_counts[enumOrd(ESceneZone::nature)]},
                    });
            }

            return default_value_v;
        }

        std::error_code shutdown() override {
            std::error_code first_err{};
            if (m_load_started) {
                trianglePass().clearInstances();
            }
            std::size_t released = 0u;
            for (auto asset = m_assets.rbegin(); asset != m_assets.rend(); ++asset) {
                if (asset->m_uploaded.m_receipt == 0u) {
                    continue;
                }
                PPR_RETAIN_ERROR_ON_FAIL(Demo, first_err, trianglePass().releaseScene(asset->m_uploaded));
                asset->m_uploaded = TrianglePass::UploadedScene{};
                ++released;
            }
            m_assets.clear();
            m_asset_indices.clear();
            const bool load_succeeded = m_load_succeeded;
            m_load_succeeded = false;
            m_load_started = false;
            PPR_LOG(Demo, info, "colony fixture release result", {
                {"released", released},
                {"load_succeeded", load_succeeded},
                {"error", first_err.message()},
                });

            PPR_RETAIN_ERROR_ON_FAIL(Demo, first_err, super_t::shutdown());

            return first_err;
        }

    private:
        Array<LoadedAsset> m_assets{};
        Array<std::size_t> m_asset_indices{};
        bool m_load_started = false;
        bool m_load_succeeded = false;
        bool m_submission_logged = false;
    };
}

int main(const int argc, char *argv[]) {
    demo::TurboLarbin app("ppr", std::span(&argv[0], pP::checked_cast<std::size_t>(argc)));
    const std::error_code err = app.run();
    return err.value();
}
