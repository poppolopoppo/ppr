#include "pP/Macros.h"
#include "colony/Colony.Elements.h"

import engine.app;
import engine.core;
import engine.math;
import engine.mesh;
import engine.rhi;
import engine.sim;
import imgui;
import std;

import game.colony.buildings;
import game.colony.driver;
import game.colony.sim_packet;
import game.colony.translator;
import game.colony.panel;
import game.colony.pathfinding;
import game.colony.smoke;
import game.config;
import game.display;

namespace demo {
    using namespace pP;
    PPR_DEFINE_LOG_CATEGORY(Demo, info, none);

    class TurboLarbin final : public ApplicationEditor {
        colony::ColonyDriver m_driver{};
        TriangleBagHandle m_quad_bag{};
        MaterialHandle m_agent_material{};
        MaterialHandle m_path_material{};
        // Action slots: colony key rows first (one per shortcut in the
        // binding table below), then tool press and tool cursor. The tool
        // slots derive from the colony count, so adding a binding row
        // never collides with them.
        static constexpr std::size_t kColonyKeyActions = 15u;
        static constexpr std::size_t kToolActions = 2u;
        static constexpr std::size_t kActionCount = kColonyKeyActions + kToolActions;
        static constexpr std::size_t kToolPressAction = kColonyKeyActions;
        static constexpr std::size_t kToolCursorAction = kColonyKeyActions + 1u;

        /// One row of the colony binding table: the intent every source
        /// dispatches, the physical key, the input-action name, and the
        /// player-facing key label + tip. The same row builds the key action
        /// AND the shortcut row the panel shows (button tooltips, the colony
        /// hint line, the Help window), so visible shortcut text can never
        /// drift from what is actually bound - there is no second key map.
        struct Binding final {
            colony::EColonyIntent m_kind;
            u32 m_arg;
            InputKey m_key;
            string_literal m_description;
            const char *m_label;
            const char *m_tip;
        };

        std::array<std::unique_ptr<InputAction>, kActionCount> m_actions{};
        std::unique_ptr<InputMapping> m_colony_mapping{};
        std::unique_ptr<InputMapping> m_tool_mapping{};
        std::array<colony::ColonyShortcut, kColonyKeyActions> m_shortcut_rows{};
        std::error_code m_input_error{};
        bool m_driver_ready{false};
        bool m_debug_draw_ready{false};
        u32 m_agent_count{colony::kAgentCount};
        // Bounded-run fixture: `--auto-exit S` (alias `--exit-after S`) exits
        // gracefully after S wall-clock seconds from the first update();
        // the start stamps lazily to avoid pre-initialize issues.
        std::optional<TimeDuration> m_auto_exit_seconds{};
        // Borrowed from the editor; released before editor shutdown.
        safe_ptr<PanCameraController> m_pan_controller{};
        // Tool gesture ownership (interaction contract items 2+5). Press
        // snapshots ownership (world vs UI/camera) with the active tool and
        // client position; only a world-origin press acquires ownership.
        // Release completes the owned gesture exactly once; cancellation
        // (Esc, tool switch, focus loss, regen, shutdown) clears transient
        // drag state but NEVER erases the accepted queue — update()'s drain
        // owns accepted commands after accept, so a same-poll
        // press/release still digs exactly one valid cell.
        enum class EToolGestureOwner : u8 {
            none = 0,
            world,
        };

        static constexpr std::size_t kMaxQueuedDragCells = 256u;
        static constexpr std::size_t kMaxDragVisitedCells = 2048u;

        struct ToolGesture {
            bool m_dig_armed{false};
            EToolGestureOwner m_owner{EToolGestureOwner::none};
            bool m_held{false};
            float2 m_cursor_client{zero_v};
            std::optional<sim::GlobalCellPos> m_accepted_click{};
            Array<sim::GlobalCellPos> m_drag_queue{};
            Array<sim::GlobalCellPos> m_visited{};
            std::optional<sim::GlobalCellPos> m_last_cell{};
            // Slice 5 footprint anchor: the press snapshot for build_wall and
            // demolish. The dig accepted queue above is never touched by the
            // footprint path; footprint orders commit directly at release.
            std::optional<sim::GlobalCellPos> m_footprint_anchor{};
        };

        ToolGesture m_tool{};

        /// Slice 5 footprint preview: logic fills it while a build_wall or
        /// demolish press is held (anchor to current, inclusive, normalized
        /// to min/max, clamped per-axis); the designer lane draws it. The
        /// existing hover outline stays dig-only. `m_reason` is "" while the
        /// rect is committable, otherwise it names why release will not
        /// commit ("press outside the world", "drag outside the world").
        struct BuildPreview {
            bool m_active{false};
            sim::GlobalCellPos m_anchor{};
            sim::GlobalCellPos m_min{};
            sim::GlobalCellPos m_max{};
            bool m_clamped{false};
            const char *m_reason{""};
            // Per-axis clamp flags beside m_clamped (trailing fields only,
            // so the aggregate order above is untouched): m_clamped stays
            // the OR of the two for existing readers.
            bool m_clamped_x{false};
            bool m_clamped_y{false};
        };

        BuildPreview m_build_preview{};
        bool m_show_highlight{true};
        IWindowService::WindowFocusedCallback::Handle m_focus_handle{};
        // Lifetime rule: the broadcast stores a non-owning function_ref
        // view, so this member owns the callable and the handle is reset
        // in shutdown() before member destruction.
        std23::move_only_function<std::error_code(const Window &, bool)> m_on_window_focused{};

        // Slice 3 UI session state: one owned block the panel reads and the
        // intent dispatch writes, the pointer hover shared by the preview
        // outline/status line/Inspector, cumulative inter-frame timing for
        // the Performance section, and the client extent last pushed to the
        // camera controller (Slice 2 resize-deviation fix).
        colony::ColonyUiState m_ui_state{};
        colony::ColonyHover m_hover{};
        // Phase 5 display session, all game-owned. The startup baseline is
        // the "Back to startup" reset target (captured once from the
        // resolved invocation with values and provenance; runtime edits
        // never touch it). The monitor cache refreshes about once a second
        // while the Display window stands; the acknowledgement watch and
        // trial countdown live in m_ui_state.m_display and are polled once
        // a frame.
        config::Resolved<DisplaySettings> m_display_baseline{};
        Array<colony::ColonyMonitorView> m_display_monitors{};
        std::string m_display_fullscreen_monitor{};
        bool m_display_fullscreen{false};
        float m_display_monitor_age{3600.0f};
        // Owned handle-free capability and geometry views for the Display
        // window's switching controls and observed-geometry telemetry.
        // Refreshed with the monitor cache above; a failed query keeps the
        // flags clear so the panel reads "unavailable", never stale data.
        DisplayCapabilities m_display_capabilities{};
        bool m_display_has_capabilities{false};
        std::error_code m_display_capabilities_error{};
        WindowDisplayGeometry m_display_geometry{};
        bool m_display_has_geometry{false};
        // Slice 4 selection: the stable agent key (NEVER a row index) plus
        // scratch naming the selection on the status line. The hovered key
        // is the marker under the pointer (same pick a click runs), and the
        // needs rows are the per-frame O2/rest read keyed by that same
        // stable key - the view borrows them for one frame only.
        std::optional<u64> m_selected_agent{};
        std::optional<u64> m_hovered_agent{};
        std::vector<colony::AgentNeedsRow> m_agent_needs_rows{};
        std::string m_status_scratch{};
        float m_frame_ms_last{0.0f};
        float m_frame_avg{0.0f};
        float m_frame_max{0.0f};
        u64 m_frame_count{0u};
        float m_main_ms_last{0.0f};

        // Single source of truth for every render-lane readout (the
        // overlay below and the Performance tab): the worker-published
        // last-execute gauge, relaxed load, ms. Zero until the first lane
        // frame completes (or when no lane owns the renderer). Non-const:
        // renderLane() is the existing protected accessor, no new
        // plumbing. noexcept, no allocation.
        [[nodiscard]] float laneExecuteMs_() noexcept {
            const RenderLane *lane = renderLane();
            if (lane == nullptr) {
                return 0.0f;
            }
            return static_cast<float>(lane->lastExecuteUs()) / 1000.0f;
        }

        // Slice 6 frame sampler: bounded 240-sample ring of inter-frame dt
        // (ms). Boundary: update()'s dt (elapsed between update() calls) -
        // it is NOT GPU/present time and is never presented as such. Memory:
        // 240 floats (960 B) plus one reused sort scratch (<= 960 B).
        // Percentiles recompute at most every 15 frames from a copied-sorted
        // window (nearest-rank; O(n log n), n <= 240). Sampling continues
        // while paused (frames still tick); sim step counters freeze
        // naturally in the driver. The ring never resets except by its own
        // overwrite window.
        static constexpr std::size_t kFrameWindow = 240u;
        static constexpr u64 kFrameStatsEvery = 15u;
        static constexpr float kFrameBudgetMs = 1000.0f / 60.0f;
        std::vector<float> m_frame_ring{};
        std::vector<float> m_frame_sort{};
        std::size_t m_frame_head{};
        u64 m_frame_samples{};
        u64 m_frame_stats_frame{};
        bool m_frame_stats_stale{true};
        float m_frame_p50{0.0f};
        float m_frame_p95{0.0f};
        float m_frame_min{0.0f};
        float m_frame_window_max{0.0f};
        u64 m_frame_over{0u};
        // Slice 6 work-queue counters (game-side; no driver change): cells
        // handed to requestDig since launch/regen. The driver's digTotal
        // resets on the same regen path (clearToolState), so issued minus
        // applied estimates pending; the driver FIFO sheds under overflow,
        // so the estimate is an upper bound. View state only.
        u64 m_dig_issued{0u};
        // P0 serial-explicit owned sim packet (same-thread build/consume;
        // engine.app must not link sim, so the sim-carrying packet lives
        // here). Single-use per frame; cleared after the overlay phase.
        std::optional<colony::SimFramePacket> m_sim_packet{};
        // A present failure or world regeneration invalidates the pass cache, so
        // the next frame's packet must carry every resident chunk's cells.
        bool m_present_full_refresh{false};
        // A pass cache drop requested on main rides the next staged
        // payload's m_reset_caches onto the lane; the returned settlement
        // re-arms this on any non-applied outcome. No side-array ack book:
        // the per-slot RenderFrameResult carries the ack manifest.
        bool m_grid_reset_pending{false};
        // Grid overlay flag (V, off by default), mirroring the H highlight
        // path: this member holds the state only, and submitDebugInstances_
        // draws the chunk-boundary bands while it stands.
        bool m_show_grid{false};
        float2 m_viewport_extent{zero_v};
        /// Confirmed regeneration waiting for the next update() head (see
        /// the regenerate case in dispatchIntent_ for why it defers).
        bool m_regen_pending{false};

        [[nodiscard]] std::error_code regenerate_() {
            // Entropy is chosen at the application boundary, never inside the simulation.
            try {
                std::random_device entropy{};
                const u64 seed = std::mt19937_64{entropy()}();
                PPR_LOG(
                    Demo, info, "colony regeneration",
                    {
                        {"seed", seed},
                    });
                const std::error_code error = m_driver.regenerate(seed);
                // Reset the render-owned pass cache through the next staged payload.
                m_grid_reset_pending = true;
                m_present_full_refresh = true;
                return error;
            } catch (const std::system_error &error) {
                return error.code();
            }
        }

        void bindAction_(
            const std::size_t index, const string_literal description, const InputKey &key,
            InputTriggerEvent callback) {
            m_actions[index] = std::make_unique<InputAction>(description, EInputValueType::digital);
            m_actions[index]->setStarted(std::move(callback));
            m_colony_mapping->mapInputKey(SharedInputAction(m_actions[index].get()), key);
        }

        void cancelToolGesture_() noexcept {
            // Release consume FIRST, then relinquish ownership; the accepted
            // queue survives (the update() drain owns it after accept).
            if (m_actions[kToolPressAction]) {
                m_actions[kToolPressAction]->setConsumeInput(false);
            }
            if (m_actions[kToolCursorAction]) {
                m_actions[kToolCursorAction]->setConsumeInput(false);
            }
            m_tool.m_owner = EToolGestureOwner::none;
            m_tool.m_held = false;
            m_tool.m_visited.clear();
            m_tool.m_last_cell.reset();
            // Slice 5: the footprint transient clears with the gesture; the
            // dig accepted queue still survives (the update() drain owns it).
            m_tool.m_footprint_anchor.reset();
            m_build_preview = BuildPreview{};
        }

        void acquireToolGesture_() noexcept {
            try {
                // Slice 5: build_wall and demolish arm a footprint gesture under
                // the same ownership gates as dig; the dig body below is
                // otherwise byte-identical.
                const bool footprint_tool = m_ui_state.m_tool == colony::EColonyTool::build_wall or
                                            m_ui_state.m_tool == colony::EColonyTool::demolish;
                const bool armed = (m_tool.m_dig_armed or footprint_tool) and not m_tool.m_held and not m_input_error;
                if (not armed) {
                    return;
                }

                // A press starting over UI never acquires world ownership.
                const safe_ptr<IUIService> ui = getServices().tryGet<IUIService>();
                const bool ui_captured = ui and ui->hasMouseCaptureUnlessPopupClose();
                if (ui_captured) {
                    return;
                }

                m_tool.m_held = true;
                m_tool.m_owner = EToolGestureOwner::world;
                m_actions[kToolPressAction]->setConsumeInput(true);
                m_actions[kToolCursorAction]->setConsumeInput(true);

                if (m_tool.m_dig_armed) {
                    // Press immediately queues one accepted click (latest wins if an
                    // earlier same-poll press has not drained yet); release
                    // finalizes drag state but never un-queues it.
                    m_tool.m_accepted_click = pickCell_(m_tool.m_cursor_client);
                    m_tool.m_visited.clear();
                    m_tool.m_last_cell.reset();
                    if (m_tool.m_accepted_click.has_value()) {
                        m_tool.m_visited.push_back(*m_tool.m_accepted_click);
                        m_tool.m_last_cell = m_tool.m_accepted_click;
                    }
                    return;
                }

                // Footprint snapshot: anchor the press cell and publish the
                // preview; the dig accepted queue is never touched here.
                m_tool.m_visited.clear();
                m_tool.m_last_cell.reset();
                m_tool.m_footprint_anchor = pickCell_(m_tool.m_cursor_client);
                m_build_preview = BuildPreview{};
                m_build_preview.m_active = true;
                if (not m_tool.m_footprint_anchor.has_value()) {
                    m_build_preview.m_reason = "press outside the world";
                    return;
                }
                m_build_preview.m_anchor = *m_tool.m_footprint_anchor;
                m_build_preview.m_min = *m_tool.m_footprint_anchor;
                m_build_preview.m_max = *m_tool.m_footprint_anchor;
            } catch (...) {
                retainInputError_(currentException_());
                cancelToolGesture_();
            }
        }

        void trackToolCursor_(const float2 cursor_absolute) noexcept {
            try {
                m_tool.m_cursor_client = cursor_absolute;

                // World-origin drags stay world-owned across panels (no UI
                // re-check here); UI-origin drags never set held, so this gate
                // also stops them becoming world-owned mid-gesture.
                const bool track_drag = m_tool.m_held and
                                        m_tool.m_owner == EToolGestureOwner::world and
                                        not m_input_error;
                if (not track_drag) {
                    return;
                }

                // Slice 5: footprint drags update the preview rect only; the dig
                // visited/queue body below stays byte-identical.
                if (not m_tool.m_dig_armed) {
                    if (m_ui_state.m_tool == colony::EColonyTool::build_wall or
                        m_ui_state.m_tool == colony::EColonyTool::demolish) {
                        updateBuildPreview_();
                    }
                    return;
                }

                const std::optional<sim::GlobalCellPos> cell = pickCell_(cursor_absolute);
                if (not cell.has_value() or m_tool.m_last_cell == cell) {
                    return;
                }
                const bool seen = std::ranges::any_of(
                    m_tool.m_visited, [&](const sim::GlobalCellPos known) noexcept { return known == *cell; });
                if (seen) {
                    m_tool.m_last_cell = cell;
                    return;
                }

                // Bounded visited set (drop-oldest); an evicted cell may re-fire
                // if revisited later in the same drag.
                if (m_tool.m_visited.size() >= kMaxDragVisitedCells) {
                    m_tool.m_visited.erase(m_tool.m_visited.begin());
                }
                m_tool.m_visited.push_back(*cell);
                m_tool.m_last_cell = cell;

                // Bounded drag queue (drop-oldest tail under extreme event
                // rates); the click slot is never shed.
                if (m_tool.m_drag_queue.size() >= kMaxQueuedDragCells) {
                    m_tool.m_drag_queue.erase(m_tool.m_drag_queue.begin());
                }
                m_tool.m_drag_queue.push_back(*cell);
            } catch (...) {
                retainInputError_(currentException_());
                cancelToolGesture_();
            }
        }

