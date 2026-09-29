#include "pP/Macros.h"
#include "colony/Colony.Elements.h"
#include "colony/StageTiming.h"

import engine.app;
import engine.core;
import engine.math;
import engine.mesh;
import engine.sim;
import std;

import game.colony.buildings;
import game.colony.driver;
import game.colony.translator;
import game.colony.panel;
import game.colony.pathfinding;
import game.colony.smoke;
import game.colony.timetrials;

namespace demo {
    using namespace pP;
    PPR_DEFINE_LOG_CATEGORY(Demo, info, none);

    // Slice 6 scripted workloads (measurement fixtures only; every flag off
    // preserves the production loop byte-identically). The edit table/period
    // are read-only copies of the S2 wall-building values in TimeTrials.cpp
    // (fixed 8x2 rock rects, period 30 frames); the trials runner itself is
    // never invoked from here.
    inline constexpr sim::GlobalCellPos kProfileWallRects[4] = {
        {2000u, 2000u},
        {2016u, 2000u},
        {2000u, 2016u},
        {2016u, 2016u},
    };
    inline constexpr u32 kProfileWallWidth = 8u;
    inline constexpr u32 kProfileWallHeight = 2u;
    inline constexpr u32 kProfileWallCells = kProfileWallWidth * kProfileWallHeight;
    inline constexpr u32 kProfileEditPeriod = 30u;
    // Pan route: world-unit deltas (1 world unit == 1 cell) applied every 60
    // frames; the four steps sum to zero, so the route is bounded and stays
    // in-world around the 2048,2048 start. Open-loop: concurrent user camera
    // input is assumed absent during capture (a user pan between waypoints
    // shifts the subsequent snap origin).
    inline constexpr u32 kProfilePanPeriod = 60u;
    const float3 kProfilePanDeltas[4] = {
        float3{256.0f, 0.0f, 0.0f},
        float3{0.0f, 256.0f, 0.0f},
        float3{-256.0f, 0.0f, 0.0f},
        float3{0.0f, -256.0f, 0.0f},
    };

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
        u32 m_agent_count{colony::kAgentCount};
        bool m_profile_csv_active{false};
        u64 m_profile_csv_remaining{0u};
        u64 m_profile_csv_total{0u};
        std::string m_profile_label{"live"};
        std::vector<u64> m_profile_frame_us{};
        u64 m_profile_backlog_frames{0u};
        // Slice 6 windows: first K frames run fully but are excluded from the
        // frame vector and both stage tables (counted nowhere); the N-frame
        // countdown below runs on measured (post-skip) frames only, so the
        // run totals K + N frames. K = 0 preserves current behavior.
        u64 m_profile_skip{0u};
        u64 m_profile_frame_index{0u};
        bool m_profile_edits{false};
        bool m_profile_pan{false};
        // Non-owning view of the editor-owned pan controller (stashed at
        // initialize, cleared before super shutdown); the scripted pan
        // route snaps it with teleport so each waypoint lands exactly.
        safe_ptr<PanCameraController> m_pan_controller{};
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

        /// Profiling fixture override (agent count for the driver spawn path).
        /// Defaults to the production count; `--profile100` sets 100.
        void setAgentCount(const u32 count) noexcept {
            m_agent_count = count;
        }

        /// Live CSV fixture: after N measured (post-skip) frames print the
        /// driver+translator timing CSVs and exit(0). 0 disables. Live dt is
        /// real-time, so frame/tick counts vary run to run; the CSV shapes
        /// match the trials runner.
        void setProfileCsvFrames(const u64 frames) noexcept {
            m_profile_csv_active = frames > 0u;
            m_profile_csv_remaining = frames;
            m_profile_csv_total = frames;
            m_profile_frame_us.clear();
            m_profile_backlog_frames = 0u;
            m_profile_frame_index = 0u;
            try {
                m_profile_frame_us.reserve(static_cast<std::size_t>(frames));
            } catch (...) {
            }
        }

