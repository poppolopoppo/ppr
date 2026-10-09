export module game.display;

import engine.app;
import engine.core;

import std;

export namespace pP::game {
    /// Generated picture-settings overlay next to the executable. Resolved
    /// through hal::process::currentExecutablePath (explicit platform
    /// boundary), never through an implicit working-directory search.
    inline constexpr std::string_view kDisplayOverlayFileName{"user-settings.toml"};

    /// Keep/Revert trial length for newly applied picture settings (seconds,
    /// ticked by the run-loop delta so the countdown uses injected time).
    inline constexpr float kDisplayConfirmSeconds{15.0f};

    /// Monitor telemetry refresh period while the Display window stands.
    /// Trial restoration geometry is acquired separately at submission.
    inline constexpr float kDisplayMonitorRefreshSeconds{1.0f};

    /// Panel-owned draft of the nine runtime picture fields. Plain values
    /// only: validation and submission happen through the typed intent path
    /// in main.cpp, never from the panel draw itself. The monitor holds a
    /// revalidated NAME ("auto" selects the primary monitor), never a
    /// native handle; zero width/height keeps the current size, and zero
    /// refresh requests the platform default.
    struct DisplayDraft {
        EFramePacing m_pacing{EFramePacing::follow_vsync};
        u32 m_cap_hz{60u};
        bool m_paced_vsync{true};
        bool m_throttle_unfocused{true};
        EDisplayWindowMode m_window_mode{EDisplayWindowMode::windowed};
        std::string m_monitor{"auto"};
        u32 m_width{0u};
        u32 m_height{0u};
        u32 m_refresh_hz{0u};
    };

    /// Trial state for newly applied picture settings. Armed only after a
    /// successful acknowledgement; expiry asks the caller to revert through
    /// the same transaction path. Session-only: dies with the UI state.
    /// Countdown cessation does not resolve the trial: a failed revert keeps
    /// it armed, without further automatic attempts, until Keep or success.
    /// The native geometry snapshot is the pre-trial observed window
    /// (position, size, exclusive state) carried from the submit watch to
    /// the arm, so a revert restores what was on screen — requested zeros
    /// mean "keep current size" and can never do that on their own.
    struct DisplayConfirm {
        bool m_armed{false};
        bool m_countdown_active{false};
        float m_seconds_left{0.0f};
        DisplaySettings m_prior{};
        bool m_revert_pending{false};
        WindowDisplayGeometry m_prior_geometry{};
        bool m_has_prior_geometry{false};
    };

    [[nodiscard]] Expected<DisplayDraft> displayDraftFrom(const DisplaySettings &settings) noexcept;

    [[nodiscard]] Expected<DisplaySettings> displaySettingsFrom(const DisplayDraft &draft) noexcept;

    [[nodiscard]] bool displaySettingsEqual(const DisplaySettings &lhs, const DisplaySettings &rhs) noexcept;

    /// True when the draft matches committed settings (no unapplied edits).
    [[nodiscard]] bool displayDraftMatches(const DisplayDraft &draft, const DisplaySettings &committed) noexcept;

    /// Canonical pacing spelling for layer building ("uncapped",
    /// "follow-vsync", "paced"; empty when the value has no domain entry).
    [[nodiscard]] std::string_view pacingName(EFramePacing pacing) noexcept;

    /// Canonical window-mode spelling for layer building ("windowed",
    /// "fullscreen"; empty when the value has no domain entry).
    [[nodiscard]] std::string_view windowModeName(EDisplayWindowMode mode) noexcept;

    /// Availability of native window switching for one owned capability
    /// snapshot. Null capabilities (enumeration failed) and backends that
    /// report no window changes both read as unsupported with a grounded
    /// one-line reason; the panel keeps its read-only telemetry either way.
    struct DisplaySwitchingAvailability final {
        bool m_supported{false};
        const char *m_reason{""};
    };

    [[nodiscard]] DisplaySwitchingAvailability displaySwitchingAvailability(
        const DisplayCapabilities *capabilities) noexcept;

    /// One accepted-runtime-override layer from a draft. Validated later by
    /// applyDisplayRuntime; an invalid draft value fails there, never here.
    [[nodiscard]] config::Layer displayRuntimeLayer(const DisplayDraft &draft, std::string origin);

    /// Transactional validation of one draft against committed state: a
    /// scratch copy passes applyRuntime (runtime-mutability, conversion,
    /// domain, range), so failure leaves committed untouched and fills
    /// diagnostics. Invalid/disallowed fields are rejected, never clamped.
    [[nodiscard]] Expected<config::Resolved<DisplaySettings>> applyDisplayRuntime(
        const config::Resolved<DisplaySettings> &committed, const config::Layer &layer,
        config::Diagnostics &diagnostics);

    /// Validates a panel draft the same way the Apply intent does:
    /// schema rules (domain, range, cross-field) only, no capabilities.
    [[nodiscard]] Expected<config::Resolved<DisplaySettings>> validateDisplayDraft(
        const config::Resolved<DisplaySettings> &committed, const DisplayDraft &draft,
        config::Diagnostics &diagnostics);

    /// Validates a panel draft the way the submitted transaction does:
    /// schema rules first, then capability rules (known monitor name,
    /// enumerated exclusive resolution/refresh). A null-free snapshot with
    /// no window support still validates windowed drafts; fullscreen drafts
    /// need a matching enumeration.
    [[nodiscard]] Expected<config::Resolved<DisplaySettings>> validateDisplayDraft(
        const config::Resolved<DisplaySettings> &committed, const DisplayDraft &draft,
        const DisplayCapabilities &capabilities, config::Diagnostics &diagnostics);

