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
        lighting_scene,
    };

    inline constexpr config::EnumDomain<EGameCommand, 6u> game_command_domain{{
        config::EnumEntry<EGameCommand>{EGameCommand::interactive, "interactive"},
        config::EnumEntry<EGameCommand>{EGameCommand::smoke, "smoke"},
        config::EnumEntry<EGameCommand>{EGameCommand::validate_worldgen, "validate-worldgen"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_preview, "worldgen-preview"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_workshop, "worldgen-workshop"},
        config::EnumEntry<EGameCommand>{EGameCommand::lighting_scene, "lighting-scene"},
    }};

    enum class EWorldLighting : u8 {
        legacy,
        direct_window,
        single_bounce_window,
        temporal_cascades
    };

    inline constexpr config::EnumDomain<EWorldLighting, 4u> world_lighting_domain{{
        config::EnumEntry<EWorldLighting>{EWorldLighting::legacy, "legacy"},
        config::EnumEntry<EWorldLighting>{EWorldLighting::direct_window, "direct-window"},
        config::EnumEntry<EWorldLighting>{EWorldLighting::single_bounce_window, "single-bounce-window"},
        config::EnumEntry<EWorldLighting>{EWorldLighting::temporal_cascades, "temporal-cascades"},
    }};

    struct MapSizeDomain {
        [[nodiscard]] std::error_code validate(const u32 &value) const noexcept;

        [[nodiscard]] std::string describe() const;
    };

    /// Game-owned invocation settings: command selection, profiling fixtures,
    /// bounded-run duration and the explicit config
    /// file path. Display pacing and RHI validation live in the engine.app
    /// schemas; this schema only declares the game.* bindings of the combined
    /// invocation CLI.
    struct GameSettings {
        static constexpr config::PathDomain pack_path_domain{1024u, false};

        std::optional<double> m_exit_after{};
        std::optional<std::string> m_config{};
        std::optional<std::string> m_lighting_showcase_frame_capture{};
        std::optional<std::string> m_lighting_showcase_door_check{};
        std::optional<std::string> m_lighting_showcase_lamp_check{};
        std::optional<std::string> m_direct_window_frame_capture{};
        std::optional<std::string> m_worldgen_root{};
        std::optional<std::string> m_worldgen_pack_set{};
        std::optional<std::string> m_worldgen_output{};
        std::optional<std::string> m_worldgen_gpu_output{};
        std::string m_worldgen_profile{"frontier"};
        std::string m_tool_spawn_material{"water"};
        double m_tool_radius_m{2.0}, m_tool_acceleration_m_s2{100.0};
        u64 m_worldgen_seed{42u};
        u32 m_tool_spawn_count{64u};
        u32 m_map_size{2048u};
        u32 m_rc_probe_spacing{4u}, m_rc_cascade_count{6u}, m_rc_base_directions{4u};
        float m_rc_interval_scale{1.0f};
        i32 m_rc_sky_hour{-1};
        u32 m_rc_day_seconds{86400u};
        EGameCommand m_command{EGameCommand::interactive};
        EWorldLighting m_world_lighting{EWorldLighting::temporal_cascades};
        bool m_worldgen_report{}, m_worldgen_acceptance{}, m_lighting_scene_qualified_fp64{};
        bool m_lighting_showcase{}, m_lighting_showcase_qualified_fp64{}, m_lighting_showcase_metrics{};
        bool m_lighting_showcase_screen_contact{}, m_rc_skip_table{true}, m_rc_distant_query_check{};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &GameSettings::m_rc_distant_query_check,
                    Metadata{
                        "game.rc_distant_query_check", "Check offscreen gameplay lighting during Matter updates",
                        "World lighting", EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_rc_skip_table,
                    Metadata{
                        "game.rc_skip_table", "Use R8UI temporal RC empty-block skips", "World lighting",
                        EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &GameSettings::m_rc_probe_spacing,
                    Metadata{
                        "game.rc_probe_spacing", "Temporal RC probe spacing in native cells (power of two)",
                        "World lighting", EMutability::startup, ESource::all, false},
                    Range<u32>{2u, 64u}),
                field(
                    &GameSettings::m_rc_cascade_count,
                    Metadata{
                        "game.rc_cascade_count", "Temporal RC level count including refined field", "World lighting",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{2u, 8u}),
                field(
                    &GameSettings::m_rc_base_directions,
                    Metadata{
                        "game.rc_base_directions", "Temporal RC base angular samples (4, 8, or 16)", "World lighting",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{4u, 16u}),
                field(
                    &GameSettings::m_rc_sky_hour,
                    Metadata{
                        "game.rc_sky_hour", "Initial solar hour (-1 uses the source boundary)", "World lighting",
                        EMutability::startup, ESource::all, false},
                    Range<i32>{-1, 23}),
                field(
                    &GameSettings::m_rc_day_seconds,
                    Metadata{
                        "game.rc_day_seconds", "Simulation seconds per sky day", "World lighting", EMutability::startup,
                        ESource::all, false},
                    Range<u32>{24u, 86400u}),
                field(
                    &GameSettings::m_rc_interval_scale,
                    Metadata{
                        "game.rc_interval_scale", "Temporal RC interval length scale", "World lighting",
                        EMutability::startup, ESource::all, false},
                    Range<float>{0.25f, 16.0f}),
                field(
                    &GameSettings::m_world_lighting,
                    Metadata{
                        "game.world_lighting",
                        "Opt-in direct-window or single-bounce-window terrain lighting with BLACK boundary sky; static terrain tools, no scattering or particle optical sources",
                        "World lighting", EMutability::startup, ESource::all, false},
                    world_lighting_domain),
                field(
                    &GameSettings::m_direct_window_frame_capture,
                    Metadata{
                        "game.direct_window_frame_capture",
                        "Capture one completed actual generated-world direct-window frame as display-encoded PPM",
                        "World lighting", EMutability::startup, ESource::cli, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_lighting_showcase_lamp_check,
                    Metadata{
                        "game.lighting_showcase_lamp_check",
                        "Exercise the mounted amber lamp and capture on/off/on playable frames in an existing directory",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_lighting_showcase_door_check,
                    Metadata{
                        "game.lighting_showcase_door_check",
                        "Exercise the real service door and capture closed/open playable frames in an existing directory",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_lighting_showcase_frame_capture,
                    Metadata{
                        "game.lighting_showcase_frame_capture",
                        "Write one completed interactive showcase frame, including UI, as display-encoded PPM",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    pack_path_domain),
                field(
                    &GameSettings::m_lighting_showcase,
                    Metadata{
                        "game.lighting_showcase",
                        "Play the bounded catalog-backed lighting showcase; terrain and source tools are disabled",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &GameSettings::m_lighting_showcase_screen_contact,
                    Metadata{
                        "game.lighting_showcase_screen_contact",
                        "Enable the bounded current-frame backwall screen-contact experiment", "Lighting showcase",
                        EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &GameSettings::m_lighting_showcase_metrics,
                    Metadata{
                        "game.lighting_showcase_metrics",
                        "Measure producer and warm whole-frame GPU work without a surface capture; diagnostic only",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &GameSettings::m_lighting_showcase_qualified_fp64,
                    Metadata{
                        "game.lighting_showcase_qualified_fp64",
                        "Explicit current-session RTE64 assertion for the lighting showcase; default strict FP32",
                        "Lighting showcase", EMutability::startup, ESource::cli, false},
                    boolean_domain),
                field(
                    &GameSettings::m_lighting_scene_qualified_fp64,
                    Metadata{
                        "game.lighting_scene_qualified_fp64",
                        "Assert RTE64 qualification for this Vulkan device session and use experimental FP64 refinement in --lighting-scene",
                        "Lighting capture", EMutability::startup, ESource::cli, false},
                    boolean_domain),
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
                    &GameSettings::m_tool_radius_m,
                    Metadata{
                        "game.tool_radius_m", "Simulation brush radius in metres", "Simulation tools",
                        EMutability::startup, ESource::all, false},
                    Range<double>{1.0 / 3.0, 32.0}),
                field(
                    &GameSettings::m_tool_acceleration_m_s2,
                    Metadata{
                        "game.tool_acceleration_m_s2", "Push and pull acceleration in metres per second squared",
                        "Simulation tools", EMutability::startup, ESource::all, false},
                    Range<double>{0.0, 1000.0}),
                field(
                    &GameSettings::m_tool_spawn_count,
                    Metadata{
                        "game.tool_spawn_count", "Maximum particles created by one spawn stroke", "Simulation tools",
                        EMutability::startup, ESource::all, false},
                    Range<u32>{1u, 4096u}),
                field(
                    &GameSettings::m_tool_spawn_material,
                    Metadata{
                        "game.tool_spawn_material", "Particle material key used by the spawn tool", "Simulation tools",
                        EMutability::startup, ESource::all, false},
                    asset::QualifiedName::reference_domain),
                field(
                    &GameSettings::m_map_size,
                    Metadata{
                        "game.map_size", "Square map edge in fine cells: 1024, 2048 or 4096", "World generation",
                        EMutability::startup, ESource::all, false},
                    MapSizeDomain{}),
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