        /// Slice 5 preview update: anchor to cursor, inclusive, normalized to
        /// min/max, then clamped per-axis to kMaxFootprintEdge with the press
        /// cell held fixed (the cursor side gives). A nullopt anchor or
        /// cursor pick marks the preview invalid with a reason; release then
        /// posts that reason instead of committing.
        void updateBuildPreview_() noexcept {
            if (not m_build_preview.m_active) {
                return;
            }
            if (not m_tool.m_footprint_anchor.has_value()) {
                m_build_preview.m_reason = "press outside the world";
                return;
            }
            const std::optional<sim::GlobalCellPos> cell = pickCell_(m_tool.m_cursor_client);
            if (not cell.has_value()) {
                m_build_preview.m_reason = "drag outside the world";
                return;
            }

            const i64 anchor_x = static_cast<i64>(m_tool.m_footprint_anchor->m_x);
            const i64 anchor_y = static_cast<i64>(m_tool.m_footprint_anchor->m_y);
            const i64 cursor_x = static_cast<i64>(cell->m_x);
            const i64 cursor_y = static_cast<i64>(cell->m_y);
            i64 lo_x = anchor_x < cursor_x ? anchor_x : cursor_x;
            i64 hi_x = anchor_x < cursor_x ? cursor_x : anchor_x;
            i64 lo_y = anchor_y < cursor_y ? anchor_y : cursor_y;
            i64 hi_y = anchor_y < cursor_y ? cursor_y : anchor_y;
            const i64 max_edge = static_cast<i64>(colony::kMaxFootprintEdge);
            bool clamped_x = false;
            bool clamped_y = false;
            if (hi_x - lo_x >= max_edge) {
                clamped_x = true;
                if (cursor_x >= anchor_x) {
                    hi_x = anchor_x + max_edge - 1;
                } else {
                    lo_x = anchor_x - max_edge + 1;
                }
            }
            if (hi_y - lo_y >= max_edge) {
                clamped_y = true;
                if (cursor_y >= anchor_y) {
                    hi_y = anchor_y + max_edge - 1;
                } else {
                    lo_y = anchor_y - max_edge + 1;
                }
            }

            m_build_preview.m_anchor = *m_tool.m_footprint_anchor;
            m_build_preview.m_min = sim::GlobalCellPos{static_cast<u32>(lo_x), static_cast<u32>(lo_y)};
            m_build_preview.m_max = sim::GlobalCellPos{static_cast<u32>(hi_x), static_cast<u32>(hi_y)};
            m_build_preview.m_clamped = clamped_x or clamped_y;
            m_build_preview.m_clamped_x = clamped_x;
            m_build_preview.m_clamped_y = clamped_y;
            m_build_preview.m_reason = "";
        }

        /// Slice 5 release commit: exactly one direct driver order for the
        /// preview rect, never through the dig FIFO. Invalid previews post
        /// their reason; driver rejections post with the driver error code;
        /// success names the rect and its queue state. No cells mutate on any
        /// failure path; the caller always clears the gesture after this.
        void commitBuildGesture_() noexcept {
            if (not m_build_preview.m_active) {
                return;
            }
            const bool is_wall = m_ui_state.m_tool == colony::EColonyTool::build_wall;
            if (m_build_preview.m_reason[0] != '\0' or not m_tool.m_footprint_anchor.has_value()) {
                postNotice_(
                    m_build_preview.m_reason[0] != '\0' ? m_build_preview.m_reason : "no placement to commit",
                    std::errc::invalid_argument);
                return;
            }

            const sim::GlobalCellPos min = m_build_preview.m_min;
            const u32 width = m_build_preview.m_max.m_x - min.m_x + 1u;
            const u32 height = m_build_preview.m_max.m_y - min.m_y + 1u;
            Expected<sim::Entity> placed{std::unexpect, std::make_error_code(std::errc::invalid_argument)};
            if (is_wall) {
                placed = m_driver.buildWall(
                    colony::Footprint{
                        .m_min = min, .m_width = width, .m_height = height, .m_element = colony::kElementRock});
            } else {
                placed = m_driver.demolish(min, width, height);
            }
            if (not placed) {
                const std::error_code error = placed.error();
                retainInputError_(error);
                PPR_LOG(
                    Demo, warning, "build order rejected",
                    {
                        {"w", width},
                        {"h", height},
                        {"error", error.value()},
                        {"category", error.category().name()},
                    });
                postDisplayNotice_(is_wall ? "Rock wall order rejected" : "Demolish order rejected", error);
                return;
            }

            // Success carries no error code: name the rect and whether it
            // waits on Step/Resume (paused) or progresses on its own.
            // Progress after this is driver-side; this never polls errands.
            try {
                m_status_scratch = std::string(is_wall ? "Rock wall " : "Demolish ") +
                                   std::to_string(width) +
                                   "x" +
                                   std::to_string(height) +
                                   (m_driver.paused() ? " queued (paused - Step or Resume to progress)"
                                                      : " queued - progressing");
                m_ui_state.m_notice.m_text = m_status_scratch;
                m_ui_state.m_notice.m_error = std::error_code{};
                m_ui_state.m_notice.m_seconds_left = colony::kNoticeSeconds;
            } catch (...) {
                retainInputError_(currentException_());
                m_ui_state.m_notice.m_text.clear();
                m_ui_state.m_notice.m_seconds_left = 0.0f;
            }
        }

        void completeToolGesture_() noexcept {
            // An owned release finalizes exactly once; stray releases no-op.
            const bool owned = m_tool.m_held and m_tool.m_owner == EToolGestureOwner::world;
            if (not owned) {
                return;
            }
            // Slice 5: footprint tools commit one direct driver order here;
            // dig orders drain through update() and inspect holds nothing.
            // cancelToolGesture_() below clears held, so a second release
            // can never commit twice.
            if (m_ui_state.m_tool == colony::EColonyTool::build_wall or
                m_ui_state.m_tool == colony::EColonyTool::demolish) {
                updateBuildPreview_();
                commitBuildGesture_();
            }
            cancelToolGesture_();
        }

        /// Shared screen mapping for one frame: the camera snapshot plus the
        /// coordinate rect the pick normalizes through. pickCell runs its
        /// continuous map through this struct and worldToClient is that
        /// map's exact inverse on continuous plane coordinates (before
        /// flooring/clamping) — one formula family, no second hand-derived
        /// mapping. Pointer and marker cannot diverge after pan/zoom/resize
        /// /DPI: both directions consume the same snapshot and the same
        /// rect, so a round trip lands back on the source pixel, and the
        /// overlay draws with the snapshot the scene rendered with.
        struct ScreenMap {
            CameraSnapshot m_snapshot{};
            float2 m_extent{zero_v}; // client px, == ImGui coords (see below)
            bool m_valid{false};

            /// Continuous pick map, byte-identical to the old pickCell_
            /// body: client-local input normalizes in client space directly
            /// (GLFW cursor positions are client-area relative, so dividing
            /// by the client extent is the whole map; normalized spaces are
            /// DPI scale-free, so no backend-specific flip applies).
            [[nodiscard]] std::optional<float2> pickContinuous(const float2 cursor_client) const noexcept {
                if (not m_valid) {
                    return std::nullopt;
                }
                const int2 client_px{static_cast<int>(cursor_client.x), static_cast<int>(cursor_client.y)};
                const float2 uv{
                    static_cast<float>(client_px.x) / m_extent.x, static_cast<float>(client_px.y) / m_extent.y};
                const bool inside_client = uv.x >= 0.0f and uv.x <= 1.0f and uv.y >= 0.0f and uv.y <= 1.0f;
                if (not inside_client) {
                    return std::nullopt;
                }

                // Row-vector contract: clip = world * view_projection, so the camera
                // invert maps NDC back to world via mul(clip, m_invert_view_projection).
                const float2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
                const float4 world_h = float4{ndc.x, ndc.y, 0.0f, 1.0f} * m_snapshot.m_invert_view_projection;
                const bool degenerate = world_h.w > -1e-6f and world_h.w < 1e-6f;
                if (degenerate) {
                    return std::nullopt;
                }
                return float2{world_h.x / world_h.w, world_h.y / world_h.w};
            }

            /// Cell pick: the continuous map above, then floor plus the
            /// kWorldEdge clamp. Results are unchanged from the old body.
            [[nodiscard]] std::optional<sim::GlobalCellPos> pickCell(const float2 cursor_client) const noexcept {
                const std::optional<float2> world = pickContinuous(cursor_client);
                if (not world.has_value()) {
                    return std::nullopt;
                }
                const bool inside_world = world->x >= 0.0f and
                                          world->y >= 0.0f and
                                          world->x < static_cast<float>(sim::kWorldEdge) and
                                          world->y < static_cast<float>(sim::kWorldEdge);
                if (not inside_world) {
                    return std::nullopt;
                }
                return sim::GlobalCellPos{static_cast<u32>(world->x), static_cast<u32>(world->y)};
            }

            /// Exact inverse of pickContinuous on continuous coordinates:
            /// world through the snapshot's view-projection to NDC, then
            /// the pick's uv/client steps backwards. Degenerate w and
            /// behind-camera (NDC depth outside the [0,1] ortho range)
            /// projections have no meaningful screen position and are
            /// rejected; off-screen x/y still projects (the overlay clip
            /// rect trims it). The result is client px, which ARE ImGui
            /// coords: the ImGui service sets DisplaySize to the client
            /// extent and delivers mouse positions in client px, so no
            /// framebuffer scaling leaks in here (see the framebuffer to
            /// ImGui conversion at the overlay clip).
            [[nodiscard]] std::optional<float2> worldToClient(const float2 world) const noexcept {
                if (not m_valid) {
                    return std::nullopt;
                }
                const float4 clip = float4{world.x, world.y, 0.0f, 1.0f} * m_snapshot.m_view_projection;
                const bool degenerate = clip.w > -1e-6f and clip.w < 1e-6f;
                if (degenerate) {
                    return std::nullopt;
                }
                const float rcp_w = 1.0f / clip.w;
                const float ndc_x = clip.x * rcp_w;
                const float ndc_y = clip.y * rcp_w;
                const float ndc_z = clip.z * rcp_w;
                const bool behind = ndc_z < 0.0f or ndc_z > 1.0f;
                if (behind) {
                    return std::nullopt;
                }
                const float2 uv{(ndc_x + 1.0f) * 0.5f, (1.0f - ndc_y) * 0.5f};
                return float2{uv.x * m_extent.x, uv.y * m_extent.y};
            }
        };

        /// One mapping per use from the live viewport and camera. Reads no
        /// ImGui state, so input-event paths (press/drag, possibly before
        /// the first NewFrame) stay safe; the overlay reads the IO scale
        /// itself after the panel has run.
        [[nodiscard]] ScreenMap screenMap_() const noexcept {
            ScreenMap map{};
            const safe_ptr<const WindowViewport> viewport = getMainViewport();
            const safe_ptr<const Camera> camera = getMainCamera();
            const bool have_view = viewport and camera;
            if (not have_view) {
                return map;
            }
            const Viewport &view = viewport->getViewport();
            const int2 extent = view.getClientRect().m_extent;
            if (extent.x <= 0 or extent.y <= 0) {
                return map;
            }
            map.m_snapshot = camera->getSnapshot();
            map.m_extent = float2{static_cast<float>(extent.x), static_cast<float>(extent.y)};
            map.m_valid = true;
            return map;
        }

        [[nodiscard]] std::optional<sim::GlobalCellPos> pickCell_(const float2 cursor_client) const noexcept {
            return screenMap_().pickCell(cursor_client);
        }

        void initializeInput_() {
            m_colony_mapping = std::make_unique<InputMapping>("colony_input_mapping");

            // Chord-free keys: the colony set (Space, N, 1-3, R, G, H, V, F,
            // Esc plus I, B, F1, F3) stays disjoint from the camera set
            // (WASD/QE, arrows, page keys, MMB/RMB/wheel, gamepad); every
            // key here was checked repo-wide and is bound nowhere else (F
            // confirmed free: only the key/ImGui mappings define it, and
            // Tab stays off-limits to ImGui navigation). UI still gets
            // first refusal (ImGui capture consumes before this map).
            // One table, three consumers: the key actions bound below, the
            // shortcut rows the panel renders, and (through those rows) every
            // tooltip and Help line - so no second key map can drift.
            const std::array<Binding, kColonyKeyActions> bindings{{
                {.m_kind = colony::EColonyIntent::pause_toggle,
                    .m_arg = 0u,
                    .m_key = InputKey::space_bar,
                    .m_description = "ColonyPause",
                    .m_label = "Space",
                    .m_tip = "Pause or resume the simulation"},
                {.m_kind = colony::EColonyIntent::step,
                    .m_arg = 0u,
                    .m_key = InputKey::n,
                    .m_description = "ColonyStep",
                    .m_label = "N",
                    .m_tip = "Advance one tick (while paused)"},
                {.m_kind = colony::EColonyIntent::set_speed,
                    .m_arg = 1u,
                    .m_key = InputKey::one,
                    .m_description = "ColonySpeed1",
                    .m_label = "1",
                    .m_tip = "Run at 1x speed"},
                {.m_kind = colony::EColonyIntent::set_speed,
                    .m_arg = 2u,
                    .m_key = InputKey::two,
                    .m_description = "ColonySpeed2",
                    .m_label = "2",
                    .m_tip = "Run at 2x speed"},
                {.m_kind = colony::EColonyIntent::set_speed,
                    .m_arg = 3u,
                    .m_key = InputKey::three,
                    .m_description = "ColonySpeed3",
                    .m_label = "3",
                    .m_tip = "Run at 3x speed"},
                {.m_kind = colony::EColonyIntent::regenerate,
                    .m_arg = 0u,
                    .m_key = InputKey::r,
                    .m_description = "ColonyRegenerate",
                    .m_label = "R",
                    .m_tip = "Ask to replace this world (confirm first)"},
                {.m_kind = colony::EColonyIntent::toggle_dig,
                    .m_arg = 0u,
                    .m_key = InputKey::g,
                    .m_description = "ColonyDigMode",
                    .m_label = "G",
                    .m_tip = "Arm or stow the dig tool"},
                {.m_kind = colony::EColonyIntent::toggle_edit_highlight,
                    .m_arg = 0u,
                    .m_key = InputKey::h,
                    .m_description = "ColonyHighlight",
                    .m_label = "H",
                    .m_tip = "Toggle the edit highlight"},
                {// V is free repo-wide: no game, camera (WASD/QE,
                 // arrows, page keys, mouse/wheel, gamepad, modifiers),
                 // or engine mapping binds it (definition only).
                    .m_kind = colony::EColonyIntent::toggle_grid_overlay,
                    .m_arg = 0u,
                    .m_key = InputKey::v,
                    .m_description = "ColonyGridOverlay",
                    .m_label = "V",
                    .m_tip = "Toggle the chunk grid overlay"},
                {.m_kind = colony::EColonyIntent::cancel,
                    .m_arg = 0u,
                    .m_key = InputKey::escape,
                    .m_description = "ColonyCancelGesture",
                    .m_label = "Esc",
                    .m_tip = "Stop following, or cancel the gesture, tool, pin, or Help"},
                {.m_kind = colony::EColonyIntent::select_tool,
                    .m_arg = static_cast<u32>(colony::EColonyTool::inspect),
                    .m_key = InputKey::i,
                    .m_description = "ColonyInspect",
                    .m_label = "I",
                    .m_tip = "Select the inspect tool"},
                {.m_kind = colony::EColonyIntent::open_build_menu,
                    .m_arg = 0u,
                    .m_key = InputKey::b,
                    .m_description = "ColonyBuildMenu",
                    .m_label = "B",
                    .m_tip = "Open the build menu (no placement yet)"},
                {.m_kind = colony::EColonyIntent::find_next,
                    .m_arg = 0u,
                    .m_key = InputKey::f,
                    .m_description = "ColonyFindColonist",
                    .m_label = "F",
                    .m_tip = "Select the next colonist in stable-key order"},
                {.m_kind = colony::EColonyIntent::help,
                    .m_arg = 0u,
                    .m_key = InputKey::f1,
                    .m_description = "ColonyHelp",
                    .m_label = "F1",
                    .m_tip = "Toggle this help window"},
                {.m_kind = colony::EColonyIntent::toggle_performance,
                    .m_arg = 0u,
                    .m_key = InputKey::f3,
                    .m_description = "ColonyPerformance",
                    .m_label = "F3",
                    .m_tip = "Switch the panel between Colony and Performance"},
            }};

            for (std::size_t index = 0u; index < bindings.size(); ++index) {
                const Binding &row = bindings[index];
                m_shortcut_rows[index] = colony::ColonyShortcut{
                    .m_kind = row.m_kind, .m_arg = row.m_arg, .m_key = row.m_label, .m_tip = row.m_tip};
                // Keys and buttons share this one dispatch: the callback
                // only translates a physical press into the typed intent.
                bindAction_(
                    index, row.m_description, row.m_key,
                    [this, kind = row.m_kind, arg = row.m_arg](const InputActionEvent &, const InputKey &) {
                        dispatchIntent_(colony::ColonyIntent{.m_kind = kind, .m_arg = arg});
                    });
            }

            // These keys do not overlap the camera mapping; UI still gets first refusal.
            getMainPlayer()->getListener().addInputMapping(
                m_colony_mapping, static_cast<int>(EInputMappingPriority::camera));

            // Arbitration: ImGui capture first (foreground listener consumes),
            // then tool > camera > selection. The tool mapping dispatches before
            // the camera mapping (lower priority number runs first); both tool
            // actions start transparent and only consume while the tool owns the
            // left-button gesture, retaining suppression through release. Eating
            // mouse_2d freezes camera pan/rotate (mouse_2d rotate included) until
            // the completed event or a dig-mode cancel.
            m_tool_mapping = std::make_unique<InputMapping>("colony_tool_mapping");
            m_actions[kToolPressAction] = std::make_unique<InputAction>(
                "ColonyToolPress", EInputValueType::digital, EInputActionFlags::none);
            m_actions[kToolPressAction]->setStarted([this](const InputActionEvent &, const InputKey &) noexcept {
                acquireToolGesture_();
                // Same press, inspect tool: pin/unpin the picked cell
                // (no-op for dig, which owns the gesture instead).
                observeInspectPress_();
            });
            m_actions[kToolPressAction]->setCompleted(
                [this](const InputActionEvent &, const InputKey &) noexcept { completeToolGesture_(); });
            m_tool_mapping->mapInputKey(
                SharedInputAction(m_actions[kToolPressAction].get()), InputKey::left_mouse_button);

            m_actions[kToolCursorAction] = std::make_unique<InputAction>(
                "ColonyToolCursor", EInputValueType::axis_2d, EInputActionFlags::none);
            m_actions[kToolCursorAction]->setTriggered(
                [this](const InputActionEvent &event, const InputKey &) noexcept {
                    trackToolCursor_(event.getAxis2DValue().m_absolute);
                });
            m_tool_mapping->mapInputKey(SharedInputAction(m_actions[kToolCursorAction].get()), InputKey::mouse_2d);

            getMainPlayer()->getListener().addInputMapping(
                m_tool_mapping, static_cast<int>(EInputMappingPriority::camera) - 1);

            // Focus loss cancels the transient gesture only (the accepted
            // queue survives). Ownership: the member owns the callable, the
            // broadcast holds only a view, the handle is reset first in
            // shutdown(). Absent service leaves it unwired.
            if (const safe_ptr<IWindowService> windows = getServices().tryGet<IWindowService>(); windows.isValid()) {
                m_on_window_focused = [this](const Window &, const bool focused) noexcept -> std::error_code {
                    if (not focused) {
                        cancelToolGesture_();
                    }
                    return default_value_v;
                };
                m_focus_handle = windows->whenWindowFocused(m_on_window_focused);
            }
        }

