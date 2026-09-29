#include "pP/Macros.h"

import engine.app;
import engine.core;
import engine.math;
import engine.mesh;
import engine.sim;
import std;

import game.colony.driver;
import game.colony.translator;
import game.colony.panel;
import game.colony.pathfinding;
import game.colony.smoke;

namespace demo {
    using namespace pP;
    PPR_DEFINE_LOG_CATEGORY(Demo, info, none);

    class TurboLarbin final : public ApplicationEditor {
        colony::ColonyDriver m_driver{};
        colony::ColonyTranslator m_translator{};
        TriangleBagHandle m_quad_bag{};
        MaterialHandle m_agent_material{};
        MaterialHandle m_path_material{};
        std::array<std::unique_ptr<InputAction>, 6u> m_actions{};
        std::unique_ptr<InputMapping> m_colony_mapping{};
        std::error_code m_input_error{};
        bool m_driver_ready{false};
        bool m_debug_draw_ready{false};

        [[nodiscard]] std::error_code regenerate_() {
            // Entropy is chosen at the application boundary, never inside the simulation.
            try {
                std::random_device entropy{};
                const u64 seed = std::mt19937_64{entropy()}();
                PPR_LOG(Demo, info, "colony regeneration", {{"seed", seed},});
                const std::error_code error = m_driver.regenerate(seed);
                m_translator.reset(getGridPass());
                return error;
            } catch (const std::system_error &error) {
                return error.code();
            }
        }

        void bindAction_(const std::size_t index, const string_literal description, const InputKey &key,
                         InputTriggerEvent callback) {
            m_actions[index] = std::make_unique<InputAction>(description, EInputValueType::digital);
            m_actions[index]->setStarted(std::move(callback));
            m_colony_mapping->mapInputKey(SharedInputAction(m_actions[index].get()), key);
        }

