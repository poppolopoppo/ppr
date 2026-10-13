module;

#include "pP/Macros.h"

module game.config;

import engine.app;
import engine.core;
import game.display;

import std;

namespace pP::game {
    PPR_DEFINE_LOG_CATEGORY(GameConfig, info, none);
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
            std::ifstream file{path, std::ios::binary};
            if (not file) {
                diagnostics.add(
                    path, "game.config", "cannot open config file '" + path + "'",
                    config::make_error_code(config::EConfigErrc::codec_error), "config");
                return std::unexpected{config::make_error_code(config::EConfigErrc::codec_error)};
            }
            const std::string bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            auto document = config::decode(bytes, path, diagnostics);
            if (not document) {
                return std::unexpected{document.error()};
            }
            return config::toLayer(std::move(*document), path);
        }
    } // namespace

    [[nodiscard]] config::CliSchema gameCliSchema() {
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
                                  "Validate the complete content pack and textures without a window or GPU."));
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
            valueOption("game.worldgen_profile", "worldgen-profile", "World profile key (default frontier)."));
        appendOption(
            schema.m_options, valueOption("game.worldgen_seed", "worldgen-seed", "Unsigned 64-bit world seed."));
        appendOption(
            schema.m_options,
            switchOption(
                "game.profile100", "profile100", "100-agent profiling preset (also selects profiling defaults)."));
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
                "  --profile100, --auto-exit/--exit-after, and --no-focus-throttle\n"
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
        const bool profiling = game.has_value() and (game->m_value.m_profile100 or
                                                        cliHas(cli, "game.exit_after") or
                                                        cliHas(cli, "display.throttle_unfocused"));

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

        if (settings.m_worldgen_acceptance and
            (settings.m_command != EGameCommand::interactive or settings.m_profile100)) {
            const auto error = config::make_error_code(config::EConfigErrc::cross_field_invalid);
            diagnostics.add(
                "invocation", "game.worldgen_acceptance",
                "Acceptance requires an authored interactive session without --profile100", error);
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