        /// Skip window for the live CSV fixture: the first K frames run fully
        /// but are excluded from the frame vector, the backlog count, and both
        /// stage tables (the tables reset on window entry). 0 preserves
        /// current behavior. The skip only shifts measurement; the edit/pan
        /// schedules below always key off frame 0.
        void setProfileSkip(const u64 frames) noexcept {
            m_profile_skip = frames;
        }

        /// Scripted edit workload: every 30 frames alternate buildWall /
        /// demolish on the rotating rock rects above (S2 schedule, frame 0
        /// first), so edit-induced backlog lands inside measured windows.
        void setProfileEdits(const bool enabled) noexcept {
            m_profile_edits = enabled;
        }

        /// Scripted pan workload: every 60 frames snap the main camera along
        /// the zero-sum waypoint route above (frame 0 first). Open-loop; see
        /// the route note for the no-user-input assumption.
        void setProfilePan(const bool enabled) noexcept {
            m_profile_pan = enabled;
        }

        /// Label attached to every profile-csv banner/frame line. Default "live".
        void setProfileLabel(std::string label) {
            m_profile_label = std::move(label);
        }

        static void printLiveTimingsCsv_(const colony::StageTimings &timings) {
            std::println("stage,calls,total_us,mean_us,max_us");
            for (std::size_t index = 0u; index < static_cast<std::size_t>(colony::Stage::Count); ++index) {
                const colony::Stage stage = static_cast<colony::Stage>(index);
                const colony::StageStat &stat = timings.m_stats[index];
                const u64 calls = stat.m_calls;
                const u64 total = stat.m_microseconds;
                const double mean = calls > 0u ? static_cast<double>(total) / static_cast<double>(calls) : 0.0;
                // NOTE: see TimeTrials.cpp — printf dodges consteval _Format_checker
                // C3546 on this toolchain; output is byte-identical CSV.
                std::printf("%s,%llu,%llu,%.2f,%llu\n", colony::stageName(stage),
                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(total),
                    static_cast<double>(mean), static_cast<unsigned long long>(stat.m_max_us));
            }
        }

        /// Active workload flags for the banners below (empty when every flag
        /// is off, so default output stays byte-identical); keeps evidence
        /// self-describing without touching the user label.
        [[nodiscard]] std::string profileWorkloadSuffix_() const {
            std::string suffix{};
            if (m_profile_edits) {
                suffix += " edits";
            }
            if (m_profile_pan) {
                suffix += " pan";
            }
            if (m_profile_skip > 0u) {
                suffix += std::format(" skip={}", m_profile_skip);
            }
            return suffix;
        }

        void printProfileCsv_() const {
            const std::string workload = profileWorkloadSuffix_();
            std::println(stderr, "[profile-csv label={}{}] driver timings (live dt is real-time; frame counts vary)",
                m_profile_label, workload);
            printLiveTimingsCsv_(m_driver.stageTimings());
            std::println(stderr,
                "[profile-csv label={}{}] translator timings (live dt is real-time; frame counts vary)",
                m_profile_label, workload);
            printLiveTimingsCsv_(m_translator.stageTimings());
            std::println(stderr,
                "[profile-csv label={}{}] frame intervals (live dt is real-time; frame counts vary)",
                m_profile_label, workload);
            printProfileFrameCsv_();
        }

