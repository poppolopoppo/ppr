module;

#include "StageTiming.h"

export module game.colony.panel;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.driver;
import game.display;
import std;

export namespace pP::colony {
    /// Read-only presentation counts supplied by the renderer/caller.
    /// Path counters ride the same struct (appended): the panel never
    /// imports the pathfinding module. Shape unchanged from Slice 1.
    struct ColonyPanelCounts final {
        u32 m_visible_chunks{};
        u32 m_visible_tiles{};
        u32 m_paths{};
        u32 m_partials{};
        u32 m_blocked{};
        u32 m_errands{};
        u32 m_dug_total{};
        u32 m_wake_radius{};
        u32 m_affected_chunks{};
        bool m_dig_mode{};
        bool m_show_highlight{};
    };

    /// Tools the player can select; `EColonyIntent::select_tool` carries one
    /// in `ColonyIntent::m_arg`. A toggle is not a selection, so the dig
    /// arm/stow gesture has its own intent (see `toggle_dig` below).
    enum class EColonyTool : u8 {
        inspect,
        dig,
        build_wall,
        demolish,
    };

    /// Sections of the right side panel; `EColonyIntent::select_tab` carries
    /// one in `ColonyIntent::m_arg`.
    enum class EColonyPanelTab : u8 {
        colony,
        performance,
    };

    /// The single intent vocabulary for the colony UI. Every toolbar button,
    /// menu item, section tab, and keyboard shortcut dispatches one of these
    /// through the caller's one dispatch function, so button/shortcut parity
    /// holds by construction. The panel never touches simulation state.
    enum class EColonyIntent : u8 {
        pause_toggle,
        step,
        set_speed, // m_arg = 1 | 2 | 3
        regenerate, // m_confirmed = false opens the confirm modal
        // (R and "New world"); true performs it
        select_tool, // m_arg = EColonyTool
        toggle_dig, // G and the Dig button arm/stow the dig tool
        toggle_edit_highlight, // H: flip the edit-chunk highlight
        toggle_grid_overlay, // V: flip the chunk grid overlay (flag only; the debug submission draws it)
        cancel, // Esc chain: follow, held gesture, tool, pin, help
        focus_selection, // jump to the selection and follow it (no argument)
        find_next, // F: select the next colonist in stable-key order
        toggle_performance, // F3: flip between the two panel sections
        help, // F1: toggle the help window
        open_build_menu, // B: opens the menu; never places by itself
        toggle_pin, // Inspector pin button; cell clicks pin directly
        dismiss_notice, // status-line Dismiss button
        select_tab, // m_arg = EColonyPanelTab (section buttons)
        toggle_display, // Display button: open or close the Display window
        display_apply, // apply the Display draft through a display transaction
        display_reset_overrides, // runtime values back to the startup snapshot, same path
        display_save, // write the acknowledged values to the user-settings overlay
        display_keep, // confirm the trial picture settings
        display_revert, // revert the trial to the pre-apply values, same path
        display_use_monitor_cap, // draft paced + cap from the reported monitor refresh
    };

    /// One typed intent; trivially copyable so keyboard callbacks, buttons,
    /// and the panel's Esc mirror can all hand it across the frame boundary
    /// without allocation.
    struct ColonyIntent final {
        EColonyIntent m_kind{};
        u32 m_arg{};
        bool m_confirmed{};
    };

    /// One row of the caller-owned binding table. The same table builds the
    /// key actions and feeds tooltips and the Help window, so shortcut text
    /// can never drift from the keys actually bound (no second map).
    struct ColonyShortcut final {
        EColonyIntent m_kind{};
        u32 m_arg{};
        const char *m_key{}; // "Space", "F3", ...
        const char *m_tip{}; // "Pause or resume the simulation"
    };

    /// How long a posted notice stays on the status line (seconds).
    inline constexpr float kNoticeSeconds = 6.0f;

    /// Bounded, short-lived, dismissible status notice. Keeps the
    /// originating error code for the muted suffix; never routes through the
    /// per-frame frame-message channel (which is cleared every frame).
    struct ColonyNotice final {
        std::string m_text{};
        std::error_code m_error{};
        float m_seconds_left{};

        [[nodiscard]] bool visible() const noexcept {
            return m_seconds_left > 0.0f and not m_text.empty();
        }
    };