    /// Merges acknowledged settings into an existing overlay document: every
    /// display.* entry is replaced by the persistable display properties only
    /// (never profiling/validation/child-run/crash switches — this schema
    /// owns none of those), while unrelated entries keep their order and
    /// values. Hand-edited base files are never touched by this path.
    [[nodiscard]] Expected<config::Document> buildDisplayOverlayDocument(
        const config::Document &existing, const DisplaySettings &committed);

    /// Encodes an overlay document with the generated-file header. The header
    /// documents that the command line and environment entries still take
    /// priority over this file on the next launch.
    [[nodiscard]] Expected<std::string> encodeDisplayOverlay(const config::Document &document);

    /// Same-directory temporary file plus atomic replace where the platform
    /// supports it. Reports the failure; never claims success falsely.
    [[nodiscard]] std::error_code writeFileAtomically(
        const std::filesystem::path &path, std::string_view bytes) noexcept;

    /// Reads one overlay file as a file-precedence layer. A missing file is
    /// absence (nullopt), never an error; an unreadable or undecodable file
    /// is an error with diagnostics, and the caller must not overwrite it.
    [[nodiscard]] Expected<std::optional<config::Layer>> loadOverlayLayer(
        const std::filesystem::path &path, config::Diagnostics &diagnostics);

    /// Overlay location for this executable. Fails only when the platform
    /// cannot report its own executable path.
    [[nodiscard]] Expected<std::filesystem::path> resolveDisplayOverlayPath();

    /// Arms the Keep/Revert trial after a successful acknowledgement.
    [[nodiscard]] std::error_code armDisplayConfirm(DisplayConfirm &confirm, const DisplaySettings &prior) noexcept;

    /// Arms the trial with the pre-trial native geometry alongside the
    /// requested policy; the revert path materializes explicit settings
    /// from both through displayRevertSettings. Absence keeps the
    /// requested-policy-only behavior.
    [[nodiscard]] std::error_code armDisplayConfirm(
        DisplayConfirm &confirm, const DisplaySettings &prior,
        const std::optional<WindowDisplayGeometry> &geometry) noexcept;

    /// Transfers prepared acknowledgement snapshots without allocating.
    void armDisplayConfirmOwned(
        DisplayConfirm &confirm, DisplaySettings &&prior, std::optional<WindowDisplayGeometry> &&geometry) noexcept;

    /// True while a trial confirmation is unresolved (armed, including a
    /// pending revert or a failed one): Apply and Reset must wait for Keep
    /// or a successful revert, in the UI and in the dispatch.
    [[nodiscard]] bool displayTrialUnresolved(const DisplayConfirm &confirm) noexcept;

    /// Materializes the revert target from a trial: the requested pre-trial
    /// policy with the window fields pinned to the observed pre-trial
    /// geometry (explicit size, monitor, mode, refresh), so restoring
    /// through the transaction path returns the previous on-screen state
    /// instead of re-requesting "keep current size".
    [[nodiscard]] Expected<DisplaySettings> displayRevertSettings(const DisplayConfirm &confirm) noexcept;

    /// Acquire current geometry at the submission boundary, never from the
    /// telemetry cache. Window mutations require a usable restoration size;
    /// policy-only submissions may proceed when geometry is unavailable.
    [[nodiscard]] Expected<std::optional<WindowDisplayGeometry>> captureDisplayTrialGeometry(
        bool window_change, std23::function_ref<Expected<WindowDisplayGeometry>()> acquire);

    /// True when committed state carries runtime-layer assignments: Reset
    /// has something to remove even when the draft matches committed
    /// (which is exactly the post-acknowledgement state).
    [[nodiscard]] bool displayHasRuntimeOverrides(const config::Resolved<DisplaySettings> &resolved) noexcept;

    /// Reset candidate from the lower-precedence baseline (startup
    /// resolution including file/environment/command-line values): a copy
    /// carrying values and provenance, never a runtime layer and never
    /// factory defaults.
    [[nodiscard]] config::Resolved<DisplaySettings> displayResetCandidate(
        const config::Resolved<DisplaySettings> &baseline);

    /// Ticks an armed trial by injected seconds. Returns true once, on the
    /// tick the trial expires; while a revert is pending the trial holds
    /// (the acknowledgement decides) and never expires underneath it.
    [[nodiscard]] bool tickDisplayConfirm(DisplayConfirm &confirm, float dt_seconds) noexcept;

    /// Ticks the trial for one run-loop poll. The poll that arms the trial
    /// must pass armed_this_poll so its delta (which includes the
    /// acknowledgement wait) is not charged to the countdown; only
    /// post-acknowledgement time ticks the trial.
    [[nodiscard]] bool tickDisplayConfirmPoll(DisplayConfirm &confirm, float dt_seconds, bool armed_this_poll) noexcept;

    void disarmDisplayConfirm(DisplayConfirm &confirm) noexcept;

    /// Reads an overlay file as bytes. A missing file is absence
    /// (nullopt), never an error; a filesystem probe failure, an
    /// unopenable file, or a mid-read stream failure is an error with
    /// diagnostics, and the caller must not overwrite the file.
    [[nodiscard]] Expected<std::optional<std::string>> readOverlayBytes(
        const std::filesystem::path &path, config::Diagnostics &diagnostics);

    /// Production sized-read operation. Short reads (including premature
    /// EOF) and buffer exceptions are failures, never successful truncation.
    [[nodiscard]] Expected<std::string> readOverlayBytes(
        std::istream &stream, std::uintmax_t size, const std::filesystem::path &path, config::Diagnostics &diagnostics);
} // namespace pP::game