        /// The single intent dispatch: every keyboard shortcut (the binding
        /// table above) and every panel button, menu item, and section tab
        /// lands here, so button/shortcut parity holds by construction. Not
        /// noexcept: a confirmed regenerate propagates driver failures the
        /// way the old R binding did (sticky, aborting at the next frame's
        /// error check before any further driver use).
        void dispatchIntent_(const colony::ColonyIntent intent) {
            switch (intent.m_kind) {
                case colony::EColonyIntent::pause_toggle: m_driver.togglePaused(); break;

                case colony::EColonyIntent::step:
                    if (not m_driver.paused()) {
                        postNotice_("Pause to use Step", std::errc::invalid_argument);
                        break;
                    }
                    if (not m_input_error) {
                        m_input_error = m_driver.stepOne();
                    }
                    break;

                case colony::EColonyIntent::set_speed:
                    if (intent.m_arg < 1u or intent.m_arg > 3u) {
                        postNotice_("Speed must be 1x, 2x, or 3x", std::errc::invalid_argument);
                        break;
                    }
                    if (not m_input_error) {
                        m_input_error = m_driver.setSpeed(intent.m_arg);
                    }
                    break;

                case colony::EColonyIntent::regenerate:
                    if (not intent.m_confirmed) {
                        // R and "New world" only ask; the modal's Confirm
                        // re-dispatches the same intent with m_confirmed.
                        m_ui_state.m_regen_confirm_open = true;
                        break;
                    }
                    // Regenerate before the next update stages the new world.
                    m_regen_pending = true;
                    break;

                case colony::EColonyIntent::select_tool:
                    if (intent.m_arg <= static_cast<u32>(colony::EColonyTool::demolish)) {
                        selectTool_(static_cast<colony::EColonyTool>(intent.m_arg));
                    }
                    break;

                case colony::EColonyIntent::toggle_dig:
                    // Same gesture as G: arm dig, or stow back to Inspect.
                    selectTool_(
                        m_ui_state.m_tool == colony::EColonyTool::dig ? colony::EColonyTool::inspect
                                                                      : colony::EColonyTool::dig);
                    break;

                case colony::EColonyIntent::toggle_edit_highlight: m_show_highlight = not m_show_highlight; break;

                case colony::EColonyIntent::toggle_grid_overlay:
                    // Mirrors the H highlight toggle: a flag flip only. The
                    // debug submission (submitDebugInstances_) reads
                    // m_show_grid and draws the chunk-boundary bands while
                    // it stands; default off.
                    m_show_grid = not m_show_grid;
                    break;

                case colony::EColonyIntent::cancel:
                    // While a popup stood open last frame, Esc belongs to it
                    // (ImGui's NavUpdateCancelRequest closes non-modal
                    // popups; the modal handles its own). A non-modal popup
                    // does NOT raise WantCaptureKeyboard (only an active
                    // widget or a modal does), so without this gate the chain
                    // would also advance - clearing a pin the player never
                    // touched - on the very Esc that just dismissed the menu.
                    if (not m_ui_state.m_popup_visible_last_frame) {
                        cancelIntent_();
                    }
                    break;

                case colony::EColonyIntent::focus_selection:
                    // Two-step from a colonist row's Focus button: the
                    // button parks the row key in the UI state first (the
                    // intent carries no argument), so adopt it through the
                    // same selection path a world click uses, then focus.
                    // The Inspector's Focus button parks no key, so it
                    // keeps focusing whatever is already selected.
                    if (const std::optional<u64> requested = m_ui_state.m_focus_agent) {
                        m_ui_state.m_focus_agent.reset();
                        if (const std::optional<colony::AgentSummary> row = findAgent_(*requested)) {
                            selectAgent_(*row);
                        }
                    }
                    focusSelectedAgent_();
                    break;

                case colony::EColonyIntent::find_next:
                    // F and the panel's Find colonist button: one intent,
                    // one handler. Selection only - it never arms a follow;
                    // the status line names the agent through the same
                    // selectAgent_ path a world click uses.
                    selectNextAgent_();
                    break;

                case colony::EColonyIntent::toggle_performance:
                    m_ui_state.m_tab = m_ui_state.m_tab == colony::EColonyPanelTab::performance
                                           ? colony::EColonyPanelTab::colony
                                           : colony::EColonyPanelTab::performance;
                    break;

                case colony::EColonyIntent::help: m_ui_state.m_help_open = not m_ui_state.m_help_open; break;

                case colony::EColonyIntent::open_build_menu:
                    // Opens the menu only; the menu rows select the tool and
                    // placement commits on release, so B never places by itself.
                    m_ui_state.m_build_menu_open = true;
                    break;

                case colony::EColonyIntent::toggle_pin: togglePin_(); break;

                case colony::EColonyIntent::dismiss_notice: m_ui_state.m_notice = {}; break;

                case colony::EColonyIntent::select_tab:
                    if (intent.m_arg <= static_cast<u32>(colony::EColonyPanelTab::performance)) {
                        m_ui_state.m_tab = static_cast<colony::EColonyPanelTab>(intent.m_arg);
                    }
                    break;

                case colony::EColonyIntent::toggle_display:
                    // The window's draft is uncommitted scratch: closing
                    // discards it (resynced on the next open). An armed trial
                    // keeps counting while closed; expiry reverts to the
                    // known-good values, which is the safe direction.
                    m_ui_state.m_display_open = not m_ui_state.m_display_open;
                    if (not m_ui_state.m_display_open) {
                        m_ui_state.m_display.m_synced = false;
                    }
                    break;

                case colony::EColonyIntent::display_apply: submitDisplayDraft_(); break;

                case colony::EColonyIntent::display_reset_overrides: submitDisplayReset_(); break;

                case colony::EColonyIntent::display_save: saveDisplayOverlay_(); break;

                case colony::EColonyIntent::display_keep:
                    game::disarmDisplayConfirm(m_ui_state.m_display.m_confirm);
                    m_ui_state.m_display.m_revert_failed = false;
                    postDisplayNotice_("Keeping the new picture settings.", std::error_code{});
                    break;

                case colony::EColonyIntent::display_revert: revertDisplayNow_(); break;

                case colony::EColonyIntent::display_use_monitor_cap: useMonitorCapAsDraft_(); break;

                default:
                    // Future intents get a handler here; never silent.
                    break;
            }
        }

        /// Standing instruction on the status line (shown when no notice is
        /// visible); rewritten only on an actual tool change so a repeated
        /// selection cannot stomp a fresh notice's window.
        [[nodiscard]] static std::error_code currentException_() noexcept {
            try {
                throw;
            } catch (const std::bad_alloc &) {
                return std::make_error_code(std::errc::not_enough_memory);
            } catch (const std::system_error &error) {
                return error.code();
            } catch (...) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
        }

        void retainInputError_(const std::error_code error) noexcept {
            if (not m_input_error) {
                m_input_error = error;
            }
        }

        void setStatus_(const char *const text) noexcept {
            try {
                m_ui_state.m_instruction = text;
            } catch (...) {
                retainInputError_(currentException_());
                m_ui_state.m_instruction.clear();
            }
        }

        /// Bounded status-line notice: keeps the originating error code for
        /// the muted suffix and expires after colony::kNoticeSeconds. It
        /// never routes through the per-frame frame-message channel (that
        /// one is cleared every frame, so it cannot hold an error the
        /// player has time to read).
        void postNotice_(const char *const text, const std::errc error) noexcept {
            postDisplayNotice_(text, std::make_error_code(error));
        }

        /// Status notice from owned text (dynamic display messages). Same
        /// bounded lifetime as postNotice_; failures carry their code for
        /// the muted suffix, successes state their result.
        void postDisplayNotice_(const std::string_view text, const std::error_code error) noexcept {
            retainInputError_(error);
            m_ui_state.m_notice.m_error = error;
            try {
                m_ui_state.m_notice.m_text = text;
                m_ui_state.m_notice.m_seconds_left = colony::kNoticeSeconds;
            } catch (...) {
                retainInputError_(currentException_());
                m_ui_state.m_notice.m_text.clear();
                m_ui_state.m_notice.m_seconds_left = 0.0f;
            }
        }

        /// First resolution diagnostic as one bounded notice line. May
        /// allocate; every caller runs inside a try block.
        [[nodiscard]] std::string displayRejectionText_(const config::Diagnostics &diagnostics) {
            std::string text{"Picture change rejected."};
            if (not diagnostics.entries().empty()) {
                const auto &first = diagnostics.entries().front();
                text = first.m_property + ": " + first.m_message;
                if (text.size() > 180u) {
                    text.resize(180u);
                }
            }
            return text;
        }

        /// Apply: validate the draft as one runtime layer, then submit
        /// through requestDisplaySettings. Commit happens on acknowledgement
        /// only; the poll below watches for it. A queued candidate is never
        /// labeled applied. While a trial confirmation is unresolved the
        /// intent is rejected (Keep or Revert first), never silently
        /// dropped and never stacked onto the trial baseline.
        void submitDisplayDraft_() noexcept {
            try {
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                if (game::displayTrialUnresolved(session.m_confirm)) {
                    postDisplayNotice_(
                        "Confirm the trial picture settings (Keep or Revert) before applying more changes.",
                        std::make_error_code(std::errc::device_or_resource_busy));
                    return;
                }
                if (displayController().pending()) {
                    postDisplayNotice_(
                        "A picture change is already in flight.",
                        std::make_error_code(std::errc::device_or_resource_busy));
                    return;
                }

                const game::DisplayDraft draft = session.m_draft;
                config::Diagnostics diagnostics{};
                // Early rejection against the cached snapshot (stale cache
                // reads as no support, never as a pass); the transaction
                // itself re-enumerates and gates again on submit.
                const DisplayCapabilities no_capabilities{};
                const DisplayCapabilities &capabilities = m_display_has_capabilities ? m_display_capabilities
                                                                                     : no_capabilities;
                auto candidate = game::validateDisplayDraft(
                    displayController().resolved(), draft, capabilities, diagnostics);
                if (not candidate) {
                    retainInputError_(candidate.error());
                    const std::string reason = displayRejectionText_(diagnostics);
                    session.m_last_error = reason;
                    postDisplayNotice_(reason, candidate.error());
                    return;
                }

                const bool vsync_affecting = presentationVsync(candidate->m_value) !=
                                             presentationVsync(displayController().committed());
                const bool window_change = displayWindowChange(candidate->m_value, displayController().committed());
                auto geometry = game::captureDisplayTrialGeometry(
                    window_change, [this] { return displayWindowGeometry(); });
                if (not geometry) {
                    retainInputError_(geometry.error());
                    session.m_last_error = "Picture change failed: cannot capture the current window for Revert.";
                    postDisplayNotice_(session.m_last_error, geometry.error());
                    return;
                }
                session.m_watch_prior = displayController().committed();
                session.m_watch_geometry = std::move(*geometry);

                if (const std::error_code error = requestDisplaySettings(std::move(*candidate), diagnostics)) {
                    retainInputError_(error);
                    const std::string reason = diagnostics.entries().empty() ? error.message()
                                                                             : displayRejectionText_(diagnostics);
                    session.m_last_error = "Picture change failed: " + reason + ".";
                    postDisplayNotice_(session.m_last_error, error);
                    return;
                }

                session.m_watch_ack = true;
                session.m_watch_revert = false;
                session.m_watch_revision = displayController().resolved().m_revision;
                session.m_watch_vsync_affecting = vsync_affecting;
                session.m_watch_window_change = window_change;
                session.m_revert_failed = false;
            } catch (...) {
                postDisplayNotice_("Picture change failed.", currentException_());
            }
        }

        /// Reset to startup: submit the lower-precedence baseline (values
        /// and provenance from the resolved invocation) as the candidate,
        /// so acknowledgement removes the runtime assignments instead of
        /// stacking another runtime layer. Gated like Apply while a trial
        /// or transaction is open, but independently of the draft: after an
        /// acknowledgement the draft matches while overrides remain.
        void submitDisplayReset_() noexcept {
            try {
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                if (game::displayTrialUnresolved(session.m_confirm)) {
                    postDisplayNotice_(
                        "Confirm the trial picture settings (Keep or Revert) before applying more changes.",
                        std::make_error_code(std::errc::device_or_resource_busy));
                    return;
                }
                if (displayController().pending()) {
                    postDisplayNotice_(
                        "A picture change is already in flight.",
                        std::make_error_code(std::errc::device_or_resource_busy));
                    return;
                }
                if (not game::displayHasRuntimeOverrides(displayController().resolved())) {
                    postDisplayNotice_("Picture settings already match the startup values.", std::error_code{});
                    return;
                }

                config::Resolved<DisplaySettings> candidate = game::displayResetCandidate(m_display_baseline);
                auto draft = game::displayDraftFrom(candidate.m_value);
                if (not draft) {
                    postDisplayNotice_("Picture change failed.", draft.error());
                    return;
                }
                const bool vsync_affecting = presentationVsync(candidate.m_value) !=
                                             presentationVsync(displayController().committed());
                const bool window_change = displayWindowChange(candidate.m_value, displayController().committed());
                auto geometry = game::captureDisplayTrialGeometry(
                    window_change, [this] { return displayWindowGeometry(); });
                if (not geometry) {
                    retainInputError_(geometry.error());
                    session.m_last_error = "Picture change failed: cannot capture the current window for Revert.";
                    postDisplayNotice_(session.m_last_error, geometry.error());
                    return;
                }
                session.m_watch_prior = displayController().committed();
                session.m_watch_geometry = std::move(*geometry);

                config::Diagnostics diagnostics{};
                if (const std::error_code error = requestDisplaySettings(std::move(candidate), diagnostics)) {
                    retainInputError_(error);
                    const std::string reason = diagnostics.entries().empty() ? error.message()
                                                                             : displayRejectionText_(diagnostics);
                    session.m_last_error = "Picture change failed: " + reason + ".";
                    postDisplayNotice_(session.m_last_error, error);
                    return;
                }

                session.m_watch_ack = true;
                session.m_watch_revert = false;
                session.m_watch_revision = displayController().resolved().m_revision;
                session.m_watch_vsync_affecting = vsync_affecting;
                session.m_watch_window_change = window_change;
                session.m_revert_failed = false;
                session.m_draft = std::move(*draft);
                session.m_synced = true;
            } catch (...) {
                postDisplayNotice_("Picture change failed.", currentException_());
            }
        }

