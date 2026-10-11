module;

#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import game.config;

import std;

namespace pP::game::tests::detail::compat {
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
        auto preset = resolveCli({"--profile100"}, preset_diagnostics);
        PPR_TEST_ASSERT(preset.has_value());
        PPR_TEST_ASSERT(preset->m_profiling);
        PPR_TEST_ASSERT(not preset->m_display.m_value.m_throttle_unfocused);

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
        auto overridden = resolveCli({"--profile100", "--focus-throttle"}, override_diagnostics);
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
                {"--profile100", "--display.pacing=follow-vsync", "--display.pacing=paced", "--display.cap_hz=60",
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

    PPR_UNIT_TEST (executable_boundary_rejects_unknown_options) {
        Diagnostics unknown_diagnostics{};
        const auto unknown = invoke({"ppr", "--bogus"}, unknown_diagnostics);
        PPR_TEST_ASSERT(not unknown.has_value());
        PPR_TEST_ASSERT(unknown.error() == make_error_code(EConfigErrc::unknown_option));
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
            detail::compat::worldgen_validation_invocation,
            detail::compat::worldgen_preview_is_distinct_and_requires_explicit_output,
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
            detail::compat::executable_boundary_rejects_unknown_options,
            detail::compat::retired_crash_harness_rejected,
        });
    };

    const UnitTest &gameConfigTests() noexcept {
        return game_config;
    }
} // namespace pP::game::tests
