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
        // Action slots: 0-5 colony keys, 6 dig-mode toggle, 7 highlight toggle,
        // 8 tool press, 9 tool cursor. The array size derives from the counts.
        static constexpr std::size_t kColonyKeyActions = 8u;
        static constexpr std::size_t kToolActions = 2u;
        static constexpr std::size_t kActionCount = kColonyKeyActions + kToolActions;
        static constexpr std::size_t kDigModeAction = 6u;
        static constexpr std::size_t kHighlightAction = 7u;
        static constexpr std::size_t kToolPressAction = 8u;
        static constexpr std::size_t kToolCursorAction = 9u;
        std::array<std::unique_ptr<InputAction>, kActionCount> m_actions{};
        std::unique_ptr<InputMapping> m_colony_mapping{};
        std::unique_ptr<InputMapping> m_tool_mapping{};
        std::error_code m_input_error{};
        bool m_driver_ready{false};
        bool m_debug_draw_ready{false};
        bool m_dig_mode{false};
        bool m_show_highlight{true};
        bool m_tool_gesture{false};
        float2 m_cursor_client{zero_v};
        std::optional<sim::GlobalCellPos> m_pending_pick{};

        [[nodiscard]] std::error_code regenerate_() {
            // Entropy is chosen at the application boundary, never inside the simulation.
            try {
                std::random_device entropy{};
                const u64 seed = std::mt19937_64{entropy()}();
                PPR_LOG(Demo, info, "colony regeneration", {{"seed", seed}, });
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

        void cancelToolGesture_() noexcept {
            m_tool_gesture = false;
            m_pending_pick.reset();
            if (m_actions[kToolPressAction]) {
                m_actions[kToolPressAction]->setConsumeInput(false);
            }
            if (m_actions[kToolCursorAction]) {
                m_actions[kToolCursorAction]->setConsumeInput(false);
            }
        }

        [[nodiscard]] std::optional<sim::GlobalCellPos> pickCell_(const float2 cursor_client) const noexcept {
            const safe_ptr<const WindowViewport> viewport = getMainViewport();
            const safe_ptr<const Camera> camera = getMainCamera();
            const bool have_view = viewport and
                                   camera;
            if (not have_view) {
                return std::nullopt;
            }

            // Client pixels round-trip through screen space so the window/client
            // origin lands in the normalized lookup; both spaces are pixels, so
            // DPI cancels with no backend-specific flip.
            const Viewport &view = viewport->getViewport();
            const int2 client_px{static_cast<int>(cursor_client.x), static_cast<int>(cursor_client.y)};
            const float2 uv = view.screenToClientNormalized(view.clientToScreen(client_px));
            const bool inside_client = uv.x >= 0.0f and
                                       uv.x <= 1.0f and
                                       uv.y >= 0.0f and
                                       uv.y <= 1.0f;
            if (not inside_client) {
                return std::nullopt;
            }

            // Row-vector contract: clip = world * view_projection, so the camera
            // invert maps NDC back to world via mul(clip, m_invert_view_projection).
            const float2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
            const CameraSnapshot &snapshot = camera->getSnapshot();
            const float4 world_h = float4{ndc.x, ndc.y, 0.0f, 1.0f} * snapshot.m_invert_view_projection;
            const bool degenerate = world_h.w > -1e-6f and
                                    world_h.w < 1e-6f;
            if (degenerate) {
                return std::nullopt;
            }
            const float world_x = world_h.x / world_h.w;
            const float world_y = world_h.y / world_h.w;
            const bool inside_world = world_x >= 0.0f and
                                      world_y >= 0.0f and
                                      world_x < static_cast<float>(sim::kWorldEdge) and
                                      world_y < static_cast<float>(sim::kWorldEdge);
            if (not inside_world) {
                return std::nullopt;
            }
            return sim::GlobalCellPos{static_cast<u32>(world_x), static_cast<u32>(world_y)};
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

            // G/H are chord-free toggles: free of the colony keys above and of the
            // camera set (WASD/QE, arrows, page up/down, wheel, mouse_2d at
            // Controller.cpp:326), single-hand reachable, mnemonic (ground, highlight).
            bindAction_(kDigModeAction, "ColonyDigMode", InputKey::g,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    m_dig_mode = not m_dig_mode;
                    const bool cancel_owned = not m_dig_mode and
                                              m_tool_gesture;
                    if (cancel_owned) {
                        cancelToolGesture_();
                    }
                });
            bindAction_(kHighlightAction, "ColonyHighlight", InputKey::h,
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    m_show_highlight = not m_show_highlight;
                });

            // These keys do not overlap the camera mapping; UI still gets first refusal.
            getMainPlayer()->getListener().addInputMapping(m_colony_mapping,
                static_cast<int>(EInputMappingPriority::camera));

            // Arbitration: ImGui capture first (foreground listener consumes),
            // then tool > camera > selection. The tool mapping dispatches before
            // the camera mapping (lower priority number runs first); both tool
            // actions start transparent and only consume while the tool owns the
            // left-button gesture, retaining suppression through release. Eating
            // mouse_2d freezes camera pan/rotate (mouse_2d rotate included) until
            // the completed event or a dig-mode cancel.
            m_tool_mapping = std::make_unique<InputMapping>("colony_tool_mapping");
            m_actions[kToolPressAction] = std::make_unique<InputAction>("ColonyToolPress",
                EInputValueType::digital, EInputActionFlags::none);
            m_actions[kToolPressAction]->setStarted(
                [this](const InputActionEvent &, const InputKey &) noexcept {
                    const bool armed = m_dig_mode and
                                       not m_tool_gesture and
                                       not m_input_error;
                    if (not armed) {
                        return;
                    }
                    const safe_ptr<IUIService> ui = getServices().tryGet<IUIService>();
                    const bool ui_captured = ui and
                                             ui->hasMouseCaptureUnlessPopupClose();
                    if (ui_captured) {
                        return;
                    }
                    m_tool_gesture = true;
                    m_actions[kToolPressAction]->setConsumeInput(true);
                    m_actions[kToolCursorAction]->setConsumeInput(true);
                    m_pending_pick = pickCell_(m_cursor_client);
                });
            m_actions[kToolPressAction]->setCompleted(
                [this](const InputActionEvent &, const InputKey &) noexcept { cancelToolGesture_(); });
            m_tool_mapping->mapInputKey(SharedInputAction(m_actions[kToolPressAction].get()),
                InputKey::left_mouse_button);

            m_actions[kToolCursorAction] = std::make_unique<InputAction>("ColonyToolCursor",
                EInputValueType::axis_2d, EInputActionFlags::none);
            m_actions[kToolCursorAction]->setTriggered(
                [this](const InputActionEvent &event, const InputKey &) noexcept {
                    m_cursor_client = event.getAxis2DValue().m_absolute;
                    const bool track_pick = m_tool_gesture and
                                            not m_input_error;
                    if (track_pick) {
                        m_pending_pick = pickCell_(m_cursor_client);
                    }
                });
            m_tool_mapping->mapInputKey(SharedInputAction(m_actions[kToolCursorAction].get()), InputKey::mouse_2d);

            getMainPlayer()->getListener().addInputMapping(m_tool_mapping,
                static_cast<int>(EInputMappingPriority::camera) - 1);
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
            const std::span<const sim::ChunkPos> edits = m_driver.lastEditChunks();
            colony::drawColonyPanel(m_driver, colony::ColonyPanelCounts{
                .m_visible_chunks = visible,
                .m_visible_tiles = visible,
                .m_paths = paths.m_paths,
                .m_partials = paths.m_partials,
                .m_blocked = paths.m_blocked,
                .m_errands = m_driver.errandCount(),
                .m_dug_total = m_driver.digTotal(),
                .m_wake_radius = colony::kDigWakeCells,
                .m_affected_chunks = safe_narrowing(edits.size()),
                .m_dig_mode = m_dig_mode,
                .m_show_highlight = m_show_highlight,
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

        [[nodiscard]] std::error_code submitChunkBorder_(TrianglePass &pass, const sim::ChunkPos chunk,
                                                         const u32 expand, const MaterialHandle material) {
            const float origin_x = static_cast<float>(chunk.m_x * sim::kChunkEdge);
            const float origin_y = static_cast<float>(chunk.m_y * sim::kChunkEdge);
            const float expand_f = static_cast<float>(expand);
            const float min_x = origin_x - expand_f;
            const float min_y = origin_y - expand_f;
            const float edge = static_cast<float>(sim::kChunkEdge) + expand_f * 2.0f;

            // Four one-cell borders; submitInstance snapshots the model.
            const float4x4 edges[4] = {
                debugModel_(min_x + edge * 0.5f, min_y + 0.5f, 0.0f, edge, 1.0f),
                debugModel_(min_x + edge * 0.5f, min_y + edge - 0.5f, 0.0f, edge, 1.0f),
                debugModel_(min_x + 0.5f, min_y + edge * 0.5f, 0.0f, 1.0f, edge),
                debugModel_(min_x + edge - 0.5f, min_y + edge * 0.5f, 0.0f, 1.0f, edge),
            };
            for (const float4x4 &model: edges) {
                if (const std::error_code error = pass.submitInstance(m_quad_bag, material, model)) {
                    return error;
                }
            }
            return {};
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

            // Overlay phase: last-edit chunk outlines plus the wake extent. The
            // quad bag and both materials are reused (no new GPU handles); the
            // world-space instances encode with the same camera snapshot as the
            // agent/path instances above in render().
            if (m_show_highlight) {
                for (const sim::ChunkPos chunk: m_driver.lastEditChunks()) {
                    if (const std::error_code error = submitChunkBorder_(pass, chunk, 0u, m_path_material)) {
                        return error;
                    }
                    if (const std::error_code error =
                            submitChunkBorder_(pass, chunk, colony::kDigWakeCells, m_agent_material)) {
                        return error;
                    }
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code update(const TimeSpan dt) override {
            // Input poll, camera, pass updates, and UI run in super; pass encode
            // and present run in render(), after this body, so the submits below
            // still land in the same frame's render.
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::update(dt));
            if (m_input_error) [[unlikely]] {
                return m_input_error;
            }

            // Phase: input/edit. The owned gesture records at most one pick per
            // frame; the latest intent wins, matching the pending-dig slot.
            if (m_pending_pick.has_value()) {
                const sim::GlobalCellPos cell = *m_pending_pick;
                m_pending_pick.reset();
                if (const std::error_code error = m_driver.requestDig(cell, 1u, 1u)) {
                    m_input_error = error;
                    return error;
                }
            }

            // Phase: simulate. Paused updates apply the pending tool intent with
            // zero runSystems; zero-tick frames keep the intent queued for the
            // next tick.
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_driver.update(dt));

            // Phase: present. Drain edit chunks into presentation every frame,
            // including paused and zero-tick frames, then stage the uploads.
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_driver.presentEdits(m_translator));
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_translator.submit(m_driver.grid(), getGridPass()));

            // Phase: overlay + panel.
            PPR_RETURN_ERROR_ON_FAIL(Demo, submitDebugInstances_());
            return drawPanel_();
        }

        [[nodiscard]] std::error_code shutdown() override {
            std::error_code first_error{};
            // Detach the tool gesture first: cancel ownership so the camera
            // mapping below never observes a half-torn suppressor, then drop
            // the higher-priority tool mapping before the colony one.
            cancelToolGesture_();
            if (m_tool_mapping) {
                if (not getMainPlayer()->getListener().removeInputMapping(*m_tool_mapping)) {
                    first_error = make_error_code(std::errc::not_connected);
                }
                m_tool_mapping.reset();
            }
            // Unregister borrowed action pointers and callbacks before releasing owners.
            if (m_colony_mapping) {
                if (not getMainPlayer()->getListener().removeInputMapping(*m_colony_mapping)) {
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
    if (argc > 1 and std::string_view{argv[1]} == "--smoke") {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        return pP::colony::runColonySmoke(args).value();
    }
    demo::TurboLarbin app("ppr", args);
    return app.run().value();
}
