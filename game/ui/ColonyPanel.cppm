module;


export module game.colony.panel;

import engine.app;
import engine.core;
import engine.sim;
import game.colony.agents;
import game.display;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // panel types
    // ------------------------------------------------------------------

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

    /// The single intent vocabulary for the colony UI. Every toolbar button,
    /// menu item, section tab, and keyboard shortcut dispatches one of these
    /// through the caller's one dispatch function, so button/shortcut parity
    /// holds by construction. The panel never touches simulation state.
    enum class EColonyIntent : u8 {
        pause_toggle,
        step,
        set_speed, // m_arg = 1 | 2 | 3
        regenerate, // opens the in-game New Game workspace
        // (R and "New world"); true performs it
        select_tool, // m_arg = EColonyTool
        toggle_dig, // G and the Dig button arm/stow the dig tool
        toggle_edit_highlight, // H: flip the edit-chunk highlight
        toggle_grid_overlay, // V: flip the chunk grid overlay (flag only; the debug submission draws it)
        cancel, // Esc chain: follow, held gesture, tool, pin, help
        focus_selection, // jump to the selection and follow it (no argument)
        find_next, // F: select the next colonist in stable-key order
        toggle_performance, // F3: toggle only the independent Performance window
        toggle_colony, // independent Colony/context window
        help, // F1: toggle the help window
        open_build_menu, // B: opens the menu; never places by itself
        toggle_pin, // pin or unpin the inspected cell (world click or Esc today)
        dismiss_notice, // context notice Dismiss button
        toggle_display, // Display button: open or close the Display window
        display_apply, // apply the Display draft through a display transaction
        display_reset_overrides, // runtime values back to the startup snapshot, same path
        display_save, // write the acknowledged values to the user-settings overlay
        display_keep, // confirm the trial picture settings
        display_revert, // revert the trial to the pre-apply values, same path
        display_use_monitor_cap, // draft paced + cap from the reported monitor refresh
        select_preview, // m_arg = EGridDiagnosticMode; range lives in UI draft.
        open_workshop,
        quit, // same graceful host transition as the native close control
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

    /// How long a posted notice stays in the tool context (seconds).
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

    using ColonyMonitorView = ui::DisplayWidgetMonitor;
    using ColonyDisplaySnapshot = ui::DisplayWidgetSnapshot;

    /// Display session owned by the caller-side UI state. The draft holds
    /// uncommitted edits (resynced from committed whenever the window opens
    /// and after every acknowledgement); the trial counts down an
    /// acknowledged change that touched presentation; the watch tracks one
    /// submitted transaction until its acknowledgement lands; the last error
    /// stays on screen until the next success. Closing the window discards
    /// the draft but never cancels an armed trial: expiry reverts to the
    /// known-good values, which is the safe direction.
    struct ColonyDisplaySession final {
        game::DisplayConfirm m_confirm{};
        /// Pre-submit native geometry carried to the trial arm, so the
        /// revert restores observed state instead of re-requesting zeros.
        std::optional<WindowDisplayGeometry> m_watch_geometry{std::nullopt};
        game::DisplayDraft m_draft{};
        DisplaySettings m_watch_prior{};
        std::string m_last_error{};
        u64 m_watch_revision{0u};
        bool m_synced{false};
        bool m_watch_ack{false};
        bool m_watch_vsync_affecting{false};
        bool m_watch_window_change{false};
        bool m_watch_revert{false};
        bool m_revert_failed{false};
    };

    /// Mutable UI session state owned by the caller. The panel reads all of
    /// it and writes only session bookkeeping (`m_build_menu_open` as a
    /// one-shot trigger, `m_popup_visible_last_frame` for focus hand-back, and
    /// `m_focus_agent` as the one-shot Focus key hand-off); everything else
    /// changes through the intent dispatch.
    ///
    /// The Display session below follows the same rule one level down: the
    /// panel edits the draft + trial countdown in place (uncommitted scratch,
    /// discarded when the window closes), while Apply/Reset/Save/Keep/Revert
    /// are intents the caller submits through the display transaction path.
    struct ColonyUiState final {
        // owned values and drafts:
        std::string m_instruction{"Inspect: click a cell to pin it in the Inspector."};
        /// One-shot Focus hand-off: a row's Focus button parks the key it
        /// was pressed for here BEFORE it emits `focus_selection`, because
        /// that intent carries no argument. The caller adopts the key
        /// through its normal agent-selection path and then focuses; it is
        /// cleared on consumption, so a stale key can never fire twice.
        std::optional<u64> m_focus_agent{};
        ColonyNotice m_notice{};
        ColonyDisplaySession m_display{};

        // selection and diagnostic controls:
        std::optional<sim::GlobalCellPos> m_pinned{};
        EGridDiagnosticMode m_preview_mode{EGridDiagnosticMode::textured};
        float m_temperature_min_k{200.0f};
        float m_temperature_max_k{400.0f};

        // tool and independent window visibility:
        EColonyTool m_tool{EColonyTool::inspect};
        bool m_colony_open{true};
        bool m_performance_open{false};
        bool m_display_open{false};
        bool m_build_menu_open{false};
        bool m_help_open{false};
        /// First-run hint block inside the Help window, dismissed for this
        /// session only. Deliberately not persisted (this boundary writes no
        /// config or file), so the next launch shows it again; Help/F1 still
        /// reopens Help whenever the player wants the reference back.
        bool m_intro_dismissed{false};
        /// Panel bookkeeping: was any popup open at the end of the last
        /// frame (drives focus hand-back on the popup-close transition).
        bool m_popup_visible_last_frame{false};
    };

    /// Resolved pointer hover over the world, from the caller's single
    /// `pickCell_` path, so the preview outline and the tool/selection context
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

    /// A client-pixel point the caller has already projected from the world.
    /// The frame's client extent equals the ImGui display extent, so this is
    /// an ImGui coordinate; the panel never owns a camera and never projects.
    struct ColonyScreenPoint final {
        float m_x{};
        float m_y{};
    };

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

    // ------------------------------------------------------------------
    // synchronous presentation snapshot
    // ------------------------------------------------------------------

    enum class ECellPresence : u8 {
        unavailable,
        unresident,
        vacuum,
        material,
        invalid_material
    };

    struct ColonyParticlePresentation final {
        std::string_view m_material{};
        u64 m_id{};
        double m_temperature_k{};
        double m_volume_m3{};
        double m_mass_kg{};
    };

    struct ColonyCellPresentation final {
        std::string_view m_material{};
        std::string_view m_biome{};
        Array<std::string_view> m_features{};
        std::optional<ColonyParticlePresentation> m_particle{};
        std::optional<AgentSummary> m_nearest{};
        sim::GlobalCellPos m_position{};
        float m_temperature_k{};
        u16 m_material_id{};
        ECellPresence m_presence{ECellPresence::unavailable};
        bool m_generated{};
    };

    struct ColonyWorldPresentation final {
        std::string_view m_pack{};
        std::string_view m_profile{};
        std::array<u64, 5u> m_scratch_bytes{};
        std::array<double, 5u> m_phase_seconds{};
        u64 m_seed{};
        u64 m_content_fingerprint{};
        u64 m_retained_bytes{};
        u64 m_sites{};
        u64 m_features{};
        u64 m_matter_regions{};
        u32 m_generator_version{};
        u32 m_particles{};
        bool m_available{};
    };

    /// The adapter refreshes this reusable storage before drawing. Borrowed
    /// names and rows are valid only until the synchronous draw returns;
    /// neither this object nor detached rendering retains a driver pointer.
    struct ColonyPresentation final {
        ColonyCellPresentation m_hover{};
        ColonyCellPresentation m_inspected{};
        ColonyWorldPresentation m_world{};
        std::span<const AgentSummary> m_agents{};
        u64 m_seed{};
        u64 m_tick_count{};
        double m_simulated_ms{};
        u32 m_speed{};
        u32 m_resident_chunks{};
        bool m_paused{};
        bool m_has_hover{};
        bool m_has_inspected{};
    };

    /// Everything the panel renders this frame; all read-only snapshots.
    struct ColonyUiView final {
        const ColonyPresentation *m_colony{};
        /// Host-owned history; borrowed only by this synchronous draw, never retained or observed here.
        const ui::PerformanceWidget *m_performance{};
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
        /// Screen anchor for a pinned Inspector cell, projected by the caller
        /// from the live camera. Absent when nothing is pinned or the pinned
        /// cell is off screen; a pinned read-out then falls back to the pointer.
        std::optional<ColonyScreenPoint> m_pinned_screen{};
    };

    /// Result of one panel frame: at most one intent, plus the request that
    /// the caller hand keyboard focus back to the game, plus the chrome
    /// geometry for screen-space feedback (logical px). Only the full-width
    /// ribbon reserves an edge; bottom/left/right are zero. Finite panels use
    /// native input capture and cover background feedback with opaque surfaces.
    struct ColonyUiFrame final {
        std::optional<ColonyIntent> m_intent{};
        float m_chrome_top{};
        float m_chrome_bottom{};
        float m_chrome_right{};
        float m_chrome_left{};
        bool m_focus_release{false};
    };

    // ------------------------------------------------------------------
    // panel api
    // ------------------------------------------------------------------

    /// Draws a simulation/management ribbon, left supported-tool rail, a
    /// pointer-following or pinned-cell Inspect read-out and compact left
    /// Colony/Performance summary, contextual bottom status, native utilities
    /// and build/regenerate popups. The central world stays
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
        const ColonyUiView &view, const ColonyDisplaySnapshot &display, const std::span<const ColonyShortcut> shortcuts,
        ColonyUiState &state);
} // namespace pP::colony