        /// Keep/Revert intent and trial-expiry path: submit the pre-trial
        /// requested policy pinned to the observed pre-trial geometry
        /// through the same transaction path, so explicit sizes restore the
        /// previous window instead of re-requesting "keep current size".
        /// A failed revert is an operational error: sticky notice plus the
        /// persistent window line, never a dismissed notification.
        void revertDisplayNow_() noexcept {
            try {
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                // Stop time, not the trial: failure retains the submission gate
                // and baseline until Keep or a successful explicit retry.
                session.m_confirm.m_countdown_active = false;
                if (displayController().pending()) {
                    postDisplayNotice_(
                        "A picture change is already in flight.",
                        std::make_error_code(std::errc::device_or_resource_busy));
                    return;
                }
                auto revert = game::displayRevertSettings(session.m_confirm);
                if (not revert) {
                    session.m_revert_failed = true;
                    postDisplayNotice_("Revert failed.", revert.error());
                    return;
                }
                if (game::displaySettingsEqual(*revert, displayController().committed())) {
                    game::disarmDisplayConfirm(session.m_confirm);
                    session.m_revert_failed = false;
                    postDisplayNotice_("Picture settings already match the previous values.", std::error_code{});
                    return;
                }
                config::Diagnostics diagnostics{};
                const DisplayCapabilities no_capabilities{};
                const DisplayCapabilities &capabilities = m_display_has_capabilities ? m_display_capabilities
                                                                                     : no_capabilities;
                auto draft = game::displayDraftFrom(*revert);
                if (not draft) {
                    session.m_revert_failed = true;
                    postDisplayNotice_("Revert failed.", draft.error());
                    return;
                }
                auto candidate = game::validateDisplayDraft(
                    displayController().resolved(), *draft, capabilities, diagnostics);
                if (not candidate) {
                    retainInputError_(candidate.error());
                    session.m_revert_failed = true;
                    session.m_confirm.m_revert_pending = false;
                    const std::string reason = displayRejectionText_(diagnostics);
                    session.m_last_error = reason;
                    session.m_revert_failed = true;
                    session.m_confirm.m_revert_pending = false;
                    postDisplayNotice_(reason, candidate.error());
                    return;
                }
                session.m_watch_prior = displayController().committed();
                session.m_watch_geometry = m_display_has_geometry
                                               ? std::optional<WindowDisplayGeometry>{m_display_geometry}
                                               : std::nullopt;
                if (const std::error_code error = requestDisplaySettings(std::move(*candidate), diagnostics)) {
                    retainInputError_(error);
                    session.m_revert_failed = true;
                    session.m_confirm.m_revert_pending = false;
                    const std::string reason = diagnostics.entries().empty() ? error.message()
                                                                             : displayRejectionText_(diagnostics);
                    session.m_last_error = "Revert failed: " +
                                           reason +
                                           ". The previous picture settings may still be active.";
                    session.m_revert_failed = true;
                    session.m_confirm.m_revert_pending = false;
                    postDisplayNotice_(session.m_last_error, error);
                    return;
                }
                session.m_watch_ack = true;
                session.m_watch_revert = true;
                session.m_watch_revision = displayController().resolved().m_revision;
                session.m_watch_vsync_affecting = false;
                session.m_watch_window_change = false;
                session.m_confirm.m_revert_pending = true;
            } catch (...) {
                m_ui_state.m_display.m_revert_failed = true;
                m_ui_state.m_display.m_confirm.m_revert_pending = false;
                postDisplayNotice_("Revert failed.", currentException_());
            }
        }

        /// "Use N Hz as cap" intent: draft paced mode with the reported
        /// monitor refresh, narrowed to the schema domain edge. Apply still
        /// rejects anything outside [1, 1000] with diagnostics.
        void useMonitorCapAsDraft_() noexcept {
            try {
                const colony::ColonyMonitorView *monitor = nullptr;
                for (const colony::ColonyMonitorView &entry: m_display_monitors) {
                    if (entry.m_primary) {
                        monitor = &entry;
                        break;
                    }
                }
                if (monitor == nullptr and not m_display_monitors.empty()) {
                    monitor = &m_display_monitors.front();
                }
                if (monitor == nullptr or monitor->m_refresh_hz <= 0) {
                    postDisplayNotice_("No monitor refresh reported.", std::make_error_code(std::errc::not_supported));
                    return;
                }
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                session.m_draft.m_pacing = EFramePacing::paced;
                session.m_draft.m_cap_hz = static_cast<u32>(
                    monitor->m_refresh_hz > 1000 ? 1000 : monitor->m_refresh_hz);
            } catch (...) {
                postDisplayNotice_("Monitor cap failed.", currentException_());
            }
        }

        /// Save intent: write the acknowledged values only. Committed state
        /// publishes on acknowledgement, so it always reflects successful
        /// application — never the draft. Merges over the existing overlay
        /// (unrelated entries preserved; an unreadable file is never
        /// overwritten) through a same-directory temp file plus atomic
        /// replace. Success is claimed only after the rename lands.
        void saveDisplayOverlay_() noexcept {
            try {
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                auto overlay_path = game::resolveDisplayOverlayPath();
                if (not overlay_path) {
                    retainInputError_(overlay_path.error());
                    session.m_last_error = "Picture settings NOT saved: the program folder is unavailable.";
                    postDisplayNotice_(session.m_last_error, overlay_path.error());
                    return;
                }
                config::Diagnostics diagnostics{};
                config::Document existing{};
                {
                    auto prior = game::readOverlayBytes(*overlay_path, diagnostics);
                    if (not prior) {
                        retainInputError_(prior.error());
                        session.m_last_error = "Picture settings NOT saved: the existing file is unreadable, "
                                               "refusing to overwrite it.";
                        postDisplayNotice_(session.m_last_error, prior.error());
                        return;
                    }
                    if (prior->has_value()) {
                        auto decoded = config::decode(**prior, overlay_path->string(), diagnostics);
                        if (not decoded) {
                            retainInputError_(decoded.error());
                            session.m_last_error = "Picture settings NOT saved: the existing file is unreadable, "
                                                   "refusing to overwrite it.";
                            postDisplayNotice_(session.m_last_error, decoded.error());
                            return;
                        }
                        existing = std::move(*decoded);
                    }
                }
                auto merged = game::buildDisplayOverlayDocument(existing, displayController().committed());
                if (not merged) {
                    retainInputError_(merged.error());
                    session.m_last_error = "Picture settings NOT saved: " + merged.error().message() + ".";
                    postDisplayNotice_(session.m_last_error, merged.error());
                    return;
                }
                auto bytes = game::encodeDisplayOverlay(*merged);
                if (not bytes) {
                    retainInputError_(bytes.error());
                    session.m_last_error = "Picture settings NOT saved: " + bytes.error().message() + ".";
                    postDisplayNotice_(session.m_last_error, bytes.error());
                    return;
                }
                if (const std::error_code error = game::writeFileAtomically(*overlay_path, *bytes)) {
                    retainInputError_(error);
                    session.m_last_error = "Picture settings NOT saved: " + error.message() + ".";
                    postDisplayNotice_(session.m_last_error, error);
                    return;
                }
                session.m_last_error.clear();
                if (game::displayDraftMatches(session.m_draft, displayController().committed())) {
                    postDisplayNotice_(
                        "Picture settings saved. "
                        "Command line and environment entries still take priority on next launch.",
                        std::error_code{});
                } else {
                    postDisplayNotice_(
                        "Saved the applied picture settings; open edits were not included. "
                        "Command line and environment entries still take priority.",
                        std::error_code{});
                }
            } catch (...) {
                postDisplayNotice_("Picture settings NOT saved.", currentException_());
            }
        }

        /// Monitor telemetry cache for the Display window (primary name,
        /// mode, refresh; fullscreen state). Publish only complete replacements.
        [[nodiscard]] std::error_code refreshDisplayMonitors_() noexcept {
            m_display_monitor_age = 0.0f;
            const auto unavailable = [this](const std::error_code error) noexcept {
                m_display_has_capabilities = false;
                m_display_has_geometry = false;
                m_display_capabilities_error = error;
                m_display_monitors.clear();
                m_display_fullscreen = false;
                m_display_fullscreen_monitor.clear();
                return error;
            };
            try {
                const safe_ptr<IWindowService> windows = getServices().tryGet<IWindowService>();
                if (not windows.isValid()) {
                    return unavailable(std::make_error_code(std::errc::not_connected));
                }
                Array<colony::ColonyMonitorView> fresh{};
                const std::error_code enumeration_error = windows->enumerateMonitors(
                    [&](const SharedMonitor &monitor) -> std::error_code {
                        if (not monitor.isValid() or fresh.size() >= 8u) {
                            return {};
                        }
                        const VideoMode &mode = monitor->m_video_mode;
                        fresh.push_back(
                            colony::ColonyMonitorView{
                                .m_name = monitor->m_name,
                                .m_width = mode.m_resolution.x,
                                .m_height = mode.m_resolution.y,
                                .m_refresh_hz = mode.m_refresh_rate,
                                .m_primary = monitor->m_primary_monitor,
                            });
                        return {};
                    });
                if (enumeration_error) {
                    return unavailable(enumeration_error);
                }
                auto capabilities = displayCapabilities();
                if (not capabilities) {
                    return unavailable(capabilities.error());
                }
                auto geometry = displayWindowGeometry();
                bool is_fullscreen = false;
                std::string fullscreen_monitor{};
                const SharedWindow &main_window = windows->getMainWindow();
                if (main_window.isValid()) {
                    const SharedMonitor fullscreen = windows->getWindowFullscreenMonitor(*main_window);
                    if (fullscreen.isValid()) {
                        fullscreen_monitor = fullscreen->m_name;
                        is_fullscreen = true;
                    }
                }
                m_display_monitors = std::move(fresh);
                m_display_capabilities = std::move(*capabilities);
                m_display_has_capabilities = true;
                m_display_capabilities_error = {};
                m_display_has_geometry = geometry.has_value();
                if (geometry) {
                    m_display_geometry = std::move(*geometry);
                }
                m_display_fullscreen_monitor = std::move(fullscreen_monitor);
                m_display_fullscreen = is_fullscreen;
                return {};
            } catch (...) {
                return unavailable(currentException_());
            }
        }

        /// Once-a-frame display watch: detect the acknowledgement of one
        /// submitted change (the resolved revision moves on ack only),
        /// surface failures with their code, arm the Keep/Revert trial after
        /// an acknowledged presentation change, tick the trial by this
        /// frame's post-acknowledgement delta only (the arming poll holds
        /// its delta, so a delayed acknowledgement cannot expire instantly),
        /// and expire it into a revert through the same path. Operational
        /// failures return through update without consuming unfinished watches.
        [[nodiscard]] std::error_code pollDisplaySession_(const float dt_seconds) noexcept {
            try {
                if (m_ui_state.m_display_open) {
                    m_display_monitor_age += dt_seconds;
                    if (m_display_monitor_age >= game::kDisplayMonitorRefreshSeconds) {
                        if (const std::error_code error = refreshDisplayMonitors_()) {
                            retainInputError_(error);
                            return m_input_error;
                        }
                    }
                }
                colony::ColonyDisplaySession &session = m_ui_state.m_display;
                bool armed_this_poll = false;
                if (session.m_watch_ack and not displayController().pending()) {
                    if (displayController().resolved().m_revision != session.m_watch_revision) {
                        auto draft = game::displayDraftFrom(displayController().committed());
                        if (not draft) {
                            retainInputError_(draft.error());
                            return m_input_error;
                        }
                        session.m_last_error.clear();
                        session.m_draft = std::move(*draft);
                        session.m_synced = true;
                        if (session.m_watch_revert) {
                            session.m_watch_revert = false;
                            session.m_revert_failed = false;
                            game::disarmDisplayConfirm(session.m_confirm);
                            postDisplayNotice_("Picture settings reverted.", std::error_code{});
                        } else if (session.m_watch_vsync_affecting or session.m_watch_window_change) {
                            game::armDisplayConfirmOwned(
                                session.m_confirm, std::move(session.m_watch_prior),
                                std::move(session.m_watch_geometry));
                            armed_this_poll = true;
                            postDisplayNotice_(
                                "Picture settings applied. Keep or Revert them in the Display window.",
                                std::error_code{});
                        } else {
                            postDisplayNotice_("Picture settings applied.", std::error_code{});
                        }
                    } else {
                        session.m_last_error.clear();
                        std::error_code error = displayController().transactionError();
                        if (not error) {
                            error = std::make_error_code(std::errc::operation_canceled);
                        }
                        retainInputError_(error);
                        if (session.m_watch_revert) {
                            session.m_confirm.m_revert_pending = false;
                            session.m_revert_failed = true;
                            session.m_last_error = "Revert failed: " +
                                                   error.message() +
                                                   ". The previous picture settings may still be active.";
                        } else {
                            session.m_last_error = "Picture change failed: " + error.message() + ".";
                        }
                        postDisplayNotice_(session.m_last_error, error);
                    }
                    session.m_watch_ack = false;
                    session.m_watch_revert = false;
                }
                if (game::tickDisplayConfirmPoll(session.m_confirm, dt_seconds, armed_this_poll)) {
                    revertDisplayNow_();
                }
                return m_input_error;
            } catch (...) {
                retainInputError_(currentException_());
                m_ui_state.m_notice.m_text.clear();
                m_ui_state.m_notice.m_seconds_left = 0.0f;
                return m_input_error;
            }
        }

        /// Display teardown: disarm the trial and drop the watch. A
        /// submitted-but-unacked change settles through the normal lane
        /// drain in super shutdown; this session starts nothing new here.
        void cancelDisplaySession_() noexcept {
            game::disarmDisplayConfirm(m_ui_state.m_display.m_confirm);
            m_ui_state.m_display.m_watch_ack = false;
            m_ui_state.m_display.m_watch_revert = false;
            m_ui_state.m_display_open = false;
        }

        /// Tool selection keeps the dig invariant (armed == tool == dig).
        /// Build/demolish arm the footprint gesture the release commits.
        /// Switching tools cancels any held transient gesture; the accepted
        /// dig queue stays queued per the lifetime policy above.
        void selectTool_(const colony::EColonyTool tool) noexcept {
            if (tool == m_ui_state.m_tool) {
                return;
            }

            m_ui_state.m_tool = tool;
            m_tool.m_dig_armed = tool == colony::EColonyTool::dig;
            if (m_tool.m_held) {
                cancelToolGesture_();
            }

            switch (tool) {
                case colony::EColonyTool::dig: setStatus_("Dig: click a cell to dig it, hold to drag."); break;
                case colony::EColonyTool::build_wall:
                    setStatus_("Rock wall: press and drag a rect, release to order.");
                    break;
                case colony::EColonyTool::demolish:
                    setStatus_("Demolish: press and drag a rect, release to order.");
                    break;
                case colony::EColonyTool::inspect:
                default: setStatus_("Inspect: click a cell to pin it in the Inspector."); break;
            }
        }

        /// Esc chain - one honest action per press: stop follow first
        /// (hands the camera back before touching tools or selection),
        /// then a held world gesture, a non-inspect tool, the cell pin,
        /// the agent selection, Help, and otherwise no transient state.
        void cancelIntent_() noexcept {
            if (m_pan_controller and m_pan_controller->getFollowTarget().has_value()) {
                stopFollow_();
                return;
            }
            const bool holding_world = m_tool.m_held and m_tool.m_owner == EToolGestureOwner::world;
            if (holding_world) {
                cancelToolGesture_();
                return;
            }
            if (m_ui_state.m_tool != colony::EColonyTool::inspect) {
                selectTool_(colony::EColonyTool::inspect);
                return;
            }
            if (m_ui_state.m_pinned.has_value()) {
                m_ui_state.m_pinned.reset();
                return;
            }
            if (m_selected_agent.has_value()) {
                clearAgentSelection_();
                return;
            }
            if (m_ui_state.m_help_open) {
                m_ui_state.m_help_open = false;
                return;
            }
            cancelToolGesture_();
        }

        /// Inspector pin button: a pure toggle when a target exists, an
        /// honest notice when the pointer is not over the world. An agent
        /// selection consumes the press first (single selection model).
        void togglePin_() noexcept {
            if (m_ui_state.m_pinned.has_value()) {
                m_ui_state.m_pinned.reset();
                return;
            }
            if (m_selected_agent.has_value()) {
                clearAgentSelection_();
                return;
            }
            if (m_hover.m_world) {
                m_ui_state.m_pinned = m_hover.m_target;
                return;
            }
            postNotice_("Nothing under the pointer to pin", std::errc::invalid_argument);
        }

