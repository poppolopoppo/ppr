module;

#include "pP/UnitTest.h"

module game.tests;

import engine.app;
import engine.core;
import engine.asset;
import engine.math;
import game.config;

import std;

namespace pP::game::tests::detail::compat {
    // ------------------------------------------------------------------
    // configuration compatibility
    // ------------------------------------------------------------------

    using namespace pP::config;

    [[nodiscard]] Array<std::string_view> spell(const std::initializer_list<const char *> words) {
        Array<std::string_view> args{};
        for (const char *word: words) {
            args.emplace_back(word != nullptr ? word : "");
        }
        return args;
    }

    // Pure CLI-only resolution seam: parse one argv tail (no program name)
    // and resolve with no lower layers (no file I/O, no environment reads).
    [[nodiscard]] Expected<GameInvocation> resolveCli(
        const std::initializer_list<const char *> words, Diagnostics &diagnostics) {
        const CliSchema schema = gameCliSchema();
        Array<std::string_view> args = spell(words);
        auto parsed = parseCliArgs(std::span<const std::string_view>{args.data(), args.size()}, schema, diagnostics);
        if (not parsed) {
            return std::unexpected{parsed.error()};
        }
        return resolveGameCli(*parsed, std::span<const Layer>{}, diagnostics);
    }

    // Full executable-boundary resolution (program name first, then the tail).
    // Reads explicit --config files and the process environment.
    [[nodiscard]] Expected<GameInvocation> invoke(
        const std::initializer_list<const char *> words, Diagnostics &diagnostics) {
        Array<std::string> owned{};
        for (const char *word: words) {
            owned.emplace_back(word != nullptr ? word : "");
        }
        Array<char *> argv{};
        for (auto &entry: owned) {
            argv.push_back(entry.data());
        }
        return resolveGameInvocation(static_cast<int>(argv.size()), argv.data(), diagnostics);
    }

    PPR_UNIT_TEST (selectors_pick_commands) {
        Diagnostics smoke_diagnostics{};
        auto smoke = resolveCli({"--smoke"}, smoke_diagnostics);
        PPR_TEST_ASSERT(smoke.has_value());
        PPR_TEST_ASSERT(smoke->m_game.m_value.m_command == EGameCommand::smoke);

        Diagnostics default_diagnostics{};
        auto interactive = resolveCli({}, default_diagnostics);
        PPR_TEST_ASSERT(interactive.has_value());
        PPR_TEST_ASSERT(interactive->m_game.m_value.m_command == EGameCommand::interactive);

        Diagnostics workshop_diagnostics{};
        const auto workshop = resolveCli(
            {"--worldgen-workshop", "--worldgen-seed=18446744073709551615"}, workshop_diagnostics);
        PPR_TEST_ASSERT(workshop.has_value());
        PPR_TEST_ASSERT(workshop->m_game.m_value.m_command == EGameCommand::worldgen_workshop);
        PPR_TEST_ASSERT(not workshop->m_game.m_value.m_worldgen_root);
        PPR_TEST_ASSERT(workshop->m_game.m_value.m_worldgen_seed == (std::numeric_limits<u64>::max)());
    };

    PPR_UNIT_TEST (map_size_uses_one_numeric_domain_for_cli_and_toml) {
        Diagnostics defaults;
        const auto normal = resolveCli({}, defaults);
        PPR_TEST_ASSERT(normal and normal->m_game.m_value.m_map_size == 1024u);
        PPR_TEST_ASSERT(gameHelpText().find("map-size") != std::string::npos);

        for (const u32 edge: {1024u, 2048u, 4096u}) {
            const auto option = "--map-size=" + std::to_string(edge);
            Diagnostics cli_diagnostics;
            const auto cli = resolveCli({option.c_str()}, cli_diagnostics);
            PPR_TEST_ASSERT(cli and cli->m_game.m_value.m_map_size == edge and not cli_diagnostics.hasErrors());

            Diagnostics file_diagnostics;
            const auto document = decode(
                "[game]\nmap_size = " + std::to_string(edge) + "\n", "map-size.toml", file_diagnostics);
            PPR_TEST_ASSERT(document.has_value());
            const auto layer = toLayer(*document, "map-size.toml");
            const auto resolved = resolveGameCli(CliResult{}, std::span<const Layer>{&layer, 1u}, file_diagnostics);
            PPR_TEST_ASSERT(resolved and resolved->m_game.m_value.m_map_size == edge);
            const auto *const origin = resolved->m_game.find("game.map_size");
            PPR_TEST_ASSERT(origin != nullptr and origin->m_location == "map-size.toml:2");
        }

        Diagnostics canonical_diagnostics;
        const auto canonical = resolveCli({"--game.map_size=2048"}, canonical_diagnostics);
        PPR_TEST_ASSERT(canonical and canonical->m_game.m_value.m_map_size == 2048u);

        for (const auto *const value: {"0", "128", "1536", "8192", "-1", "1024.5", "large"}) {
            const auto option = std::string{"--map-size="} + value;
            Diagnostics cli_diagnostics;
            PPR_TEST_ASSERT(not resolveCli({option.c_str()}, cli_diagnostics));
            PPR_TEST_ASSERT(cli_diagnostics.hasErrors());

            Diagnostics file_diagnostics;
            const auto document = decode(
                std::string{"[game]\nmap_size = "} + value + "\n", "map-size.toml", file_diagnostics);
            if (document) {
                const auto layer = toLayer(*document, "map-size.toml");
                PPR_TEST_ASSERT(not resolveGameCli(CliResult{}, std::span<const Layer>{&layer, 1u}, file_diagnostics));
            }

            PPR_TEST_ASSERT(file_diagnostics.hasErrors());
        }
    };

    PPR_UNIT_TEST (worldgen_validation_invocation) {
        Diagnostics diagnostics{};
        const auto command = resolveCli(
            {"--validate-worldgen", "--worldgen-root=pack", "--worldgen-profile=hollows",
                "--worldgen-seed=18446744073709551615", "--worldgen-output=previews"},
            diagnostics);
        PPR_TEST_ASSERT(command.has_value());
        PPR_TEST_ASSERT(command->m_game.m_value.m_command == EGameCommand::validate_worldgen);
        PPR_TEST_ASSERT(command->m_game.m_value.m_worldgen_root == "pack");
        PPR_TEST_ASSERT(command->m_game.m_value.m_worldgen_output == "previews");
        PPR_TEST_ASSERT(command->m_game.m_value.m_worldgen_profile == "hollows");
        PPR_TEST_ASSERT(not command->m_game.m_value.m_worldgen_gpu_output);
        PPR_TEST_ASSERT(command->m_game.m_value.m_worldgen_seed == (std::numeric_limits<u64>::max)());
        for (const auto seed: {"--worldgen-seed=-1", "--worldgen-seed=18446744073709551616"}) {
            Diagnostics bad{};
            PPR_TEST_ASSERT(not resolveCli({seed}, bad));
        }
        Diagnostics conflict{};
        PPR_TEST_ASSERT(not resolveCli({"--smoke", "--validate-worldgen"}, conflict));
        PPR_TEST_ASSERT(conflict.entries().back().m_code == make_error_code(EConfigErrc::conflicting_command));
        Diagnostics explicit_value{};
        PPR_TEST_ASSERT(not resolveCli({"--validate-worldgen=smoke"}, explicit_value));
        PPR_TEST_ASSERT(gameHelpText().find("validate-worldgen") != std::string::npos);
    };

