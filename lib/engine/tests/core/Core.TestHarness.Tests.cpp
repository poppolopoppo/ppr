module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import engine.tests;

import std;

namespace pP::tests::detail::harness {
    // ------------------------------------------------------------------
    // test harness
    // ------------------------------------------------------------------

    using namespace pP::config;

    [[nodiscard]] Expected<ParsedTests> parse(
        const std::initializer_list<const char *> words, Diagnostics &diagnostics) {
        Array<std::string_view> args{};
        for (const char *word: words) {
            args.emplace_back(word);
        }
        return parseTestInvocation(
            std::span<const std::string_view>{args.data(), args.size()}, "core-tests", diagnostics);
    }

    PPR_UNIT_TEST (defaults_match_legacy) {
        Diagnostics diagnostics{};
        auto parsed = parse({}, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(not parsed->m_help_requested);
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_loops == (mem::is_asan_enabled_v ? 3u : 1u));
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_filter.empty());
        PPR_TEST_ASSERT(not parsed->m_settings.m_value.m_child_run);
        PPR_TEST_ASSERT(not parsed->m_settings.m_value.m_shuffle_seed.has_value());
        PPR_TEST_ASSERT(not parsed->m_settings.m_value.m_no_shuffle);
        PPR_TEST_ASSERT(shuffleEnabled(*parsed));
    };

    PPR_UNIT_TEST (legacy_selection_flags) {
        Diagnostics diagnostics{};
        auto parsed = parse({"--run-test", "core/math", "--child-run"}, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_filter == "core/math");
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_child_run);
    };

    PPR_UNIT_TEST (shuffle_forms) {
        Diagnostics bare_diagnostics{};
        auto bare = parse({"--shuffle"}, bare_diagnostics);
        PPR_TEST_ASSERT(bare.has_value());
        PPR_TEST_ASSERT(not bare->m_settings.m_value.m_shuffle_seed.has_value());
        PPR_TEST_ASSERT(shuffleEnabled(*bare));

        Diagnostics seed_diagnostics{};
        auto seed = parse({"--shuffle", "42"}, seed_diagnostics);
        PPR_TEST_ASSERT(seed.has_value());
        PPR_TEST_ASSERT(seed->m_settings.m_value.m_shuffle_seed == 42u);
        PPR_TEST_ASSERT(shuffleEnabled(*seed));

        Diagnostics equals_diagnostics{};
        auto equals = parse({"--shuffle=7"}, equals_diagnostics);
        PPR_TEST_ASSERT(equals.has_value());
        PPR_TEST_ASSERT(equals->m_settings.m_value.m_shuffle_seed == 7u);

        Diagnostics off_diagnostics{};
        auto off = parse({"--no-shuffle"}, off_diagnostics);
        PPR_TEST_ASSERT(off.has_value());
        PPR_TEST_ASSERT(not shuffleEnabled(*off));
    };

    PPR_UNIT_TEST (shuffle_option_order_precedence) {
        Diagnostics first_diagnostics{};
        auto seed_then_off = parse({"--shuffle", "7", "--no-shuffle"}, first_diagnostics);
        PPR_TEST_ASSERT(seed_then_off.has_value());
        PPR_TEST_ASSERT(not shuffleEnabled(*seed_then_off));

        Diagnostics second_diagnostics{};
        auto off_then_seed = parse({"--no-shuffle", "--shuffle", "9"}, second_diagnostics);
        PPR_TEST_ASSERT(off_then_seed.has_value());
        PPR_TEST_ASSERT(shuffleEnabled(*off_then_seed));
        PPR_TEST_ASSERT(off_then_seed->m_settings.m_value.m_shuffle_seed == 9u);
    };

    PPR_UNIT_TEST (explicit_false_no_shuffle_enables_shuffling) {
        Diagnostics bare_diagnostics{};
        auto bare_false = parse({"--no-shuffle=false"}, bare_diagnostics);
        PPR_TEST_ASSERT(bare_false.has_value());
        PPR_TEST_ASSERT(not bare_false->m_settings.m_value.m_no_shuffle);
        PPR_TEST_ASSERT(shuffleEnabled(*bare_false));

        // Order still wins: a winning --no-shuffle=false re-enables
        // shuffling even after --shuffle, keeping the earlier seed.
        Diagnostics order_diagnostics{};
        auto seed_then_false = parse({"--shuffle", "7", "--no-shuffle=false"}, order_diagnostics);
        PPR_TEST_ASSERT(seed_then_false.has_value());
        PPR_TEST_ASSERT(shuffleEnabled(*seed_then_false));
        PPR_TEST_ASSERT(seed_then_false->m_settings.m_value.m_shuffle_seed == 7u);

        // ... while a trailing bare --no-shuffle still disables.
        Diagnostics trailing_diagnostics{};
        auto false_then_off = parse({"--no-shuffle=false", "--no-shuffle"}, trailing_diagnostics);
        PPR_TEST_ASSERT(false_then_off.has_value());
        PPR_TEST_ASSERT(not shuffleEnabled(*false_then_off));
    };

    PPR_UNIT_TEST (explicit_no_shuffle_file_layer_disables_without_cli_mention) {
        Layer file_layer{};
        file_layer.m_source = ESource::file;
        file_layer.m_origin = "settings.toml";
        file_layer.m_assignments.push_back(
            Assignment{"test.no_shuffle", "", SourceValue::boolean(true), "settings.toml:1", 0u});

        Diagnostics diagnostics{};
        const Layer *layers = std::addressof(file_layer);
        auto parsed = parseTestInvocation(
            std::span<const std::string_view>{}, "core-tests", std::span<const Layer>{layers, 1}, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(not shuffleEnabled(*parsed));
    };

    PPR_UNIT_TEST (loop_forms) {
        Diagnostics space_diagnostics{};
        auto space = parse({"--loop", "2"}, space_diagnostics);
        PPR_TEST_ASSERT(space.has_value());
        PPR_TEST_ASSERT(space->m_settings.m_value.m_loops == 2u);

        Diagnostics suffix_diagnostics{};
        auto suffix = parse({"--loops", "3"}, suffix_diagnostics);
        PPR_TEST_ASSERT(suffix.has_value());
        PPR_TEST_ASSERT(suffix->m_settings.m_value.m_loops == 3u);

        Diagnostics equals_diagnostics{};
        auto equals = parse({"--loop=4"}, equals_diagnostics);
        PPR_TEST_ASSERT(equals.has_value());
        PPR_TEST_ASSERT(equals->m_settings.m_value.m_loops == 4u);
    };

    PPR_UNIT_TEST (loop_zero_stays_successful_noop) {
        Diagnostics diagnostics{};
        auto parsed = parse({"--loop", "0"}, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(not parsed->m_help_requested);
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_loops == 0u);
    };

    PPR_UNIT_TEST (help_suppresses_execution_regardless_of_position) {
        Diagnostics first_diagnostics{};
        auto help_first = parse({"--help", "--loop", "1"}, first_diagnostics);
        PPR_TEST_ASSERT(help_first.has_value());
        PPR_TEST_ASSERT(help_first->m_help_requested);

        Diagnostics second_diagnostics{};
        auto help_last = parse({"--loop", "1", "--help"}, second_diagnostics);
        PPR_TEST_ASSERT(help_last.has_value());
        PPR_TEST_ASSERT(help_last->m_help_requested);

        Diagnostics short_diagnostics{};
        auto short_help = parse({"-h"}, short_diagnostics);
        PPR_TEST_ASSERT(short_help.has_value());
        PPR_TEST_ASSERT(short_help->m_help_requested);
    };

    PPR_UNIT_TEST (malformed_inputs_are_errors) {
        Diagnostics alpha_diagnostics{};
        PPR_TEST_ASSERT(not parse({"--loop", "abc"}, alpha_diagnostics).has_value());

        Diagnostics trailing_diagnostics{};
        PPR_TEST_ASSERT(not parse({"--loop", "12abc"}, trailing_diagnostics).has_value());

        Diagnostics missing_diagnostics{};
        PPR_TEST_ASSERT(not parse({"--loop"}, missing_diagnostics).has_value());

        Diagnostics seed_diagnostics{};
        PPR_TEST_ASSERT(not parse({"--shuffle", "4x"}, seed_diagnostics).has_value());

        Diagnostics filter_diagnostics{};
        PPR_TEST_ASSERT(not parse({"--run-test"}, filter_diagnostics).has_value());

        Diagnostics unknown_diagnostics{};
        const auto unknown = parse({"--bogus"}, unknown_diagnostics);
        PPR_TEST_ASSERT(not unknown.has_value());
        PPR_TEST_ASSERT(unknown.error() == make_error_code(EConfigErrc::unknown_option));
    };

    PPR_UNIT_TEST (explicit_seed_preserved_for_first_iteration) {
        Diagnostics diagnostics{};
        auto parsed = parse({"--shuffle", "1234"}, diagnostics);
        PPR_TEST_ASSERT(parsed.has_value());
        PPR_TEST_ASSERT(parsed->m_settings.m_value.m_shuffle_seed == 1234u);
        const PropertyOrigin *origin = parsed->m_settings.find("test.shuffle_seed");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::cli);
    };
} // namespace pP::tests::detail::harness

namespace pP::tests {
    const UnitTest test_harness = UnitTest::Named("test_harness") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::harness::defaults_match_legacy,
            detail::harness::legacy_selection_flags,
            detail::harness::shuffle_forms,
            detail::harness::shuffle_option_order_precedence,
            detail::harness::explicit_false_no_shuffle_enables_shuffling,
            detail::harness::explicit_no_shuffle_file_layer_disables_without_cli_mention,
            detail::harness::loop_forms,
            detail::harness::loop_zero_stays_successful_noop,
            detail::harness::help_suppresses_execution_regardless_of_position,
            detail::harness::malformed_inputs_are_errors,
            detail::harness::explicit_seed_preserved_for_first_iteration,
        });
    };

    const UnitTest &testHarnessTests() noexcept {
        return test_harness;
    }
} // namespace pP::tests