        void printProfileFrameCsv_() const {
            // Sorted copy percentiles (nearest-rank ceil); thresholds are
            // 16.667ms / 33.333ms frame budgets; backlog counts frames ending
            // with a non-empty collider queue.
            const std::size_t count = m_profile_frame_us.size();
            u64 p50 = 0u;
            u64 p95 = 0u;
            u64 p99 = 0u;
            u64 max_us = 0u;
            u64 over_16667 = 0u;
            u64 over_33333 = 0u;
            if (count > 0u) {
                std::vector<u64> sorted = m_profile_frame_us;
                std::sort(sorted.begin(), sorted.end());
                const auto at_rank = [&sorted, count](const u64 percent) noexcept -> u64 {
                    std::size_t index = (static_cast<std::size_t>(percent) * count + 99u) / 100u;
                    index = index > 0u ? index - 1u : 0u;
                    if (index >= count) {
                        index = count - 1u;
                    }
                    return sorted[index];
                };
                p50 = at_rank(50u);
                p95 = at_rank(95u);
                p99 = at_rank(99u);
                max_us = sorted[count - 1u];
                for (const u64 value: sorted) {
                    if (value > 16667u) {
                        ++over_16667;
                    }
                    if (value > 33333u) {
                        ++over_33333;
                    }
                }
            }
            std::println("frame_label,frames,p50_us,p95_us,p99_us,max_us,over_16667,over_33333,backlog_frames");
            // NOTE: see TimeTrials.cpp — printf dodges consteval _Format_checker
            // C3546 on this toolchain; output is byte-identical CSV.
            std::printf("%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", m_profile_label.c_str(),
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(p50),
                static_cast<unsigned long long>(p95), static_cast<unsigned long long>(p99),
                static_cast<unsigned long long>(max_us), static_cast<unsigned long long>(over_16667),
                static_cast<unsigned long long>(over_33333),
                static_cast<unsigned long long>(m_profile_backlog_frames));
        }

