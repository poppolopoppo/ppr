export module game.config;

import engine.app;
import engine.core;

import std;

export namespace pP::game {
    /// Game command selector. `--smoke`, `--load100`, and `--time-trials`
    /// select a headless command; the default runs the interactive editor.
    /// Selectors are CLI-only fixed selections accepted in any position
    /// (first-position usage is preserved as a subset). Conflicting
    /// selectors (different commands) and explicit selector values are
    /// rejected as invalid usage before resolution.
    enum class EGameCommand : u8 {
        interactive,
        smoke,
        load100,
        time_trials,
    };

    inline constexpr config::EnumDomain<EGameCommand, 4u> game_command_domain{{
        config::EnumEntry<EGameCommand>{EGameCommand::interactive, "interactive"},
        config::EnumEntry<EGameCommand>{EGameCommand::smoke, "smoke"},
        config::EnumEntry<EGameCommand>{EGameCommand::load100, "load100"},
        config::EnumEntry<EGameCommand>{EGameCommand::time_trials, "time-trials"},
    }};

    /// Game-owned invocation settings: command selection, profiling fixtures,
    /// bounded-run duration, CLI-only diagnostics, and the explicit config
    /// file path. Display pacing and RHI validation live in the engine.app
    /// schemas; this schema only declares the game.* bindings of the combined
    /// invocation CLI.
    struct GameSettings {
        EGameCommand m_command{EGameCommand::interactive};
        bool m_profile100{false};
        u64 m_profile_csv_frames{0u};
        std::string m_profile_label{"live"};
        u64 m_profile_skip{0u};
        bool m_profile_edits{false};
        bool m_profile_pan{false};
        std::optional<double> m_exit_after{};
        std::optional<std::string> m_diag_crash{};
        std::optional<std::string> m_config{};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &GameSettings::m_command,
                    Metadata{
                        "game.command", "Game command selector", "Command", EMutability::startup, ESource::cli, false},
                    game_command_domain),
                field(
                    &GameSettings::m_profile100,
                    Metadata{
                        "game.profile100", "100-agent profiling preset (also selects profiling defaults)", "Profiling",
                        EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &GameSettings::m_profile_csv_frames,
                    Metadata{
                        "game.profile_csv_frames", "Live CSV dump after N measured frames (0 disables)", "Profiling",
                        EMutability::startup, ESource::all, false},
                    any_domain),
                field(
                    &GameSettings::m_profile_label,
                    Metadata{
                        "game.profile_label", "Label attached to every profile-csv banner/frame line", "Profiling",
                        EMutability::startup, ESource::all, false},
                    StringDomain{1024u, true}),
                field(
                    &GameSettings::m_profile_skip,
                    Metadata{
                        "game.profile_skip", "Measured-window skip: first K frames run but are excluded", "Profiling",
                        EMutability::startup, ESource::all, false},
                    any_domain),
                field(
                    &GameSettings::m_profile_edits,
                    Metadata{
                        "game.profile_edits", "Scripted S2 edit schedule inside measured windows", "Profiling",
                        EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_profile_pan,
                    Metadata{
                        "game.profile_pan", "Scripted camera route inside measured windows", "Profiling",
                        EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_exit_after,
                    Metadata{
                        "game.exit_after", "Exit gracefully after S wall-clock seconds", "Run", EMutability::startup,
                        ESource::all, false},
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                field(
                    &GameSettings::m_diag_crash,
                    Metadata{
                        "game.diag_crash", "TEST-ONLY armed crash mechanism (abort|failfast)", "Diagnostics",
                        EMutability::startup, ESource::cli, false},
                    StringDomain{64u, true}),
                field(
                    &GameSettings::m_config,
                    Metadata{
                        "game.config", "Explicit base configuration file (TOML)", "Config", EMutability::startup,
                        ESource::cli, false},
                    PathDomain{1024u, false}),
            };
        }

        [[nodiscard]] static std::error_code validate(const GameSettings &settings, config::Diagnostics &diagnostics);
    };

    /// Fully resolved game invocation: typed settings per owning schema plus
    /// the parsed CLI (provenance for trigger detection) and whether the
    /// profiling-default layer was selected.
    struct GameInvocation {
        config::Resolved<GameSettings> m_game{};
        config::Resolved<DisplaySettings> m_display{};
        config::Resolved<StartupSettings> m_startup{};
        config::CliResult m_cli{};
        Array<std::string> m_config_paths{};
        bool m_profiling{false};
        bool m_help_requested{false};
    };

    /// Combined invocation CLI: game bindings plus the display/startup
    /// bindings owned by engine.app. `--help`/`-h` are reserved by the CLI
    /// adapter and suppress execution deterministically.
    [[nodiscard]] config::CliSchema gameCliSchema();

    /// Named profiling-default layer: supplies display.throttle_unfocused =
    /// false at the lowest precedence so an explicit setting overrides it.
    /// Selected by the legacy profiling triggers (see gameHelpText).
    [[nodiscard]] config::Layer makeProfilingDefaults();

    /// Generated help text from option metadata plus the profiling policy.
    [[nodiscard]] std::string gameHelpText();

    /// Pure resolution seam (no file I/O, no environment reads): partitions
    /// one parsed CLI plus caller-supplied lower-precedence layers across the
    /// three schemas and resolves each transactionally.
    [[nodiscard]] Expected<GameInvocation> resolveGameCli(
        const config::CliResult &cli, std::span<const config::Layer> lowerLayers, config::Diagnostics &diagnostics);

    /// Full executable-boundary resolution: parse CLI, load explicit
    /// --config files in argument order, collect the process environment,
    /// then resolve. Returns an invalid-usage error on any failure; the
    /// caller prints diagnostics and exits with kExitInvalidUsage. With
    /// --help no files are read and no settings resolve.
    [[nodiscard]] Expected<GameInvocation> resolveGameInvocation(
        int argc, char *const *argv, config::Diagnostics &diagnostics);

    /// Same as resolveGameInvocation, plus the generated picture-settings
    /// overlay between the base files and the environment (plan §1.5 order:
    /// base files, user overlay, environment, CLI, runtime overrides). A
    /// missing overlay file means first launch and loads nothing; an
    /// unreadable or undecodable overlay is invalid usage like a bad base
    /// file. An empty path skips the overlay entirely. Test executables use
    /// resolveGameInvocation and never load this file on their own.
    [[nodiscard]] Expected<GameInvocation> resolveGameInvocationWithOverlay(
        int argc, char *const *argv, const std::filesystem::path &overlay_path, config::Diagnostics &diagnostics);

    /// Executable schema check for all three invocation schemas.
    [[nodiscard]] Expected<void> validateGameSchemas(config::Diagnostics &diagnostics);
} // namespace pP::game
