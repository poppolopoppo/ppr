module;

#include "pP/Macros.h"

module game.config;

import engine.app;
import engine.core;
import engine.asset;
import game.display;

import std;

namespace pP::game {

    // ------------------------------------------------------------------
    // cli option definitions
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(GameConfig, info, none)
    namespace {
        [[nodiscard]] bool cliHas(const config::CliResult &cli, const std::string_view canonical) noexcept {
            for (const auto &assignment: cli.m_assignments) {
                if (assignment.m_name == canonical) {
                    return true;
                }
            }
            return false;
        }

        void appendOption(Array<config::OptionDecl> &options, config::OptionDecl option) {
            options.push_back(std::move(option));
        }

        [[nodiscard]] config::OptionDecl switchOption(
            const std::string_view canonical, const std::string_view legacy, const std::string_view description) {
            config::OptionDecl option{};
            option.m_canonical = canonical;
            option.m_description = description;
            option.m_aliases.push_back(legacy);
            option.m_is_switch = true;
            return option;
        }

        [[nodiscard]] config::OptionDecl valueOption(
            const std::string_view canonical, const std::string_view legacy, const std::string_view description) {
            config::OptionDecl option{};
            option.m_canonical = canonical;
            option.m_description = description;
            option.m_aliases.push_back(legacy);
            return option;
        }

        [[nodiscard]] config::OptionDecl selectorOption(
            const std::string_view alias, const std::string_view value, const std::string_view description) {
            config::OptionDecl option{};
            option.m_canonical = "game.command";
            option.m_description = description;
            option.m_aliases.push_back(alias);
            option.m_value_optional = true;
            option.m_absent_value = value;
            return option;
        }

        /// Fixed command-selection check over one parsed CLI: selectors take
        /// no values (the bare spelling records its fixed command), so an
        /// explicit value under a selector alias is invalid usage; two
        /// selectors naming different commands conflict. Repeat mentions of
        /// the same command are idempotent. Non-text assignments (e.g. an
        /// explicit reset) fall through to resolution diagnostics.
        [[nodiscard]] std::error_code checkSelectorConflicts(
            const config::CliResult &cli, config::Diagnostics &diagnostics) {
            const hal::ProfileScope scope{"Game.Config.CheckSelectors"};
            constexpr std::string_view kAliases[][2] = {
                {"smoke", "smoke"},
                {"validate-worldgen", "validate-worldgen"},
                {"worldgen-preview", "worldgen-preview"},
                {"worldgen-workshop", "worldgen-workshop"},
            };
            Array<std::string> seen{};

            for (const auto &assignment: cli.m_assignments) {
                if (assignment.m_name != "game.command" or assignment.m_value.m_kind != config::ESourceKind::text) {
                    continue;
                }
                for (const auto &[alias, fixed]: kAliases) {
                    if (assignment.m_alias_used == alias and assignment.m_value.m_text != fixed) {
                        diagnostics.add(
                            assignment.m_location, "game.command",
                            "selector '--" + assignment.m_alias_used + "' takes no value",
                            config::make_error_code(config::EConfigErrc::unexpected_value), assignment.m_alias_used);
                        return config::make_error_code(config::EConfigErrc::unexpected_value);
                    }
                }
                bool known = false;
                for (const auto &prior: seen) {
                    if (prior == assignment.m_value.m_text) {
                        known = true;
                        break;
                    }
                }
                if (not known) {
                    seen.push_back(assignment.m_value.m_text);
                }
            }

            if (seen.size() > 1u) {
                diagnostics.add(
                    "cli", "game.command",
                    "conflicting command selectors '" + seen.front() + "' and '" + seen.back() + "'",
                    config::make_error_code(config::EConfigErrc::conflicting_command), seen.back());
                return config::make_error_code(config::EConfigErrc::conflicting_command);
            }
            return {};
        }

        [[nodiscard]] Array<std::string> configPathsFrom(const config::CliResult &cli) {
            Array<std::string> paths{};
            for (const auto &assignment: cli.m_assignments) {
                if (assignment.m_name == "game.config" and assignment.m_value.m_kind == config::ESourceKind::text) {
                    paths.push_back(assignment.m_value.m_text);
                }
            }
            return paths;
        }

