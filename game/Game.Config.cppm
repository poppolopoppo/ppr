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
        exercise_display,
    };

    inline constexpr config::EnumDomain<EGameCommand, 6u> game_command_domain{{
        config::EnumEntry<EGameCommand>{EGameCommand::interactive, "interactive"},
        config::EnumEntry<EGameCommand>{EGameCommand::smoke, "smoke"},
        config::EnumEntry<EGameCommand>{EGameCommand::validate_worldgen, "validate-worldgen"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_preview, "worldgen-preview"},
        config::EnumEntry<EGameCommand>{EGameCommand::worldgen_workshop, "worldgen-workshop"},
        config::EnumEntry<EGameCommand>{EGameCommand::exercise_display, "exercise-display"},
    }};

    struct MapSizeDomain {
        [[nodiscard]] std::error_code validate(const u32 &value) const noexcept;

        [[nodiscard]] std::string describe() const;
    };

    /// Maintained profiling capture selector. The command launches the ordinary
    /// interactive application and records bounded evidence; it is not a
    /// gameplay toggle and conflicts with every other command selector.
    enum class EGameProfileCapture : u8 {
        systems,
        graphics,
    };

    inline constexpr config::EnumDomain<EGameProfileCapture, 2u> profile_capture_domain{{
        config::EnumEntry<EGameProfileCapture>{EGameProfileCapture::systems, "systems"},
        config::EnumEntry<EGameProfileCapture>{EGameProfileCapture::graphics, "graphics"},
    }};

    enum class EGameProfileWorkload : u8 {
        frozen,
        live,
    };

    inline constexpr config::EnumDomain<EGameProfileWorkload, 2u> profile_workload_domain{{
        config::EnumEntry<EGameProfileWorkload>{EGameProfileWorkload::frozen, "frozen"},
        config::EnumEntry<EGameProfileWorkload>{EGameProfileWorkload::live, "live"},
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
        std::optional<std::string> m_capture_frame{};
        std::optional<std::string> m_worldgen_root{};
        std::optional<std::string> m_worldgen_pack_set{};
        std::optional<std::string> m_worldgen_output{};
        std::optional<std::string> m_worldgen_gpu_output{};
        std::optional<std::string> m_exercise_output{};
        std::optional<std::string> m_profile_output{};
        std::optional<EGameProfileCapture> m_profile_capture{};
        EGameProfileWorkload m_profile_workload{EGameProfileWorkload::frozen};
        std::string m_worldgen_profile{"frontier"};
        std::string m_tool_spawn_material{"water"};
        double m_tool_radius_m{2.0}, m_tool_acceleration_m_s2{100.0};
        u64 m_worldgen_seed{42u};
        u32 m_tool_spawn_count{64u};
        u32 m_map_size{1024u};
        EGameCommand m_command{EGameCommand::interactive};
        bool m_worldgen_report{}, m_worldgen_acceptance{};

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
                    &GameSettings::m_exercise_output,
                    Metadata{
                        "game.exercise_output", "Fresh artifact directory for the display exercise", "Display exercise",
                        EMutability::startup, ESource::all, false},
                    PathDomain{1024u, false}),
                field(
                    &GameSettings::m_profile_output,
                    Metadata{
                        "game.profile_output", "Fresh artifact directory for --profile-capture", "Profiling capture",
                        EMutability::startup, ESource::all, false},
                    PathDomain{1024u, false}),
                field(
                    &GameSettings::m_profile_capture,
                    Metadata{
                        "game.profile_capture", "Bounded profiling capture of the ordinary lab: systems or graphics",
                        "Profiling capture", EMutability::startup, ESource::cli, false},
                    profile_capture_domain),
                field(
                    &GameSettings::m_profile_workload,
                    Metadata{
                        "game.profile_workload", "Profiling workload: frozen pauses before the first tick",
                        "Profiling capture", EMutability::startup, ESource::cli, false},
                    profile_workload_domain),
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
                        "game.map_size",
                        "Square map edge in fine cells: rendered games use 1024; 2048 and 4096 remain authoring extents",
                        "World generation", EMutability::startup, ESource::all, false},
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
                field(
                    &GameSettings::m_capture_frame,
                    Metadata{
                        "game.capture_frame", "Write the final interactive frame to this PNG path", "Capture",
                        EMutability::startup, ESource::cli, false},
                    PathDomain{1024u, false}),
            };
        }
    };

    // ------------------------------------------------------------------
    // sky and post display settings
    // ------------------------------------------------------------------

    /// Runtime-tunable sky and post settings. The retained sky hour keeps its
    /// -1 black-boundary meaning: -1 selects no sky contribution and 0..23
    /// rotate the sky. The retained diagnostic view and its cascade atlas
    /// level inspect the one lighting pipeline; they never select an
    /// alternative algorithm or change the allocated cascade count. Persisted
    /// under `game.world_light.*` and edited in the Display window's
    /// "Lighting & Post" section.
    struct WorldLightDisplaySettings {
        i32 m_sky_hour{-1};
        ELightingDiagnostic m_diagnostic{ELightingDiagnostic::off};
        u32 m_cascade{1u};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &WorldLightDisplaySettings::m_sky_hour,
                    Metadata{
                        "game.world_light.sky_hour",
                        "Field sky solar hour (-1 uses the black boundary sky; 12 is noon)", "World lighting",
                        EMutability::runtime, ESource::all, true},
                    Range<i32>{-1, 23}),
                field(
                    &WorldLightDisplaySettings::m_diagnostic,
                    Metadata{
                        "game.world_light.diagnostic", "Retained lighting inspection view", "World lighting",
                        EMutability::runtime, ESource::all, true},
                    lighting_diagnostic_domain),
                field(
                    &WorldLightDisplaySettings::m_cascade,
                    Metadata{
                        "game.world_light.cascade",
                        "Inspection cascade atlas level (1-5); not an active cascade-count setting", "World lighting",
                        EMutability::runtime, ESource::all, true},
                    Range<u32>{WorldLightingPass::kInspectionCascadeMin, WorldLightingPass::kInspectionCascadeMax}),
            };
        }
    };

    /// Retained inspection selection carried by the live session value. The
    /// cascade entry is inspection-only and never changes the allocated or
    /// updated cascade count.
    [[nodiscard]] constexpr LightingInspectionSettings lightingInspectionFrom(
        const WorldLightDisplaySettings &settings) noexcept {
        return LightingInspectionSettings{settings.m_diagnostic, settings.m_cascade};
    }

    /// Retained pixelization control: the resolved-scene resolution factor P.
    /// P1 keeps the resolved extent without a reduction but Hable
    /// reconstruction stays on. Persisted under `game.pixel.*`.
    struct PixelDisplaySettings {
        u32 m_pixel_scale{3u};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &PixelDisplaySettings::m_pixel_scale,
                    Metadata{
                        "game.pixel.scale", "Pixelization factor P (1..8): render the scene at 1/P resolution",
                        "Rendering", EMutability::runtime, ESource::all, true},
                    Range<u32>{1u, 8u}),
            };
        }
    };

    /// Runtime-tunable independent post-processing settings. Eye adaptation
    /// and bloom are applied to PostProcessPass through applyPostProcessSettings;
    /// reconstruction history and lighting are not part of this schema.
    struct PostDisplaySettings {
        bool m_auto_exposure{true};
        float m_exposure_ev{0.0f};
        float m_adaptation_tau{0.7f};
        float m_ev_min{-4.0f};
        float m_ev_max{4.0f};
        bool m_bloom{true};
        float m_bloom_intensity{0.125f};
        float m_bloom_threshold{1.0f};

        [[nodiscard]] static std::error_code validate(
            const PostDisplaySettings &settings, config::Diagnostics &diagnostics);

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &PostDisplaySettings::m_auto_exposure,
                    Metadata{
                        "game.post.auto_exposure", "Adapt exposure from a scene luminance histogram", "Post",
                        EMutability::runtime, ESource::all, true},
                    boolean_domain),
                field(
                    &PostDisplaySettings::m_exposure_ev,
                    Metadata{
                        "game.post.exposure_ev", "Manual exposure offset in EV", "Post", EMutability::runtime,
                        ESource::all, true},
                    Range<float>{-16.0f, 16.0f}),
                field(
                    &PostDisplaySettings::m_adaptation_tau,
                    Metadata{
                        "game.post.adaptation_tau", "Eye-adaptation time constant in seconds", "Post",
                        EMutability::runtime, ESource::all, true},
                    Range<float>{0.1f, 5.0f}),
                field(
                    &PostDisplaySettings::m_ev_min,
                    Metadata{
                        "game.post.ev_min", "Lower adaptation clamp around manual exposure (EV)", "Post",
                        EMutability::runtime, ESource::all, true},
                    Range<float>{-8.0f, 8.0f}),
                field(
                    &PostDisplaySettings::m_ev_max,
                    Metadata{
                        "game.post.ev_max", "Upper adaptation clamp around manual exposure (EV)", "Post",
                        EMutability::runtime, ESource::all, true},
                    Range<float>{-8.0f, 8.0f}),
                field(
                    &PostDisplaySettings::m_bloom,
                    Metadata{
                        "game.post.bloom", "Enable SPD bloom on the resolved HDR scene", "Post", EMutability::runtime,
                        ESource::all, true},
                    boolean_domain),
                field(
                    &PostDisplaySettings::m_bloom_intensity,
                    Metadata{
                        "game.post.bloom_intensity", "Bloom contribution scale (0 to 2)", "Post", EMutability::runtime,
                        ESource::all, true},
                    Range<float>{0.0f, 2.0f}),
                field(
                    &PostDisplaySettings::m_bloom_threshold,
                    Metadata{
                        "game.post.bloom_threshold", "Bloom bright-pass threshold (0 to 16)", "Post",
                        EMutability::runtime, ESource::all, true},
                    Range<float>{0.0f, 16.0f}),
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
        config::Resolved<WorldLightDisplaySettings> m_world_light{};
        config::Resolved<PixelDisplaySettings> m_pixel{};
        config::Resolved<PostDisplaySettings> m_post{};
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

    /// CLI bindings for the world light, pixelization and post display
    /// settings, composed into gameCliSchema. Canonical names are the property
    /// names; the kebab-case aliases are the legacy game spellings.
    [[nodiscard]] Array<config::OptionDecl> worldLightPostCliOptions();

    /// Merges acknowledged world-light, pixelization and post settings into an
    /// existing overlay document: every `game.world_light.*`, `game.pixel.*`
    /// and `game.post.*` entry is replaced by the persistable properties only,
    /// while unrelated entries (including `display.*`) keep their order and
    /// values. Hand-edited base files are never touched by this path.
    [[nodiscard]] Expected<config::Document> buildLightingPostOverlayDocument(
        const config::Document &existing, const WorldLightDisplaySettings &world_light,
        const PixelDisplaySettings &pixel, const PostDisplaySettings &post);

    /// Panel-draft validation: apply the draft as one runtime layer over
    /// committed state (schema rules, domain, range, cross-field only).
    [[nodiscard]] Expected<config::Resolved<WorldLightDisplaySettings>> validateWorldLightDraft(
        const config::Resolved<WorldLightDisplaySettings> &committed, const WorldLightDisplaySettings &draft,
        config::Diagnostics &diagnostics);

    /// Panel-draft validation for the post settings, same contract.
    [[nodiscard]] Expected<config::Resolved<PostDisplaySettings>> validatePostDraft(
        const config::Resolved<PostDisplaySettings> &committed, const PostDisplaySettings &draft,
        config::Diagnostics &diagnostics);

    /// Panel-draft validation for the pixelization factor, same contract.
    [[nodiscard]] Expected<config::Resolved<PixelDisplaySettings>> validatePixelDraft(
        const config::Resolved<PixelDisplaySettings> &committed, const PixelDisplaySettings &draft,
        config::Diagnostics &diagnostics);

    /// Applies the live post display settings to the independent post owner.
    /// Pixelization travels separately on the owned frame request because it is
    /// a rendering-resolution control, not a post-processing parameter.
    void applyPostProcessSettings(PostProcessSettings &post_process, const PostDisplaySettings &post) noexcept;
} // namespace pP::game