    /// One monitor's reported identity for the Display window. Assembled by
    /// the caller from the window service (cached about once a second while
    /// the window stands); a zero refresh means the service did not report
    /// one, and the panel prints "unknown" instead of guessing.
    struct ColonyMonitorView final {
        std::string m_name{};
        int m_width{};
        int m_height{};
        int m_refresh_hz{};
        bool m_primary{false};
    };

    /// Everything the Display window renders: the §5.1 copied controller
    /// view (requested policy, computed targets, throttle reason), the
    /// acknowledged device report, the pending transaction plus the last
    /// failure, and the monitor telemetry. Requested policy and reported
    /// outcomes stay in separate sections; unavailable measurements read as
    /// unavailable, never as zero. The monitor list borrows the caller's
    /// cache for the frame, like the agent-needs span in ColonyUiView. The
    /// capability and geometry views are owned by the caller (handle-free
    /// snapshots from displayCapabilities/displayWindowGeometry); null
    /// means the query failed, and the switching controls render disabled
    /// with the stored reason while telemetry stays visible.
    struct ColonyDisplaySnapshot final {
        DisplayView m_view{};
        DisplayAppliedState m_applied{};
        EDisplayTransaction m_transaction{EDisplayTransaction::idle};
        std::error_code m_error{};
        bool m_has_pending{false};
        u64 m_pending_id{0u};
        u64 m_revision{0u};
        /// True when committed state carries runtime-layer assignments:
        /// Reset has something to remove even when the draft matches
        /// committed (the post-acknowledgement state), so it gates
        /// independently of the Apply dirty state.
        bool m_has_runtime_overrides{false};
        std::span<const ColonyMonitorView> m_monitors{};
        bool m_fullscreen{false};
        std::string_view m_fullscreen_monitor{};
        const DisplayCapabilities *m_capabilities{nullptr};
        std::error_code m_capabilities_error{};
        const WindowDisplayGeometry *m_geometry{nullptr};
    };

    /// Display session owned by the caller-side UI state. The draft holds
    /// uncommitted edits (resynced from committed whenever the window opens
    /// and after every acknowledgement); the trial counts down an
    /// acknowledged change that touched presentation; the watch tracks one
    /// submitted transaction until its acknowledgement lands; the last error
    /// stays on screen until the next success. Closing the window discards
    /// the draft but never cancels an armed trial: expiry reverts to the
    /// known-good values, which is the safe direction.
    struct ColonyDisplaySession final {
        bool m_synced{false};
        game::DisplayDraft m_draft{};
        game::DisplayConfirm m_confirm{};
        bool m_watch_ack{false};
        u64 m_watch_revision{0u};
        bool m_watch_vsync_affecting{false};
        bool m_watch_window_change{false};
        bool m_watch_revert{false};
        DisplaySettings m_watch_prior{};
        /// Pre-submit native geometry carried to the trial arm, so the
        /// revert restores observed state instead of re-requesting zeros.
        std::optional<WindowDisplayGeometry> m_watch_geometry{std::nullopt};
        bool m_revert_failed{false};
        std::string m_last_error{};
    };

    /// Mutable UI session state owned by the caller. The panel reads all of
    /// it and writes only session bookkeeping (`m_build_menu_open` as a
    /// one-shot trigger, `m_regen_confirm_open` when the player decides,
    /// `m_popup_visible_last_frame` for focus hand-back, and
    /// `m_focus_agent` as the one-shot Focus key hand-off); everything else
    /// changes through the intent dispatch.
    ///
    /// The Display session below follows the same rule one level down: the
    /// panel edits the draft + trial countdown in place (uncommitted scratch,
    /// discarded when the window closes), while Apply/Reset/Save/Keep/Revert
    /// are intents the caller submits through the display transaction path.
    struct ColonyUiState final {
        EColonyTool m_tool{EColonyTool::inspect};
        EColonyPanelTab m_tab{EColonyPanelTab::colony};
        std::optional<sim::GlobalCellPos> m_pinned{};
        std::string m_instruction{"Inspect: click a cell to pin it in the Inspector."};
        bool m_build_menu_open{false};
        bool m_regen_confirm_open{false};
        bool m_help_open{false};
        /// First-run hint block inside the Help window, dismissed for this
        /// session only. Deliberately not persisted (this boundary writes no
        /// config or file), so the next launch shows it again; "?"/F1 still
        /// reopens Help whenever the player wants the reference back.
        bool m_intro_dismissed{false};
        /// Panel bookkeeping: was any popup open at the end of the last
        /// frame (drives focus hand-back on the popup-close transition).
        bool m_popup_visible_last_frame{false};
        /// One-shot Focus hand-off: a row's Focus button parks the key it
        /// was pressed for here BEFORE it emits `focus_selection`, because
        /// that intent carries no argument. The caller adopts the key
        /// through its normal agent-selection path and then focuses; it is
        /// cleared on consumption, so a stale key can never fire twice.
        std::optional<u64> m_focus_agent{};
        ColonyNotice m_notice{};
        bool m_display_open{false};
        ColonyDisplaySession m_display{};
    };

