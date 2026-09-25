#include "pP/Macros.h"

import engine.core;
import engine.image;
import engine.math;
import engine.mesh;
import engine.rhi;
import engine.app;
import imgui_internal;
import std;

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

        static constexpr float kFixtureScale = 1.0f;
        // Kepler-9 Cutaway Waystation: shell and gate frame a compact route
        // from ground-level services to the catwalk and domestic room.
        static constexpr std::array<Placement, 21u> kHabitatLayout{
            {
                {ESceneZone::shell, 0u, {-1.0f, 0.0f, 0.0f}},
                {ESceneZone::shell, 0u, {0.0f, 0.0f, 0.0f}},
                {ESceneZone::shell, 0u, {1.0f, 0.0f, 0.0f}},
                {ESceneZone::shell, 1u, {-1.0f, 0.0f, -0.45f}},
                {ESceneZone::shell, 2u, {0.0f, 0.0f, -0.45f}},
                {ESceneZone::shell, 3u, {1.0f, 0.0f, -0.45f}},
                {ESceneZone::shell, 4u, {1.0f, 0.0f, -0.25f}},
                {ESceneZone::circulation, 5u, {-0.75f, 0.0f, 0.25f}},
                {ESceneZone::circulation, 10u, {0.0f, 0.55f, -0.1f}},
                {ESceneZone::circulation, 10u, {1.0f, 0.55f, -0.1f}},
                {ESceneZone::circulation, 14u, {0.75f, 0.0f, 0.3f}},
                {ESceneZone::services, 8u, {-1.6f, 0.0f, 0.1f}},
                {ESceneZone::services, 11u, {-0.65f, 0.0f, 0.1f}},
                {ESceneZone::services, 12u, {0.2f, 0.0f, 0.3f}},
                {ESceneZone::services, 13u, {-0.8f, 0.3f, -0.15f}},
                {ESceneZone::services, 6u, {-0.3f, 0.7f, -0.15f}},
                {ESceneZone::services, 6u, {0.3f, 0.7f, -0.15f}},
                {ESceneZone::services, 6u, {0.9f, 0.7f, -0.15f}},
                {ESceneZone::habitat, 7u, {0.8f, 0.0f, 0.3f}},
                {ESceneZone::habitat, 9u, {1.55f, 0.0f, 0.15f}},
                {ESceneZone::habitat, 15u, {1.55f, 0.4f, 0.15f}},
            }
        };

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
                if (not
                    scene.has_value())
                [[unlikely]] {
                    PPR_LOG(Demo, warning, "colony asset import failed", {
                        {"file", std::string{spec.m_file}},
                        {"pack", std::string{spec.m_pack}},
                        {"error", scene.error().message()},
                        });
                    continue;
                }
                Array<image::ImageAsset> images{};
                bool decoded_all = true;
                for (const mesh::ImageRef &ref: scene->m_images) {
                    mem::SharedBuffer bytes{};
                    if (ref.m_is_file) {
                        Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(model_dir / ref.m_rel_path);
                        if (not
                            mapped.has_value())
                        [[unlikely]] {
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
                    if (not
                        bytes.isValid())
                    [[unlikely]] {
                        PPR_LOG(Demo, warning, "colony texture bytes are invalid", {
                            {"file", std::string{spec.m_file}},
                            {"texture", ref.m_name},
                            });
                        decoded_all = false;
                        break;
                    }
                    Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                        bytes.getBufferData(), ref.m_ext, image::ImageDecodeDesc{}, image::ImageUsage::color);
                    if (not
                        decoded.has_value())
                    [[unlikely]] {
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
                if (not
                    uploaded.has_value())
                [[unlikely]] {
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

            m_load_succeeded = not
                    m_assets.empty();
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

            trianglePass().clearInstances();
            std::size_t submitted = 0u;
            std::size_t active_placements = 0u;
            std::array<u32, 5u> zone_counts{};
            for (const Placement &placement: kHabitatLayout) {
                if (placement.m_asset >= m_asset_indices.size()) [[unlikely]] {
                    return make_error_code(std::errc::invalid_argument);
                }
                const std::size_t asset_index = m_asset_indices[placement.m_asset];
                if (asset_index == kNoAsset) {
                    continue;
                }
                ++active_placements;
                ++zone_counts[enumOrd(placement.m_zone)];
                const LoadedAsset &asset = m_assets[asset_index];
                const float4x4 placement_transform{
                    float4{kFixtureScale, 0.0f, 0.0f, 0.0f},
                    float4{0.0f, kFixtureScale, 0.0f, 0.0f},
                    float4{0.0f, 0.0f, kFixtureScale, 0.0f},
                    float4{placement.m_position[0], placement.m_position[1], placement.m_position[2], 1.0f},
                };
                std::size_t primitive_cursor = 0u;
                for (const mesh::SceneInstance &instance: asset.m_scene.m_instances) {
                    const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                    const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                    if (mesh_index >= asset.m_scene.m_meshes.size()
                        or
                        node_index >= asset.m_scene.m_nodes.size())
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
                        if (instance.m_materialOverride != mesh::kInvalidMaterial) {
                            const std::size_t material_index = static_cast<std::size_t>(*instance.m_materialOverride);
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

#if PPR_ENABLE_DEBUG
            if (const auto ui = getServices().get<IUIService>(); ui.isValid()) {
                ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ui->getContext()));
                static bool g_show_demo_window{true};
                ImGui::ShowDemoWindow(&g_show_demo_window);
            }
#endif

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
