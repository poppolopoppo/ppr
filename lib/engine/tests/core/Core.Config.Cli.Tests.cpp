module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;

import std;

namespace pP::tests::detail::cfg_cli {
    // ------------------------------------------------------------------
    // config cli
    // ------------------------------------------------------------------

    using namespace pP::config;

    struct CliSettings {
        bool m_verbose{false};
        bool m_shuffle{false};
        u32 m_loops{1};
        std::string m_label{"suite"};
        std::optional<i32> m_seed{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &CliSettings::m_verbose,
                    Metadata{"app.verbose", "Verbose output", "App", EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &CliSettings::m_shuffle,
                    Metadata{"app.shuffle", "Shuffle tests", "App", EMutability::startup, ESource::all, false},
                    boolean_domain),
                field(
                    &CliSettings::m_loops,
                    Metadata{"app.loops", "Loop count", "App", EMutability::startup, ESource::all, false},
                    Range<u32>{0u, 100u}),
                field(
                    &CliSettings::m_label,
                    Metadata{"app.label", "Run label", "App", EMutability::startup, ESource::all, false},
                    StringDomain{64, true}),
                field(
                    &CliSettings::m_seed,
                    Metadata{"app.seed", "Shuffle seed", "App", EMutability::startup, ESource::all, false},
                    Range<i32>{0, 1000000}),
            };
        }
    };

    [[nodiscard]] CliSchema makeSchema() {
        CliSchema schema{};
        schema.m_program = "tests";
        schema.m_description = "Test runner invocation.";

        OptionDecl verbose{};
        verbose.m_canonical = "app.verbose";
        verbose.m_description = "Enable verbose output.";
        verbose.m_aliases.push_back("v");
        verbose.m_is_switch = true;
        verbose.m_negations.push_back("no-verbose");
        schema.m_options.push_back(std::move(verbose));

        OptionDecl shuffle{};
        shuffle.m_canonical = "app.shuffle";
        shuffle.m_description = "Shuffle tests with an optional seed.";
        shuffle.m_value_optional = true;
        shuffle.m_absent_value = "true";
        schema.m_options.push_back(std::move(shuffle));

        OptionDecl loops{};
        loops.m_canonical = "app.loops";
        loops.m_description = "Number of loop iterations.";
        loops.m_aliases.push_back("loop");
        loops.m_aliases.push_back("n");
        loops.m_repeatable = true;
        schema.m_options.push_back(std::move(loops));

        OptionDecl label{};
        label.m_canonical = "app.label";
        label.m_description = "Run label.";
        label.m_aliases.push_back("l");
        schema.m_options.push_back(std::move(label));

        OptionDecl seed{};
        seed.m_canonical = "app.seed";
        seed.m_description = "Explicit shuffle seed.";
        schema.m_options.push_back(std::move(seed));

        CommandDecl run{};
        run.m_name = "run";
        run.m_description = "Run the suite.";
        schema.m_commands.push_back(run);

        CommandDecl list{};
        list.m_name = "list";
        list.m_description = "List tests.";
        schema.m_commands.push_back(list);

        return schema;
    }

    [[nodiscard]] Array<std::string_view> spell(const std::initializer_list<const char *> words) {
        Array<std::string_view> args{};
        for (const char *word: words) {
            args.emplace_back(word);
        }
        return args;
    }

    PPR_UNIT_TEST (long_value_forms) {
        const CliSchema schema = makeSchema();

        Diagnostics space{};
        const auto spaced = parseCliArgs(spell({"--loops", "5"}), schema, space);
        PPR_TEST_ASSERT(spaced.has_value());
        PPR_TEST_ASSERT(spaced->m_assignments.size() == 1u);
        PPR_TEST_ASSERT(spaced->m_assignments.front().m_name == "app.loops");
        PPR_TEST_ASSERT(spaced->m_assignments.front().m_value.m_text == "5");

        Diagnostics equals{};
        const auto equal = parseCliArgs(spell({"--loops=6"}), schema, equals);
        PPR_TEST_ASSERT(equal.has_value());
        PPR_TEST_ASSERT(equal->m_assignments.front().m_value.m_text == "6");

        Diagnostics alias{};
        const auto aliased = parseCliArgs(spell({"--loop", "7"}), schema, alias);
        PPR_TEST_ASSERT(aliased.has_value());
        PPR_TEST_ASSERT(aliased->m_assignments.front().m_name == "app.loops");
        PPR_TEST_ASSERT(aliased->m_assignments.front().m_alias_used == "loop");
    };

    PPR_UNIT_TEST (presence_switch_and_explicit_forms) {
        const CliSchema schema = makeSchema();

        Diagnostics bare_diagnostics{};
        const auto bare = parseCliArgs(spell({"--verbose"}), schema, bare_diagnostics);
        PPR_TEST_ASSERT(bare.has_value());
        PPR_TEST_ASSERT(bare->m_assignments.front().m_value.m_kind == ESourceKind::boolean);
        PPR_TEST_ASSERT(bare->m_assignments.front().m_value.m_boolean);

        Diagnostics explicit_diagnostics{};
        const auto explicit_off = parseCliArgs(spell({"--verbose=off"}), schema, explicit_diagnostics);
        PPR_TEST_ASSERT(explicit_off.has_value());
        PPR_TEST_ASSERT(explicit_off->m_assignments.front().m_value.m_kind == ESourceKind::text);

        // End to end: explicit boolean text resolves through the token set.
        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<CliSettings>(
            std::array<Layer, 1>{toLayer(std::move(*explicit_off))}, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(not resolved->m_value.m_verbose);
    };

    PPR_UNIT_TEST (negated_alias) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto negated = parseCliArgs(spell({"--no-verbose"}), schema, diagnostics);
        PPR_TEST_ASSERT(negated.has_value());
        PPR_TEST_ASSERT(negated->m_assignments.front().m_name == "app.verbose");
        PPR_TEST_ASSERT(negated->m_assignments.front().m_alias_used == "no-verbose");
        PPR_TEST_ASSERT(negated->m_assignments.front().m_value.m_kind == ESourceKind::boolean);
        PPR_TEST_ASSERT(not negated->m_assignments.front().m_value.m_boolean);
    };

    PPR_UNIT_TEST (short_alias) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto short_form = parseCliArgs(spell({"-v"}), schema, diagnostics);
        PPR_TEST_ASSERT(short_form.has_value());
        PPR_TEST_ASSERT(short_form->m_assignments.front().m_name == "app.verbose");
    };

    PPR_UNIT_TEST (missing_required_value) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto result = parseCliArgs(spell({"--loops"}), schema, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (required_values_reject_options_and_terminator) {
        const CliSchema schema = makeSchema();

        // An option-like next token is never consumed as the value.
        Diagnostics option_like{};
        const auto stole_option = parseCliArgs(spell({"--label", "--help"}), schema, option_like);
        PPR_TEST_ASSERT(not stole_option.has_value());
        PPR_TEST_ASSERT(stole_option.error() == make_error_code(EConfigErrc::missing_value));

        // The "--" terminator is never consumed as the value either.
        Diagnostics terminator{};
        const auto stole_terminator = parseCliArgs(spell({"--label", "--"}), schema, terminator);
        PPR_TEST_ASSERT(not stole_terminator.has_value());
        PPR_TEST_ASSERT(stole_terminator.error() == make_error_code(EConfigErrc::missing_value));

        // Dash-prefixed strings need the inline form.
        Diagnostics inline_dash{};
        const auto dashed = parseCliArgs(spell({"--label=--help"}), schema, inline_dash);
        PPR_TEST_ASSERT(dashed.has_value());
        PPR_TEST_ASSERT(dashed->m_assignments.front().m_value.m_text == "--help");

        // Negative numbers still work as required values.
        Diagnostics negative{};
        const auto depth = parseCliArgs(spell({"--loops", "-2"}), schema, negative);
        PPR_TEST_ASSERT(depth.has_value());
        PPR_TEST_ASSERT(depth->m_assignments.front().m_value.m_text == "-2");
    };

    PPR_UNIT_TEST (optional_value_forms) {
        const CliSchema schema = makeSchema();

        Diagnostics absent_diagnostics{};
        const auto absent = parseCliArgs(spell({"--shuffle"}), schema, absent_diagnostics);
        PPR_TEST_ASSERT(absent.has_value());
        PPR_TEST_ASSERT(absent->m_assignments.front().m_value.m_text == "true");

        // A following option is not consumed as the optional value.
        Diagnostics next_is_option{};
        const auto before_option = parseCliArgs(spell({"--shuffle", "--verbose"}), schema, next_is_option);
        PPR_TEST_ASSERT(before_option.has_value());
        PPR_TEST_ASSERT(before_option->m_assignments.size() == 2u);
        PPR_TEST_ASSERT(before_option->m_assignments.front().m_value.m_text == "true");

        Diagnostics with_value{};
        const auto supplied = parseCliArgs(spell({"--shuffle", "off"}), schema, with_value);
        PPR_TEST_ASSERT(supplied.has_value());
        PPR_TEST_ASSERT(supplied->m_assignments.size() == 1u);
        PPR_TEST_ASSERT(supplied->m_assignments.front().m_value.m_text == "off");
    };

    PPR_UNIT_TEST (repeated_options_last_wins) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto repeated = parseCliArgs(spell({"--loops", "1", "--loops", "2"}), schema, diagnostics);
        PPR_TEST_ASSERT(repeated.has_value());
        PPR_TEST_ASSERT(repeated->m_assignments.size() == 2u);

        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<CliSettings>(
            std::array<Layer, 1>{toLayer(std::move(*repeated))}, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_loops == 2u);
    };

    PPR_UNIT_TEST (terminator_and_command_selection) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto run = parseCliArgs(spell({"run"}), schema, diagnostics);
        PPR_TEST_ASSERT(run.has_value());
        PPR_TEST_ASSERT(run->m_command == "run");
        PPR_TEST_ASSERT(run->m_assignments.empty());

        Diagnostics conflict{};
        PPR_TEST_ASSERT(not parseCliArgs(spell({"run", "list"}), schema, conflict).has_value());
        PPR_TEST_ASSERT(conflict.hasErrors());

        // Options after "--" are positionals, not options.
        Diagnostics terminated{};
        PPR_TEST_ASSERT(not parseCliArgs(spell({"--", "--loops"}), schema, terminated).has_value());

        // But a command after "--" still selects.
        Diagnostics command_after{};
        const auto after = parseCliArgs(spell({"--", "run"}), schema, command_after);
        PPR_TEST_ASSERT(after.has_value());
        PPR_TEST_ASSERT(after->m_command == "run");

        // Unknown positionals are errors.
        Diagnostics positional{};
        PPR_TEST_ASSERT(not parseCliArgs(spell({"frobnicate"}), schema, positional).has_value());
    };

    PPR_UNIT_TEST (unknown_option_is_an_error) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        const auto result = parseCliArgs(spell({"--bogus"}), schema, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::unknown_option));
        const std::string text = diagnostics.format();
        PPR_TEST_ASSERT(text.find("--bogus") != std::string::npos);
        PPR_TEST_ASSERT(text.find("argv[0]") != std::string::npos);
    };

    PPR_UNIT_TEST (help_flag_and_text) {
        const CliSchema schema = makeSchema();

        Diagnostics long_diagnostics{};
        const auto long_help = parseCliArgs(spell({"--help"}), schema, long_diagnostics);
        PPR_TEST_ASSERT(long_help.has_value());
        PPR_TEST_ASSERT(long_help->m_help_requested);

        Diagnostics short_diagnostics{};
        const auto short_help = parseCliArgs(spell({"-h"}), schema, short_diagnostics);
        PPR_TEST_ASSERT(short_help.has_value());
        PPR_TEST_ASSERT(short_help->m_help_requested);

        const std::string text = generateHelp(schema);
        PPR_TEST_ASSERT(text.find("tests") != std::string::npos);
        PPR_TEST_ASSERT(text.find("--loops") != std::string::npos);
        PPR_TEST_ASSERT(text.find("--no-verbose") != std::string::npos);
        PPR_TEST_ASSERT(text.find("run") != std::string::npos);

        const CliSettings defaults{};
        const std::string with_defaults = generateHelp(schema, defaults);
        PPR_TEST_ASSERT(with_defaults.find("app.loops = 1") != std::string::npos);
    };

    PPR_UNIT_TEST (cli_schema_validation) {
        const CliSchema schema = makeSchema();
        Diagnostics good{};
        PPR_TEST_ASSERT(validateCliSchema<CliSettings>(schema, good).has_value());

        CliSchema bad = makeSchema();
        OptionDecl ghost{};
        ghost.m_canonical = "app.ghost";
        bad.m_options.push_back(std::move(ghost));
        Diagnostics bad_diagnostics{};
        PPR_TEST_ASSERT(not validateCliSchema<CliSettings>(bad, bad_diagnostics).has_value());

        CliSchema collision = makeSchema();
        OptionDecl clash{};
        clash.m_canonical = "app.label";
        clash.m_aliases.push_back("loop");
        collision.m_options.push_back(std::move(clash));
        Diagnostics collision_diagnostics{};
        PPR_TEST_ASSERT(not validateCliSchema<CliSettings>(collision, collision_diagnostics).has_value());
    };

    PPR_UNIT_TEST (end_to_end_invocation) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        auto parsed = parseCliArgs(
            spell({"run", "--loops", "3", "--no-verbose", "--label=nightly"}), schema, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(parsed->m_command == "run");

        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<CliSettings>(
            std::array<Layer, 1>{toLayer(std::move(*parsed))}, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_loops == 3u);
        PPR_TEST_ASSERT(not resolved->m_value.m_verbose);
        PPR_TEST_ASSERT(resolved->m_value.m_label == "nightly");
        const PropertyOrigin *origin = resolved->find("app.loops");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::cli);
    };

    PPR_UNIT_TEST (explicit_reset_through_empty_value) {
        const CliSchema schema = makeSchema();

        Diagnostics diagnostics{};
        auto parsed = parseCliArgs(spell({"--seed="}), schema, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(parsed->m_assignments.front().m_value.m_kind == ESourceKind::reset);

        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<CliSettings>(
            std::array<Layer, 1>{toLayer(std::move(*parsed))}, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(not resolved->m_value.m_seed.has_value());
    };

    struct HelpSettings {
        std::string m_name{"public-default"};
        std::string m_secret{"hunter2"};
        std::string m_unbound{"hidden-default"};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &HelpSettings::m_name,
                    Metadata{"app.name", "Name", "App", EMutability::startup, ESource::all, false},
                    StringDomain{64, true}),
                field(
                    &HelpSettings::m_secret,
                    Metadata{"app.secret", "Secret", "App", EMutability::startup, ESource::all, false, true},
                    StringDomain{64, true}),
                field(
                    &HelpSettings::m_unbound,
                    Metadata{"app.unbound", "Unbound", "App", EMutability::startup, ESource::all, false},
                    StringDomain{64, true}),
            };
        }
    };

    [[nodiscard]] CliSchema makeHelpSchema() {
        CliSchema schema{};
        schema.m_program = "demo";

        OptionDecl name{};
        name.m_canonical = "app.name";
        name.m_description = "Public name.";
        schema.m_options.push_back(std::move(name));

        OptionDecl secret{};
        secret.m_canonical = "app.secret";
        secret.m_description = "Sensitive secret.";
        schema.m_options.push_back(std::move(secret));

        return schema;
    }

    PPR_UNIT_TEST (help_redacts_sensitive_and_unbound_defaults) {
        const CliSchema schema = makeHelpSchema();
        const HelpSettings defaults{};
        const std::string text = generateHelp(schema, defaults);
        PPR_TEST_ASSERT(text.find("hunter2") == std::string::npos);
        PPR_TEST_ASSERT(text.find("<redacted>") != std::string::npos);
        PPR_TEST_ASSERT(text.find("public-default") != std::string::npos);
        PPR_TEST_ASSERT(text.find("hidden-default") == std::string::npos);
        PPR_TEST_ASSERT(text.find("app.unbound") == std::string::npos);
    }

    PPR_UNIT_TEST (exit_code_constant) {
        PPR_TEST_ASSERT(kExitInvalidUsage == 2);
    };
} // namespace pP::tests::detail::cfg_cli

namespace pP::tests {
    const UnitTest config_cli = UnitTest::Named("config_cli") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::cfg_cli::long_value_forms,
            detail::cfg_cli::presence_switch_and_explicit_forms,
            detail::cfg_cli::negated_alias,
            detail::cfg_cli::short_alias,
            detail::cfg_cli::missing_required_value,
            detail::cfg_cli::required_values_reject_options_and_terminator,
            detail::cfg_cli::optional_value_forms,
            detail::cfg_cli::repeated_options_last_wins,
            detail::cfg_cli::terminator_and_command_selection,
            detail::cfg_cli::unknown_option_is_an_error,
            detail::cfg_cli::help_flag_and_text,
            detail::cfg_cli::cli_schema_validation,
            detail::cfg_cli::end_to_end_invocation,
            detail::cfg_cli::explicit_reset_through_empty_value,
            detail::cfg_cli::help_redacts_sensitive_and_unbound_defaults,
        });
    };

    const UnitTest &configCliTests() noexcept {
        return config_cli;
    }
} // namespace pP::tests