    /// Resolved pointer hover over the world, from the caller's single
    /// `pickCell_` path, so the preview outline and the status/Inspector
    /// reads describe exactly the cell a click would act on.
    struct ColonyHover final {
        bool m_over_ui{}; // pointer captured by chrome (unless a
        // world-owned drag carries it across)
        bool m_world{}; // the pick produced an in-world cell
        bool m_diggable{}; // resident chunk with a non-vacuum element
        sim::GlobalCellPos m_target{};
        /// Why no dig would land here ("" when diggable). Player-facing,
        /// short, ASCII: "pointer over the panel", "outside the world",
        /// "chunk not resident", "vacuum: nothing to dig".
        const char *m_reason{""};
    };

    /// Honest inter-frame timing for the Performance tab. The boundary is
    /// the caller's update-loop dt (elapsed between update() calls); it is
    /// never GPU time and is never presented as such. Averages/max are
    /// cumulative since launch and labelled accordingly.
    ///
    /// Slice 6 stage units: counter rows record calls only (0us always);
    /// every other row is wall-clock us. The designer prints counters as
    /// COUNTS, never us. Classification verified against StageTiming.h
    /// (CacheHit/CacheMiss/NeededCells/ChunksDrained are the counter rows).
    [[nodiscard]] constexpr bool stageIsCounter(const Stage stage) noexcept {
        return stage == Stage::CacheHit or
               stage == Stage::CacheMiss or
               stage == Stage::NeededCells or
               stage == Stage::ChunksDrained;
    }

    struct ColonyPerfView final {
        float m_frame_ms{};
        float m_avg_frame_ms{};
        float m_max_frame_ms{};
        u64 m_frames{};
        /// Bounded game UI lane split (last values, ms): main-lane
        /// update-body wall on main plus render-lane last completed lane
        /// execution from RenderLane's display-only gauge. The inter-frame
        /// total above stays the cadence reference; avg/max/ring below are
        /// untouched.
        float m_main_ms{};
        float m_render_ms{};
        /// Slice 6 windowed frame stats: bounded 240-sample ring of
        /// inter-frame dt (ms, NOT GPU/present); percentiles are nearest-rank
        /// over the window, refreshed at most every 15 frames.
        /// m_window_def names the ring size.
        float m_p50_ms{};
        float m_p95_ms{};
        float m_min_ms{};
        float m_window_max_ms{};
        u64 m_over_budget{}; // samples above the 16.67ms reference, in-window
        u64 m_window_samples{}; // min(total pushes, m_window_def)
        u32 m_window_def{};
        /// Failed observation refresh retains explicitly stale display values.
        bool m_statistics_stale{true};
        /// Slice 6 simulation: m_sim_ms is SIMULATED time (tick * period),
        /// never CPU; m_tick_hz is derived (1000 * ticks / simMs), 0 before
        /// the first tick.
        double m_tick_hz{};
        u64 m_tick_count{};
        double m_sim_ms{};
        bool m_paused{};
        u32 m_speed{};
        /// Slice 6 presentation: translator submitted chunks, GridPass staged
        /// and cached tiles, last-edit chunk count, grid dirty-chunk count.
        u32 m_submitted_chunks{};
        u32 m_total_chunks{};
        u32 m_staged_tiles{};
        u32 m_cached_tiles{};
        u32 m_last_edit_chunks{};
        u32 m_dirty_chunks{};
        /// Slice 6 work queue: deferred collider chunks (backlog), live
        /// errands, game-side dig issued count, and the issued-minus-applied
        /// pending estimate (an upper bound: the driver FIFO sheds under
        /// overflow; both sides reset on regen).
        u64 m_collider_pending{};
        u32 m_live_errands{};
        u64 m_dig_issued{};
        u64 m_dig_pending_est{};
        /// GPU-assisted validation: effective `--gpu-validation` opt-in
        /// state at RHI init (default off; accepted and ignored in Release).
        bool m_gpu_validation{false};
    };