    protected:
        [[nodiscard]] std::error_code initialize() override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::initialize());
            PPR_RETURN_ERROR_ON_FAIL(Demo,
                m_driver.init(colony::ColonyDriverDesc{.m_seed = 1234567u, .m_agent_count = m_agent_count}));
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
            // Stash a non-owning view for the scripted pan route (the editor
            // owns the controller after the move below).
            m_pan_controller.reset(camera_controller.get());
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

        /// Issues one scripted edit period (S2 schedule by value): period p
        /// builds (p even) or demolishes (p odd) on rect[(p / 2) % 4], with the
        /// same grid-probe flip as the trials runner so every period performs
        /// real cell changes. Progressive errands drain through the normal
        /// tick budget; presentation flows through presentEdits below.
        [[nodiscard]] std::error_code issueProfileEdit_(const u64 frame) {
            const u32 period = static_cast<u32>(frame / kProfileEditPeriod);
            const sim::GlobalCellPos origin = kProfileWallRects[(period / 2u) % 4u];
            u32 rock = 0u;
            for (u32 dy = 0u; dy < kProfileWallHeight; ++dy) {
                for (u32 dx = 0u; dx < kProfileWallWidth; ++dx) {
                    const auto cell = m_driver.grid().getCell({origin.m_x + dx, origin.m_y + dy});
                    if (not cell) {
                        return cell.error();
                    }
                    if (cell->m_element == colony::kElementRock) {
                        ++rock;
                    }
                }
            }
            const bool base_build = period % 2u == 0u;
            bool build = base_build;
            if ((base_build and rock == kProfileWallCells) or (not base_build and rock == 0u)) {
                build = not base_build;
            }
            if (build) {
                const Expected<sim::Entity> wall = m_driver.buildWall(colony::Footprint{.m_min = origin,
                    .m_width = kProfileWallWidth, .m_height = kProfileWallHeight,
                    .m_element = colony::kElementRock});
                if (not wall) {
                    return wall.error();
                }
            } else {
                const Expected<sim::Entity> teardown =
                    m_driver.demolish(origin, kProfileWallWidth, kProfileWallHeight);
                if (not teardown) {
                    return teardown.error();
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code update(const TimeSpan dt) override {
            // Phase: profile workload index. Counts every live-loop frame while
            // any profiling workload is armed; all flags off leaves the counter
            // at zero and every schedule below untouched (production default).
            const bool profile_workload = m_profile_csv_active or m_profile_edits or m_profile_pan;
            const u64 profile_frame = profile_workload ? m_profile_frame_index++ : 0u;
            const bool profile_in_window = profile_frame >= m_profile_skip;
            // Skip entry: reset both stage tables so spawn work drains out of
            // the CSV tables; the frame vector/backlog below are gated by the
            // same window. Skip 0 takes no reset (current behavior preserved).
            if (profile_workload and m_profile_skip > 0u and profile_frame == m_profile_skip) {
                m_driver.resetStageTimings();
                m_translator.resetStageTimings();
            }

            // Phase: scripted pan. Teleport snap before the camera integrates
            // so this frame's render uses the waypoint; open-loop per the
            // route note (no user camera input during capture).
            if (m_profile_pan and m_pan_controller and profile_frame % kProfilePanPeriod == 0u) {
                const std::size_t pan_step = static_cast<std::size_t>((profile_frame / kProfilePanPeriod) % 4u);
                const float3 eye = m_pan_controller->getPosition() + kProfilePanDeltas[pan_step];
                m_pan_controller->translate(eye, true);
            }

            // Phase: live frame interval (`--profile-csv N` only, post-skip):
            // wall time of this body plus end-of-frame collider backlog.
            // Bounded to N entries; pre-window frames record nowhere.
            const bool profile_sample = m_profile_csv_active and profile_in_window;
            struct ProfileFrameScope {
                std::vector<u64> *samples;
                const u64 *cap;
                u64 *backlog;
                const colony::ColonyDriver *driver;
                std::chrono::steady_clock::time_point start;
                bool armed;
                void record() noexcept {
                    try {
                        if (not armed or samples == nullptr or cap == nullptr or backlog == nullptr or
                                driver == nullptr) {
                            return;
                        }
                        armed = false;
                        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
                        const u64 elapsed = static_cast<u64>(
                            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
                        if (samples->size() < static_cast<std::size_t>(*cap)) {
                            samples->push_back(elapsed);
                        }
                        if (driver->colliderPending() != 0u) {
                            ++(*backlog);
                        }
                    } catch (...) {
                    }
                }
                ~ProfileFrameScope() noexcept {
                    record();
                }
            };
            ProfileFrameScope profile_scope{&m_profile_frame_us, &m_profile_csv_total, &m_profile_backlog_frames,
                &m_driver,
                profile_sample ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{},
                profile_sample};

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

            // Phase: scripted edits. S2-matching alternating build/demolish
            // (frame 0 first) ahead of the tick, so errand backlog accrues
            // inside measured windows; failures are sticky like tool intent.
            if (m_profile_edits and profile_frame % kProfileEditPeriod == 0u) {
                if (const std::error_code error = issueProfileEdit_(profile_frame)) {
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
            PPR_RETURN_ERROR_ON_FAIL(Demo, drawPanel_());

            // Phase: live CSV profile. Inactive unless `--profile-csv N` set it;
            // after N frames the driver+translator CSVs print and the process
            // exits(0). Live dt is real-time, so frame counts vary run to run.
            // NOTE: graceful requestExit, never std::exit — the latter skips
            // inverse-of-setup teardown and traps in static destruction.
            // The frame interval stops above so the dump itself is not timed.
            profile_scope.record();
            if (profile_sample) {
                if (--m_profile_csv_remaining == 0u) {
                    printProfileCsv_();
                    m_profile_csv_active = false;
                    requestExit();
                }
            }
            return {};
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
            // Release the camera view before the editor destroys the controller.
            m_pan_controller.reset();
            PPR_RETAIN_ERROR_ON_FAIL(Demo, first_error, super_t::shutdown());
            return first_error;
        }
    };
}

#ifdef _WIN32
// Debugger-less run support: OutputDebugString payloads (D3D12 validation)
// and third-party legacy thread-naming raises are fatal without a debugger
// (unhandled DBG_PRINTEXCEPTION_C / 0x406D1388 terminate the process). This
// handler mirrors debugger behavior: log-and-continue for prints, swallow
// thread-naming raises, and stack-dump genuine traps before death. Engine
// behavior is unchanged when healthy; see Slice 6 notes for the allocator
// abort this scaffolding diagnosed (TrianglePass ScratchPad overflow).
namespace {
extern "C" {
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *name);
__declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
} // extern "C"
using DiagSigHandlerFn = void(__cdecl *)(int);
extern "C" __declspec(dllimport) DiagSigHandlerFn __cdecl signal(int sig, DiagSigHandlerFn handler);
extern "C" __declspec(dllimport) void __cdecl _exit(int status);

using DiagSymInitFn = int(__stdcall *)(void *, const char *, int);
using DiagSymFromAddrFn = int(__stdcall *)(void *, unsigned long long, unsigned long long *, void *);
using DiagCaptureFn = unsigned short(__stdcall *)(unsigned long, unsigned long, void **, unsigned long *);
using DiagAddVehFn = long(__stdcall *)(unsigned long, void *);

struct DiagSymInfo {
    unsigned long SizeOfStruct;
    unsigned long TypeIndex;
    unsigned long long Reserved[2];
    unsigned long Index;
    unsigned long Size;
    unsigned long long ModBase;
    unsigned long Flags;
    unsigned long long Value;
    unsigned long long Address;
    unsigned long Register;
    unsigned long Scope;
    unsigned long Tag;
    int NameLen;
    int MaxNameLen;
    char Name[512];
};

void diagDumpStack(const char *reason, const unsigned long code) noexcept {
    static bool entered = false;
    if (entered) {
        return;
    }
    entered = true;
    std::fprintf(stderr, "[trap] %s code=%lu\n", reason, code);
    void *kernel = LoadLibraryA("kernel32.dll");
    DiagCaptureFn capture = nullptr;
    if (kernel != nullptr) {
        capture = reinterpret_cast<DiagCaptureFn>(GetProcAddress(kernel, "CaptureStackBackTrace"));
    }
    void *frames[40] = {};
    unsigned long hash = 0u;
    unsigned short taken = 0u;
    if (capture != nullptr) {
        taken = capture(0u, 40u, frames, &hash);
    }
    void *dbghelp = LoadLibraryA("Dbghelp.dll");
    DiagSymFromAddrFn symAddr = nullptr;
    bool sym_ok = false;
    if (dbghelp != nullptr) {
        const auto symInit = reinterpret_cast<DiagSymInitFn>(GetProcAddress(dbghelp, "SymInitialize"));
        symAddr = reinterpret_cast<DiagSymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
        if (symInit != nullptr) {
            sym_ok = symInit(reinterpret_cast<void *>(-1), nullptr, 1) != 0;
        }
    }
    for (unsigned short i = 0u; i < taken; ++i) {
        const unsigned long long addr = reinterpret_cast<unsigned long long>(frames[i]);
        if (symAddr != nullptr and sym_ok) {
            DiagSymInfo info{};
            info.SizeOfStruct = sizeof(DiagSymInfo);
            info.MaxNameLen = 511;
            unsigned long long disp = 0u;
            if (symAddr(reinterpret_cast<void *>(-1), addr, &disp, &info) != 0) {
                std::fprintf(stderr, "  #%u 0x%llx %s+0x%llx\n", i, addr, info.Name, disp);
                continue;
            }
        }
        std::fprintf(stderr, "  #%u 0x%llx\n", i, addr);
    }
    std::fprintf(stderr, "[trap] end\n");
}

void __cdecl diagSigAbrt(const int sig) noexcept {
    diagDumpStack("SIGABRT", static_cast<unsigned long>(sig));
    _exit(3);
}

long __stdcall diagVeh(void *pointers) noexcept {
    // EXCEPTION_POINTERS: [0] = record (ExceptionCode at +0, param count at
    // +24, params at +32). For DBG_PRINTEXCEPTION_C, params are [len, string].
    const void *record = *reinterpret_cast<void **>(pointers);
    const char *bytes = static_cast<const char *>(record);
    const unsigned long code = *reinterpret_cast<const unsigned long *>(bytes);
    unsigned long count = 0u;
    unsigned long long info0 = 0u;
    const char *info1 = nullptr;
    {
        unsigned long n = 0u;
        __try {
            n = *reinterpret_cast<const unsigned long *>(bytes + 24);
        } __except (1) {
            n = 0u;
        }
        count = n > 4u ? 4u : n;
        if (count > 0u) {
            __try {
                info0 = *reinterpret_cast<const unsigned long long *>(bytes + 32);
            } __except (1) {
                info0 = 0u;
            }
        }
        if (count > 1u) {
            __try {
                info1 = *reinterpret_cast<const char *const *>(bytes + 40);
            } __except (1) {
                info1 = nullptr;
            }
        }
    }
    // NOTE: benign prints must NOT touch diagDumpStack (its once-guard is
    // reserved for the real trap); otherwise a later SIGABRT exits silently.
    // Same for 0x406D1388 legacy thread-naming: debuggers swallow it by
    // protocol, and our own HAL only raises it under IsDebuggerPresent, so a
    // third-party raise in a debugger-less run must be swallowed, not trapped.
    if (code == 0x406D1388u) {
        std::fprintf(stderr, "[dbgthread] swallowed legacy thread-name raise\n");
        return -1; // EXCEPTION_CONTINUE_EXECUTION, as a debugger would
    }
    if (code == 0xE06D7363u and count >= 3u) {
        // MSVC C++ exception: params are [magic, object, ThrowInfo*].
        // Decode the first catchable type name WITHOUT touching the guard.
        const void *throw_info = nullptr;
        __try {
            throw_info = *reinterpret_cast<void *const *>(bytes + 48);
        } __except (1) {
            throw_info = nullptr;
        }
        const char *type_name = nullptr;
        if (throw_info != nullptr) {
            __try {
                const char *ti = static_cast<const char *>(throw_info);
                const void *arr = *reinterpret_cast<void *const *>(ti + 16);
                int n = 0;
                if (arr != nullptr) {
                    n = *reinterpret_cast<const int *>(arr);
                }
                if (n > 0) {
                    const void *first =
                        *reinterpret_cast<void *const *>(static_cast<const char *>(arr) + 8);
                    if (first != nullptr) {
                        const void *desc =
                            *reinterpret_cast<void *const *>(static_cast<const char *>(first) + 16);
                        if (desc != nullptr) {
                            type_name = static_cast<const char *>(desc) + 16;
                        }
                    }
                }
            } __except (1) {
                type_name = nullptr;
            }
        }
        if (type_name != nullptr and type_name[0] != '\0') {
            __try {
                std::fprintf(stderr, "[cppthrow] %s\n", type_name);
            } __except (1) {
                std::fprintf(stderr, "[cppthrow] <unprintable>\n");
            }
        } else {
            std::fprintf(stderr, "[cppthrow] <undecoded>\n");
        }
        return 0; // EXCEPTION_CONTINUE_SEARCH: normal unwind/terminate proceeds
    }
    if (code == 0x40010006u) {
        // OutputDebugString payload: log it, then continue execution exactly
        // as a debugger would (this raise is non-fatal by design).
        if (info1 != nullptr) {
            __try {
                std::fprintf(stderr, "[dbgprint len=%llu] %s\n", info0, info1);
            } __except (1) {
                std::fprintf(stderr, "[dbgprint] <unreadable>\n");
            }
        } else {
            std::fprintf(stderr, "[dbgprint] <no payload> params=%lu\n", count);
        }
        return -1; // EXCEPTION_CONTINUE_EXECUTION
    }
    diagDumpStack("VEH", code);
    return 0; // EXCEPTION_CONTINUE_SEARCH
}

struct DiagTrapInstaller {
    DiagTrapInstaller() noexcept {
        signal(22, &diagSigAbrt); // SIGABRT
        void *kernel = LoadLibraryA("kernel32.dll");
        if (kernel != nullptr) {
            const auto addVeh =
                reinterpret_cast<DiagAddVehFn>(GetProcAddress(kernel, "AddVectoredExceptionHandler"));
            if (addVeh != nullptr) {
                addVeh(1u, reinterpret_cast<void *>(&diagVeh));
            }
        }
    }
};

DiagTrapInstaller g_diag_trap{};
} // namespace
#endif

int main(const int argc, char *argv[]) {
    const std::span<const char *const> args{argv, pP::checked_cast<std::size_t>(argc)};
    if (argc > 1 and std::string_view{argv[1]} == "--smoke") {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        return pP::colony::runColonySmoke(args).value();
    }
    if (argc > 1 and std::string_view{argv[1]} == "--load100") {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        const auto load = pP::colony::runLoad100();
        if (not load) {
            return load.error().value();
        }
        return 0;
    }
    if (argc > 1 and std::string_view{argv[1]} == "--time-trials") {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        const auto trials = pP::colony::runTimeTrials();
        if (not trials) {
            return trials.error().value();
        }
        return 0;
    }
    demo::TurboLarbin app("ppr", args);
    // Slice 6 profiling fixture: deterministic 100-agent load for PIX
    // captures. Production default stays 3; the interactive loop is identical.
    // `--profile-csv N` adds a live CSV dump after N measured frames (exit(0));
    // it never alters the simulation, only reports timings.
    // `--profile-label <name>` (default "live") tags every banner/frame line.
    // `--profile-skip K` (default 0) runs the first K frames fully but excludes
    // them from every table; the N-frame countdown runs post-skip (K + N total).
    // `--profile-edits` / `--profile-pan` add the scripted S2 edit schedule /
    // camera route inside measured windows (frame 0 first, combinable).
    for (std::size_t index = 0u; index < args.size(); ++index) {
        const std::string_view arg{args[index]};
        if (arg == "--profile100") {
            app.setAgentCount(100u);
        } else if (arg == "--profile-csv" and index + 1u < args.size()) {
            pP::u64 frames = 0u;
            const std::string_view count{args[++index]};
            std::from_chars(count.data(), count.data() + count.size(), frames);
            app.setProfileCsvFrames(frames);
        } else if (arg.starts_with("--profile-csv=")) {
            pP::u64 frames = 0u;
            const std::string_view count = arg.substr(std::string_view{"--profile-csv="}.size());
            std::from_chars(count.data(), count.data() + count.size(), frames);
            app.setProfileCsvFrames(frames);
        } else if (arg == "--profile-label" and index + 1u < args.size()) {
            const std::string_view label{args[++index]};
            app.setProfileLabel(std::string{label});
        } else if (arg.starts_with("--profile-label=")) {
            const std::string_view label = arg.substr(std::string_view{"--profile-label="}.size());
            app.setProfileLabel(std::string{label});
        } else if (arg == "--profile-skip" and index + 1u < args.size()) {
            pP::u64 frames = 0u;
            const std::string_view count{args[++index]};
            std::from_chars(count.data(), count.data() + count.size(), frames);
            app.setProfileSkip(frames);
        } else if (arg.starts_with("--profile-skip=")) {
            pP::u64 frames = 0u;
            const std::string_view count = arg.substr(std::string_view{"--profile-skip="}.size());
            std::from_chars(count.data(), count.data() + count.size(), frames);
            app.setProfileSkip(frames);
        } else if (arg == "--profile-edits") {
            app.setProfileEdits(true);
        } else if (arg == "--profile-pan") {
            app.setProfilePan(true);
        }
    }
    // Make run-loop failures visible: a bare .value() turns any loop error
    // into a silent terminate (exit 3). Catch, print, then propagate.
    try {
        const std::error_code run_error = app.run();
        if (run_error) {
            std::fprintf(stderr, "[fatal] app.run: %s (category=%s value=%d)\n", run_error.message().c_str(),
                run_error.category().name(), run_error.value());
        }
        return run_error.value();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[fatal-ex] %s\n", error.what());
        return 100;
    } catch (...) {
        std::fprintf(stderr, "[fatal-ex] unknown non-std exception\n");
        return 101;
    }
}
