export module game.config;

// ------------------------------------------------------------------
// game config
// ------------------------------------------------------------------

import engine.app;
import engine.core;
import engine.asset;

import std;

export namespace pP::game {
    /// Game command selector. `--smoke` selects the headless smoke test; the default runs the interactive editor.
    /// Selectors are CLI-only fixed selections accepted in any position
    /// (first-position usage is preserved as a subset). Conflicting
    /// selectors (different commands) and explicit selector values are
    /// rejected as invalid usage before resolution.
    enum class EGameCommand : u8 {
        interactive,
        smoke,
        validate_worldgen,
        worldgen_preview,
        worldgen_workshop,
    };

    inline constexpr config::EnumDomain<EGameCommand, 5u> game_command_domain{{
        config::EnumEntry<EGameCommand>{EGameCommand::interactive, "interactive"},
        config::EnumEntry<EGameCommand>{EGameCommand::smoke, "smoke"},
        config::EnumEntry<EGameCommand>{EGameCommand::validate_worldgen, "validate-worldgen"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_preview, "worldgen-preview"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_workshop, "worldgen-workshop"},
    }};

    /// Game-owned invocation settings: command selection, profiling fixtures,
    /// bounded-run duration and the explicit config
    /// file path. Display pacing and RHI validation live in the engine.app
    /// schemas; this schema only declares the game.* bindings of the combined
    /// invocation CLI.
    struct GameSettings {
        static constexpr config::PathDomain pack_path_domain{1024u, false};

        std::optional<double> m_exit_after{};
        std::optional<std::string> m_config{};
        std::optional<std::string> m_worldgen_root{};
        std::optional<std::string> m_worldgen_pack_set{};
        std::optional<std::string> m_worldgen_output{};
        std::optional<std::string> m_worldgen_gpu_output{};
        std::string m_worldgen_profile{"frontier"};
        u64 m_worldgen_seed{42u};
        EGameCommand m_command{EGameCommand::interactive};
        bool m_profile100{false}, m_worldgen_report{}, m_worldgen_acceptance{};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &GameSettings::m_worldgen_acceptance,
                    Metadata{
                        "game.worldgen_acceptance", "Run the bounded interactive world-generation acceptance sequence",
                        "World generation", EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_worldgen_report,
                    Metadata{
                        "game.worldgen_report", "Report observed interactive world-generation cadence at shutdown",
                        "World generation", EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_worldgen_gpu_output,
                    Metadata{
                        "game.worldgen_gpu_output", "Offscreen GPU closeup directory", "World generation",
                        EMutability::startup, ESource::all, false},
                    PathDomain{1024u, false}),
                field(
                    &GameSettings::m_worldgen_output,
                    Metadata{
                        "game.worldgen_output", "Headless diagnostic export directory", "World generation",
                        EMutability::startup, ESource::all, false},
                    PathDomain{1024u, false}),
                field(
                    &GameSettings::m_worldgen_pack_set,
                    Metadata{
                        "game.worldgen_pack_set", "Explicit pack-set TOML file (roots relative to that file)",
                        "World generation", EMutability::startup, ESource::all, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_worldgen_root,
                    Metadata{
                        "game.worldgen_root", "Explicit world-generation pack root", "World generation",
                        EMutability::startup, ESource::all, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_worldgen_profile,
                    Metadata{
                        "game.worldgen_profile", "World-generation profile key", "World generation",
                        EMutability::startup, ESource::all, false},
                    asset::QualifiedName::reference_domain),
                field(
                    &GameSettings::m_worldgen_seed,
                    Metadata{
                        "game.worldgen_seed", "World-generation unsigned 64-bit seed", "World generation",
                        EMutability::startup, ESource::all, false},
                    Range<u64>{0u, (std::numeric_limits<u64>::max)()}),
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
                    &GameSettings::m_exit_after,
                    Metadata{
                        "game.exit_after", "Exit gracefully after S wall-clock seconds", "Run", EMutability::startup,
                        ESource::all, false},
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                field(
                    &GameSettings::m_config,
                    Metadata{
                        "game.config", "Explicit base configuration file (TOML)", "Config", EMutability::startup,
                        ESource::cli, false},
                    PathDomain{1024u, false}),
            };
        }
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

    /// Explicit roots only. A pack-set never implicitly mounts the default pack;
    /// supplying both root and pack-set is an invocation conflict.
    [[nodiscard]] Expected<Array<std::filesystem::path>> loadWorldgenRoots(
        const GameSettings &settings, config::Diagnostics &diagnostics);

    /// Named profiling-default layer: supplies display.throttle_unfocused =
    /// false at the lowest precedence so an explicit setting overrides it.
    /// Selected by the profiling triggers described in gameHelpText.
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