    /// Slice 5 footprint preview read-out for the tool strip: one live
    /// snapshot the caller assembles while a build_wall/demolish press is
    /// held. `m_reason` is the caller's own stable string literal ("" while
    /// the rect would commit on release), so the panel reports it verbatim
    /// and never owns it; width/height are inclusive cells and mean nothing
    /// while the preview is inactive or the reason is set.
    struct ColonyPreviewView final {
        bool m_active{};
        bool m_clamped{};
        u32 m_width{};
        u32 m_height{};
        u32 m_max_edge{}; // footprint cap, for the strip's "max N"/"Clamped"
        const char *m_reason{""};
        // Per-axis clamp flags beside m_clamped (trailing fields only, so
        // the aggregate order above is untouched): m_clamped stays the OR
        // of the two for existing readers.
        bool m_clamped_x{false};
        bool m_clamped_y{false};
    };

    /// One colonist's live needs for the row read-out, paired with the
    /// stable agent key: summaries rebuild every tick, so the key is the
    /// only durable way to match a needs read to its row. Assembled by the
    /// caller once a frame; negative values mean "no component to read".
    struct AgentNeedsRow final {
        u64 m_key{};
        float m_o2{};
        float m_rest{};
    };

    /// Everything the panel renders this frame; all read-only snapshots.
    struct ColonyUiView final {
        ColonyPanelCounts m_counts{};
        ColonyHover m_hover{};
        ColonyPerfView m_perf{};
        /// Slice 5 footprint preview read-out (the strip's size line).
        ColonyPreviewView m_preview{};
        /// Slice 4 colonist read-outs: the selected key, the marker the
        /// pointer hovers (so a row can mirror its marker), whether the
        /// camera is following, and live O2/rest keyed by stable agent
        /// key. All read-only, all assembled by the caller per frame.
        std::optional<u64> m_selected_agent{};
        std::optional<u64> m_hovered_agent{};
        bool m_following{};
        std::span<const AgentNeedsRow> m_agent_needs{};
        /// Stage tables (Stage::Count rows each; counter rows report calls
        /// only and print no timings).
        std::span<const StageStat> m_driver_stages{};
        std::span<const StageStat> m_presentation_stages{};
    };

    /// Result of one panel frame: at most one intent, plus the request that
    /// the caller hand keyboard focus back to the game, plus the chrome
    /// geometry the caller needs to clip its screen-space feedback overlay
    /// (ImGui coords / logical px, measured this frame: top-bar height,
    /// status-line height, right side-panel width, 0 when the side panel
    /// skipped a degenerate window). The overlay clips to DisplaySize minus
    /// these bands so feedback never draws over chrome.
    struct ColonyUiFrame final {
        std::optional<ColonyIntent> m_intent{};
        bool m_focus_release{false};
        float m_chrome_top{};
        float m_chrome_bottom{};
        float m_chrome_right{};
    };

    /// Draws the full colony chrome: a wrapping top simulation bar plus a
    /// contextual tool strip, a right side panel (Colony and Performance
    /// sections with a persistent Inspector), a bottom status line, the help
    /// window, the display window, and the build/regenerate popups. The central world stays
    /// mouse pass-through: no dockspace, no full-screen host window, no
    /// invisible input sinks, fixed chrome bands only.
    ///
    /// Returns at most one intent (buttons, menu items, section tabs,
    /// colonist row Focus/Find/Stop-follow buttons, display actions, and the Esc keyboard
    /// mirror) and `m_focus_release`, which the caller
    /// honors by clearing window focus so shortcuts regain keyboard capture
    /// after a completed chrome click. The panel never requests focus
    /// release while a popup is open: ImGui closes every popup for a null
    /// focus request (FocusWindow(NULL) -> ClosePopupsOverWindow(NULL)),
    /// so open popups own their own close path.
    ///
    /// The display snapshot carries the copied controller view plus monitor
    /// telemetry; the panel never touches the renderer, calls lane-affine
    /// functions, or resolves services. Every applied display change leaves
    /// as a typed intent the caller submits through requestDisplaySettings.
    [[nodiscard]] ColonyUiFrame drawColonyUi(
        const ColonyDriver &driver, const ColonyUiView &view, const ColonyDisplaySnapshot &display,
        const std::span<const ColonyShortcut> shortcuts, ColonyUiState &state);
} // namespace pP::colony