        /// Inspect click: runs right after acquireToolGesture_ on the same
        /// press, so it reuses that function's capture rule (a press over UI
        /// never touches the world selection). Colonists win over wall
        /// cells: an agent hit pins the agent key, a wall cell pins the
        /// cell (releasing any agent), and re-clicking the active selection
        /// releases it; a click off-world says so instead of silently
        /// doing nothing.
        void observeInspectPress_() noexcept {
            if (m_ui_state.m_tool != colony::EColonyTool::inspect or m_tool.m_dig_armed) {
                return;
            }

            const safe_ptr<IUIService> ui = getServices().tryGet<IUIService>();
            if (ui and ui->hasMouseCaptureUnlessPopupClose()) {
                return;
            }

            const std::optional<sim::GlobalCellPos> cell = pickCell_(m_tool.m_cursor_client);
            if (not cell.has_value()) {
                postNotice_("Nothing under the pointer to pin", std::errc::invalid_argument);
                return;
            }

            // Cell-center world point against the submitted marker quads.
            const float2 pointer{static_cast<float>(cell->m_x) + 0.5f, static_cast<float>(cell->m_y) + 0.5f};
            if (const std::optional<u64> agent = pickAgent_(pointer)) {
                if (m_selected_agent == agent) {
                    clearAgentSelection_();
                } else if (const std::optional<colony::AgentSummary> row = findAgent_(*agent)) {
                    selectAgent_(*row);
                }
                return;
            }

            if (m_ui_state.m_pinned == cell) {
                m_ui_state.m_pinned.reset();
            } else {
                clearAgentSelection_();
                m_ui_state.m_pinned = cell;
            }
        }

        /// Stable-key agent lookup over the live summaries; nullopt when the
        /// agent is gone (deletion/regeneration). Copies the row: summaries
        /// rebuild every tick, so never hold the reference.
        [[nodiscard]] std::optional<colony::AgentSummary> findAgent_(const u64 key) const noexcept {
            for (const colony::AgentSummary &row: m_driver.agentSummaries()) {
                if (row.m_key == key) {
                    return row;
                }
            }
            return std::nullopt;
        }

        /// Agent hit-test against the submitted marker quads: same center
        /// (raw m_x/m_y) and same half-size as submitDebugInstances_
        /// (0.8 x 1.8 submitted, matching the 0.4/0.9 body halves),
        /// expanded by the scale-aware pick radius floored at
        /// kAgentPickMinWorld so the selected outer ring stays covered at
        /// the closest zoom. Nearest center wins, ties go to the lower key.
        static constexpr float kAgentPickPx = 8.0f;
        /// Minimum pick-halo radius in world units: the selected outer ring
        /// overhangs the body by gap + 1.5 * band + outer_gap, which sits at
        /// its clamps (0.10 + 1.5 * 0.25 + 0.30 = 0.775) at the closest zoom
        /// while the pixel-derived halo (kAgentPickPx * scale) keeps
        /// shrinking, so the halo keeps this floor and the outer ring stays
        /// clickable there. The inside-test only grows, so body and
        /// inner-ring clicks still land everywhere; nearest-center wins and
        /// ties still go to the lower key, and no marker moves.
        static constexpr float kAgentPickMinWorld = 0.775f;

        [[nodiscard]] std::optional<u64> pickAgent_(const float2 world_xy) const noexcept {
            if (not m_pan_controller) {
                return std::nullopt;
            }
            const float radius = std::max(kAgentPickPx * m_pan_controller->getOrthoScale(), kAgentPickMinWorld);
            std::optional<u64> best{};
            float best_d2 = std::numeric_limits<float>::max();
            for (const colony::AgentSummary &row: m_driver.agentSummaries()) {
                const float dx = world_xy.x - row.m_x;
                const float dy = world_xy.y - row.m_y;
                const bool inside = std::abs(dx) <= 0.4f + radius and std::abs(dy) <= 0.9f + radius;
                if (not inside) {
                    continue;
                }
                const float d2 = dx * dx + dy * dy;
                if (not best.has_value() or d2 < best_d2 or (d2 == best_d2 and row.m_key < *best)) {
                    best = row.m_key;
                    best_d2 = d2;
                }
            }
            return best;
        }

        /// Name a selected agent on the status line (stable key + live cell).
        void setAgentStatus_(const char *const verb, const colony::AgentSummary &row) noexcept {
            try {
                m_status_scratch = std::string(verb) +
                                   " colonist #" +
                                   std::to_string(row.m_key) +
                                   " at (" +
                                   std::to_string(static_cast<int>(row.m_x)) +
                                   ", " +
                                   std::to_string(static_cast<int>(row.m_y)) +
                                   ")";
                setStatus_(m_status_scratch.c_str());
            } catch (...) {
                retainInputError_(currentException_());
                m_ui_state.m_instruction.clear();
            }
        }

        /// Pin an agent key: releases any cell pin (single selection) and
        /// names the agent. Follow is NOT armed here; the focus intent arms
        /// it, and a live follow retargets through refreshSelection_.
        void selectAgent_(const colony::AgentSummary &row) noexcept {
            m_selected_agent = row.m_key;
            m_ui_state.m_pinned.reset();
            setAgentStatus_("Inspecting", row);
        }

        /// Release agent selection and stop follow (Esc step, regen,
        /// shutdown, deletion share it).
        void clearAgentSelection_() noexcept {
            m_selected_agent.reset();
            stopFollow_();
        }

        /// Stop follow without touching selection; the controller target is
        /// the single source of follow state.
        void stopFollow_() noexcept {
            if (m_pan_controller) {
                m_pan_controller->setFollowTarget(std::nullopt);
            }
        }

        /// focus_selection: teleport to the selected agent's live position
        /// at inspection zoom, then arm follow (refreshed every frame).
        /// Moves no agent entity; view only.
        void focusSelectedAgent_() noexcept {
            if (not m_selected_agent.has_value()) {
                postNotice_("Select a colonist first", std::errc::invalid_argument);
                return;
            }
            const std::optional<colony::AgentSummary> row = findAgent_(*m_selected_agent);
            if (not row.has_value()) {
                m_selected_agent.reset();
                stopFollow_();
                setStatus_("Selected colonist is gone.");
                return;
            }
            if (not m_pan_controller) {
                return;
            }

            // Inspection zoom: keep the current scale when already close
            // (0.05 shows ~20px/cell, 0.5 keeps context). setOrthoScale
            // cannot fail inside its own validation range.
            const float scale = std::clamp(m_pan_controller->getOrthoScale(), 0.05f, 0.5f);
            (void) m_pan_controller->setOrthoScale(scale);
            m_pan_controller->focusOn(float2{row->m_x, row->m_y});
            m_pan_controller->setFollowTarget(float3{row->m_x, row->m_y, 0.0f});
            setAgentStatus_("Following", *row);
        }

        /// find_next (F and the panel's Find colonist button): cycle the
        /// selection through live agents in stable-key order, wrapping, and
        /// name the result through the same selectAgent_ path a world click
        /// uses. Selection only - it never arms a follow.
        void selectNextAgent_() noexcept {
            const std::span<const colony::AgentSummary> rows = m_driver.agentSummaries();
            if (rows.empty()) {
                postNotice_("No colonists to select", std::errc::invalid_argument);
                return;
            }
            u64 first = std::numeric_limits<u64>::max();
            u64 next = std::numeric_limits<u64>::max();
            for (const colony::AgentSummary &row: rows) {
                first = std::min(first, row.m_key);
                const bool after_current = not m_selected_agent.has_value() or row.m_key > *m_selected_agent;
                if (after_current) {
                    next = std::min(next, row.m_key);
                }
            }
            const u64 key = next != std::numeric_limits<u64>::max() ? next : first;
            if (const std::optional<colony::AgentSummary> row = findAgent_(key)) {
                selectAgent_(*row);
            }
        }

        /// Per-frame selection validation + follow refresh, ahead of the
        /// controller update in super: a key that no longer resolves means
        /// deletion or regeneration — clear selection and follow with a
        /// status note. A live follow target refreshes from the live
        /// position; a null controller target means cancelled (manual, Esc,
        /// or focus reset) and is never re-armed here.
        void refreshSelection_() noexcept {
            if (not m_selected_agent.has_value()) {
                return;
            }
            const std::optional<colony::AgentSummary> row = findAgent_(*m_selected_agent);
            if (not row.has_value()) {
                m_selected_agent.reset();
                stopFollow_();
                setStatus_("Selected colonist is gone.");
                return;
            }
            if (m_pan_controller and m_pan_controller->getFollowTarget().has_value()) {
                m_pan_controller->setFollowTarget(float3{row->m_x, row->m_y, 0.0f});
            }
        }

        /// One hover resolution per frame feeding the dig preview outline,
        /// the status line, and the Inspector: exactly the cell a click
        /// would act on right now. A world-owned drag keeps hovering across
        /// panels (gesture contract); any other pointer over chrome
        /// suppresses the world read so a cell hidden behind a panel is
        /// never reported as hover.
        void refreshHover_() noexcept {
            m_hover = colony::ColonyHover{};
            // Slice 4 marker hover: cleared first, then re-resolved below.
            // A pointer over chrome or off-world leaves it empty, so a row
            // never mirrors a marker the pointer is no longer on.
            m_hovered_agent.reset();

            const bool world_drag = m_tool.m_held and m_tool.m_owner == EToolGestureOwner::world;
            const safe_ptr<IUIService> ui = getServices().tryGet<IUIService>();
            m_hover.m_over_ui = not world_drag and ui and ui->hasMouseCaptureUnlessPopupClose();
            if (m_hover.m_over_ui) {
                m_hover.m_reason = "pointer over the panel";
                return;
            }

            const std::optional<sim::GlobalCellPos> cell = pickCell_(m_tool.m_cursor_client);
            if (not cell.has_value()) {
                m_hover.m_reason = "outside the world";
                return;
            }
            m_hover.m_world = true;
            m_hover.m_target = *cell;

            // The marker under the pointer is the same pick a click runs
            // (cell centre first, agent beats wall cell, nearest, ties to
            // the lower key), so the hovered row and the row a click would
            // select can never disagree. Read-only: the summaries are
            // queried, never written.
            const float2 marker_pointer{static_cast<float>(cell->m_x) + 0.5f, static_cast<float>(cell->m_y) + 0.5f};
            m_hovered_agent = pickAgent_(marker_pointer);

            // getCell reads a non-resident chunk back as vacuum, so the dig
            // reason needs a separate residency probe.
            const bool resident = m_driver.grid().isResident(sim::chunkOf(*cell));
            u16 element = colony::kElementVacuum;
            if (const Expected<sim::Cell> read = m_driver.grid().getCell(*cell); read) {
                element = read->m_element;
            }
            m_hover.m_diggable = resident and element != colony::kElementVacuum;
            if (not m_tool.m_dig_armed) {
                return;
            }
            if (not resident) {
                m_hover.m_reason = "chunk not resident";
            } else if (not m_hover.m_diggable) {
                m_hover.m_reason = "vacuum: nothing to dig";
            }
        }

        /// Slice 4 needs read: live O2/rest for the colonist rows, keyed by
        /// the same stable agent key the summaries and the selection use
        /// (never a row index). Rebuilt every frame from the registry so the
        /// values are as fresh as the rows; the panel borrows them for one
        /// frame only. Read-only: no simulation state is written, so the
        /// snapshot stays pure.
        void refreshAgentNeeds_() {
            m_agent_needs_rows.clear();
            sim::Registry &registry = m_driver.registry();
            for (const auto &[entity, agent]: registry.view<colony::Agent>()) {
                const colony::Needs *needs = registry.get<colony::Needs>(entity);
                if (needs == nullptr) {
                    continue;
                }
                m_agent_needs_rows.push_back(
                    colony::AgentNeedsRow{
                        .m_key = agent.m_key,
                        .m_o2 = needs->m_o2,
                        .m_rest = needs->m_rest,
                    });
            }
        }

        /// Slice 2 deviation fix: the camera controller's viewport extent
        /// must follow the client extent every frame (it drives
        /// cursor-anchored zoom and world bounds). Explicit no-op while the
        /// extent is degenerate (minimized/zero-sized window), so the last
        /// valid extent stays in force instead of a zero push, and no-op
        /// again once the extent is unchanged.
        void refreshControllerViewport_() noexcept {
            const safe_ptr<const WindowViewport> viewport = getMainViewport();
            if (not viewport) {
                return;
            }
            const int2 extent = viewport->getViewport().getClientRect().m_extent;
            if (extent.x <= 0 or extent.y <= 0) {
                return;
            }
            const float2 next{static_cast<float>(extent.x), static_cast<float>(extent.y)};
            if (next.x == m_viewport_extent.x and next.y == m_viewport_extent.y) {
                return;
            }
            m_viewport_extent = next;
            if (m_pan_controller) {
                m_pan_controller->setViewportSize(next);
            }
        }

        /// Per-frame UI observation: the bounded notice countdown plus
        /// honest inter-frame timing (dt between update() calls) for the
        /// Performance section - never GPU time, labelled as such there.
        /// Slice 6 appends the bounded sampler push below; percentile work
        /// stays on the 15-frame stale cadence in refreshFrameStats_.
        void observeFrame_(const TimeSpan dt) noexcept {
            const float seconds = std::chrono::duration<float>(dt).count();
            if (m_ui_state.m_notice.m_seconds_left > 0.0f) {
                m_ui_state.m_notice.m_seconds_left -= seconds;
                if (m_ui_state.m_notice.m_seconds_left < 0.0f) {
                    m_ui_state.m_notice.m_seconds_left = 0.0f;
                }
            }

            const float frame_ms = seconds * 1000.0f;
            ++m_frame_count;
            m_frame_ms_last = frame_ms;
            m_frame_avg += (frame_ms - m_frame_avg) / static_cast<float>(m_frame_count);
            if (frame_ms > m_frame_max) {
                m_frame_max = frame_ms;
            }

            // Observation only: samples/statistics never drive simulation,
            // or admission decisions. Failed insertions do
            // not advance the ring; the previous statistics remain stale.
            // Sampling continues while paused - frames still tick; only the
            // sim step counters freeze, naturally, in the driver.
            try {
                if (m_frame_ring.capacity() < kFrameWindow) {
                    m_frame_ring.reserve(kFrameWindow);
                }
                if (m_frame_ring.size() < kFrameWindow) {
                    m_frame_ring.push_back(frame_ms);
                } else {
                    m_frame_ring[m_frame_head] = frame_ms;
                }
                m_frame_head = (m_frame_head + 1u) % kFrameWindow;
                ++m_frame_samples;
            } catch (...) {
                m_frame_stats_stale = true;
                return;
            }
            if (m_frame_count - m_frame_stats_frame >= kFrameStatsEvery or
                (m_frame_stats_frame == 0u and m_frame_samples > 0u)) {
                refreshFrameStats_();
            }
        }

        /// Slice 6 stale-copy refresh: nearest-rank p50/p95 (through the
        /// shared core helper, same ceil-rank convention as before) plus
        /// min, max, and over-budget count over the ring window. Runs at
        /// most every 15 frames; allocation failure - or an empty helper
        /// result - keeps the previous stale values.
        void refreshFrameStats_() noexcept {
            m_frame_stats_stale = true;
            const std::size_t count = m_frame_ring.size();
            if (count == 0u) {
                return;
            }
            try {
                m_frame_sort.resize(count);
                const auto p50 = percentile_nearest_rank(m_frame_ring, m_frame_sort, 0.5f);
                const auto p95 = percentile_nearest_rank(m_frame_ring, m_frame_sort, 0.95f);
                if (not p50 or not p95) {
                    return;
                }
                u64 over = 0u;
                for (const float sample: m_frame_sort) {
                    if (sample > kFrameBudgetMs) {
                        ++over;
                    }
                }
                m_frame_over = over;
                m_frame_p50 = *p50;
                m_frame_p95 = *p95;
                m_frame_min = m_frame_sort.front();
                m_frame_window_max = m_frame_sort.back();
                m_frame_stats_frame = m_frame_count;
                m_frame_stats_stale = false;
            } catch (...) {
                // Best-effort diagnostic statistics retain their stale flag.
            }
        }