    PPR_UNIT_TEST (worldgen_acceptance_is_explicit_and_interactive_only) {
        Diagnostics diagnostics;
        const auto invocation = resolveCli(
            {"--worldgen-acceptance", "--worldgen-seed=18446744073709551615"}, diagnostics);
        PPR_TEST_ASSERT(invocation and invocation->m_game.m_value.m_worldgen_acceptance);
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_worldgen_seed == (std::numeric_limits<u64>::max)());
        Diagnostics disabled;
        const auto off = resolveCli({"--worldgen-acceptance=false"}, disabled);
        PPR_TEST_ASSERT(off and not off->m_game.m_value.m_worldgen_acceptance);
        for (const auto *const selector: {"--smoke", "--validate-worldgen"}) {
            Diagnostics invalid;
            PPR_TEST_ASSERT(not resolveCli({selector, "--worldgen-acceptance"}, invalid));
            PPR_TEST_ASSERT(invalid.hasErrors());
        }
    };

    PPR_UNIT_TEST (worldgen_pack_set_and_qualified_world_are_explicit_inputs) {
        Diagnostics diagnostics;
        const auto invocation = resolveCli(
            {"--validate-worldgen", "--worldgen-pack-set=sets/selection.toml", "--worldgen-profile=addon:frontier"},
            diagnostics);
        PPR_TEST_ASSERT(invocation.has_value());
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_worldgen_pack_set == "sets/selection.toml");
        PPR_TEST_ASSERT(not invocation->m_game.m_value.m_worldgen_root);
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_worldgen_profile == "addon:frontier");

        const std::string qualified = std::string(asset::QualifiedName::maximum_namespace_bytes, 'a') +
                                      ":" +
                                      std::string(config::kMaxKeyBytes, 'b');
        const auto option = "--worldgen-profile=" + qualified;
        Diagnostics boundary;
        PPR_TEST_ASSERT(resolveCli({option.c_str()}, boundary).has_value());

        const auto too_long = option + "b";
        Diagnostics oversized;
        PPR_TEST_ASSERT(not resolveCli({too_long.c_str()}, oversized));
        PPR_TEST_ASSERT(oversized.entries().front().m_code == EConfigErrc::out_of_range);

        Diagnostics conflict;
        PPR_TEST_ASSERT(not resolveCli({"--worldgen-root=base", "--worldgen-pack-set=selection.toml"}, conflict));
        PPR_TEST_ASSERT(conflict.hasErrors());
        PPR_TEST_ASSERT(gameHelpText().find("worldgen-pack-set") != std::string::npos);
    };

    PPR_UNIT_TEST (pack_set_roots_are_relative_owned_and_never_discovered) {
        const auto parent = hal::process::currentExecutablePath().parent_path();
        std::filesystem::path directory;
        static std::atomic<u32> ordinal{};
        for (u32 attempt = 0u; attempt != 4096u; ++attempt) {
            const auto candidate = parent / ("pack-selection-test-" + std::to_string(ordinal.fetch_add(1u)));
            if (std::filesystem::create_directory(candidate)) {
                directory = std::filesystem::canonical(candidate);
                break;
            }
        }

        PPR_TEST_ASSERT(not directory.empty());
        PPR_DEFER {
            std::error_code cleanup;
            std::filesystem::remove_all(directory, cleanup);
            PPR_TEST_ASSERT(not cleanup);
        };

        PPR_TEST_ASSERT(std::filesystem::create_directory(directory / "base"));
        PPR_TEST_ASSERT(std::filesystem::create_directory(directory / "addon"));
        const auto selection = directory / "selection.toml";
        const auto write = [&](const std::string_view bytes) {
            std::ofstream output{selection, std::ios::binary | std::ios::trunc};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(output.good());
        };

        // Alias ordering is deterministic; neither root needs a discoverable
        // manifest merely to resolve this application-owned selection file.
        write("schema_version = 1\n[packs.z_addon]\nroot = 'addon'\n[packs.a_base]\nroot = 'base'\n");
        GameSettings settings;
        settings.m_worldgen_pack_set = selection.string();
        Diagnostics diagnostics;
        const auto roots = loadWorldgenRoots(settings, diagnostics);
        PPR_TEST_ASSERT(roots and roots->size() == 2u);
        PPR_TEST_ASSERT((*roots)[0u] == directory / "base");
        PPR_TEST_ASSERT((*roots)[1u] == directory / "addon");

        for (const std::string_view bytes:
            {"schema_version = 1\n", "schema_version = 1\n[packs]\n",
                "schema_version = 1\n[packs.a]\nroot = 'addon'\n[packs.b]\nroot = './addon'\n",
                "schema_version = 1\n[packs.a]\nroot = 'missing'\n", "schema_version = 2\n[packs.a]\nroot = 'base'\n",
                "schema_version = 1\n[packs.a]\nroot = 'base'\nunknown = true\n"}) {
            write(bytes);
            Diagnostics invalid;
            PPR_TEST_ASSERT(not loadWorldgenRoots(settings, invalid));
        }

        std::filesystem::resize_file(selection, asset::kMaxManifestBytes + 1u);
        Diagnostics large;
        const auto oversized = loadWorldgenRoots(settings, large);
        PPR_TEST_ASSERT(not oversized and oversized.error() == std::errc::file_too_large);
        PPR_TEST_ASSERT(roots->front() == directory / "base");
    };

    PPR_UNIT_TEST (worldgen_preview_is_distinct_and_requires_explicit_output) {
        Diagnostics diagnostics{};
        const auto preview = resolveCli(
            {"--worldgen-preview", "--worldgen-output=previews", "--worldgen-gpu-output=closeups"}, diagnostics);
        PPR_TEST_ASSERT(preview and preview->m_game.m_value.m_command == EGameCommand::worldgen_preview);
        PPR_TEST_ASSERT(preview->m_game.m_value.m_worldgen_gpu_output == "closeups");
        for (const auto options:
            {std::initializer_list<const char *>{"--worldgen-preview"},
                {"--validate-worldgen", "--worldgen-gpu-output=closeups"}, {"--worldgen-gpu-output=closeups"}}) {
            Diagnostics invalid{};
            const auto result = resolveCli(options, invalid);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::cross_field_invalid));
        }
        Diagnostics conflict{};
        const auto conflicting = resolveCli({"--worldgen-preview", "--validate-worldgen"}, conflict);
        PPR_TEST_ASSERT(not conflicting and conflicting.error() == make_error_code(EConfigErrc::conflicting_command));
        Diagnostics explicit_value{};
        PPR_TEST_ASSERT(not resolveCli({"--worldgen-preview=smoke"}, explicit_value));
        PPR_TEST_ASSERT(gameHelpText().find("worldgen-preview") != std::string::npos);
    };

    PPR_UNIT_TEST (display_exercise_requires_output_and_conflicts_with_other_selectors) {
        Diagnostics diagnostics{};
        const auto invocation = resolveCli({"--exercise-display", "--exercise-output=artifacts"}, diagnostics);
        PPR_TEST_ASSERT(invocation and invocation->m_game.m_value.m_command == EGameCommand::exercise_display);
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_exercise_output == "artifacts");
        for (const auto options:
            {std::initializer_list<const char *>{"--exercise-display"}, {"--exercise-output=artifacts"},
                {"--smoke", "--exercise-output=artifacts"}}) {
            Diagnostics invalid{};
            const auto result = resolveCli(options, invalid);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::cross_field_invalid));
        }
        Diagnostics conflict{};
        const auto conflicting = resolveCli(
            {"--exercise-display", "--exercise-output=artifacts", "--worldgen-preview", "--worldgen-output=previews"},
            conflict);
        PPR_TEST_ASSERT(not conflicting and conflicting.error() == make_error_code(EConfigErrc::conflicting_command));
        Diagnostics explicit_value{};
        PPR_TEST_ASSERT(not resolveCli({"--exercise-display=smoke"}, explicit_value));
        PPR_TEST_ASSERT(gameHelpText().find("exercise-display") != std::string::npos);
    };

    PPR_UNIT_TEST (profile_capture_requires_output_and_conflicts_with_other_selectors) {
        Diagnostics diagnostics{};
        const auto invocation = resolveCli(
            {"--profile-capture=graphics", "--profile-output=profile", "--profile-workload=live"}, diagnostics);
        PPR_TEST_ASSERT(invocation.has_value());
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_profile_capture == EGameProfileCapture::graphics);
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_profile_output == "profile");
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_profile_workload == EGameProfileWorkload::live);
        PPR_TEST_ASSERT(invocation->m_game.m_value.m_command == EGameCommand::interactive);

        Diagnostics default_workload{};
        const auto frozen = resolveCli({"--profile-capture=systems", "--profile-output=profile"}, default_workload);
        PPR_TEST_ASSERT(frozen.has_value());
        PPR_TEST_ASSERT(frozen->m_game.m_value.m_profile_workload == EGameProfileWorkload::frozen);

        for (const auto options:
            {std::initializer_list<const char *>{"--profile-capture=systems"}, {"--profile-output=profile"}}) {
            Diagnostics invalid{};
            const auto result = resolveCli(options, invalid);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::cross_field_invalid));
        }

        for (const auto options:
            {std::initializer_list<const char *>{
                 "--profile-capture=systems", "--profile-output=profile", "--exercise-display",
                 "--exercise-output=artifacts"},
                {"--profile-capture=systems", "--profile-output=profile", "--smoke"},
                {"--profile-capture=systems", "--profile-output=profile", "--worldgen-acceptance"}}) {
            Diagnostics conflict{};
            PPR_TEST_ASSERT(not resolveCli(options, conflict));
        }

        Diagnostics bad_tool{};
        PPR_TEST_ASSERT(not resolveCli({"--profile-capture=etw", "--profile-output=profile"}, bad_tool));
        PPR_TEST_ASSERT(gameHelpText().find("profile-capture") != std::string::npos);
    };

    PPR_UNIT_TEST (retired_workload_commands_rejected) {
        for (const char *option:
            {"--load100", "--time-trials", "--game.command=load100", "--game.command=time-trials"}) {
            Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not resolveCli({option}, diagnostics).has_value());
        }
    };

    PPR_UNIT_TEST (repeated_same_selector_is_idempotent) {
        Diagnostics diagnostics{};
        auto twice = resolveCli({"--smoke", "--smoke"}, diagnostics);
        PPR_TEST_ASSERT(twice.has_value());
        PPR_TEST_ASSERT(twice->m_game.m_value.m_command == EGameCommand::smoke);
    };

    PPR_UNIT_TEST (selector_explicit_values_rejected) {
        Diagnostics inline_diagnostics{};
        PPR_TEST_ASSERT(not resolveCli({"--smoke=load100"}, inline_diagnostics).has_value());

        Diagnostics unknown_diagnostics{};
        PPR_TEST_ASSERT(not resolveCli({"--smoke=foo"}, unknown_diagnostics).has_value());

        Diagnostics separate_diagnostics{};
        PPR_TEST_ASSERT(not resolveCli({"--smoke", "load100"}, separate_diagnostics).has_value());

        Diagnostics empty_diagnostics{};
        PPR_TEST_ASSERT(not resolveCli({"--smoke="}, empty_diagnostics).has_value());
    };

    PPR_UNIT_TEST (legacy_duration_forms) {
        Diagnostics space_diagnostics{};
        auto space = resolveCli({"--auto-exit", "5"}, space_diagnostics);
        PPR_TEST_ASSERT(space.has_value());
        PPR_TEST_ASSERT(space->m_game.m_value.m_exit_after == 5.0);
        PPR_TEST_ASSERT(space->m_profiling);

        Diagnostics equals_diagnostics{};
        auto equals = resolveCli({"--auto-exit=5"}, equals_diagnostics);
        PPR_TEST_ASSERT(equals.has_value());
        PPR_TEST_ASSERT(equals->m_game.m_value.m_exit_after == 5.0);

        Diagnostics alias_diagnostics{};
        auto alias = resolveCli({"--exit-after", "7"}, alias_diagnostics);
        PPR_TEST_ASSERT(alias.has_value());
        PPR_TEST_ASSERT(alias->m_game.m_value.m_exit_after == 7.0);
    };

    PPR_UNIT_TEST (zero_duration_rejected_even_when_shadowed) {
        Diagnostics shadowed_diagnostics{};
        const auto shadowed = resolveCli({"--auto-exit=0", "--auto-exit=1"}, shadowed_diagnostics);
        PPR_TEST_ASSERT(not shadowed.has_value());
        PPR_TEST_ASSERT(shadowed.error() == make_error_code(EConfigErrc::out_of_range));

        Diagnostics zero_diagnostics{};
        const auto zero = resolveCli({"--auto-exit", "0"}, zero_diagnostics);
        PPR_TEST_ASSERT(not zero.has_value());
        PPR_TEST_ASSERT(zero.error() == make_error_code(EConfigErrc::out_of_range));

        Diagnostics negative_diagnostics{};
        PPR_TEST_ASSERT(not resolveCli({"--auto-exit=-3"}, negative_diagnostics).has_value());
    };

    PPR_UNIT_TEST (retired_profiling_options_rejected) {
        for (const char *option:
            {"--profile-csv=10", "--profile-label=nightly", "--profile-skip=3", "--profile-edits", "--profile-pan",
                "--game.profile_csv_frames=10", "--game.profile_label=nightly", "--game.profile_skip=3",
                "--game.profile_edits", "--game.profile_pan"}) {
            Diagnostics diagnostics{};
            const auto result = resolveCli({option}, diagnostics);
            PPR_TEST_ASSERT(not result.has_value());
            PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::unknown_option));
        }
        for (const std::string_view property:
            {"game.profile_csv_frames", "game.profile_label", "game.profile_skip", "game.profile_edits",
                "game.profile_pan"}) {
            Layer file{};
            file.m_source = ESource::file;
            file.m_origin = "settings.toml";
            file.m_assignments.push_back(
                Assignment{std::string{property}, "", SourceValue::text("1"), "settings.toml:1", 0u});
            Diagnostics diagnostics{};
            const CliResult cli{};
            const auto result = resolveGameCli(cli, std::span<const Layer>{std::addressof(file), 1u}, diagnostics);
            PPR_TEST_ASSERT(not result.has_value());
            PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::unknown_property));
        }

        const std::string help = gameHelpText();
        for (const std::string_view option:
            {"profile-csv", "profile-label", "profile-skip", "profile-edits", "profile-pan"}) {
            PPR_TEST_ASSERT(help.find(option) == std::string::npos);
        }
    };

    PPR_UNIT_TEST (profiling_defaults_and_explicit_override) {
        Diagnostics preset_diagnostics{};
        const auto retired = resolveCli({"--profile100"}, preset_diagnostics);
        PPR_TEST_ASSERT(not retired.has_value() and preset_diagnostics.hasErrors());
        PPR_TEST_ASSERT(gameHelpText().find("profile100") == std::string::npos);

        Diagnostics report_diagnostics{};
        const auto report = resolveCli({"--worldgen-report"}, report_diagnostics);
        PPR_TEST_ASSERT(report and report->m_game.m_value.m_worldgen_report);
        PPR_TEST_ASSERT(not report->m_profiling);
        Diagnostics disabled_report_diagnostics{};
        const auto disabled_report = resolveCli({"--worldgen-report=false"}, disabled_report_diagnostics);
        PPR_TEST_ASSERT(disabled_report and not disabled_report->m_game.m_value.m_worldgen_report);

        Diagnostics exit_diagnostics{};
        auto bounded = resolveCli({"--auto-exit", "5"}, exit_diagnostics);
        PPR_TEST_ASSERT(bounded.has_value());
        PPR_TEST_ASSERT(bounded->m_profiling);

        Diagnostics throttle_diagnostics{};
        auto throttle = resolveCli({"--no-focus-throttle"}, throttle_diagnostics);
        PPR_TEST_ASSERT(throttle.has_value());
        PPR_TEST_ASSERT(throttle->m_profiling);
        PPR_TEST_ASSERT(not throttle->m_display.m_value.m_throttle_unfocused);

        // An explicit display setting overrides the profiling default.
        Diagnostics override_diagnostics{};
        auto overridden = resolveCli({"--exit-after=5", "--focus-throttle"}, override_diagnostics);
        PPR_TEST_ASSERT(overridden.has_value());
        PPR_TEST_ASSERT(overridden->m_profiling);
        PPR_TEST_ASSERT(overridden->m_display.m_value.m_throttle_unfocused);
    };

    PPR_UNIT_TEST (startup_booleans_resolve_both_ways) {
        Diagnostics on_diagnostics{};
        auto on = resolveCli({"--core-validation"}, on_diagnostics);
        PPR_TEST_ASSERT(on.has_value());
        PPR_TEST_ASSERT(on->m_startup.m_value.m_core_validation);
        const PropertyOrigin *on_origin = on->m_startup.find("startup.core_validation");
        PPR_TEST_ASSERT(on_origin != nullptr);
        PPR_TEST_ASSERT(on_origin->m_source == ESource::cli);

        Diagnostics off_diagnostics{};
        auto off = resolveCli({"--core-validation=false"}, off_diagnostics);
        PPR_TEST_ASSERT(off.has_value());
        PPR_TEST_ASSERT(not off->m_startup.m_value.m_core_validation);
        const PropertyOrigin *off_origin = off->m_startup.find("startup.core_validation");
        PPR_TEST_ASSERT(off_origin != nullptr);
        PPR_TEST_ASSERT(off_origin->m_source == ESource::cli);

        Diagnostics gpu_diagnostics{};
        auto gpu = resolveCli({"--gpu-validation=false"}, gpu_diagnostics);
        PPR_TEST_ASSERT(gpu.has_value());
        PPR_TEST_ASSERT(not gpu->m_startup.m_value.m_gpu_validation);

        // Defaults carry no explicit origin, so the executable boundary
        // leaves the RHI build defaults untouched unless flagged.
        Diagnostics default_diagnostics{};
        auto defaults = resolveCli({}, default_diagnostics);
        PPR_TEST_ASSERT(defaults.has_value());
        PPR_TEST_ASSERT(not defaults->m_startup.m_value.m_core_validation);
        PPR_TEST_ASSERT(not defaults->m_startup.m_value.m_gpu_validation);
        const PropertyOrigin *default_origin = defaults->m_startup.find("startup.core_validation");
        PPR_TEST_ASSERT(default_origin != nullptr);
        PPR_TEST_ASSERT(default_origin->m_source == ESource::none);
    };

    PPR_UNIT_TEST (cli_overrides_lower_layers) {
        Layer file_layer{};
        file_layer.m_source = ESource::file;
        file_layer.m_origin = "settings.toml";
        file_layer.m_assignments.push_back(
            Assignment{"game.exit_after", "", SourceValue::floating(3.0), "settings.toml:1", 0u});
        file_layer.m_assignments.push_back(
            Assignment{"display.throttle_unfocused", "", SourceValue::boolean(true), "settings.toml:2", 1u});

        const CliSchema schema = gameCliSchema();
        Diagnostics parse_diagnostics{};
        auto parsed = parseCliArgs(spell({"--exit-after", "9"}), schema, parse_diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());

        Diagnostics resolve_diagnostics{};
        const Layer *layers = std::addressof(file_layer);
        auto resolved = resolveGameCli(*parsed, std::span<const Layer>{layers, 1}, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_game.m_value.m_exit_after == 9.0);
        PPR_TEST_ASSERT(resolved->m_display.m_value.m_throttle_unfocused);

        // File-only values apply when the CLI is silent.
        Diagnostics file_parse_diagnostics{};
        auto empty = parseCliArgs(spell({}), schema, file_parse_diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        Diagnostics file_resolve_diagnostics{};
        auto file_only = resolveGameCli(*empty, std::span<const Layer>{layers, 1}, file_resolve_diagnostics);
        PPR_TEST_ASSERT(file_only.has_value());
        PPR_TEST_ASSERT(file_only->m_game.m_value.m_exit_after == 3.0);
    };

    PPR_UNIT_TEST (display_canonical_options_resolve) {
        Diagnostics diagnostics{};
        const auto resolved = resolveCli(
            {"--display.pacing=paced", "--display.cap_hz=30", "--display.paced_vsync=false",
                "--display.throttle_unfocused=false", "--display.window_mode=fullscreen", "--display.monitor=auto",
                "--display.width=1920", "--display.height=1080", "--display.refresh_hz=60"},
            diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        const DisplaySettings &display = resolved->m_display.m_value;
        PPR_TEST_ASSERT(display.m_pacing == EFramePacing::paced);
        PPR_TEST_ASSERT(display.m_cap_hz == 30u);
        PPR_TEST_ASSERT(not display.m_paced_vsync);
        PPR_TEST_ASSERT(not display.m_throttle_unfocused);
        config::visit(display, [&](const auto &member, const auto &) {
            const PropertyOrigin *origin = resolved->m_display.find(member.m_metadata.m_name);
            PPR_TEST_ASSERT(origin != nullptr);
            PPR_TEST_ASSERT(origin->m_source == ESource::cli);
        });

        Diagnostics uncapped_diagnostics{};
        const auto uncapped = resolveCli(
            {"--display.pacing", "uncapped", "--display.cap_hz", "1", "--display.paced_vsync",
                "--display.throttle_unfocused"},
            uncapped_diagnostics);
        PPR_TEST_ASSERT(uncapped.has_value());
        PPR_TEST_ASSERT(uncapped->m_display.m_value.m_pacing == EFramePacing::uncapped);
        PPR_TEST_ASSERT(uncapped->m_display.m_value.m_cap_hz == 1u);
        PPR_TEST_ASSERT(uncapped->m_display.m_value.m_paced_vsync);
        PPR_TEST_ASSERT(uncapped->m_display.m_value.m_throttle_unfocused);

        Diagnostics follow_diagnostics{};
        const auto follow = resolveCli(
            {"--display.pacing=follow-vsync", "--display.cap_hz=1000", "--display.paced_vsync=true"},
            follow_diagnostics);
        PPR_TEST_ASSERT(follow.has_value());
        PPR_TEST_ASSERT(follow->m_display.m_value.m_pacing == EFramePacing::follow_vsync);
        PPR_TEST_ASSERT(follow->m_display.m_value.m_cap_hz == 1000u);
        PPR_TEST_ASSERT(follow->m_display.m_value.m_paced_vsync);
        PPR_TEST_ASSERT(not follow->m_profiling);
    };

    PPR_UNIT_TEST (display_cli_values_use_schema_validation) {
        struct InvalidCase {
            const char *m_option;
            std::string_view m_property;
            EConfigErrc m_error;
        };
        const InvalidCase cases[] = {
            {"--display.pacing=follow_vsync", "display.pacing", EConfigErrc::unknown_enum_value},
            {"--display.pacing=1", "display.pacing", EConfigErrc::unknown_enum_value},
            {"--display.cap_hz=0", "display.cap_hz", EConfigErrc::out_of_range},
            {"--display.cap_hz=1001", "display.cap_hz", EConfigErrc::out_of_range},
            {"--display.cap_hz=30hz", "display.cap_hz", EConfigErrc::invalid_integer},
            {"--display.paced_vsync=bogus", "display.paced_vsync", EConfigErrc::invalid_boolean},
            {"--display.throttle_unfocused=bogus", "display.throttle_unfocused", EConfigErrc::invalid_boolean},
        };
        for (const auto &entry: cases) {
            Diagnostics diagnostics{};
            const auto resolved = resolveCli({entry.m_option}, diagnostics);
            PPR_TEST_ASSERT(not resolved.has_value());
            PPR_TEST_ASSERT(resolved.error() == make_error_code(entry.m_error));
            PPR_TEST_ASSERT(diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == entry.m_property);
            PPR_TEST_ASSERT(not diagnostics.entries().front().m_location.empty());
        }

        for (const char *option: {"--display.pacing", "--display.cap_hz"}) {
            Diagnostics diagnostics{};
            const auto resolved = resolveCli({option}, diagnostics);
            PPR_TEST_ASSERT(not resolved.has_value());
            PPR_TEST_ASSERT(resolved.error() == make_error_code(EConfigErrc::missing_value));
        }
    };

    PPR_UNIT_TEST (display_cli_precedence_and_shadowed_validation) {
        Layer file{};
        file.m_source = ESource::file;
        file.m_origin = "settings.toml";
        file.m_assignments = {
            Assignment{"display.pacing", "", SourceValue::text("uncapped"), "settings.toml:1", 0u},
            Assignment{"display.cap_hz", "", SourceValue::integer(120), "settings.toml:2", 1u},
            Assignment{"display.paced_vsync", "", SourceValue::boolean(true), "settings.toml:3", 2u},
            Assignment{"display.throttle_unfocused", "", SourceValue::boolean(false), "settings.toml:4", 3u},
        };
        Layer environment = file;
        environment.m_source = ESource::environment;
        environment.m_origin = "environment";
        environment.m_assignments[1].m_value = SourceValue::text("90");
        const Layer layers[] = {file, environment};
        const CliSchema schema = gameCliSchema();
        Diagnostics diagnostics{};
        const auto parsed = parseCliArgs(
            spell(
                {"--exit-after=5", "--display.pacing=follow-vsync", "--display.pacing=paced", "--display.cap_hz=60",
                    "--display.cap_hz=30", "--display.paced_vsync=false", "--no-focus-throttle",
                    "--display.throttle_unfocused=true"}),
            schema, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        const auto resolved = resolveGameCli(*parsed, layers, diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_profiling);
        PPR_TEST_ASSERT(resolved->m_display.m_value.m_pacing == EFramePacing::paced);
        PPR_TEST_ASSERT(resolved->m_display.m_value.m_cap_hz == 30u);
        PPR_TEST_ASSERT(not resolved->m_display.m_value.m_paced_vsync);
        PPR_TEST_ASSERT(resolved->m_display.m_value.m_throttle_unfocused);

        const auto empty = parseCliArgs(spell({}), schema, diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        const auto lower_only = resolveGameCli(*empty, layers, diagnostics);
        PPR_TEST_ASSERT(lower_only.has_value());
        PPR_TEST_ASSERT(lower_only->m_display.m_value.m_cap_hz == 90u);
        PPR_TEST_ASSERT(lower_only->m_display.find("display.cap_hz")->m_source == ESource::environment);

        file.m_assignments[1].m_value = SourceValue::integer(0);
        Diagnostics invalid_diagnostics{};
        const auto invalid = resolveGameCli(
            *parsed, std::span<const Layer>{std::addressof(file), 1}, invalid_diagnostics);
        PPR_TEST_ASSERT(not invalid.has_value());
        PPR_TEST_ASSERT(invalid.error() == make_error_code(EConfigErrc::out_of_range));
        PPR_TEST_ASSERT(invalid_diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (display_options_appear_in_generated_game_help) {
        const std::string help = gameHelpText();
        for (const std::string_view option:
            {"--display.pacing", "--display.cap_hz", "--display.paced_vsync", "--display.throttle_unfocused",
                "--display.window_mode", "--display.monitor", "--display.width", "--display.height",
                "--display.refresh_hz", "--focus-throttle", "--no-focus-throttle"}) {
            PPR_TEST_ASSERT(help.find(option) != std::string::npos);
        }
        config::visit(DisplaySettings{}, [&](const auto &member, const auto &) {
            PPR_TEST_ASSERT(help.find(member.m_metadata.m_description) != std::string::npos);
        });
    };

    PPR_UNIT_TEST (help_suppresses_without_reading_config) {
        Diagnostics help_diagnostics{};
        auto help = invoke({"ppr", "--config", "/definitely/not/here.toml", "--help"}, help_diagnostics);
        PPR_TEST_ASSERT(help.has_value());
        PPR_TEST_ASSERT(help->m_help_requested);

        Diagnostics trailing_diagnostics{};
        auto trailing = invoke({"ppr", "--smoke", "--help"}, trailing_diagnostics);
        PPR_TEST_ASSERT(trailing.has_value());
        PPR_TEST_ASSERT(trailing->m_help_requested);

        Diagnostics missing_diagnostics{};
        const auto missing = invoke({"ppr", "--config", "/definitely/not/here.toml"}, missing_diagnostics);
        PPR_TEST_ASSERT(not missing.has_value());
        PPR_TEST_ASSERT(missing.error() == make_error_code(EConfigErrc::codec_error));
    };

    PPR_UNIT_TEST (mapped_config_files_preserve_owned_results_empty_input_and_errors) {
        static std::atomic<u32> ordinal{};
        const auto parent = hal::process::currentExecutablePath().parent_path();
        std::filesystem::path directory;
        for (;;) {
            directory = parent / ("mapped-game-config-test-" + std::to_string(ordinal.fetch_add(1u)));
            std::error_code error;
            const bool created = std::filesystem::create_directory(directory, error);
            PPR_TEST_ASSERT(not error);
            if (created) {
                break;
            }
        }

        PPR_DEFER {
            // Best-effort test cleanup also runs after a failed assertion.
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        };
        const auto path = directory / "settings.toml";
        const std::string argument = path.string();

        const auto write = [&](const std::string_view bytes) {
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            if (not bytes.empty()) {
                file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }

            PPR_TEST_ASSERT(file.good());
        };

        write("[game]\nexit_after = 2.25\n");
        Diagnostics diagnostics;
        const auto result = invoke({"ppr", "--config", argument.c_str()}, diagnostics);
        PPR_TEST_ASSERT(result.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(result->m_game.m_value.m_exit_after == 2.25);
        PPR_TEST_ASSERT(std::filesystem::remove(path));

        const auto *const origin = result->m_game.find("game.exit_after");
        PPR_TEST_ASSERT(origin != nullptr and origin->m_location == argument + ":2");
        write({});
        const auto empty = invoke({"ppr", "--config", argument.c_str()}, diagnostics);
        PPR_TEST_ASSERT(empty.has_value() and not diagnostics.hasErrors());

        write("[game]\nexit_after = nope\n");
        const auto malformed = invoke({"ppr", "--config", argument.c_str()}, diagnostics);
        PPR_TEST_ASSERT(not malformed and malformed.error() == make_error_code(EConfigErrc::codec_error));
        PPR_TEST_ASSERT(diagnostics.format().find(argument + ":2") != std::string::npos);

        PPR_TEST_ASSERT(std::filesystem::remove(path));
        diagnostics.clear();
        const auto missing = invoke({"ppr", "--config", argument.c_str()}, diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == make_error_code(EConfigErrc::codec_error));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "game.config");
        PPR_TEST_ASSERT(diagnostics.entries().front().m_alias == "config");
    };

    PPR_UNIT_TEST (executable_boundary_rejects_unknown_options) {
        Diagnostics unknown_diagnostics{};
        const auto unknown = invoke({"ppr", "--bogus"}, unknown_diagnostics);
        PPR_TEST_ASSERT(not unknown.has_value());
        PPR_TEST_ASSERT(unknown.error() == make_error_code(EConfigErrc::unknown_option));
    };

    PPR_UNIT_TEST (simulation_tools_resolve_bounded_physical_settings) {
        Diagnostics defaults;
        const auto normal = resolveCli({}, defaults);
        PPR_TEST_ASSERT(normal and normal->m_game.m_value.m_tool_acceleration_m_s2 == 100.0);

        Diagnostics diagnostics;
        const auto configured = resolveCli(
            {"--tool-radius=3.5", "--tool-strength=0", "--spawn-count=4096", "--spawn-material=addon:sand"},
            diagnostics);
        PPR_TEST_ASSERT(configured.has_value());
        const auto &settings = configured->m_game.m_value;
        PPR_TEST_ASSERT(settings.m_tool_radius_m == 3.5 and settings.m_tool_acceleration_m_s2 == 0.0);
        PPR_TEST_ASSERT(settings.m_tool_spawn_count == 4096u and settings.m_tool_spawn_material == "addon:sand");

        for (const auto *const invalid:
            {"--tool-radius=0", "--tool-radius=33", "--tool-strength=-1", "--tool-strength=1001", "--tool-strength=nan",
                "--spawn-count=0", "--spawn-count=4097", "--spawn-material="}) {
            Diagnostics rejected;
            PPR_TEST_ASSERT(not resolveCli({invalid}, rejected));
            PPR_TEST_ASSERT(rejected.hasErrors());
        }
    };

    PPR_UNIT_TEST (sky_and_post_settings_parse_and_validate) {
        Diagnostics defaults_diagnostics;
        const auto normal = resolveCli({}, defaults_diagnostics);
        PPR_TEST_ASSERT(normal.has_value());
        PPR_TEST_ASSERT(normal->m_world_light.m_value.m_sky_hour == 8);
        PPR_TEST_ASSERT(normal->m_world_light.m_value.m_diagnostic == ELightingDiagnostic::off);
        PPR_TEST_ASSERT(normal->m_world_light.m_value.m_cascade == 1u);
        PPR_TEST_ASSERT(normal->m_pixel.m_value.m_pixel_scale == 3u);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_auto_exposure);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_exposure_ev == 0.0f);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_adaptation_tau == 0.7f);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_ev_min == -4.0f and normal->m_post.m_value.m_ev_max == 4.0f);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_bloom);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_bloom_intensity == 0.125f);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_bloom_threshold == 1.0f);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_sharpen);
        PPR_TEST_ASSERT(normal->m_post.m_value.m_sharpen_strength == 0.5f);

        Diagnostics configured_diagnostics;
        const auto configured = resolveCli(
            {"--world-light-sky-hour=12", "--world-light-diagnostic=cascade", "--world-light-cascade=5",
                "--pixel-scale=2", "--post-auto-exposure", "--post-exposure-ev=2.5", "--post-bloom-intensity=1.25",
                "--post-sharpen-strength=0.25"},
            configured_diagnostics);
        PPR_TEST_ASSERT(configured.has_value());
        PPR_TEST_ASSERT(configured->m_world_light.m_value.m_sky_hour == 12);
        PPR_TEST_ASSERT(configured->m_world_light.m_value.m_diagnostic == ELightingDiagnostic::cascade);
        PPR_TEST_ASSERT(configured->m_world_light.m_value.m_cascade == 5u);
        PPR_TEST_ASSERT(configured->m_pixel.m_value.m_pixel_scale == 2u);
        PPR_TEST_ASSERT(configured->m_post.m_value.m_auto_exposure);
        PPR_TEST_ASSERT(configured->m_post.m_value.m_exposure_ev == 2.5f);
        PPR_TEST_ASSERT(configured->m_post.m_value.m_bloom_intensity == 1.25f);
        PPR_TEST_ASSERT(configured->m_post.m_value.m_sharpen);
        PPR_TEST_ASSERT(configured->m_post.m_value.m_sharpen_strength == 0.25f);

        for (const auto *const invalid:
            {"--world-light-sky-hour=24", "--world-light-sky-hour=-2", "--post-exposure-ev=-16.1",
                "--post-adaptation-tau=0.09", "--post-ev-min=-8.1", "--post-ev-max=8.1", "--post-bloom-intensity=2.1",
                "--post-bloom-threshold=16.1", "--post-exposure-ev=nan", "--post-adaptation-tau=inf",
                "--post-bloom-threshold=nan", "--post-sharpen-strength=-0.01", "--post-sharpen-strength=1.01",
                "--post-sharpen-strength=nan", "--post-sharpen-strength=inf", "--world-light-diagnostic=debug",
                "--world-light-diagnostic=none", "--world-light-cascade=0", "--world-light-cascade=6",
                "--pixel-scale=0", "--pixel-scale=9"}) {
            Diagnostics rejected;
            PPR_TEST_ASSERT(not resolveCli({invalid}, rejected));
            PPR_TEST_ASSERT(rejected.hasErrors());
        }

        // The cascade selection is an inspection atlas level, never an active
        // cascade-count control: the compiled schedule still owns all five.
        PPR_TEST_ASSERT(WorldLightingPass::kCascadeLevels == 5u);
        PPR_TEST_ASSERT(configured->m_world_light.m_value.m_cascade <= WorldLightingPass::kCascadeLevels);

        Diagnostics cross_diagnostics;
        PPR_TEST_ASSERT(not resolveCli({"--post-ev-min=2", "--post-ev-max=-2"}, cross_diagnostics));
        PPR_TEST_ASSERT(cross_diagnostics.hasErrors());

        // The retained sky hour round-trips through the runtime draft layer.
        config::Resolved<WorldLightDisplaySettings> committed_world{};
        WorldLightDisplaySettings sky_draft{};
        sky_draft.m_sky_hour = 12;
        Diagnostics sky_diagnostics;
        const auto sky_round_trip = validateWorldLightDraft(committed_world, sky_draft, sky_diagnostics);
        PPR_TEST_ASSERT(sky_round_trip.has_value());
        PPR_TEST_ASSERT(sky_round_trip->m_value.m_sky_hour == 12);

        // The retained inspection domain round-trips, and unknown diagnostics or
        // out-of-range cascade levels are configuration errors, never clamped.
        WorldLightDisplaySettings inspection_draft{};
        inspection_draft.m_diagnostic = ELightingDiagnostic::cascade;
        inspection_draft.m_cascade = 5u;
        Diagnostics inspection_diagnostics;
        const auto inspection_round_trip = validateWorldLightDraft(
            committed_world, inspection_draft, inspection_diagnostics);
        PPR_TEST_ASSERT(inspection_round_trip.has_value());
        PPR_TEST_ASSERT(inspection_round_trip->m_value.m_diagnostic == ELightingDiagnostic::cascade);
        PPR_TEST_ASSERT(inspection_round_trip->m_value.m_cascade == 5u);

        WorldLightDisplaySettings unknown_diagnostic = inspection_draft;
        unknown_diagnostic.m_diagnostic = static_cast<ELightingDiagnostic>(200);
        Diagnostics unknown_diagnostics;
        PPR_TEST_ASSERT(not validateWorldLightDraft(committed_world, unknown_diagnostic, unknown_diagnostics));
        PPR_TEST_ASSERT(unknown_diagnostics.hasErrors());

        for (const u32 cascade: {0u, 6u}) {
            WorldLightDisplaySettings invalid_cascade = inspection_draft;
            invalid_cascade.m_cascade = cascade;
            Diagnostics cascade_diagnostics;
            PPR_TEST_ASSERT(not validateWorldLightDraft(committed_world, invalid_cascade, cascade_diagnostics));
            PPR_TEST_ASSERT(cascade_diagnostics.hasErrors());
        }

        // Pixelization P0/P9 are the same configuration rejection at the draft
        // boundary; P1..P8 resolve and P1 still means full-size Hable resolve.
        config::Resolved<PixelDisplaySettings> committed_pixel{};
        for (const u32 scale: {0u, 9u}) {
            PixelDisplaySettings invalid_pixel{scale};
            Diagnostics pixel_diagnostics;
            PPR_TEST_ASSERT(not validatePixelDraft(committed_pixel, invalid_pixel, pixel_diagnostics));
            PPR_TEST_ASSERT(pixel_diagnostics.hasErrors());
        }

        for (const u32 scale: {1u, 3u, 8u}) {
            PixelDisplaySettings valid_pixel{scale};
            Diagnostics pixel_diagnostics;
            const auto resolved_pixel = validatePixelDraft(committed_pixel, valid_pixel, pixel_diagnostics);
            PPR_TEST_ASSERT(resolved_pixel.has_value());
            PPR_TEST_ASSERT(resolved_pixel->m_value.m_pixel_scale == scale);
            PPR_TEST_ASSERT(not pixel_diagnostics.hasErrors());
        }

        const std::string help = gameHelpText();
        PPR_TEST_ASSERT(help.find("world-light-sky-hour") != std::string::npos);
        PPR_TEST_ASSERT(help.find("world-light-diagnostic") != std::string::npos);
        PPR_TEST_ASSERT(help.find("world-light-cascade") != std::string::npos);
        PPR_TEST_ASSERT(help.find("pixel-scale") != std::string::npos);
        PPR_TEST_ASSERT(help.find("post-palette") == std::string::npos);
        PPR_TEST_ASSERT(help.find("world-light-samples-per-frame") == std::string::npos);
        PPR_TEST_ASSERT(help.find("world-light-debug") == std::string::npos);
        PPR_TEST_ASSERT(help.find("world-light-atlas-ambient") == std::string::npos);
        PPR_TEST_ASSERT(help.find("world-lighting") == std::string::npos);
    };

    PPR_UNIT_TEST (retired_world_light_settings_are_rejected) {
        // The retained settings keep their approved defaults.
        const WorldLightDisplaySettings defaults{};
        PPR_TEST_ASSERT(defaults.m_sky_hour == 8);
        PPR_TEST_ASSERT(defaults.m_diagnostic == ELightingDiagnostic::off);
        PPR_TEST_ASSERT(defaults.m_cascade == 1u);
        const PixelDisplaySettings pixel_defaults{};
        PPR_TEST_ASSERT(pixel_defaults.m_pixel_scale == 3u);

        // The retired world-light controls have no active owner: an explicit
        // assignment through any source must fail resolution with diagnostics
        // instead of silently resolving to a dead value.
        for (const char *const obsolete:
            {"--world-light-enabled", "--world-light-samples-per-frame=1048576", "--world-light-levels=6",
                "--world-light-atlas-ambient=0.3", "--world-light-debug=probes", "--world-lighting=field",
                "--post-palette"}) {
            Diagnostics diagnostics;
            PPR_TEST_ASSERT(not resolveCli({obsolete}, diagnostics));
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }
    };

    PPR_UNIT_TEST (invalid_post_draft_is_rejected_and_not_persisted) {
        PostDisplaySettings invalid{};
        invalid.m_ev_min = 4.0f;
        invalid.m_ev_max = -4.0f;

        // The draft validator rejects the cross-field violation with diagnostics.
        config::Resolved<PostDisplaySettings> committed{};
        Diagnostics diagnostics{};
        const auto rejected = validatePostDraft(committed, invalid, diagnostics);
        PPR_TEST_ASSERT(not rejected.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());

        // The persist boundary refuses the invalid draft, so it never reaches the
        // overlay and never fails the next launch's resolution.
        const auto persisted = buildLightingPostOverlayDocument(
            config::Document{}, WorldLightDisplaySettings{}, PixelDisplaySettings{}, invalid);
        PPR_TEST_ASSERT(not persisted.has_value());
    };

    PPR_UNIT_TEST (sharpen_defaults_round_trip_and_mapping) {
        // Factory defaults: the sharpen is on at the panel's mid strength.
        const PostDisplaySettings defaults{};
        PPR_TEST_ASSERT(defaults.m_sharpen);
        PPR_TEST_ASSERT(defaults.m_sharpen_strength == 0.5f);

        // A persisted `false` survives the overlay write and a fresh resolution.
        PostDisplaySettings persisted{};
        persisted.m_sharpen = false;
        persisted.m_sharpen_strength = 0.75f;
        const auto overlay = buildLightingPostOverlayDocument(
            config::Document{}, WorldLightDisplaySettings{}, PixelDisplaySettings{}, persisted);
        PPR_TEST_ASSERT(overlay.has_value());
        const Layer persisted_layer = toLayer(*overlay, "lighting-post.toml");
        const Layer *const persisted_layers = std::addressof(persisted_layer);
        Diagnostics persisted_diagnostics{};
        const auto persisted_resolved = resolveGameCli(
            CliResult{}, std::span<const Layer>{persisted_layers, 1u}, persisted_diagnostics);
        PPR_TEST_ASSERT(persisted_resolved.has_value() and not persisted_diagnostics.hasErrors());
        PPR_TEST_ASSERT(not persisted_resolved->m_post.m_value.m_sharpen);
        PPR_TEST_ASSERT(persisted_resolved->m_post.m_value.m_sharpen_strength == 0.75f);

        // The default picture keeps the sharpen enabled across the same trip.
        const auto default_overlay = buildLightingPostOverlayDocument(
            config::Document{}, WorldLightDisplaySettings{}, PixelDisplaySettings{}, defaults);
        PPR_TEST_ASSERT(default_overlay.has_value());
        const Layer default_layer = toLayer(*default_overlay, "lighting-post.toml");
        const Layer *const default_layers = std::addressof(default_layer);
        Diagnostics default_diagnostics{};
        const auto default_resolved = resolveGameCli(
            CliResult{}, std::span<const Layer>{default_layers, 1u}, default_diagnostics);
        PPR_TEST_ASSERT(default_resolved.has_value() and not default_diagnostics.hasErrors());
        PPR_TEST_ASSERT(default_resolved->m_post.m_value.m_sharpen);
        PPR_TEST_ASSERT(default_resolved->m_post.m_value.m_sharpen_strength == 0.5f);

        // The mapping boundary clamps an out-of-domain draft and sanitises a
        // non-finite one instead of failing the frame on the pass precondition.
        PostProcessSettings mapped{};
        PostDisplaySettings draft{};
        draft.m_sharpen = true;
        draft.m_sharpen_strength = 4.0f;
        applyPostProcessSettings(mapped, draft);
        PPR_TEST_ASSERT(mapped.m_sharpen_enabled);
        PPR_TEST_ASSERT(mapped.m_sharpen_strength == 1.0f);

        draft.m_sharpen_strength = -4.0f;
        applyPostProcessSettings(mapped, draft);
        PPR_TEST_ASSERT(mapped.m_sharpen_strength == 0.0f);

        draft.m_sharpen_strength = std::numeric_limits<float>::quiet_NaN();
        applyPostProcessSettings(mapped, draft);
        PPR_TEST_ASSERT(mapped.m_sharpen_strength == PostProcessSettings{}.m_sharpen_strength);
        PPR_TEST_ASSERT(not validatePostProcessSettings(mapped));

        draft.m_sharpen = false;
        applyPostProcessSettings(mapped, draft);
        PPR_TEST_ASSERT(not mapped.m_sharpen_enabled);
    };

    PPR_UNIT_TEST (retired_crash_harness_rejected) {
        Diagnostics diagnostics{};
        const auto result = resolveCli({"--diag-crash=abort"}, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::unknown_option));
    };
} // namespace pP::game::tests::detail::compat