        [[nodiscard]] Expected<config::Layer> loadConfigFile(
            const std::string &path, config::Diagnostics &diagnostics) {
            const hal::ProfileScope scope{"Game.Config.LoadFile"};
            MappedFile file;
            try {
                file = MappedFile{hal::io::mapFile(path, {})};
            } catch (const std::system_error &) {
                diagnostics.add(
                    path, "game.config", "cannot open config file '" + path + "'",
                    config::make_error_code(config::EConfigErrc::codec_error), "config");
                PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, config::EConfigErrc::codec_error);
            }

            const std::string_view bytes = file.size() == 0u ? std::string_view{}
                                                             : std::string_view{file.c_str(), file.size()};
            auto document = config::decode(bytes, path, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, document);

            return config::toLayer(std::move(*document), path);
        }
    } // namespace

    // ------------------------------------------------------------------
    // explicit world-generation pack selection
    // ------------------------------------------------------------------

    namespace {
        struct PackRootDeclaration {
            std::string m_root;

            static constexpr auto describe() {
                return std::tuple{config::field(
                    &PackRootDeclaration::m_root, asset::metadata("root"), GameSettings::pack_path_domain,
                    config::EPresence::required)};
            }
        };

        struct PackSetSelection {
            config::KeyedCollection<PackRootDeclaration> m_packs;
            u32 m_schema_version{};

            static constexpr auto describe() {
                auto packs = config::collection(
                    &PackSetSelection::m_packs, asset::metadata("packs"),
                    {1u, (std::numeric_limits<std::size_t>::max)()});
                packs.m_presence = config::EPresence::required;

                return std::tuple{
                    config::field(
                        &PackSetSelection::m_schema_version, asset::metadata("schema_version"), config::Range{1u, 1u},
                        config::EPresence::required),
                    packs};
            }
        };
    }

    Expected<Array<std::filesystem::path>> loadWorldgenRoots(
        const GameSettings &settings, config::Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameConfig, settings.m_worldgen_root and settings.m_worldgen_pack_set
                            ? asset::reject(
                                  diagnostics, "invocation", "game.worldgen_pack_set",
                                  "--worldgen-pack-set and --worldgen-root cannot be supplied together")
                            : std::error_code{});

        Array<std::filesystem::path> roots;
        if (not settings.m_worldgen_pack_set) {
            const auto requested = settings.m_worldgen_root
                                       ? std::filesystem::path{*settings.m_worldgen_root}
                                       : hal::process::currentExecutablePath().parent_path() / "worldgen" / "default";
            std::error_code error;
            auto root = std::filesystem::canonical(requested, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);

            roots.push_back(std::move(root));
            return roots;
        }

        const hal::ProfileScope scope{"Game.Config.LoadWorldgenPackSet"};
        std::error_code error;
        const auto file = std::filesystem::canonical(*settings.m_worldgen_pack_set, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);
        const auto size = std::filesystem::file_size(file, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameConfig,
            size > asset::kMaxManifestBytes ? std::make_error_code(std::errc::file_too_large) : std::error_code{});
        auto mapped = io::mapFile(file);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, mapped);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameConfig, mapped->size() != size ? std::make_error_code(std::errc::resource_unavailable_try_again)
                                               : std::error_code{});

        auto imported = asset::importDescriptor<PackSetSelection>(
            {file, std::string{mapped->c_str(), mapped->size()}}, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, imported);

        auto &entries = imported->m_value.m_packs.m_entries;
        std::ranges::sort(entries, {}, &config::KeyedEntry<PackRootDeclaration>::m_key);

        std::set<std::filesystem::path> mounted;
        roots.reserve(entries.size());

        for (const auto &entry: entries) {
            const auto path = std::filesystem::canonical(file.parent_path() / entry.m_value.m_root, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                GameConfig, not mounted.insert(path).second
                                ? asset::reject(
                                      diagnostics, file.string(), "packs." + entry.m_key,
                                      "Pack-set roots must be distinct", config::EConfigErrc::duplicate_key)
                                : std::error_code{});

            roots.push_back(path);
        }

        return roots;
    }

    // ------------------------------------------------------------------
    // invocation option declarations
    // ------------------------------------------------------------------

    [[nodiscard]] config::CliSchema gameCliSchema() {
        const hal::ProfileScope scope{"Game.Config.Schema"};
        config::CliSchema schema{};
        schema.m_program = "ppr";
        schema.m_description = "PPR colony demo: interactive editor, headless commands, and profiling fixtures.";

        appendOption(
            schema.m_options,
            selectorOption(
                "worldgen-workshop", "worldgen-workshop",
                "Open Worldgen Workshop without generating a complete world; open an explicit authored pack root."));

        appendOption(
            schema.m_options, selectorOption("smoke", "smoke", "Run the colony smoke test (command selector)."));
        appendOption(
            schema.m_options, selectorOption(
                                  "validate-worldgen", "validate-worldgen",
                                  "Validate content definitions without binary textures, a window or GPU."));
        appendOption(
            schema.m_options, selectorOption(
                                  "worldgen-preview", "worldgen-preview",
                                  "Generate diagnostic exports without gameplay; requires an output directory."));
        appendOption(
            schema.m_options,
            switchOption(
                "game.worldgen_acceptance", "worldgen-acceptance",
                "Exercise camera, seam edit, previews and two New Games; report and exit, fail if incomplete."));
        appendOption(
            schema.m_options, valueOption("game.worldgen_root", "worldgen-root", "Explicit content pack root."));
        appendOption(
            schema.m_options, valueOption(
                                  "game.worldgen_pack_set", "worldgen-pack-set",
                                  "Explicit pack-set TOML; mounts only listed roots, relative to this file."));
        appendOption(
            schema.m_options, switchOption(
                                  "game.worldgen_report", "worldgen-report",
                                  "Print bounded interactive cadence and publication observations at shutdown."));
        appendOption(
            schema.m_options, valueOption(
                                  "game.worldgen_gpu_output", "worldgen-gpu-output",
                                  "Explicit GPU closeup capture with --worldgen-preview."));
        appendOption(
            schema.m_options, valueOption(
                                  "game.worldgen_output", "worldgen-output",
                                  "Export flat PPM diagnostics and report with --worldgen-preview."));
        appendOption(
            schema.m_options,
            valueOption(
                "game.worldgen_profile", "worldgen-profile", "Qualified World asset (local only for one pack)."));
        appendOption(
            schema.m_options, valueOption("game.worldgen_seed", "worldgen-seed", "Unsigned 64-bit world seed."));
        appendOption(
            schema.m_options,
            valueOption(
                "game.exit_after", "auto-exit", "Exit gracefully after S wall-clock seconds (alias: exit-after)."));
        schema.m_options.back().m_aliases.push_back("exit-after");
        appendOption(
            schema.m_options, valueOption("game.config", "config", "Explicit base configuration file (TOML)."));

        for (auto &option: displayCliOptions()) {
            schema.m_options.push_back(std::move(option));
        }

        for (auto &option: startupCliOptions()) {
            schema.m_options.push_back(std::move(option));
        }

        return schema;
    }

    [[nodiscard]] config::Layer makeProfilingDefaults() {
        config::Layer layer{};
        layer.m_source = config::ESource::none;
        layer.m_origin = "profiling-defaults";
        layer.m_assignments.push_back(
            config::Assignment{
                "display.throttle_unfocused", "", config::SourceValue::boolean(false), "profiling-defaults", 0u});
        return layer;
    }

    [[nodiscard]] std::string gameHelpText() {
        const config::CliSchema schema = gameCliSchema();
        std::string text = config::generateHelp(schema, GameSettings{});
        text += "\nProfiling policy:\n"
                "  --auto-exit/--exit-after and --no-focus-throttle\n"
                "  select profiling defaults (display.throttle_unfocused = false). An explicit\n"
                "  display setting overrides the profiling default.\n"
                "\nPicture settings file:\n"
                "  The Display window's Save writes the acknowledged picture settings to\n"
                "  user-settings.toml next to the executable (hand-edited files are never\n"
                "  rewritten). Command-line options and environment entries still take\n"
                "  priority over that file on the next launch. Test programs never load\n"
                "  it on their own.\n";
        return text;
    }

    [[nodiscard]] Expected<void> validateGameSchemas(config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Game.Config.ValidateSchemas"};
        if (auto valid = config::validateSchema<GameSettings>(diagnostics); not valid) {
            return std::unexpected{valid.error()};
        }
        if (auto valid = config::validateSchema<DisplaySettings>(diagnostics); not valid) {
            return std::unexpected{valid.error()};
        }
        if (auto valid = config::validateSchema<StartupSettings>(diagnostics); not valid) {
            return std::unexpected{valid.error()};
        }
        return Expected<void>{};
    }

    [[nodiscard]] Expected<GameInvocation> resolveGameCli(
        const config::CliResult &cli, const std::span<const config::Layer> lowerLayers,
        config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Game.Config.ResolveCli"};
        if (auto valid = validateGameSchemas(diagnostics); not valid) {
            return std::unexpected{valid.error()};
        }

        if (const std::error_code selectors = checkSelectorConflicts(cli, diagnostics)) {
            return std::unexpected{selectors};
        }

        Array<config::Layer> game_layers{};
        Array<config::Layer> display_layers{};
        Array<config::Layer> startup_layers{};
        std::error_code first_error{};

        const auto partition = [&](const config::Layer &layer) {
            Array<config::Assignment> game_assigns{};
            Array<config::Assignment> display_assigns{};
            Array<config::Assignment> startup_assigns{};
            for (const auto &assignment: layer.m_assignments) {
                if (assignment.m_name.starts_with("game.")) {
                    game_assigns.push_back(assignment);
                } else if (assignment.m_name.starts_with("display.")) {
                    display_assigns.push_back(assignment);
                } else if (assignment.m_name.starts_with("startup.")) {
                    startup_assigns.push_back(assignment);
                } else {
                    diagnostics.add(
                        layer.m_origin, assignment.m_name, "unknown property '" + assignment.m_name + "'",
                        config::make_error_code(config::EConfigErrc::unknown_property), assignment.m_alias_used);
                    if (not first_error) {
                        first_error = config::make_error_code(config::EConfigErrc::unknown_property);
                    }
                }
            }
            const auto emit = [&](Array<config::Layer> &out, Array<config::Assignment> assigns) {
                config::Layer part{};
                part.m_source = layer.m_source;
                part.m_origin = layer.m_origin;
                part.m_assignments = std::move(assigns);
                out.push_back(std::move(part));
            };
            emit(game_layers, std::move(game_assigns));
            emit(display_layers, std::move(display_assigns));
            emit(startup_layers, std::move(startup_assigns));
        };

        for (const auto &layer: lowerLayers) {
            partition(layer);
        }

        config::Layer cli_layer = config::toLayer(cli, "cli");
        partition(cli_layer);

        if (first_error) {
            return std::unexpected{first_error};
        }

        auto game = config::resolve<GameSettings>(
            std::span<const config::Layer>{game_layers.data(), game_layers.size()}, diagnostics);
        const bool profiling = game.has_value() and
                               (cliHas(cli, "game.exit_after") or cliHas(cli, "display.throttle_unfocused"));

        Array<config::Layer> display_precedence{};
        if (profiling) {
            display_precedence.push_back(makeProfilingDefaults());
        }
        for (auto &layer: display_layers) {
            display_precedence.push_back(std::move(layer));
        }
        auto display = config::resolve<DisplaySettings>(
            std::span<const config::Layer>{display_precedence.data(), display_precedence.size()}, diagnostics);

        auto startup = config::resolve<StartupSettings>(
            std::span<const config::Layer>{startup_layers.data(), startup_layers.size()}, diagnostics);

        if (not game) {
            return std::unexpected{game.error()};
        }
        if (not display) {
            return std::unexpected{display.error()};
        }
        if (not startup) {
            return std::unexpected{startup.error()};
        }

        const auto &settings = game->m_value;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            GameConfig, settings.m_worldgen_root and settings.m_worldgen_pack_set
                            ? asset::reject(
                                  diagnostics, "invocation", "game.worldgen_pack_set",
                                  "--worldgen-pack-set and --worldgen-root cannot be supplied together")
                            : std::error_code{});

        if ((settings.m_command == EGameCommand::worldgen_preview and
                not settings.m_worldgen_output and
                not settings.m_worldgen_gpu_output) or
            (settings.m_command != EGameCommand::worldgen_preview and settings.m_worldgen_gpu_output)) {
            const auto error = config::make_error_code(config::EConfigErrc::cross_field_invalid);
            diagnostics.add(
                "invocation", "game.command",
                "--worldgen-preview requires --worldgen-output or --worldgen-gpu-output; GPU capture requires the preview selector",
                error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);
        }

        if (settings.m_worldgen_acceptance and settings.m_command != EGameCommand::interactive) {
            const auto error = config::make_error_code(config::EConfigErrc::cross_field_invalid);
            diagnostics.add(
                "invocation", "game.worldgen_acceptance", "Acceptance requires an authored interactive session", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(GameConfig, error);
        }

        GameInvocation invocation{};
        invocation.m_game = std::move(*game);
        invocation.m_display = std::move(*display);
        invocation.m_startup = std::move(*startup);
        invocation.m_cli = cli;
        invocation.m_config_paths = configPathsFrom(cli);
        invocation.m_profiling = profiling;
        return invocation;
    }

    [[nodiscard]] Expected<GameInvocation> resolveGameInvocation(
        const int argc, char *const *argv, config::Diagnostics &diagnostics) {
        return resolveGameInvocationWithOverlay(argc, argv, std::filesystem::path{}, diagnostics);
    }

    [[nodiscard]] Expected<GameInvocation> resolveGameInvocationWithOverlay(
        const int argc, char *const *argv, const std::filesystem::path &overlay_path,
        config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Game.Config.ResolveWithOverlay"};
        const config::CliSchema schema = gameCliSchema();
        auto parsed = config::parseCliArgs(argc, argv, schema, diagnostics);
        if (not parsed) {
            return std::unexpected{parsed.error()};
        }

        if (parsed->m_help_requested) {
            GameInvocation help{};
            help.m_cli = std::move(*parsed);
            help.m_help_requested = true;
            return help;
        }

        Array<config::Layer> lower{};
        for (const auto &path: configPathsFrom(*parsed)) {
            auto layer = loadConfigFile(path, diagnostics);
            if (not layer) {
                return std::unexpected{layer.error()};
            }
            lower.push_back(std::move(*layer));
        }

        if (not overlay_path.empty()) {
            auto overlay = loadOverlayLayer(overlay_path, diagnostics);
            if (not overlay) {
                return std::unexpected{overlay.error()};
            }
            if (overlay->has_value()) {
                lower.push_back(std::move(**overlay));
            }
        }

        Array<config::Assignment> environment{};
        auto game_environment = config::collectEnvironment<GameSettings>(config::getenvLookup, diagnostics);
        auto display_environment = config::collectEnvironment<DisplaySettings>(config::getenvLookup, diagnostics);
        auto startup_environment = config::collectEnvironment<StartupSettings>(config::getenvLookup, diagnostics);
        for (auto &assignment: game_environment) {
            environment.push_back(std::move(assignment));
        }
        for (auto &assignment: display_environment) {
            environment.push_back(std::move(assignment));
        }
        for (auto &assignment: startup_environment) {
            environment.push_back(std::move(assignment));
        }
        if (not environment.empty()) {
            config::Layer environment_layer{};
            environment_layer.m_source = config::ESource::environment;
            environment_layer.m_origin = "environment";
            environment_layer.m_assignments = std::move(environment);
            lower.push_back(std::move(environment_layer));
        }

        return resolveGameCli(*parsed, std::span<const config::Layer>{lower.data(), lower.size()}, diagnostics);
    }
} // namespace pP::game