        [[nodiscard]] std::error_code drawPanel_() {
            // Determine visible chunk tiles from the current camera snapshot.
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
                    submissions.push_back(
                        GridTileSubmission{
                            .m_chunk_id = sim::chunkIndexOf(pos),
                            .m_tile_range =
                                {min_x, min_y, min_x + static_cast<i32>(sim::kChunkEdge),
                                    min_y + static_cast<i32>(sim::kChunkEdge)},
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
            // Owned render payload: overlay/panel chunk reads consume the
            // sim packet's copies (fail closed when absent), never live
            // driver state, so post-capture mutation cannot alter reporting.
            if (not m_sim_packet) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            const std::span<const sim::ChunkPos> edits{
                m_sim_packet->m_present_chunks.data(), m_sim_packet->m_present_chunks.size()};

            // Slice 4 read-outs gathered for the panel: live needs keyed by
            // stable agent key, and the follow flag taken from the one
            // source of truth (the controller's follow target), so the Stop
            // following control only exists while a follow is actually armed.
            refreshAgentNeeds_();
            const bool following = m_pan_controller and m_pan_controller->getFollowTarget().has_value();

            // Slice 5 preview read-out for the tool strip: only a rect that
            // would actually commit reports a size - a press outside the
            // world never produced one, and its stored corners are a
            // placeholder that would lie about the footprint.
            const bool preview_rect = m_build_preview.m_active and m_build_preview.m_reason[0] == '\0';
            const u32 preview_w = preview_rect ? m_build_preview.m_max.m_x - m_build_preview.m_min.m_x + 1u : 0u;
            const u32 preview_h = preview_rect ? m_build_preview.m_max.m_y - m_build_preview.m_min.m_y + 1u : 0u;

            // Slice 6 simulation read-outs: tick rate is derived from the
            // driver's own counters (1000 * ticks / simMs); simMs is
            // SIMULATED time (tick * period), never CPU. The dig pending
            // estimate is issued-minus-applied clamped at zero (an upper
            // bound - the driver FIFO sheds under overflow).
            const u64 tick_count = m_driver.tickCount();
            const double sim_ms = m_driver.simMs();
            const double tick_hz = tick_count > 0u and sim_ms > 0.0 ? 1000.0 * static_cast<double>(tick_count) / sim_ms
                                                                    : 0.0;
            const u64 window_samples = m_frame_samples < static_cast<u64>(kFrameWindow)
                                           ? m_frame_samples
                                           : static_cast<u64>(kFrameWindow);
            const u64 dig_applied = static_cast<u64>(m_driver.digTotal());
            const u64 dig_pending_est = m_dig_issued >= dig_applied ? m_dig_issued - dig_applied : 0u;

            // One read-only view assembled from live state; the panel never
            // touches simulation state and returns at most one intent.
            const colony::ColonyUiView view{
                .m_counts =
                    colony::ColonyPanelCounts{
                        .m_visible_chunks = visible,
                        .m_visible_tiles = visible,
                        .m_paths = paths.m_paths,
                        .m_partials = paths.m_partials,
                        .m_blocked = paths.m_blocked,
                        .m_errands = m_driver.errandCount(),
                        .m_dug_total = m_driver.digTotal(),
                        .m_wake_radius = colony::kDigWakeCells,
                        .m_affected_chunks = safe_narrowing(edits.size()),
                        .m_dig_mode = m_tool.m_dig_armed,
                        .m_show_highlight = m_show_highlight,
                    },
                .m_hover = m_hover,
                .m_perf =
                    colony::ColonyPerfView{
                        .m_frame_ms = m_frame_ms_last,
                        .m_avg_frame_ms = m_frame_avg,
                        .m_max_frame_ms = m_frame_max,
                        .m_frames = m_frame_count,
                        // Lane split: main update-body wall plus the shared
                        // worker gauge (same atomic the overlay reads).
                        .m_main_ms = m_main_ms_last,
                        .m_render_ms = laneExecuteMs_(),
                        .m_p50_ms = m_frame_p50,
                        .m_p95_ms = m_frame_p95,
                        .m_min_ms = m_frame_min,
                        .m_window_max_ms = m_frame_window_max,
                        .m_over_budget = m_frame_over,
                        .m_window_samples = window_samples,
                        .m_window_def = static_cast<u32>(kFrameWindow),
                        .m_statistics_stale = m_frame_stats_stale,
                        .m_tick_hz = tick_hz,
                        .m_tick_count = tick_count,
                        .m_sim_ms = sim_ms,
                        .m_paused = m_driver.paused(),
                        .m_speed = m_driver.speed(),
                        // Read render-owned counts from completed lane results.
                        .m_submitted_chunks = renderDiagnostics().m_grid_staged_tiles,
                        .m_total_chunks = colony::ColonyTranslator::totalChunks(),
                        .m_staged_tiles = renderDiagnostics().m_grid_staged_tiles,
                        .m_cached_tiles = renderDiagnostics().m_grid_cached_tiles,
                        .m_last_edit_chunks = safe_narrowing(edits.size()),
                        .m_dirty_chunks = grid.dirtyChunkCount(),
                        .m_collider_pending = static_cast<u64>(m_driver.colliderPending()),
                        .m_live_errands = m_driver.errandCount(),
                        .m_dig_issued = m_dig_issued,
                        .m_dig_pending_est = dig_pending_est,
                        .m_gpu_validation = IRhiService::get()->isGpuAssistedValidationEnabled(),
                    },
                .m_preview =
                    colony::ColonyPreviewView{
                        .m_active = m_build_preview.m_active,
                        .m_clamped = m_build_preview.m_clamped,
                        .m_width = preview_w,
                        .m_height = preview_h,
                        .m_max_edge = colony::kMaxFootprintEdge,
                        .m_reason = m_build_preview.m_reason,
                        .m_clamped_x = m_build_preview.m_clamped_x,
                        .m_clamped_y = m_build_preview.m_clamped_y,
                    },
                .m_selected_agent = m_selected_agent,
                .m_hovered_agent = m_hovered_agent,
                .m_following = following,
                .m_agent_needs = m_agent_needs_rows,
            };

            // Display snapshot: the copied controller view (§5.1 requested
            // policy, targets, throttle reason) plus the acknowledged device
            // report, pending transaction, last failure, and the monitor
            // cache. The panel renders it read-only; applied changes return
            // as intents through dispatchIntent_ above.
            const DisplayView display_view = displayView();
            const DisplayController &display_controller = displayController();
            const std::optional<DisplayCandidate> &display_pending = display_controller.pending();
            const colony::ColonyDisplaySnapshot display_snapshot{
                .m_view = display_view,
                .m_applied = display_controller.applied(),
                .m_transaction = display_controller.transactionState(),
                .m_error = display_controller.transactionError(),
                .m_has_pending = display_pending.has_value(),
                .m_pending_id = display_pending ? display_pending->m_transaction_id : 0u,
                .m_revision = display_controller.resolved().m_revision,
                .m_has_runtime_overrides = game::displayHasRuntimeOverrides(display_controller.resolved()),
                .m_monitors =
                    std::span<const colony::ColonyMonitorView>{m_display_monitors.data(), m_display_monitors.size()},
                .m_fullscreen = m_display_fullscreen,
                .m_fullscreen_monitor = m_display_fullscreen_monitor,
                .m_capabilities = m_display_has_capabilities ? &m_display_capabilities : nullptr,
                .m_capabilities_error = m_display_capabilities_error,
                .m_geometry = m_display_has_geometry ? &m_display_geometry : nullptr,
            };

            const colony::ColonyUiFrame frame = colony::drawColonyUi(
                m_driver, view, display_snapshot, m_shortcut_rows, m_ui_state);
            if (frame.m_intent.has_value()) {
                dispatchIntent_(*frame.m_intent);
            }
            if (m_input_error) {
                return m_input_error;
            }
            // Hand the keyboard back after a completed chrome click so the
            // colony shortcuts regain capture (mirrors the engine's
            // background-click deselect). The panel never asks while a
            // popup is open: a null focus request closes every popup.
            if (frame.m_focus_release) {
                if (const safe_ptr<IUIService> ui = getServices().tryGet<IUIService>(); ui) {
                    if (const std::error_code error = ui->clearWindowFocus()) {
                        return error;
                    }
                }
            }
            // Screen-space feedback (rings/outlines/preview) draws on the
            // non-consuming foreground list after chrome, clipped to the
            // world region; input routing is untouched.
            drawFeedbackOverlay_(frame);
            // Bounded lane readout in the same overlay (total + main +
            // render (lane)); the Performance tab Split line reads the same
            // worker gauge.
            drawLaneReadout_(frame.m_chrome_bottom);
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

        /// Bounded run: exit gracefully after the given wall-clock seconds
        /// from the first update() (lazy start). Non-positive, NaN, and
        /// infinite values are rejected at the CLI; the setter stores as-is.
        void setAutoExitSeconds(const TimeDuration seconds) noexcept {
            m_auto_exit_seconds = seconds;
        }

        /// Focus-throttle opt-out: forwards to Application so profiling and
        /// bounded runs keep full frame rate when the window loses focus.
        void setNoFocusThrottle(const bool enabled) noexcept {
            Application::setNoFocusThrottle(enabled);
        }

        /// Startup picture baseline: the lower-precedence reset target with
        /// values and provenance from the resolved invocation. Captured
        /// once in main(); runtime edits never touch it.
        void noteDisplayStartup(const config::Resolved<DisplaySettings> &baseline) {
            m_display_baseline = baseline;
        }

    protected:
        [[nodiscard]] std::error_code initialize() override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::initialize());
            PPR_RETURN_ERROR_ON_FAIL(
                Demo, m_driver.init(colony::ColonyDriverDesc{.m_seed = 1234567u, .m_agent_count = m_agent_count}));
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
            // Slice 2 fixed-plane camera: world bounds keep the colony
            // on-screen at every zoom; the extent feeds cursor-anchored zoom
            // and bounds. refreshControllerViewport_() in update() now
            // follows client-extent changes every frame (Slice 3 fix).
            PPR_RETURN_ERROR_ON_FAIL(
                Demo, camera_controller->setWorldBounds(
                          float2{0.0f, 0.0f},
                          float2{static_cast<float>(sim::kWorldEdge), static_cast<float>(sim::kWorldEdge)}));
            camera_controller->setViewportSize(
                float2{static_cast<float>(client.m_extent.x), static_cast<float>(client.m_extent.y)});
            camera_controller->translate(float3{2048.0f - half_width, 2048.0f - half_height, -0.5f}, true);
            // Borrow the controller for selection and follow controls.
            m_pan_controller.reset(camera_controller.get());
            PPR_RETURN_ERROR_ON_FAIL(
                Demo, replaceMainCameraController(std::move(camera_controller), ECameraProjection::orthographic));

            initializeInput_();
            PPR_RETURN_ERROR_ON_FAIL(Demo, uploadDebugDraw_());

            // Bounded runs request normal shutdown through the application timer.
            if (m_auto_exit_seconds.has_value()) {
                PPR_LOG(Demo, warning, "auto-exit timeout setup", {{"timeout_s", m_auto_exit_seconds->count()}});

                getTimerManager().scheduleAfter(m_auto_exit_seconds.value(), [&](const TimePoint) -> std::error_code {
                    requestExit();
                    return default_value_v;
                });
            }
            return {};
        }

        [[nodiscard]] std::error_code uploadDebugDraw_() {
            // Explicit affinity-and-quiescence contract: this direct
            // upload (and the Editor's direct scene release) is legal
            // ONLY at startup before the lane owns the pass, or after
            // lane quiescence during shutdown. Mid-run it must go
            // through the staged payload transported to the owner lane.
            // Clockwise unit quad in XY facing +Z (this pipeline culls the
            // other winding); shared by capsules and path polylines.
            const mesh::StaticMeshVertex quad[4] = {
                {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f}},
                {{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f},
                    {1.0f, 1.0f, 1.0f, 1.0f}},
            };
            const u32 quad_indices[6] = {0u, 3u, 2u, 0u, 2u, 1u};

            TrianglePass &pass = getTrianglePass();
            const auto bag = pass.uploadMesh(
                std::span<const mesh::StaticMeshVertex>{quad, 4u}, std::span<const u32>{quad_indices, 6u});
            if (not bag) {
                return bag.error();
            }

            const TextureHandle no_textures[4] = {{}, {}, {}, {}};
            mesh::MaterialAsset agent_asset{};
            agent_asset.m_base_color = float4{1.0f, 0.55f, 0.15f, 1.0f};
            agent_asset.m_metallic = 0.0f;
            agent_asset.m_roughness = 1.0f;
            const auto agent_material = pass.packMaterial(agent_asset, std::span<const TextureHandle>{no_textures, 4u});
            if (not agent_material) {
                return agent_material.error();
            }

            mesh::MaterialAsset path_asset{};
            path_asset.m_base_color = float4{0.2f, 0.9f, 0.9f, 1.0f};
            path_asset.m_metallic = 0.0f;
            path_asset.m_roughness = 1.0f;
            const auto path_material = pass.packMaterial(path_asset, std::span<const TextureHandle>{no_textures, 4u});
            if (not path_material) {
                return path_material.error();
            }

            m_quad_bag = *bag;
            m_agent_material = *agent_material;
            m_path_material = *path_material;
            m_debug_draw_ready = true;
            return {};
        }