        void initializeInput_() {
            m_colony_mapping = std::make_unique<InputMapping>("colony_input_mapping");
            bindAction_(0u, "ColonyPause", InputKey::space_bar,
                [this](const InputActionEvent &, const InputKey &) noexcept { m_driver.togglePaused(); });
            bindAction_(1u, "ColonyStep", InputKey::n,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    if (m_driver.paused() and not m_input_error) {
                        m_input_error = m_driver.stepOne();
                    }
                });
            bindAction_(2u, "ColonySpeed1", InputKey::one,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    if (not m_input_error) { m_input_error = m_driver.setSpeed(1u); }
                });
            bindAction_(3u, "ColonySpeed2", InputKey::two,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    if (not m_input_error) { m_input_error = m_driver.setSpeed(2u); }
                });
            bindAction_(4u, "ColonySpeed3", InputKey::three,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    if (not m_input_error) { m_input_error = m_driver.setSpeed(3u); }
                });
            bindAction_(5u, "ColonyRegenerate", InputKey::r,
                [this](const InputActionEvent &, const InputKey &) {
                    if (not m_input_error) { m_input_error = regenerate_(); }
                });

            // These keys do not overlap the camera mapping; UI still gets first refusal.
            getMainPlayer()->getListener().addInputMapping(m_colony_mapping,
                static_cast<int>(EInputMappingPriority::camera));
        }

        [[nodiscard]] std::error_code drawPanel_() {
            // The translator's count is submitted/resident, not frustum-visible.
            // Plan the same chunk-sized tiles against the editor's current snapshot.
            Array<GridTileSubmission> submissions{};
            submissions.reserve(sim::kChunkCount);
            const sim::ChunkGrid &grid = m_driver.grid();
            for (u32 y = 0u; y < sim::kChunksPerEdge; ++y) {
                for (u32 x = 0u; x < sim::kChunksPerEdge; ++x) {
                    const sim::ChunkPos pos{x, y};
                    if (not grid.isResident(pos)) {
                        continue;
                    }
                    const i32 min_x = static_cast<i32>(x * sim::kChunkEdge);
                    const i32 min_y = static_cast<i32>(y * sim::kChunkEdge);
                    submissions.push_back(GridTileSubmission{
                        .m_chunk_id = sim::chunkIndexOf(pos),
                        .m_tile_range = {
                            min_x, min_y, min_x + static_cast<i32>(sim::kChunkEdge),
                            min_y + static_cast<i32>(sim::kChunkEdge)
                        },
                    });
                }
            }

            Expected<GridTilePlan> plan = GridPass::planTiles(getMainCamera()->getSnapshot(), submissions);
            if (not plan) [[unlikely]] {
                return plan.error();
            }
            // Slice 1 uses exactly one tile per chunk, so these counts coincide.
            const u32 visible = safe_narrowing(plan->m_tiles.size());
            const colony::PathCounts paths = m_driver.pathCounts();
            colony::drawColonyPanel(m_driver, colony::ColonyPanelCounts{
                .m_visible_chunks = visible,
                .m_visible_tiles = visible,
                .m_paths = paths.m_paths,
                .m_partials = paths.m_partials,
                .m_blocked = paths.m_blocked,
                .m_errands = m_driver.errandCount(),
            });
            return {};
        }

    public:
        using super_t = ApplicationEditor;
        using super_t::super_t;

    protected:
        [[nodiscard]] std::error_code initialize() override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::initialize());
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_driver.init(colony::ColonyDriverDesc{.m_seed = 1234567u}));
            m_driver_ready = true;

            auto camera_controller = std::make_unique<PanCameraController>();
            constexpr float ortho_scale = 0.5f;
            PPR_RETURN_ERROR_ON_FAIL(Demo, camera_controller->setOrthoScale(ortho_scale));
            camera_controller->setParallelPlane(float3{0.0f, 0.0f, -1.0f}, math::axis_y, true);
            const PixelRect &client = getMainViewport()->getViewport().getClientRect();
            if (client.m_extent.x <= 0 or client.m_extent.y <= 0) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
            // Ortho projects [0, width] x [0, height], not a centered interval.
            // Put the temperate world center in the middle of the visible pixel extent.
            const float half_width = static_cast<float>(client.m_extent.x) * ortho_scale * 0.5f;
            const float half_height = static_cast<float>(client.m_extent.y) * ortho_scale * 0.5f;
            camera_controller->translate(float3{2048.0f - half_width, 2048.0f - half_height, -0.5f}, true);
            PPR_RETURN_ERROR_ON_FAIL(Demo, replaceMainCameraController(std::move(camera_controller),
                ECameraProjection::orthographic));

            initializeInput_();
            PPR_RETURN_ERROR_ON_FAIL(Demo, uploadDebugDraw_());
            return {};
        }

        [[nodiscard]] std::error_code uploadDebugDraw_() {
            // Clockwise unit quad in XY facing +Z (this pipeline culls the
            // other winding); shared by capsules and path polylines.
            const mesh::StaticMeshVertex quad[4] = {
                {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            };
            const u32 quad_indices[6] = {0u, 3u, 2u, 0u, 2u, 1u};

            TrianglePass &pass = getTrianglePass();
            const auto bag = pass.uploadMesh(std::span<const mesh::StaticMeshVertex>{quad, 4u},
                std::span<const u32>{quad_indices, 6u});
            if (not bag) {
                return bag.error();
            }

            const TextureHandle no_textures[4] = {{}, {}, {}, {}};
            mesh::MaterialAsset agent_asset{};
            agent_asset.m_base_color = float4{1.0f, 0.55f, 0.15f, 1.0f};
            agent_asset.m_metallic = 0.0f;
            agent_asset.m_roughness = 1.0f;
            const auto agent_material = pass.packMaterial(agent_asset,
                std::span<const TextureHandle>{no_textures, 4u});
            if (not agent_material) {
                return agent_material.error();
            }

            mesh::MaterialAsset path_asset{};
            path_asset.m_base_color = float4{0.2f, 0.9f, 0.9f, 1.0f};
            path_asset.m_metallic = 0.0f;
            path_asset.m_roughness = 1.0f;
            const auto path_material = pass.packMaterial(path_asset,
                std::span<const TextureHandle>{no_textures, 4u});
            if (not path_material) {
                return path_material.error();
            }

            m_quad_bag = *bag;
            m_agent_material = *agent_material;
            m_path_material = *path_material;
            m_debug_draw_ready = true;
            return {};
        }

        [[nodiscard]] float4x4 debugModel_(const float x, const float y, const float angle, const float sx,
                                           const float sy) noexcept {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return float4x4{
                float4{sx * c, sx * s, 0.0f, 0.0f},
                float4{-sy * s, sy * c, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{x, y, 0.5f, 1.0f},
            };
        }

        [[nodiscard]] std::error_code submitDebugInstances_() {
            TrianglePass &pass = getTrianglePass();
            pass.clearInstances();
            if (not m_debug_draw_ready) {
                return {};
            }

            for (const colony::AgentSummary &agent: m_driver.agentSummaries()) {
                if (const std::error_code error =
                        pass.submitInstance(m_quad_bag, m_agent_material, debugModel_(agent.m_x, agent.m_y, 0.0f, 0.8f, 1.8f))) {
                    return error;
                }
            }

            sim::Registry &registry = m_driver.registry();
            for (const auto &[entity, agent]: registry.view<colony::Agent>()) {
                (void) agent;
                const colony::PathComp *path = registry.get<colony::PathComp>(entity);
                if (path == nullptr or path->m_partial or path->m_count == 0u) {
                    continue;
                }
                float px = 0.0f;
                float py = 0.0f;
                const sim::BodyState *pose = registry.get<sim::BodyState>(entity);
                if (pose != nullptr) {
                    px = pose->m_x;
                    py = pose->m_y;
                }

                for (u32 point = 0u; point < path->m_count; ++point) {
                    const float qx = static_cast<float>(path->m_pts[point].m_x) + 0.5f;
                    const float qy = static_cast<float>(path->m_pts[point].m_y) + 0.5f;
                    const float dx = qx - px;
                    const float dy = qy - py;
                    const float length = std::sqrt(dx * dx + dy * dy);
                    if (length > 0.01f) {
                        if (const std::error_code error = pass.submitInstance(m_quad_bag, m_path_material,
                            debugModel_((px + qx) * 0.5f, (py + qy) * 0.5f, std::atan2(dy, dx), length, 0.3f))) {
                            return error;
                        }
                    }
                    px = qx;
                    py = qy;
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code update(const TimeSpan dt) override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::update(dt));
            if (m_input_error) [[unlikely]] {
                return m_input_error;
            }
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_driver.update(dt));
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_translator.submit(m_driver.grid(), getGridPass()));
            PPR_RETURN_ERROR_ON_FAIL(Demo, submitDebugInstances_());
            return drawPanel_();
        }

        [[nodiscard]] std::error_code shutdown() override {
            std::error_code first_error{};
            // Unregister borrowed action pointers and callbacks before releasing owners.
            if (m_colony_mapping) {
                if (not getMainPlayer()->getListener().removeInputMapping(*m_colony_mapping))
                {
                    first_error = make_error_code(std::errc::not_connected);
                }
                m_colony_mapping.reset();
            }
            for (auto &action: m_actions) {
                action.reset();
            }
            if (m_driver_ready) {
                m_translator.reset(getGridPass());
                PPR_RETAIN_ERROR_ON_FAIL(Demo, first_error, m_driver.shutdown());
                m_driver_ready = false;
            }
            PPR_RETAIN_ERROR_ON_FAIL(Demo, first_error, super_t::shutdown());
            return first_error;
        }
    };
}

int main(const int argc, char *argv[]) {
    const std::span<const char *const> args{argv, pP::checked_cast<std::size_t>(argc)};
    if (argc > 1 and std::string_view{argv[1]} == "--smoke")
    {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        return pP::colony::runColonySmoke(args).value();
    }
    demo::TurboLarbin app("ppr", args);
    return app.run().value();
}