namespace pP::game::tests {
    const UnitTest game_config = UnitTest::Named("game_config") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::compat::selectors_pick_commands,
            detail::compat::simulation_tools_resolve_bounded_physical_settings,
            detail::compat::worldgen_validation_invocation,
            detail::compat::map_size_uses_one_numeric_domain_for_cli_and_toml,
            detail::compat::worldgen_pack_set_and_qualified_world_are_explicit_inputs,
            detail::compat::pack_set_roots_are_relative_owned_and_never_discovered,
            detail::compat::worldgen_preview_is_distinct_and_requires_explicit_output,
            detail::compat::display_exercise_requires_output_and_conflicts_with_other_selectors,
            detail::compat::profile_capture_requires_output_and_conflicts_with_other_selectors,
            detail::compat::worldgen_acceptance_is_explicit_and_interactive_only,
            detail::compat::retired_workload_commands_rejected,
            detail::compat::repeated_same_selector_is_idempotent,
            detail::compat::selector_explicit_values_rejected,
            detail::compat::legacy_duration_forms,
            detail::compat::zero_duration_rejected_even_when_shadowed,
            detail::compat::retired_profiling_options_rejected,
            detail::compat::profiling_defaults_and_explicit_override,
            detail::compat::startup_booleans_resolve_both_ways,
            detail::compat::cli_overrides_lower_layers,
            detail::compat::display_canonical_options_resolve,
            detail::compat::display_cli_values_use_schema_validation,
            detail::compat::display_cli_precedence_and_shadowed_validation,
            detail::compat::display_options_appear_in_generated_game_help,
            detail::compat::help_suppresses_without_reading_config,
            detail::compat::mapped_config_files_preserve_owned_results_empty_input_and_errors,
            detail::compat::executable_boundary_rejects_unknown_options,
            detail::compat::sky_and_post_settings_parse_and_validate,
            detail::compat::retired_world_light_settings_are_rejected,
            detail::compat::invalid_post_draft_is_rejected_and_not_persisted,
            detail::compat::sharpen_defaults_round_trip_and_mapping,
            detail::compat::retired_crash_harness_rejected,
        });
    };

    const UnitTest &gameConfigTests() noexcept {
        return game_config;
    }
} // namespace pP::game::tests