        [[nodiscard]] float4x4 debugModel_(
            const float x, const float y, const float angle, const float sx, const float sy) noexcept {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return float4x4{
                float4{sx * c, sx * s, 0.0f, 0.0f},
                float4{-sy * s, sy * c, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{x, y, -0.25f, 1.0f},
            };
        }

        [[nodiscard]] std::error_code submitChunkBorder_(
            TrianglePass::TriangleStagedFrame &stage, const sim::ChunkPos chunk, const u32 expand,
            const MaterialHandle material) {
            const float origin_x = static_cast<float>(chunk.m_x * sim::kChunkEdge);
            const float origin_y = static_cast<float>(chunk.m_y * sim::kChunkEdge);
            const float expand_f = static_cast<float>(expand);
            const float min_x = origin_x - expand_f;
            const float min_y = origin_y - expand_f;
            const float edge = static_cast<float>(sim::kChunkEdge) + expand_f * 2.0f;

            // Four one-cell borders; staged snapshots are replayed by the
            // lane onto the pass (m_instances replaces any editor
            // scene-staged entries, like the old pass.clearInstances).
            const float4x4 edges[4] = {
                debugModel_(min_x + edge * 0.5f, min_y + 0.5f, 0.0f, edge, 1.0f),
                debugModel_(min_x + edge * 0.5f, min_y + edge - 0.5f, 0.0f, edge, 1.0f),
                debugModel_(min_x + 0.5f, min_y + edge * 0.5f, 0.0f, 1.0f, edge),
                debugModel_(min_x + edge - 0.5f, min_y + edge * 0.5f, 0.0f, 1.0f, edge),
            };
            for (const float4x4 &model: edges) {
                stage.m_instances.push_back(
                    TrianglePass::SubmittedInstance{.m_bag = m_quad_bag, .m_material = material, .m_model = model});
            }
            return {};
        }

        /// One band (a single quad) through the shared bag and an existing
        /// material. Serves the grid overlay and the edit/wake chunk
        /// outlines that remain in the pass (no new GPU handles).
        [[nodiscard]] std::error_code submitBand_(
            TrianglePass::TriangleStagedFrame &stage, const float cx, const float cy, const float sx, const float sy,
            const MaterialHandle material) {
            stage.m_instances.push_back(
                TrianglePass::SubmittedInstance{
                    .m_bag = m_quad_bag, .m_material = material, .m_model = debugModel_(cx, cy, 0.0f, sx, sy)});
            return {};
        }

        /// Screen-space feedback palette: the same cyan/orange the debug-draw
        /// materials use, so overlay, chrome, and (remaining) world outlines
        /// agree. Cyan marks selection and diggable/pinned structure; orange
        /// marks hover and the footprint preview.
        inline static const ImU32 kFeedbackCyan = ImGui::ColorConvertFloat4ToU32(ImVec4{0.2f, 0.9f, 0.9f, 1.0f});
        inline static const ImU32 kFeedbackOrange = ImGui::ColorConvertFloat4ToU32(ImVec4{1.0f, 0.55f, 0.15f, 1.0f});

        /// Projection-derived pixels per world unit at a world point: two
        /// unit-separated points through the same forward formula the pick
        /// inverts, so screen-space line weights match the old
        /// pixel-derived band at every zoom with no second mapping.
        [[nodiscard]] static std::optional<float> overlayPixelsPerWorld_(
            const ScreenMap &map, const float2 at) noexcept {
            const std::optional<float2> origin = map.worldToClient(at);
            const std::optional<float2> unit_x = map.worldToClient(float2{at.x + 1.0f, at.y});
            if (not origin.has_value() or not unit_x.has_value()) {
                return std::nullopt;
            }
            const float ppw = std::abs(unit_x->x - origin->x);
            if (not (ppw > 0.0f) or not std::isfinite(ppw)) {
                return std::nullopt;
            }
            return ppw;
        }

        /// Project a world rect to ImGui coords. The ortho map is
        /// axis-aligned, so projecting two opposite corners is the whole
        /// rect; false when any corner has no screen position (degenerate
        /// or behind-camera), and the caller skips that shape.
        [[nodiscard]] static bool overlayProjectRect_(
            const ScreenMap &map, const float x0, const float y0, const float x1, const float y1, ImVec2 &min_out,
            ImVec2 &max_out) noexcept {
            const std::optional<float2> a = map.worldToClient(float2{x0, y0});
            const std::optional<float2> b = map.worldToClient(float2{x1, y1});
            if (not a.has_value() or not b.has_value()) {
                return false;
            }
            min_out = ImVec2{std::min(a->x, b->x), std::min(a->y, b->y)};
            max_out = ImVec2{std::max(a->x, b->x), std::max(a->y, b->y)};
            return true;
        }

        /// One rectangular outline in screen space: an ImGui rect border
        /// whose thickness centres on the edge, matching the old bands that
        /// straddled the rect edges half in and half out.
        static void overlayRectBorder_(
            ImDrawList *const draw, const ScreenMap &map, const float cx, const float cy, const float hx,
            const float hy, const float band_world, const ImU32 color) {
            if (draw == nullptr) {
                return;
            }
            ImVec2 min{};
            ImVec2 max{};
            if (not overlayProjectRect_(map, cx - hx, cy - hy, cx + hx, cy + hy, min, max)) {
                return;
            }
            const std::optional<float> ppw = overlayPixelsPerWorld_(map, float2{cx, cy});
            if (not ppw.has_value()) {
                return;
            }
            draw->AddRect(min, max, color, 0.0f, ImDrawFlags_None, band_world * *ppw);
        }

        /// One hovered/pinned cell outline: the shared rect-border helper at
        /// the cell centre with half extents 0.5 and the standing 0.5-cell
        /// band, exactly as before.
        static void overlayCellBorder_(
            ImDrawList *const draw, const ScreenMap &map, const sim::GlobalCellPos cell, const ImU32 color) {
            overlayRectBorder_(
                draw, map, static_cast<float>(cell.m_x) + 0.5f, static_cast<float>(cell.m_y) + 0.5f, 0.5f, 0.5f, 0.5f,
                color);
        }

        /// Footprint preview in screen space: the inclusive min/max corners
        /// become one rect outlined at the same pixel-derived band the
        /// marker rings use, in the build palette's orange, with the same
        /// three states as before (valid solid, clamped with badges on the
        /// capped sides, invalid broken with corner gaps). The reason stays
        /// text in the tool strip, never drawn into the world.
        void overlayFootprintPreview_(ImDrawList *const draw, const ScreenMap &map) {
            if (draw == nullptr) {
                return;
            }
            const bool committable = m_build_preview.m_reason[0] == '\0';
            if (not committable and not m_tool.m_footprint_anchor.has_value()) {
                return;
            }

            const float x0 = static_cast<float>(m_build_preview.m_min.m_x);
            const float y0 = static_cast<float>(m_build_preview.m_min.m_y);
            const float x1 = static_cast<float>(m_build_preview.m_max.m_x) + 1.0f;
            const float y1 = static_cast<float>(m_build_preview.m_max.m_y) + 1.0f;
            const float cx = (x0 + x1) * 0.5f;
            const float cy = (y0 + y1) * 0.5f;
            const float w = x1 - x0;
            const float h = y1 - y0;

            const float scale = m_pan_controller ? m_pan_controller->getOrthoScale() : 0.5f;
            const float band = std::clamp(scale * 1.5f, 0.25f, 0.75f);
            const float gap = std::clamp(scale * 1.0f, 0.10f, 0.50f);

            if (not committable) {
                // Broken outline: each side at 62% length leaves visible
                // gaps at the corners, and the thinner band sits below the
                // solid weight without ever vanishing at any zoom.
                constexpr float kSpan = 0.62f;
                const float thin = std::max(band * 0.55f, 0.15f);
                const std::optional<float> ppw = overlayPixelsPerWorld_(map, float2{cx, cy});
                if (not ppw.has_value()) {
                    return;
                }
                const float thickness = thin * *ppw;
                const std::optional<float2> top_a = map.worldToClient(float2{cx - w * kSpan * 0.5f, y0});
                const std::optional<float2> top_b = map.worldToClient(float2{cx + w * kSpan * 0.5f, y0});
                const std::optional<float2> bottom_a = map.worldToClient(float2{cx - w * kSpan * 0.5f, y1});
                const std::optional<float2> bottom_b = map.worldToClient(float2{cx + w * kSpan * 0.5f, y1});
                const std::optional<float2> left_a = map.worldToClient(float2{x0, cy - h * kSpan * 0.5f});
                const std::optional<float2> left_b = map.worldToClient(float2{x0, cy + h * kSpan * 0.5f});
                const std::optional<float2> right_a = map.worldToClient(float2{x1, cy - h * kSpan * 0.5f});
                const std::optional<float2> right_b = map.worldToClient(float2{x1, cy + h * kSpan * 0.5f});
                const bool complete = top_a.has_value() and
                                      top_b.has_value() and
                                      bottom_a.has_value() and
                                      bottom_b.has_value() and
                                      left_a.has_value() and
                                      left_b.has_value() and
                                      right_a.has_value() and
                                      right_b.has_value();
                if (not complete) {
                    return;
                }
                draw->AddLine(ImVec2{top_a->x, top_a->y}, ImVec2{top_b->x, top_b->y}, kFeedbackOrange, thickness);
                draw->AddLine(
                    ImVec2{bottom_a->x, bottom_a->y}, ImVec2{bottom_b->x, bottom_b->y}, kFeedbackOrange, thickness);
                draw->AddLine(ImVec2{left_a->x, left_a->y}, ImVec2{left_b->x, left_b->y}, kFeedbackOrange, thickness);
                draw->AddLine(
                    ImVec2{right_a->x, right_a->y}, ImVec2{right_b->x, right_b->y}, kFeedbackOrange, thickness);
                return;
            }

            overlayRectBorder_(draw, map, cx, cy, w * 0.5f, h * 0.5f, band, kFeedbackOrange);
            if (not m_build_preview.m_clamped) {
                return;
            }

            // Clamp badges: a 1.8x stroke laid one gap outside each cap
            // edge (outside = the direction the drag kept wanting to go),
            // flush with the outline's own ends. An axis is only badged
            // when it was actually cut back (its own clamped flag) and sits
            // at the cap, so a naturally full-width axis is never badged;
            // the badged side is the one away from the anchored corner -
            // the side the clamp gave back. The strip reports the same cap
            // in words, so the badge never carries the meaning alone.
            const bool cap_x = m_build_preview.m_clamped_x and (w == static_cast<float>(colony::kMaxFootprintEdge));
            const bool cap_y = m_build_preview.m_clamped_y and (h == static_cast<float>(colony::kMaxFootprintEdge));
            if (not cap_x and not cap_y) {
                return;
            }

            const float badge = band * 1.8f;
            const float offset = band * 0.5f + gap + badge * 0.5f;
            if (cap_x) {
                const bool cut_max_x = m_build_preview.m_anchor.m_x == m_build_preview.m_min.m_x;
                const float edge = cut_max_x ? x1 : x0;
                const float dir = cut_max_x ? offset : -offset;
                ImVec2 min{};
                ImVec2 max{};
                if (overlayProjectRect_(
                        map, edge + dir - badge * 0.5f, cy - (h + band) * 0.5f, edge + dir + badge * 0.5f,
                        cy + (h + band) * 0.5f, min, max)) {
                    draw->AddRectFilled(min, max, kFeedbackOrange);
                }
            }
            if (cap_y) {
                const bool cut_max_y = m_build_preview.m_anchor.m_y == m_build_preview.m_min.m_y;
                const float edge = cut_max_y ? y1 : y0;
                const float dir = cut_max_y ? offset : -offset;
                ImVec2 min{};
                ImVec2 max{};
                if (overlayProjectRect_(
                        map, cx - (w + band) * 0.5f, edge + dir - badge * 0.5f, cx + (w + band) * 0.5f,
                        edge + dir + badge * 0.5f, min, max)) {
                    draw->AddRectFilled(min, max, kFeedbackOrange);
                }
            }
        }

        /// Screen-space feedback on the non-consuming ImGui foreground draw
        /// list: no window, no capture, input routing untouched. Bound to
        /// live agents plus hover, selection, and the active preview.
        /// Chrome-occlusion policy: feedback is CLIPPED to the world region
        /// and never drawn over panels. The clip rect outer bounds come from
        /// the real framebuffer-space viewport rect (client extent times the
        /// content scale) converted to ImGui coords through
        /// DisplayFramebufferScale — verified against the ImGui service
        /// setup, which sets DisplaySize from the client extent and
        /// DisplayFramebufferScale from the content scale — outward-rounded
        /// so the clip stays conservative, then inset by this frame's panel
        /// chrome (top bar, status line, side panel). A viewport/IO mismatch
        /// skips the overlay rather than misplacing feedback.
        void drawFeedbackOverlay_(const colony::ColonyUiFrame &frame) {
            const ScreenMap map = screenMap_();
            if (not map.m_valid) {
                return;
            }
            const ImGuiIO &io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.0f or io.DisplaySize.y <= 0.0f) {
                return;
            }
            const bool extent_match = std::abs(io.DisplaySize.x - map.m_extent.x) <= 1.0f and
                                      std::abs(io.DisplaySize.y - map.m_extent.y) <= 1.0f;
            if (not extent_match) {
                return;
            }
            if (io.DisplayFramebufferScale.x <= 0.0f or io.DisplayFramebufferScale.y <= 0.0f) {
                return;
            }
            ImDrawList *draw = ImGui::GetForegroundDrawList();
            if (draw == nullptr) {
                return;
            }

            // Clip source: framebuffer viewport rect to ImGui coords.
            const float fb_w = map.m_extent.x * io.DisplayFramebufferScale.x;
            const float fb_h = map.m_extent.y * io.DisplayFramebufferScale.y;
            const float ui_w = fb_w / io.DisplayFramebufferScale.x;
            const float ui_h = fb_h / io.DisplayFramebufferScale.y;
            const ImVec2 clip_min{0.0f, frame.m_chrome_top};
            const ImVec2 clip_max{std::ceil(ui_w) - frame.m_chrome_right, std::ceil(ui_h) - frame.m_chrome_bottom};
            const bool clip_ok = clip_max.x > clip_min.x and clip_max.y > clip_min.y;
            if (not clip_ok) {
                return;
            }
            draw->PushClipRect(clip_min, clip_max, true);
            struct ClipScope {
                ImDrawList *draw;
                ~ClipScope() noexcept {
                    draw->PopClipRect();
                }
            } clip_scope{draw};

            // Colonist outlines, one treatment per agent — a persistent
            // selection outranks a transient hover, so the two never draw
            // on top of each other. Band and gaps keep the standing
            // pixel-derived formulas, so the weights match at every zoom;
            // the ring centre sits one gap outside the body plus half a
            // band, derived from the same 0.4 x 0.9 halves the hit-test
            // uses. The bolder hover band grows inward no further than
            // the body edge (gap >= 0.4 * band at every clamped zoom), so
            // it never covers the marker it outlines.
            const float scale = m_pan_controller ? m_pan_controller->getOrthoScale() : 0.5f;
            const float band = std::clamp(scale * 1.5f, 0.25f, 0.75f);
            const float gap = std::clamp(scale * 1.0f, 0.10f, 0.50f);
            const float outer_gap = std::clamp(scale * 3.0f, 0.30f, 1.50f);
            for (const colony::AgentSummary &agent: m_driver.agentSummaries()) {
                // Ring centre sits one gap outside the body, plus half a band.
                const float ring_x = 0.4f + gap + band * 0.5f;
                const float ring_y = 0.9f + gap + band * 0.5f;

                if (agent.m_key == m_selected_agent) {
                    overlayRectBorder_(draw, map, agent.m_x, agent.m_y, ring_x, ring_y, band, kFeedbackCyan);
                    // Outer ring: one outer_gap clear of the inner ring's
                    // outer edge, same band width.
                    const float outer_x = ring_x + outer_gap + band;
                    const float outer_y = ring_y + outer_gap + band;
                    overlayRectBorder_(draw, map, agent.m_x, agent.m_y, outer_x, outer_y, band, kFeedbackCyan);
                } else if (agent.m_key == m_hovered_agent) {
                    overlayRectBorder_(draw, map, agent.m_x, agent.m_y, ring_x, ring_y, band * 1.8f, kFeedbackOrange);
                } else {
                    overlayRectBorder_(draw, map, agent.m_x, agent.m_y, ring_x, ring_y, band, kFeedbackCyan);
                }
            }

            // Pointer preview in the same order the pass submission used:
            // a held footprint drag outlines the rect being sized (most
            // specific state first, dig stays out while a footprint tool
            // is active); the pinned outline stays independent of the
            // drag preview (both draw, the preview on top), so a held
            // drag never hides the pin; while dig is armed, the cell a
            // click would dig draws (cyan when diggable, agent orange
            // when it would not land); otherwise the pinned cell draws.
            const bool footprint_tool = m_ui_state.m_tool == colony::EColonyTool::build_wall or
                                        m_ui_state.m_tool == colony::EColonyTool::demolish;
            if (m_build_preview.m_active and footprint_tool) {
                if (m_ui_state.m_pinned.has_value()) {
                    overlayCellBorder_(draw, map, *m_ui_state.m_pinned, kFeedbackCyan);
                }
                overlayFootprintPreview_(draw, map);
            } else if (m_tool.m_dig_armed and m_hover.m_world) {
                const ImU32 color = m_hover.m_diggable ? kFeedbackCyan : kFeedbackOrange;
                overlayCellBorder_(draw, map, m_hover.m_target, color);
            } else if (m_ui_state.m_pinned.has_value()) {
                overlayCellBorder_(draw, map, *m_ui_state.m_pinned, kFeedbackCyan);
            }
        }

        /// Bounded lane readout: one compact line in the same ImGui overlay
        /// as the panel — total (inter-frame cadence) + main (update-body
        /// wall) + render (lane) (last completed lane execution from the
        /// worker-published gauge, the same atomic the Performance tab
        /// reads). Never handoff cost: m_render_ms_last (see render())
        /// stays a main-side wall number only. Values lag one frame
        /// (render runs after this panel). Non-interactive (NoInputs)
        /// so world picking is unaffected; skipped while the display is
        /// degenerate.
        void drawLaneReadout_(const float chrome_bottom) {
            const ImGuiIO &io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.0f or io.DisplaySize.y <= 0.0f) {
                return;
            }
            ImGui::SetNextWindowPos(
                ImVec2{8.0f, io.DisplaySize.y - chrome_bottom - 8.0f}, ImGuiCond_Always, ImVec2{0.0f, 1.0f});
            ImGui::SetNextWindowBgAlpha(0.55f);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                               ImGuiWindowFlags_NoInputs |
                                               ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoFocusOnAppearing |
                                               ImGuiWindowFlags_NoNav |
                                               ImGuiWindowFlags_NoMove;
            const bool visible = ImGui::Begin("##frame_lanes", nullptr, flags);
            struct WindowScope {
                ~WindowScope() noexcept {
                    ImGui::End();
                }
            } window_scope;
            if (not visible) {
                return;
            }
            // Honest render number: the display-only last-execute gauge
            // (worker thread, relaxed). 0.00 until the first lane frame
            // completes; never the main-side handoff cost.
            ImGui::Text(
                "Frame total %.2f ms  main %.2f ms  render (lane) %.2f ms", m_frame_ms_last, m_main_ms_last,
                laneExecuteMs_());
        }

        [[nodiscard]] std::error_code submitDebugInstances_() {
            // P2 pass-affinity: build the staged payload (owned snapshot of
            // {bag, material, model}); the lane's triangle-pass replay
            // CLEARS and re-submits all instances from it. This replaces the
            // old pass.clearInstances()/submitInstance() loop that ran on
            // main and raced the render lane's reads of the same pass state.
            TrianglePass::TriangleStagedFrame stage{};
            if (not m_debug_draw_ready) {
                submitTriangleStage(std::move(stage));
                return {};
            }

            // Slice 6 chunk grid overlay (V, off by default): one band
            // straddling every chunk boundary across the live world, and it
            // is submitted first so every body, path, and edit outline below
            // draws on top of it. The weight is half the old marker band's
            // while still coming from the same pixel-derived clamp, so the
            // grid stays a quiet reference at
            // every zoom (it never bloats close-in, never vanishes
            // zoomed-out, and never outshines a marker), and
            // cyan keeps it in the structural language shared by paths and
            // markers. Where it meets an edited
            // chunk's own outline (H) the two are the same line: that
            // outline is the fixed 0.5-cell stroke, always the heavier of
            // the pair, so the edit still reads as the marked one. The
            // band helper and the shared quad bag/materials are reused -
            // no new GPU handles.
            if (m_show_grid) {
                const float grid_scale = m_pan_controller ? m_pan_controller->getOrthoScale() : 0.5f;
                const float grid_band = std::max(std::clamp(grid_scale * 1.5f, 0.25f, 0.75f) * 0.5f, 0.10f);
                const float world_half = static_cast<float>(sim::kWorldEdge) * 0.5f;
                const float stroke = grid_band * 0.5f;
                for (u32 line = 0u; line <= sim::kChunksPerEdge; ++line) {
                    const float at = static_cast<float>(line * sim::kChunkEdge);

                    // Horizontal (y = at) and vertical (x = at) lines, each
                    // centred on the boundary and spanning the whole world.
                    if (const std::error_code error = submitBand_(
                            stage, world_half, at, world_half, stroke, m_path_material)) {
                        return error;
                    }
                    if (const std::error_code error = submitBand_(
                            stage, at, world_half, stroke, world_half, m_path_material)) {
                        return error;
                    }
                }
            }

            for (const colony::AgentSummary &agent: m_driver.agentSummaries()) {
                stage.m_instances.push_back(
                    TrianglePass::SubmittedInstance{
                        .m_bag = m_quad_bag,
                        .m_material = m_agent_material,
                        .m_model = debugModel_(agent.m_x, agent.m_y, 0.0f, 0.8f, 1.8f)});
            }

            // Agent rings, hover/selection outlines, and the pointer
            // preview now draw in the screen-space feedback overlay
            // (drawFeedbackOverlay_); the pass keeps bodies, paths, the grid
            // overlay, and the edit/wake outlines below.
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
                        stage.m_instances.push_back(
                            TrianglePass::SubmittedInstance{
                                .m_bag = m_quad_bag,
                                .m_material = m_path_material,
                                .m_model = debugModel_(
                                    (px + qx) * 0.5f, (py + qy) * 0.5f, std::atan2(dy, dx), length, 0.3f)});
                    }
                    px = qx;
                    py = qy;
                }
            }

            // Overlay phase: last-edit chunk outlines plus the wake extent. The
            // quad bag and both materials are reused (no new GPU handles); the
            // world-space instances encode with the same camera snapshot as the
            // agent/path instances above in render(). Chunks come from the
            // owned sim packet (fail closed when absent), never the live
            // driver, so post-capture edits cannot alter this frame's overlay.
            if (m_show_highlight) {
                if (not m_sim_packet) {
                    return std::make_error_code(std::errc::state_not_recoverable);
                }
                for (const sim::ChunkPos chunk: m_sim_packet->m_present_chunks) {
                    if (const std::error_code error = submitChunkBorder_(stage, chunk, 0u, m_path_material)) {
                        return error;
                    }
                    if (const std::error_code error = submitChunkBorder_(
                            stage, chunk, colony::kDigWakeCells, m_agent_material)) {
                        return error;
                    }
                }
            }

            // Pointer preview (dig/pinned outlines, footprint preview) now
            // draws in the screen-space feedback overlay; the pass keeps the
            // edit/wake outlines above.
            submitTriangleStage(std::move(stage));
            return {};
        }

        // P2 consumption-deferred ack: fired once per frame result (lane
        // pump or inline execute). Only an applied grid frame acks the
        // driver's dirty set; a dropped or failed frame leaves its chunks
        // pending so the next buildable packet re-stages them. Full-refresh
        // + cache-reset armed flags consumed here; re-armed for replay on
        // drop/rail so recovery never goes through the wrong thread.
        // Durable slot settlement: applies to every recorded grid slot —
        // applied success acks the driver's captured dirty set (only when
        // the seed + per-chunk revisions still match the live dirty set,
        // matched clearing in ColonyDriver), and clears armed full-refresh;
        // any non-applied outcome re-arms the same armed flags for the
        // retry without touching the ack. GPU retirement stay distinct:
        // it lives in the fence/ledger; this hook owns only CPU-consumption.
        void onLaneFrameConsumed_(const RenderFrameResult &result) override {
            ApplicationEditor::onLaneFrameConsumed_(result);
            // Same identity gate as the base hook: a settling result whose
            // packet matches the driver's seed but belongs to a previous run
            // would otherwise ack world edits the driver never re-staged.
            if (result.m_reservation_epoch != 0u and result.m_reservation_epoch != reservationEpoch_()) {
                return;
            }
            const bool generations_match = result.m_grid_ack_seed == m_driver.worldGeneration();
            if (result.m_grid_stage_applied and not result.m_dropped and not result.m_error) {
                // Ack + full-refresh clear ride ONLY the genuine world
                // generation: a stale result (previous world) can never
                // clear or ack current state.
                if (generations_match) {
                    Array<colony::ColonyDriver::DirtyChunk> ack{};
                    ack.reserve(result.m_grid_ack_chunk_ids.size());
                    for (u64 i = 0u; i < result.m_grid_ack_chunk_ids.size(); ++i) {
                        const u32 id = result.m_grid_ack_chunk_ids[i];
                        ack.push_back(
                            colony::ColonyDriver::DirtyChunk{
                                .m_pos = sim::ChunkPos{id % sim::kChunksPerEdge, id / sim::kChunksPerEdge},
                                .m_revision = result.m_grid_ack_revisions[i],
                            });
                    }
                    m_driver.ackPresentChunks(std::span<const colony::ColonyDriver::DirtyChunk>{ack});
                    if (result.m_grid_full_refresh) {
                        m_present_full_refresh = false;
                    }
                }
                return;
            }
            if (generations_match) {
                // Non-applied outcome re-arms this world's flags for the
                // retry; a stale (foreign-generation) result must never
                // resurrect flags the current world never had armed.
                if (result.m_grid_reset_caches) {
                    m_grid_reset_pending = true;
                }
                if (result.m_grid_full_refresh) {
                    m_present_full_refresh = true;
                }
            }
        }

        [[nodiscard]] std::error_code update(const TimeSpan dt) override {
            // Lane split, main boundary: wall time of this whole update
            // body on the main thread (client portion). RAII so every
            // early return below still stamps; two steady_clock reads, no
            // allocation.
            struct MainLaneScope {
                TurboLarbin *const m_owner;
                std::chrono::steady_clock::time_point m_start;
                ~MainLaneScope() noexcept {
                    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
                    m_owner->m_main_ms_last = std::chrono::duration<float, std::milli>(end - m_start).count();
                }
            };
            const MainLaneScope main_lane{this, std::chrono::steady_clock::now()};
            // Phase: regenerate. A confirmed regeneration runs at the very
            // head, ahead of every other phase: regen ends any held gesture
            // (accepted commands stay queued per policy and dig into the
            // fresh world), drops the pin, and schedules a full presentation
            // refresh for the new world.
            if (m_regen_pending) {
                m_regen_pending = false;
                cancelToolGesture_();
                m_ui_state.m_pinned.reset();
                // Stable keys die with the world: selection and follow go
                // here, before regenerate_() rebuilds the summaries.
                clearAgentSelection_();
                // Slice 6: the driver's digTotal resets on the same regen
                // path (clearToolState), so the game-side issued count
                // resets here to keep the pending estimate aligned.
                m_dig_issued = 0u;
                if (not m_input_error) {
                    m_input_error = regenerate_();
                }
                if (m_input_error) [[unlikely]] {
                    return m_input_error;
                }
            }

            // Phase: UI observation. Notice countdown + honest inter-frame
            // timing first, so this frame's Performance section reports the
            // dt this frame is about to run with.
            observeFrame_(dt);

            // Phase: camera viewport. Follow client-extent changes before
            // input runs (Slice 2 deviation fixed); explicit no-op while the
            // extent is degenerate or unchanged.
            refreshControllerViewport_();

            // Phase: selection. Validate the agent key and refresh an
            // active follow target from the live position, ahead of the
            // controller update in super so this frame already tracks.
            refreshSelection_();

            // Input poll, camera, pass updates, and UI run in super; pass encode
            // and present run in render(), after this body, so the submits below
            // still land in the same frame's render.
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::update(dt));
            if (m_input_error) [[unlikely]] {
                return m_input_error;
            }

            // Non-admitted frames never reach update(): the run-level gate
            // leaves busy frames before the whole virtual stage, and
            // platform/control was already serviced there. Queued tool intent
            // and undriven sim state wait for the next admitted frame. Busy
            // never fails. Sim-time preservation (B2a): skipped dt
            // accumulated into Application::m_pending_skipped_dt by the
            // run-level busy branch, so the next admitted frame's update()
            // receives dt + carry and ColonyDriver::update bounds catch-up
            // to its existing 5-slice clamp (FixedTimestep defer backlog
            // carries any remainder).

            // Phase: input/edit. Accepted commands drain in accept order
            // (click first, then drag cells); pop-then-issue so a failing
            // command cannot wedge the queue. A same-poll press/release
            // still digs exactly once; errors stay sticky and abort.
            if (m_tool.m_accepted_click.has_value()) {
                const sim::GlobalCellPos cell = *m_tool.m_accepted_click;
                m_tool.m_accepted_click.reset();
                if (const std::error_code error = m_driver.requestDig(cell, 1u, 1u)) {
                    PPR_LOG(
                        Demo, warning, "tool drain failed",
                        {
                            {"x", cell.m_x},
                            {"y", cell.m_y},
                            {"error", error.value()},
                            {"category", error.category().name()},
                        });
                    m_input_error = error;
                    return error;
                }
                // Slice 6: one cell issued (only on success, so the
                // issued-minus-applied estimate never counts rejections).
                ++m_dig_issued;
            }
            while (not m_tool.m_drag_queue.empty()) {
                const sim::GlobalCellPos cell = m_tool.m_drag_queue.front();
                m_tool.m_drag_queue.erase(m_tool.m_drag_queue.begin());
                if (const std::error_code error = m_driver.requestDig(cell, 1u, 1u)) {
                    PPR_LOG(
                        Demo, warning, "tool drain failed",
                        {
                            {"x", cell.m_x},
                            {"y", cell.m_y},
                            {"error", error.value()},
                            {"category", error.category().name()},
                        });
                    m_input_error = error;
                    return error;
                }
                ++m_dig_issued;
            }

            // Phase: simulate. Paused updates apply the pending tool intent with
            // zero runSystems; zero-tick frames keep the intent queued for the
            // next tick.
            PPR_RETURN_ERROR_ON_FAIL(Demo, m_driver.update(dt));

            // P0 serial-explicit sim packet: owned copy of tick/epoch/
            // counters/chunks/chunk-cells captured after the tick, consumed
            // by the present + overlay phases below on this same thread. A
            // regeneration or previous present failure makes this a full-refresh packet
            // carrying every resident chunk's cells.
            m_sim_packet = colony::makeSimFramePacket(getFrameIndex(), m_driver, m_present_full_refresh);

            // Phase: present. Consume the owned packet on this same thread:
            // selectively ack exactly the captured present set (post-capture
            // dirt stays pending for the next frame), stage exactly the
            // capture-time residency, and upload the captured chunk cells — never the
            // live grid, so post-capture driver mutation cannot alter what
            // the frame presents or reports. Fail closed when the owned
            // packet is missing or incoherent (tick advanced or world
            // regenerated after capture): never present from unowned or
            // stale state. A present failure forces a full-refresh packet.
            if (not m_sim_packet or
                not colony::isSimPacketCoherent(*m_sim_packet, m_driver.tickCount(), m_driver.worldGeneration())) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            GridPass::GridStagedFrame frame_stage{};
            // Reset intent rides the stage; BuildStagedFrame derives the
            // full-refresh flag and the consumption-ack manifest from the
            // owned packet (seed, chunk ids, edit revisions).
            frame_stage.m_reset_caches = std::exchange(m_grid_reset_pending, false);
            if (const std::error_code error = colony::buildGridStagedFrame(*m_sim_packet, frame_stage)) {
                // Fail-closed: restore the armed reset intent and force a
                // full-refresh retry; the stage is discarded, never applied.
                m_grid_reset_pending = frame_stage.m_reset_caches;
                m_present_full_refresh = true;
                return error;
            }
            submitGridStage(std::move(frame_stage));

            // Phase: overlay + panel. Hover resolves first so the preview
            // outline, status line, and Inspector all describe the cell a
            // click would act on this frame. The 3D debug submission keeps
            // bodies, paths, grid, and edit outlines; drawPanel_ draws
            // chrome plus the screen-space feedback overlay. The display poll
            // runs ahead of the draw so acknowledgements, the trial
            // countdown, and monitor telemetry land in this same frame's
            // Display window.
            refreshHover_();
            PPR_RETURN_ERROR_ON_FAIL(Demo, pollDisplaySession_(std::chrono::duration<float>(dt).count()));
            PPR_RETURN_ERROR_ON_FAIL(Demo, submitDebugInstances_());
            PPR_RETURN_ERROR_ON_FAIL(Demo, drawPanel_());
            // Single-use packet retired on this thread; next frame rebuilds.
            m_sim_packet.reset();

            return {};
        }

        [[nodiscard]] std::error_code shutdown() override {
            std::error_code first_error{};
            // Establish GPU quiescence before driver shutdown:
            // an UNKNOWN disposition fail-stops in super_t::shutdown() before
            // any further release.
            std::ignore = prepareGpuTeardown_();
            // Detach the tool gesture first: cancel ownership so the camera
            // mapping below never observes a half-torn suppressor, drop the
            // focus subscription, then drop the higher-priority tool mapping
            // before the colony one.
            cancelToolGesture_();
            clearAgentSelection_();
            first_error = m_input_error;
            // Display teardown first among UI state: disarm the trial and
            // drop the acknowledgement watch. A submitted-but-unacked change
            // settles through the lane drain in super shutdown below.
            cancelDisplaySession_();
            m_focus_handle.reset();
            if (m_tool_mapping) {
                if (not getMainPlayer()->getListener().removeInputMapping(*m_tool_mapping) and not first_error) {
                    first_error = make_error_code(std::errc::not_connected);
                }
                m_tool_mapping.reset();
            }
            // Unregister borrowed action pointers and callbacks before releasing owners.
            if (m_colony_mapping) {
                if (not getMainPlayer()->getListener().removeInputMapping(*m_colony_mapping) and not first_error) {
                    first_error = make_error_code(std::errc::not_connected);
                }
                m_colony_mapping.reset();
            }
            for (auto &action: m_actions) {
                action.reset();
            }
            if (m_driver_ready) {
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

int main(const int argc, char *argv[]) {
    const std::span<const char *const> args{argv, pP::checked_cast<std::size_t>(argc)};

    pP::config::Diagnostics diagnostics{};
    // Generated picture-settings overlay: resolved through the executable
    // folder (explicit platform boundary, no working-directory search) and
    // loaded between the base files and the environment. A missing file
    // means first launch and loads nothing; an undecodable file is invalid
    // usage like a bad base file. An unavailable program folder simply
    // skips the overlay; the Display window reports it on Save.
    std::filesystem::path overlay_path{};
    if (auto overlay = pP::game::resolveDisplayOverlayPath()) {
        overlay_path = std::move(*overlay);
    } else {
        std::cerr << std::format("[display] picture-settings file unavailable: {}\n", overlay.error().message());
    }
    auto invocation = pP::game::resolveGameInvocationWithOverlay(argc, argv, overlay_path, diagnostics);
    if (not invocation) {
        std::cerr << diagnostics.format() << '\n';
        return pP::config::kExitInvalidUsage;
    }
    if (invocation->m_help_requested) {
        std::println("{}", pP::game::gameHelpText());
        return 0;
    }

    const pP::game::GameSettings &settings = invocation->m_game.m_value;
    const pP::DisplaySettings &display = invocation->m_display.m_value;
    const pP::StartupSettings &startup = invocation->m_startup.m_value;

    // Single GPU-runner boundary for startup validation: explicitly supplied
    // true/false values (CLI/file/environment) reach the RHI before any
    // command constructs GPU work; defaults leave the RHI build defaults
    // (core validation in debug, GPU-assisted under ASan) untouched.
    if (const auto *origin = invocation->m_startup.find("startup.core_validation");
        origin != nullptr and origin->m_source != pP::config::ESource::none) {
        pP::IRhiService::get()->setCoreValidation(startup.m_core_validation);
    }
    if (const auto *origin = invocation->m_startup.find("startup.gpu_validation");
        origin != nullptr and origin->m_source != pP::config::ESource::none) {
        pP::IRhiService::get()->setGpuAssistedValidation(startup.m_gpu_validation);
    }

    if (settings.m_command == pP::game::EGameCommand::smoke) {
        pP::hal::installDebugAssertHooks();
        pP::hal::disableSystemErrorReporting();
        return pP::colony::runColonySmoke(pP::colony::SmokeOptions{startup.m_gpu_validation}).value();
    }
    demo::TurboLarbin app("ppr", args);
    // Optional larger workload for external profiler captures.
    if (settings.m_profile100) {
        app.setAgentCount(100u);
    }
    if (settings.m_exit_after.has_value()) {
        app.setAutoExitSeconds(pP::TimeDuration{*settings.m_exit_after});
    }
    app.setDisplaySettings(display);
    // Startup picture baseline for "Back to startup": the lower-precedence
    // values the reset intent exposes with their provenance. Runtime edits
    // never touch it.
    app.noteDisplayStartup(invocation->m_display);
    // Report operational failures and preserve their exit status.
    try {
        const std::error_code run_error = app.run();
        if (run_error) {
            std::cerr << std::format(
                "[fatal] app.run: {} (category={} value={})\n", run_error.message(), run_error.category().name(),
                run_error.value());
        }
        return run_error.value();
    } catch (const std::exception &error) {
        std::cerr << "[fatal-ex] " << error.what() << '\n';
        // Unexpected: anything escaping the app run that is not an ordinary
        // reported operational error enters the fail-fast endpoint so a WER
        // dump is captured instead of an unexplained exit code.
        pP::hal::fatalExit("unexpected-std-exception");
    } catch (...) {
        std::cerr << "[fatal-ex] unknown non-std exception\n";
        pP::hal::fatalExit("unexpected-unknown-exception");
    }
}
