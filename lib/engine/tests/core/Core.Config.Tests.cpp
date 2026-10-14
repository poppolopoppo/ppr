module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;

import std;

namespace pP::tests::detail::cfg {
    // ------------------------------------------------------------------
    // config
    // ------------------------------------------------------------------

    using namespace pP::config;

    enum class EPace : u8 {
        uncapped,
        follow_vsync,
        paced,
    };

    struct PerfSettings {
        EPace m_pacing{EPace::follow_vsync};
        u32 m_cap_hz{60};
        bool m_vsync{true};
        double m_scale{1.0};
        std::string m_label{"default"};
        std::optional<i32> m_seed{};
        std::string m_token{"secret"};
        i32 m_startup_only{7};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &PerfSettings::m_pacing,
                    Metadata{
                        "perf.pacing",
                        "Pacing mode",
                        "Perf",
                        EMutability::runtime,
                        ESource::all,
                        true,
                    },
                    enumDomain<EPace>(
                        {{EPace::uncapped, "uncapped"}, {EPace::follow_vsync, "follow-vsync"},
                            {EPace::paced, "paced"}})),
                field(
                    &PerfSettings::m_cap_hz,
                    Metadata{"perf.cap_hz", "Frame cap", "Perf", EMutability::runtime, ESource::all, true},
                    Range<u32>{1u, 1000u}),
                field(
                    &PerfSettings::m_vsync,
                    Metadata{"perf.vsync", "Vsync", "Perf", EMutability::runtime, ESource::all, true}, boolean_domain),
                field(
                    &PerfSettings::m_scale,
                    Metadata{"perf.scale", "Scale", "Perf", EMutability::runtime, ESource::all, true},
                    FloatDomain{0.0, 10.0, false}),
                field(
                    &PerfSettings::m_label,
                    Metadata{"perf.label", "Label", "Perf", EMutability::runtime, ESource::all, true},
                    StringDomain{32, true}),
                field(
                    &PerfSettings::m_seed,
                    Metadata{"perf.seed", "Seed", "Perf", EMutability::runtime, ESource::all, false},
                    Range<i32>{-1000, 1000}),
                field(
                    &PerfSettings::m_token,
                    Metadata{"perf.token", "Token", "Perf", EMutability::runtime, ESource::all, false, true},
                    StringDomain{64, false}),
                field(
                    &PerfSettings::m_startup_only,
                    Metadata{
                        "perf.startup_only",
                        "File-only startup value",
                        "Perf",
                        EMutability::startup,
                        ESource::file,
                        false,
                    },
                    Range<i32>{0, 100}),
            };
        }

        [[nodiscard]] static std::error_code validate(const PerfSettings &settings, Diagnostics &diagnostics) {
            if (settings.m_pacing == EPace::uncapped and settings.m_cap_hz != 60u) {
                diagnostics.add(
                    "resolve", "perf.cap_hz", "uncapped pacing requires the default cap",
                    make_error_code(EConfigErrc::cross_field_invalid));
                return make_error_code(EConfigErrc::cross_field_invalid);
            }
            return {};
        }
    };

    struct AliasSettings {
        u32 m_workers{4};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &AliasSettings::m_workers,
                    Metadata{"job.workers", "Workers", "Job", EMutability::startup, ESource::all, false},
                    Range<u32>{1u, 64u}),
            };
        }

        [[nodiscard]] static constexpr std::string_view environmentAlias(const std::string_view canonical) {
            if (canonical == "job.workers") {
                return "PPR_WORKERS";
            }
            return {};
        }
    };

    struct WidthSettings {
        u8 m_small{7};
        u16 m_medium{700};
        u32 m_large{70000};
        float m_ratio{1.0f};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &WidthSettings::m_small,
                    Metadata{"w.small", "Small", "W", EMutability::startup, ESource::all, false}, Range<u8>{1u, 200u}),
                field(
                    &WidthSettings::m_medium,
                    Metadata{"w.medium", "Medium", "W", EMutability::startup, ESource::all, false},
                    Range<u16>{1u, 1000u}),
                field(
                    &WidthSettings::m_large,
                    Metadata{"w.large", "Large", "W", EMutability::startup, ESource::all, false},
                    Range<u32>{1u, 100000u}),
                field(
                    &WidthSettings::m_ratio,
                    Metadata{"w.ratio", "Ratio", "W", EMutability::startup, ESource::all, false},
                    FloatDomain{0.0, 10.0, false}),
            };
        }
    };

    [[nodiscard]] Layer singleLayer(
        const ESource source, std::string name, SourceValue value, const std::string &location = "test:1") {
        Layer layer{};
        layer.m_source = source;
        layer.m_origin = "test";
        layer.m_assignments.push_back(Assignment{std::move(name), {}, std::move(value), location, 0});
        return layer;
    }

    PPR_UNIT_TEST (bounded_diagnostics_preserve_failure_count_and_clear_contract) {
        Diagnostics diagnostics{2u};
        for (u32 index = 0u; index < 5u; ++index) {
            diagnostics.add(
                "pack.toml:2", "materials.bad", "Invalid material", make_error_code(EConfigErrc::codec_error));
        }
        PPR_TEST_ASSERT(diagnostics.hasErrors() and diagnostics.size() == 5u);
        PPR_TEST_ASSERT(diagnostics.entries().size() == 2u and diagnostics.omittedCount() == 3u);
        PPR_TEST_ASSERT(diagnostics.format().find("3 further diagnostics omitted") != std::string::npos);
        diagnostics.clear();
        PPR_TEST_ASSERT(not diagnostics.hasErrors() and diagnostics.size() == 0u and diagnostics.omittedCount() == 0u);
        for (u32 index = 0u; index < 3u; ++index) {
            diagnostics.add("", "", "Bad value", make_error_code(EConfigErrc::out_of_range));
        }
        PPR_TEST_ASSERT(diagnostics.entries().size() == 2u and diagnostics.omittedCount() == 1u);

        Diagnostics no_records{0u};
        const auto layer = singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text("invalid"));
        const auto result = resolve<PerfSettings>(std::span{&layer, 1u}, no_records);
        PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::invalid_integer));
        PPR_TEST_ASSERT(no_records.entries().empty() and no_records.hasErrors() and no_records.size() != 0u);
        no_records.clear();
        no_records.addOmitted((std::numeric_limits<std::size_t>::max)());
        no_records.addOmitted(1u);
        PPR_TEST_ASSERT(no_records.hasErrors());
        PPR_TEST_ASSERT(no_records.size() == (std::numeric_limits<std::size_t>::max)());
        PPR_TEST_ASSERT(no_records.omittedCount() == no_records.size());
        PPR_TEST_ASSERT(no_records.hasErrorsSince(no_records.size()));
        PPR_TEST_ASSERT(not validateSchema<PerfSettings>(no_records));
        PPR_TEST_ASSERT(not validateEnvironmentSchema<PerfSettings>(no_records));
        PPR_TEST_ASSERT(not validateCliSchema<PerfSettings>(CliSchema{}, no_records));
        no_records.clear();
        PPR_TEST_ASSERT(validateSchema<PerfSettings>(no_records).has_value());
    };

    PPR_UNIT_TEST (defaults_resolve) {
        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(std::span<const Layer>{}, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 60u);
        PPR_TEST_ASSERT(result->m_value.m_pacing == EPace::follow_vsync);
        PPR_TEST_ASSERT(result->m_value.m_vsync);
        PPR_TEST_ASSERT(not result->m_value.m_seed.has_value());
        const PropertyOrigin *origin = result->find("perf.cap_hz");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::none);
        PPR_TEST_ASSERT(result->m_origins.size() == 8u);
    };

    PPR_UNIT_TEST (precedence_across_layers) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::file, "perf.cap_hz", SourceValue::integer(30), "base.toml:2"));
        layers.push_back(
            singleLayer(ESource::environment, "perf.cap_hz", SourceValue::text("90"), "env:PPR_PERF__CAP_HZ"));
        layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text("144"), "argv[2]"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 144u);
        const PropertyOrigin *origin = result->find("perf.cap_hz");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::cli);
        PPR_TEST_ASSERT(origin->m_location == "argv[2]");
    };

    PPR_UNIT_TEST (source_subsets_are_valid) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.label", SourceValue::text("cli-only"), "argv[1]"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_label == "cli-only");
        // Untouched properties keep struct defaults.
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 60u);
    };

    PPR_UNIT_TEST (last_assignment_wins_within_layer) {
        Layer layer{};
        layer.m_source = ESource::cli;
        layer.m_origin = "cli";
        layer.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::text("30"), "argv[1]", 0});
        layer.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::text("75"), "argv[3]", 1});
        Array<Layer> layers{};
        layers.push_back(std::move(layer));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 75u);
    };

    PPR_UNIT_TEST (unknown_property_is_an_error) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.nope", SourceValue::text("1"), "argv[1]"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::unknown_property));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (integer_text_consumption_and_overflow) {
        for (const std::string_view bad: {"abc", "60x", "0x", "", "99999999999", "-5"}) {
            Array<Layer> layers{};
            layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text(std::string{bad}), "argv[1]"));
            Diagnostics diagnostics{};
            const auto result = resolve<PerfSettings>(layers, diagnostics);
            PPR_TEST_ASSERT(not result.has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }

        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text(" 120 "), "argv[1]"));
        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 120u);
    };

    PPR_UNIT_TEST (file_integer_out_of_typed_range) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::file, "perf.cap_hz", SourceValue::integer(5000), "base.toml:3"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (typed_integer_sources_check_destination_width) {
        // 4294967297 truncates to 1 when cast to u32 without a range check.
        Array<Layer> wrap{};
        wrap.push_back(singleLayer(ESource::file, "w.large", SourceValue::integer(4294967297LL), "big.toml:1"));
        Diagnostics wrap_diagnostics{};
        PPR_TEST_ASSERT(not resolve<WidthSettings>(wrap, wrap_diagnostics).has_value());
        PPR_TEST_ASSERT(wrap_diagnostics.hasErrors());

        for (const i64 bad: {256LL, -1LL}) {
            Array<Layer> layers{};
            layers.push_back(singleLayer(ESource::file, "w.small", SourceValue::integer(bad), "w.toml:1"));
            Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not resolve<WidthSettings>(layers, diagnostics).has_value());
        }

        Array<Layer> medium{};
        medium.push_back(singleLayer(ESource::file, "w.medium", SourceValue::integer(70000), "w.toml:1"));
        Diagnostics medium_diagnostics{};
        PPR_TEST_ASSERT(not resolve<WidthSettings>(medium, medium_diagnostics).has_value());

        // Representable values still apply.
        Array<Layer> good{};
        good.push_back(singleLayer(ESource::file, "w.small", SourceValue::integer(42), "w.toml:1"));
        Diagnostics good_diagnostics{};
        const auto resolved = resolve<WidthSettings>(good, good_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_small == 42u);
    };

    PPR_UNIT_TEST (typed_floating_sources_check_destination_range) {
        // 1e300 is finite but outside float's representable range.
        Array<Layer> wide{};
        wide.push_back(singleLayer(ESource::file, "w.ratio", SourceValue::floating(1e300), "w.toml:1"));
        Diagnostics wide_diagnostics{};
        const auto wide_result = resolve<WidthSettings>(wide, wide_diagnostics);
        PPR_TEST_ASSERT(not wide_result.has_value());
        PPR_TEST_ASSERT(wide_result.error() == make_error_code(EConfigErrc::out_of_range));

        // Programmatic layers take the same typed-source path.
        Array<Layer> programmatic{};
        programmatic.push_back(singleLayer(ESource::none, "w.ratio", SourceValue::floating(1e300), "profile"));
        Diagnostics programmatic_diagnostics{};
        PPR_TEST_ASSERT(not resolve<WidthSettings>(programmatic, programmatic_diagnostics).has_value());

        Array<Layer> good{};
        good.push_back(singleLayer(ESource::file, "w.ratio", SourceValue::floating(2.5), "w.toml:1"));
        Diagnostics good_diagnostics{};
        const auto resolved = resolve<WidthSettings>(good, good_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_ratio == 2.5f);
    };

    PPR_UNIT_TEST (file_string_never_satisfies_integer) {
        // A file string "60" must not satisfy an integer property, even
        // though its contents look numeric (strict file decoding).
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::file, "perf.cap_hz", SourceValue::text("60"), "base.toml:3"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::type_mismatch));
    };

    PPR_UNIT_TEST (float_rejects_garbage_and_non_finite) {
        for (const std::string_view bad: {"abc", "1.0.0", "nan", "inf", "-inf"}) {
            Array<Layer> layers{};
            layers.push_back(singleLayer(ESource::cli, "perf.scale", SourceValue::text(std::string{bad}), "argv[1]"));
            Diagnostics diagnostics{};
            const auto result = resolve<PerfSettings>(layers, diagnostics);
            PPR_TEST_ASSERT(not result.has_value());
        }

        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.scale", SourceValue::text("2.5"), "argv[1]"));
        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_scale == 2.5);
    };

    PPR_UNIT_TEST (boolean_token_set) {
        for (const std::string_view truthy: {"1", "true", "YES", "on", "Enabled"}) {
            Array<Layer> layers{};
            layers.push_back(
                singleLayer(ESource::environment, "perf.vsync", SourceValue::text(std::string{truthy}), "env:X"));
            Diagnostics diagnostics{};
            const auto result = resolve<PerfSettings>(layers, diagnostics);
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(result->m_value.m_vsync);
        }
        for (const std::string_view falsy: {"0", "false", "No", "off", "disabled"}) {
            Array<Layer> layers{};
            layers.push_back(
                singleLayer(ESource::environment, "perf.vsync", SourceValue::text(std::string{falsy}), "env:X"));
            Diagnostics diagnostics{};
            const auto result = resolve<PerfSettings>(layers, diagnostics);
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(not result->m_value.m_vsync);
        }

        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.vsync", SourceValue::text("banana"), "argv[1]"));
        Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not resolve<PerfSettings>(layers, diagnostics).has_value());
    };

    PPR_UNIT_TEST (enum_named_domain_only) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.pacing", SourceValue::text("paced"), "argv[1]"));
        Diagnostics diagnostics{};
        const auto paced = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(paced.has_value());
        PPR_TEST_ASSERT(paced->m_value.m_pacing == EPace::paced);

        // Ordinal text is not a name.
        Array<Layer> ordinal{};
        ordinal.push_back(singleLayer(ESource::cli, "perf.pacing", SourceValue::text("2"), "argv[1]"));
        Diagnostics ordinal_diagnostics{};
        PPR_TEST_ASSERT(not resolve<PerfSettings>(ordinal, ordinal_diagnostics).has_value());

        // A file integer must not become an enum ordinal.
        Array<Layer> strict{};
        strict.push_back(singleLayer(ESource::file, "perf.pacing", SourceValue::integer(2), "base.toml:1"));
        Diagnostics strict_diagnostics{};
        const auto strict_result = resolve<PerfSettings>(strict, strict_diagnostics);
        PPR_TEST_ASSERT(not strict_result.has_value());
        PPR_TEST_ASSERT(strict_result.error() == make_error_code(EConfigErrc::type_mismatch));

        // Unknown names are rejected.
        Array<Layer> unknown{};
        unknown.push_back(singleLayer(ESource::cli, "perf.pacing", SourceValue::text("turbo"), "argv[1]"));
        Diagnostics unknown_diagnostics{};
        PPR_TEST_ASSERT(not resolve<PerfSettings>(unknown, unknown_diagnostics).has_value());
    };

    PPR_UNIT_TEST (cross_field_validation_runs_after_merge) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.pacing", SourceValue::text("uncapped"), "argv[1]"));
        layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text("144"), "argv[2]"));

        Diagnostics diagnostics{};
        const auto result = resolve<PerfSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == make_error_code(EConfigErrc::cross_field_invalid));
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (shadowed_invalid_values_still_error) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::file, "perf.cap_hz", SourceValue::text("0"), "base.toml:1"));
        layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text("120"), "argv[1]"));

        Diagnostics diagnostics{};
        // The file value is strict text for an integer: invalid even though
        // the CLI layer would shadow it.
        PPR_TEST_ASSERT(not resolve<PerfSettings>(layers, diagnostics).has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (source_restrictions_are_enforced) {
        // perf.startup_only accepts file values only.
        Array<Layer> cli{};
        cli.push_back(singleLayer(ESource::cli, "perf.startup_only", SourceValue::text("9"), "argv[1]"));
        Diagnostics cli_diagnostics{};
        const auto cli_result = resolve<PerfSettings>(cli, cli_diagnostics);
        PPR_TEST_ASSERT(not cli_result.has_value());
        PPR_TEST_ASSERT(cli_result.error() == make_error_code(EConfigErrc::source_disallowed));

        Array<Layer> file{};
        file.push_back(singleLayer(ESource::file, "perf.startup_only", SourceValue::integer(9), "base.toml:1"));
        Diagnostics file_diagnostics{};
        const auto file_result = resolve<PerfSettings>(file, file_diagnostics);
        PPR_TEST_ASSERT(file_result.has_value());
        PPR_TEST_ASSERT(file_result->m_value.m_startup_only == 9);

        // Trusted programmatic layers (ESource::none) bypass permissions.
        Array<Layer> profile{};
        profile.push_back(singleLayer(ESource::none, "perf.startup_only", SourceValue::integer(11), "profile"));
        Diagnostics profile_diagnostics{};
        const auto profile_result = resolve<PerfSettings>(profile, profile_diagnostics);
        PPR_TEST_ASSERT(profile_result.has_value());
        PPR_TEST_ASSERT(profile_result->m_value.m_startup_only == 11);
    };

    PPR_UNIT_TEST (optional_absence_supply_and_reset) {
        Diagnostics diagnostics{};
        const auto empty = resolve<PerfSettings>(std::span<const Layer>{}, diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        PPR_TEST_ASSERT(not empty->m_value.m_seed.has_value());

        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.seed", SourceValue::text("42"), "argv[1]"));
        Diagnostics supplied_diagnostics{};
        auto supplied = resolve<PerfSettings>(layers, supplied_diagnostics);
        PPR_TEST_ASSERT(supplied.has_value());
        PPR_TEST_ASSERT(supplied->m_value.m_seed.has_value());
        PPR_TEST_ASSERT(*supplied->m_value.m_seed == 42);

        Layer reset_layer{};
        reset_layer.m_source = ESource::runtime;
        reset_layer.m_origin = "runtime";
        reset_layer.m_assignments.push_back(Assignment{"perf.seed", {}, SourceValue::reset(), "panel", 0});
        Diagnostics reset_diagnostics{};
        PPR_TEST_ASSERT(applyRuntime(*supplied, reset_layer, reset_diagnostics) == std::error_code{});
        PPR_TEST_ASSERT(not supplied->m_value.m_seed.has_value());

        // Reset on a non-optional property is an explicit error.
        Layer bad_reset{};
        bad_reset.m_source = ESource::runtime;
        bad_reset.m_origin = "runtime";
        bad_reset.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::reset(), "panel", 0});
        Diagnostics bad_diagnostics{};
        PPR_TEST_ASSERT(applyRuntime(*supplied, bad_reset, bad_diagnostics) != std::error_code{});
    };

    PPR_UNIT_TEST (runtime_mutability_and_atomicity) {
        Diagnostics diagnostics{};
        auto committed = resolve<PerfSettings>(std::span<const Layer>{}, diagnostics);
        PPR_TEST_ASSERT(committed.has_value());

        // Runtime-editable property applies and updates provenance.
        Layer good{};
        good.m_source = ESource::runtime;
        good.m_origin = "panel";
        good.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::text("240"), "panel", 0});
        Diagnostics good_diagnostics{};
        PPR_TEST_ASSERT(applyRuntime(*committed, good, good_diagnostics) == std::error_code{});
        PPR_TEST_ASSERT(committed->m_value.m_cap_hz == 240u);
        const PropertyOrigin *origin = committed->find("perf.cap_hz");
        PPR_TEST_ASSERT(origin != nullptr);
        PPR_TEST_ASSERT(origin->m_source == ESource::runtime);

        // Startup-only property is rejected at runtime.
        Layer startup{};
        startup.m_source = ESource::runtime;
        startup.m_origin = "panel";
        startup.m_assignments.push_back(Assignment{"perf.startup_only", {}, SourceValue::text("3"), "panel", 0});
        Diagnostics startup_diagnostics{};
        PPR_TEST_ASSERT(applyRuntime(*committed, startup, startup_diagnostics) != std::error_code{});
        PPR_TEST_ASSERT(committed->m_value.m_startup_only == 7);

        // Failed apply leaves committed state untouched (scratch-then-commit).
        Layer bad{};
        bad.m_source = ESource::runtime;
        bad.m_origin = "panel";
        bad.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::text("5000"), "panel", 0});
        Diagnostics bad_diagnostics{};
        PPR_TEST_ASSERT(applyRuntime(*committed, bad, bad_diagnostics) != std::error_code{});
        PPR_TEST_ASSERT(committed->m_value.m_cap_hz == 240u);
    };

    PPR_UNIT_TEST (runtime_rejects_unknown_properties) {
        Diagnostics diagnostics{};
        auto committed = resolve<PerfSettings>(std::span<const Layer>{}, diagnostics);
        PPR_TEST_ASSERT(committed.has_value());

        // A mixed layer (one valid override plus one unknown property)
        // fails without changing any committed value.
        Layer mixed{};
        mixed.m_source = ESource::runtime;
        mixed.m_origin = "panel";
        mixed.m_assignments.push_back(Assignment{"perf.cap_hz", {}, SourceValue::text("240"), "panel", 0});
        mixed.m_assignments.push_back(Assignment{"perf.bogus", {}, SourceValue::text("1"), "panel", 1});
        Diagnostics mixed_diagnostics{};
        const std::error_code err = applyRuntime(*committed, mixed, mixed_diagnostics);
        PPR_TEST_ASSERT(err == make_error_code(EConfigErrc::unknown_property));
        PPR_TEST_ASSERT(mixed_diagnostics.hasErrors());
        PPR_TEST_ASSERT(committed->m_value.m_cap_hz == 60u);
    };

    PPR_UNIT_TEST (sensitive_values_are_redacted) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.token", SourceValue::text(std::string(80, 'x')), "argv[1]"));

        Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not resolve<PerfSettings>(layers, diagnostics).has_value());
        const std::string text = diagnostics.format();
        PPR_TEST_ASSERT(text.find("<redacted>") != std::string::npos);
        PPR_TEST_ASSERT(text.find(std::string(80, 'x')) == std::string::npos);
    };

    PPR_UNIT_TEST (diagnostics_carry_context) {
        Array<Layer> layers{};
        layers.push_back(singleLayer(ESource::cli, "perf.cap_hz", SourceValue::text("banana"), "argv[4]"));

        Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not resolve<PerfSettings>(layers, diagnostics).has_value());
        PPR_TEST_ASSERT(diagnostics.size() == 1u);
        const Diagnostic &entry = diagnostics.entries().front();
        PPR_TEST_ASSERT(entry.m_location == "argv[4]");
        PPR_TEST_ASSERT(entry.m_property == "perf.cap_hz");
        PPR_TEST_ASSERT(not entry.m_message.empty());
        PPR_TEST_ASSERT(entry.m_code != std::error_code{});
        PPR_TEST_ASSERT(not diagnostics.format().empty());
    };

    struct DuplicateNameSettings {
        u32 m_a{1};
        u32 m_b{2};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &DuplicateNameSettings::m_a, Metadata{"dup.x", "A", "T", EMutability::startup, ESource::all, false},
                    Range<u32>{0u, 10u}),
                field(
                    &DuplicateNameSettings::m_b, Metadata{"dup.x", "B", "T", EMutability::startup, ESource::all, false},
                    Range<u32>{0u, 10u}),
            };
        }
    };

    struct BadDefaultSettings {
        u32 m_v{5000};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &BadDefaultSettings::m_v, Metadata{"bad.v", "V", "T", EMutability::startup, ESource::all, false},
                    Range<u32>{0u, 10u}),
            };
        }
    };

    PPR_UNIT_TEST (schema_validation) {
        Diagnostics good_diagnostics{};
        PPR_TEST_ASSERT(validateSchema<PerfSettings>(good_diagnostics).has_value());

        Diagnostics dup_diagnostics{};
        PPR_TEST_ASSERT(not validateSchema<DuplicateNameSettings>(dup_diagnostics).has_value());
        PPR_TEST_ASSERT(dup_diagnostics.hasErrors());

        Diagnostics default_diagnostics{};
        PPR_TEST_ASSERT(not validateSchema<BadDefaultSettings>(default_diagnostics).has_value());
        PPR_TEST_ASSERT(default_diagnostics.hasErrors());
    };

    struct RequiredRecord {
        u32 m_value{};
        static constexpr auto describe() {
            return std::tuple{field(
                &RequiredRecord::m_value, Metadata{"value", "Required value", "Tree"}, Range<u32>{1u, 9u},
                EPresence::required)};
        }
        static std::error_code validate(const RequiredRecord &record, Diagnostics &) {
            PPR_TEST_ASSERT(record.m_value != 0u);
            return {};
        }
    };

    struct PresenceSettings {
        u32 m_required{};
        u32 m_defaulted{4u};
        std::optional<u32> m_optional{6u};
        std::optional<RequiredRecord> m_child{};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &PresenceSettings::m_required, Metadata{"required", "Required", "Tree"}, Range<u32>{1u, 9u},
                    EPresence::required),
                field(
                    &PresenceSettings::m_defaulted, Metadata{"defaulted", "Defaulted", "Tree"}, Range<u32>{1u, 9u},
                    EPresence::defaulted),
                field(
                    &PresenceSettings::m_optional, Metadata{"optional", "Optional", "Tree"}, Range<u32>{1u, 9u},
                    EPresence::optional),
                group(&PresenceSettings::m_child, Metadata{"child", "Optional record", "Tree"}, EPresence::optional)};
        }
    };

    struct RecordSettings {
        KeyedCollection<RequiredRecord> m_records{};
        static constexpr auto describe() {
            auto records = collection(&RecordSettings::m_records, Metadata{"records", "Records", "Tree"}, {1u, 4u});
            records.m_presence = EPresence::required;
            return std::tuple{records};
        }
    };

    template<u32 Default>
    struct NestedDefaultsRecord {
        u32 m_required{};
        u32 m_defaulted{Default};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &NestedDefaultsRecord::m_required, Metadata{"required", "Required", "Tree"}, Range<u32>{1u, 9u},
                    EPresence::required),
                field(
                    &NestedDefaultsRecord::m_defaulted, Metadata{"defaulted", "Defaulted", "Tree"}, Range<u32>{1u, 9u},
                    EPresence::defaulted)};
        }
    };

    template<u32 Default>
    struct RequiredDefaultsGroup {
        NestedDefaultsRecord<Default> m_record{};
        static constexpr auto describe() {
            return std::tuple{
                group(&RequiredDefaultsGroup::m_record, Metadata{"record", "Record", "Tree"}, EPresence::required)};
        }
    };

    template<u32 Default>
    struct OptionalDefaultsGroup {
        std::optional<NestedDefaultsRecord<Default>> m_record{};
        static constexpr auto describe() {
            return std::tuple{
                group(&OptionalDefaultsGroup::m_record, Metadata{"record", "Record", "Tree"}, EPresence::optional)};
        }
    };

    template<u32 Default>
    struct EmptyDefaultsCollection {
        KeyedCollection<NestedDefaultsRecord<Default>> m_records{};
        static constexpr auto describe() {
            return std::tuple{
                collection(&EmptyDefaultsCollection::m_records, Metadata{"records", "Records", "Tree"}, {0u, 4u})};
        }
    };

    PPR_UNIT_TEST (required_defaulted_optional_presence) {
        Diagnostics diagnostics;
        const auto missing = resolveRecursive<PresenceSettings>({}, diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "required");

        TreeLayer layer{singleLayer(ESource::none, "required", SourceValue::integer(2)), {}};
        diagnostics.clear();
        const auto omitted = resolveRecursive<PresenceSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(omitted.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(omitted->m_value.m_required == 2u and omitted->m_value.m_defaulted == 4u);
        PPR_TEST_ASSERT(not omitted->m_value.m_optional and not omitted->m_value.m_child);

        layer.m_groups.push_back({"child", "test:2"});
        diagnostics.clear();
        const auto empty_child = resolveRecursive<PresenceSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not empty_child and empty_child.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "child.value");

        layer.m_scalars.m_assignments.push_back({"child.value", {}, SourceValue::integer(3), "programmatic", 0u});
        diagnostics.clear();
        const auto trusted = resolveRecursive<PresenceSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(trusted.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(trusted->m_value.m_child and trusted->m_value.m_child->m_value == 3u);
        PPR_TEST_ASSERT(trusted->find("required") != nullptr and trusted->find("required")->m_source == ESource::none);
    };

    PPR_UNIT_TEST (duplicate_and_unknown_keys) {
        TreeLayer layer{
            singleLayer(ESource::none, "records.rock.value", SourceValue::integer(2)),
            {{"records.rock", "test:1"}, {"records.rock", "test:2"}}};
        Diagnostics diagnostics;
        const auto duplicate = resolveRecursive<RecordSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not duplicate and duplicate.error() == make_error_code(EConfigErrc::duplicate_key));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "records.rock");

        layer.m_groups.pop_back();
        layer.m_scalars.m_assignments.push_back({"records.rock.typo", {}, SourceValue::integer(3), "test:3", 0u});
        diagnostics.clear();
        const auto unknown = resolveRecursive<RecordSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not unknown and unknown.error() == make_error_code(EConfigErrc::unknown_property));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "records.rock.typo");
        PPR_TEST_ASSERT(diagnostics.entries().front().m_location == "test:3");
        PPR_TEST_ASSERT(diagnostics.format().find("records.rock.typo") != std::string::npos);
    };

    struct SourceOrderRecord {
        u32 m_value{}, m_other{4u};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &SourceOrderRecord::m_value, Metadata{"value", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u}, EPresence::required),
                field(
                    &SourceOrderRecord::m_other, Metadata{"other", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u})};
        }
    };

    PPR_UNIT_TEST (recursive_source_order_preserves_collection_and_origin_winners) {
        struct SourceOrderSettings {
            KeyedCollection<SourceOrderRecord> m_records;
            static constexpr auto describe() {
                auto records = collection(
                    &SourceOrderSettings::m_records, Metadata{"records", {}, {}, EMutability::startup, ESource::all},
                    {1u, 4u});
                records.m_presence = EPresence::required;
                return std::tuple{records};
            }
        };
        TreeLayer base;
        base.m_scalars.m_source = ESource::file;
        base.m_groups = {{"records.z", "base:group-z"}, {"records.a", "base:group-a"}};
        base.m_scalars.m_assignments = {
            {"records.a.value", {}, SourceValue::integer(2), "base:a-first", 99u},
            {"records.z.value", {}, SourceValue::integer(3), "base:z", 0u},
            {"records.a.value", {}, SourceValue::integer(4), "base:a-last", 1u},
            {"records.m.value", {}, SourceValue::integer(5), "base:m", 0u}};
        TreeLayer overlay;
        overlay.m_scalars.m_source = ESource::cli;
        overlay.m_groups = {{"records.b", "cli:group-b"}};
        overlay.m_scalars.m_assignments = {
            {"records.b.value", {}, SourceValue::text("7"), "cli:b", 0u},
            {"records.z.value", {}, SourceValue::text("8"), "cli:z", 0u}};
        const TreeLayer layers[] = {base, overlay};

        Diagnostics diagnostics;
        const auto result = resolveRecursive<SourceOrderSettings>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value() and not diagnostics.hasErrors());
        const auto &entries = result->m_value.m_records.m_entries;
        PPR_TEST_ASSERT(entries.size() == 4u);
        PPR_TEST_ASSERT(entries[0u].m_key == "z" and entries[0u].m_value.m_value == 8u);
        PPR_TEST_ASSERT(entries[1u].m_key == "a" and entries[1u].m_value.m_value == 4u);
        PPR_TEST_ASSERT(entries[2u].m_key == "m" and entries[2u].m_value.m_value == 5u);
        PPR_TEST_ASSERT(entries[3u].m_key == "b" and entries[3u].m_value.m_value == 7u);
        PPR_TEST_ASSERT(result->find("records")->m_location == "cli:z");
        PPR_TEST_ASSERT(result->find("records.z")->m_location == "cli:z");
        PPR_TEST_ASSERT(result->find("records.z.value")->m_source == ESource::cli);
        PPR_TEST_ASSERT(result->find("records.a.value")->m_location == "base:a-last");

        Array<std::string> origin_paths;
        for (const auto &origin: result->m_origins) {
            origin_paths.push_back(origin.m_name);
        }
        PPR_TEST_ASSERT(
            origin_paths == (Array<std::string>{
                                "records", "records.z", "records.z.value", "records.z.other", "records.a",
                                "records.a.value", "records.a.other", "records.m", "records.m.value", "records.m.other",
                                "records.b", "records.b.value", "records.b.other"}));
    };

    PPR_UNIT_TEST (recursive_prefix_neighbors_preserve_descendant_presence) {
        struct PrefixSettings {
            std::optional<RequiredRecord> m_a, m_ab, m_underscore, m_dash;
            static constexpr auto describe() {
                return std::tuple{
                    group(&PrefixSettings::m_a, Metadata{"a"}, EPresence::optional),
                    group(&PrefixSettings::m_ab, Metadata{"ab"}, EPresence::optional),
                    group(&PrefixSettings::m_underscore, Metadata{"a_b"}, EPresence::optional),
                    group(&PrefixSettings::m_dash, Metadata{"a-b"}, EPresence::optional)};
            }
        };
        TreeLayer layer;
        layer.m_scalars.m_assignments = {
            {"a-b.value", {}, SourceValue::integer(3), "dash", 0u},
            {"ab.value", {}, SourceValue::integer(4), "neighbor", 0u},
            {"a_b.value", {}, SourceValue::integer(5), "underscore", 0u},
            {"a.value", {}, SourceValue::integer(2), "descendant", 0u}};
        Diagnostics diagnostics;
        const auto present = resolveRecursive<PrefixSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(present.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(present->m_value.m_a and present->m_value.m_a->m_value == 2u);
        PPR_TEST_ASSERT(present->m_value.m_ab and present->m_value.m_ab->m_value == 4u);
        PPR_TEST_ASSERT(present->m_value.m_underscore and present->m_value.m_underscore->m_value == 5u);
        PPR_TEST_ASSERT(present->m_value.m_dash and present->m_value.m_dash->m_value == 3u);

        layer.m_scalars.m_assignments.pop_back();
        diagnostics.clear();
        const auto absent = resolveRecursive<PrefixSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(absent.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(not absent->m_value.m_a);
        PPR_TEST_ASSERT(absent->m_value.m_ab and absent->m_value.m_underscore and absent->m_value.m_dash);
    };

    PPR_UNIT_TEST (recursive_group_source_errors_preserve_authored_locations) {
        struct FileGroupSettings {
            SourceOrderRecord m_record;
            static constexpr auto describe() {
                return std::tuple{group(
                    &FileGroupSettings::m_record, Metadata{"record", {}, {}, EMutability::startup, ESource::file},
                    EPresence::required)};
            }
        };
        TreeLayer layer;
        layer.m_scalars.m_source = ESource::cli;
        layer.m_groups = {{"record", "argv:group"}};
        layer.m_scalars.m_assignments = {
            {"record.value", {}, SourceValue::text("2"), "argv:value", 0u},
            {"record.other", {}, SourceValue::text("3"), "argv:other", 0u}};
        Diagnostics diagnostics;
        const auto result = resolveRecursive<FileGroupSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::source_disallowed));
        PPR_TEST_ASSERT(diagnostics.entries().size() == 3u);
        const auto &errors = diagnostics.entries();
        PPR_TEST_ASSERT(errors[0u].m_location == "argv:group");
        PPR_TEST_ASSERT(errors[1u].m_location == "argv:value");
        PPR_TEST_ASSERT(errors[2u].m_location == "argv:other");
        for (const auto &error: errors) {
            PPR_TEST_ASSERT(error.m_property == "record");
            PPR_TEST_ASSERT(error.m_code == make_error_code(EConfigErrc::source_disallowed));
        }
    };

    PPR_UNIT_TEST (recursive_invalid_collection_keys_preserve_diagnostic_order) {
        TreeLayer layer;
        layer.m_groups = {{"records.Z", "group:z"}, {"records.A", "group:a"}};
        layer.m_scalars.m_assignments = {
            {"records.X.value", {}, SourceValue::integer(2), "scalar:x", 0u},
            {"records.B.value", {}, SourceValue::integer(3), "scalar:b", 0u}};
        Diagnostics diagnostics;
        const auto result = resolveRecursive<RecordSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::out_of_range));
        PPR_TEST_ASSERT(diagnostics.entries().size() >= 4u);
        const auto &errors = diagnostics.entries();
        const std::string_view locations[] = {"group:z", "group:a", "scalar:x", "scalar:b"};
        const std::string_view paths[] = {"records.Z", "records.A", "records.X", "records.B"};
        for (std::size_t i = 0u; i < std::size(locations); ++i) {
            PPR_TEST_ASSERT(errors[i].m_location == locations[i]);
            PPR_TEST_ASSERT(errors[i].m_property == paths[i]);
            PPR_TEST_ASSERT(errors[i].m_code == make_error_code(EConfigErrc::out_of_range));
        }
    };

    struct ScopedReference {
        std::string m_key{"unresolved"};
    };

    template<EReferenceScope Scope, bool BadCollection = false>
    struct ScopedReferenceSettings {
        ScopedReference m_target{};
        static constexpr auto describe() {
            return std::tuple{field(
                &ScopedReferenceSettings::m_target,
                Metadata{"target", "Target", "Tree", EMutability::startup, ESource::file},
                ReferenceDomain<ScopedReference>{{BadCollection ? "records..bad" : "records", "usable", Scope}})};
        }
    };

    PPR_UNIT_TEST (owner_reference_scope_is_metadata_and_validation_stays_syntax_only) {
        Diagnostics diagnostics;
        PPR_TEST_ASSERT(validateSchema<ScopedReferenceSettings<EReferenceScope::root>>(diagnostics).has_value());
        PPR_TEST_ASSERT(validateSchema<ScopedReferenceSettings<EReferenceScope::owner>>(diagnostics).has_value());
        const auto descriptor = describeProperty(
            std::get<0u>(ScopedReferenceSettings<EReferenceScope::owner>::describe()));
        PPR_TEST_ASSERT(descriptor.m_reference_target.has_value());
        PPR_TEST_ASSERT(descriptor.m_reference_target->m_scope == EReferenceScope::owner);
        PPR_TEST_ASSERT(descriptor.m_reference_target->m_collection == "records");
        PPR_TEST_ASSERT(descriptor.m_reference_target->m_suitability == "usable");

        // No records or owner are supplied to core: lookup belongs to the consumer.
        TreeLayer layer{singleLayer(ESource::file, "target", SourceValue::text("missing_but_valid")), {}};
        const auto resolved = resolveRecursive<ScopedReferenceSettings<EReferenceScope::owner>>(
            std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(resolved->m_value.m_target.m_key == "missing_but_valid");

        for (const auto &key: {std::string{}, std::string{"has.dot"}, std::string{"Upper"}, std::string(129u, 'a')}) {
            diagnostics.clear();
            layer.m_scalars.m_assignments.front().m_value = SourceValue::text(key);
            const auto invalid = resolveRecursive<ScopedReferenceSettings<EReferenceScope::owner>>(
                std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(not invalid and invalid.error() == make_error_code(EConfigErrc::out_of_range));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "target");
        }
    };

    PPR_UNIT_TEST (reference_scope_declarations_reject_invalid_targets_and_unknown_scope) {
        Diagnostics diagnostics;
        const auto bad_collection = validateSchema<ScopedReferenceSettings<EReferenceScope::owner, true>>(diagnostics);
        PPR_TEST_ASSERT(not bad_collection and bad_collection.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "target");
        diagnostics.clear();
        const auto bad_scope = validateSchema<ScopedReferenceSettings<static_cast<EReferenceScope>(255u)>>(diagnostics);
        PPR_TEST_ASSERT(not bad_scope and bad_scope.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "target");
    };

    PPR_UNIT_TEST (recursive_schema_validation) {
        Diagnostics diagnostics;
        PPR_TEST_ASSERT(validateSchema<PresenceSettings>(diagnostics).has_value());
        PPR_TEST_ASSERT(validateSchema<RecordSettings>(diagnostics).has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());

        struct BadDefault : PresenceSettings {
            BadDefault() {
                m_defaulted = 0u;
            }
        };
        diagnostics.clear();
        const auto bad_default = validateSchema<BadDefault>(diagnostics);
        PPR_TEST_ASSERT(not bad_default and bad_default.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "defaulted");

        struct ConcealedRequired {
            RequiredRecord m_record{2u};
            static constexpr auto describe() {
                return std::tuple{
                    group(&ConcealedRequired::m_record, Metadata{"record", "Record", "Tree"}, EPresence::defaulted)};
            }
        };
        diagnostics.clear();
        const auto concealed = validateSchema<ConcealedRequired>(diagnostics);
        PPR_TEST_ASSERT(not concealed and concealed.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record.value");
    };

    PPR_UNIT_TEST (recursive_required_group_validates_defaulted_descendants) {
        Diagnostics diagnostics;
        const auto invalid = validateSchema<RequiredDefaultsGroup<0u>>(diagnostics);
        PPR_TEST_ASSERT(not invalid and invalid.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.size() == 1u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record.defaulted");

        diagnostics.clear();
        PPR_TEST_ASSERT(validateSchema<RequiredDefaultsGroup<4u>>(diagnostics).has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        const auto missing = resolveRecursive<RequiredDefaultsGroup<4u>>({}, diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record");

        TreeLayer layer{};
        layer.m_groups.push_back({"record", "test:1"});
        diagnostics.clear();
        const auto missing_leaf = resolveRecursive<RequiredDefaultsGroup<4u>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not missing_leaf and missing_leaf.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record.required");

        struct InitializedCollection {
            KeyedCollection<NestedDefaultsRecord<4u>> m_records{};
            InitializedCollection() {
                m_records.m_entries.push_back({"entry", {0u, 0u}});
            }
            static constexpr auto describe() {
                return std::tuple{
                    collection(&InitializedCollection::m_records, Metadata{"records", "Records", "Tree"}, {0u, 4u})};
            }
        };
        struct RequiredCollectionGroup {
            InitializedCollection m_record{};
            static constexpr auto describe() {
                return std::tuple{group(
                    &RequiredCollectionGroup::m_record, Metadata{"record", "Record", "Tree"}, EPresence::required)};
            }
        };
        diagnostics.clear();
        const auto invalid_entry = validateSchema<RequiredCollectionGroup>(diagnostics);
        PPR_TEST_ASSERT(not invalid_entry and invalid_entry.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.size() == 1u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record.records.entry.defaulted");
    };

    PPR_UNIT_TEST (recursive_absent_optional_validates_defaulted_descendants) {
        Diagnostics diagnostics;
        const auto invalid = validateSchema<OptionalDefaultsGroup<0u>>(diagnostics);
        PPR_TEST_ASSERT(not invalid and invalid.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.size() == 1u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "record.defaulted");

        diagnostics.clear();
        PPR_TEST_ASSERT(validateSchema<OptionalDefaultsGroup<4u>>(diagnostics).has_value());
        const auto absent = resolveRecursive<OptionalDefaultsGroup<4u>>({}, diagnostics);
        PPR_TEST_ASSERT(absent.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(not absent->m_value.m_record);
    };

    PPR_UNIT_TEST (recursive_empty_collection_validates_defaulted_element_descendants) {
        Diagnostics diagnostics;
        const auto invalid = validateSchema<EmptyDefaultsCollection<0u>>(diagnostics);
        PPR_TEST_ASSERT(not invalid and invalid.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(diagnostics.size() == 1u);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "records.{key}.defaulted");

        Diagnostics capped{0u};
        const auto capped_invalid = validateSchema<EmptyDefaultsCollection<0u>>(capped);
        PPR_TEST_ASSERT(not capped_invalid and capped_invalid.error() == make_error_code(EConfigErrc::schema_invalid));
        PPR_TEST_ASSERT(capped.entries().empty() and capped.size() == 1u and capped.omittedCount() == 1u);

        diagnostics.clear();
        PPR_TEST_ASSERT(validateSchema<EmptyDefaultsCollection<4u>>(diagnostics).has_value());
        const auto empty = resolveRecursive<EmptyDefaultsCollection<4u>>({}, diagnostics);
        PPR_TEST_ASSERT(empty.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(empty->m_value.m_records.m_entries.empty());
    };

    struct VariantEmpty {
        static constexpr auto describe() {
            return std::tuple{};
        }
    };

    struct VariantNumbers {
        u32 m_amount{};
        u32 m_offset{4u};
        std::optional<u32> m_extra{};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &VariantNumbers::m_amount, Metadata{"amount", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u}, EPresence::required),
                field(
                    &VariantNumbers::m_offset, Metadata{"offset", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u}),
                field(
                    &VariantNumbers::m_extra, Metadata{"extra", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u}, EPresence::optional)};
        }
    };

    struct VariantNested {
        std::variant<VariantEmpty, VariantNumbers> m_inner{};
        static constexpr auto describe() {
            return std::tuple{taggedVariant(
                &VariantNested::m_inner, Metadata{"inner", {}, {}, EMutability::startup, ESource::all},
                EPresence::required, alternative<VariantEmpty>("empty"), alternative<VariantNumbers>("numbers"))};
        }
    };

    template<EPresence Presence>
    struct VariantSettings {
        using Payload = std::variant<VariantEmpty, VariantNumbers, VariantNested>;
        using Value = std::conditional_t<Presence == EPresence::optional, std::optional<Payload>, Payload>;
        Value m_choice{Payload{VariantEmpty{}}};
        u32 m_unrelated{4u};
        VariantSettings() {
            if constexpr (Presence == EPresence::defaulted) {
                m_choice = VariantNumbers{3u, 6u, 2u};
            }
        }
        static constexpr auto describe() {
            return std::tuple{
                taggedVariant(
                    &VariantSettings::m_choice, Metadata{"choice", {}, {}, EMutability::startup, ESource::all},
                    Presence, alternative<VariantEmpty>("empty"), alternative<VariantNumbers>("numbers"),
                    alternative<VariantNested>("nested")),
                field(
                    &VariantSettings::m_unrelated, Metadata{"unrelated", {}, {}, EMutability::startup, ESource::all},
                    Range<u32>{1u, 9u})};
        }
    };

    [[nodiscard]] TreeLayer variantLayer(
        const std::string &path, const std::string &tag, const std::string &origin = "variant:1") {
        TreeLayer layer;
        layer.m_scalars = singleLayer(ESource::none, path + ".tag", SourceValue::text(tag), origin);
        layer.m_scalars.m_origin = origin;
        layer.m_groups = {{path, origin}, {path + "." + tag, origin}};
        return layer;
    }

    void variantAssignment(TreeLayer &layer, std::string path, SourceValue value, std::string location = "variant:2") {
        layer.m_scalars.m_assignments.push_back({std::move(path), {}, std::move(value), std::move(location), 0u});
    }

    PPR_UNIT_TEST (recursive_owned_results_survive_sources_and_scratch_reuse) {
        struct Collection {
            KeyedCollection<VariantSettings<EPresence::required>> m_records;

            static constexpr auto describe() {
                return std::tuple{collection(
                    &Collection::m_records,
                    Metadata{"long_configuration_records", {}, {}, EMutability::startup, ESource::all}, {0u, 4u})};
            }
        };

        const std::string key(96u, 'k');
        const std::string path = "long_configuration_records." + key + ".choice";
        const std::string amount_path = path + ".nested.inner.numbers.amount";
        const std::string origin = "a_long_owned_source_location_that_must_survive_resolution.toml:27";
        Expected<Resolved<Collection>> result;
        Diagnostics diagnostics;
        {
            auto layer = variantLayer(path, "nested", origin);
            variantAssignment(layer, path + ".nested.inner.tag", SourceValue::text("numbers"), origin);
            variantAssignment(layer, amount_path, SourceValue::integer(7), origin);
            layer.m_groups.push_back({path + ".nested.inner", origin});
            layer.m_groups.push_back({path + ".nested.inner.numbers", origin});

            result = resolveRecursive<Collection>(std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(result.has_value() and not diagnostics.hasErrors());
        }

        {
            const auto scratch_scope = mem::ScratchPad::open();
            Array<std::byte, mem::ScratchPad> overwrite(1024u * 1024u, std::byte{0x6d});
            PPR_TEST_ASSERT(overwrite.front() == std::byte{0x6d});
        }

        PPR_TEST_ASSERT(result->m_value.m_records.m_entries.size() == 1u);
        PPR_TEST_ASSERT(result->m_value.m_records.m_entries.front().m_key == key);
        const auto &nested = std::get<VariantNested>(result->m_value.m_records.m_entries.front().m_value.m_choice);
        PPR_TEST_ASSERT(std::get<VariantNumbers>(nested.m_inner).m_amount == 7u);
        const auto *const retained = result->find(amount_path);
        PPR_TEST_ASSERT(retained != nullptr and retained->m_location == origin and retained->m_name == amount_path);
    };

    PPR_UNIT_TEST (variant_replacement_scopes_exclude_prefix_siblings) {
        struct Prefixes {
            std::variant<VariantEmpty, VariantNumbers> m_foo{}, m_foobar{}, m_foo_dash{};

            static constexpr auto describe() {
                return std::tuple{
                    taggedVariant(
                        &Prefixes::m_foo, Metadata{"foo", {}, {}, EMutability::startup, ESource::all},
                        EPresence::required, alternative<VariantEmpty>("empty"),
                        alternative<VariantNumbers>("numbers")),
                    taggedVariant(
                        &Prefixes::m_foobar, Metadata{"foobar", {}, {}, EMutability::startup, ESource::all},
                        EPresence::required, alternative<VariantEmpty>("empty"),
                        alternative<VariantNumbers>("numbers")),
                    taggedVariant(
                        &Prefixes::m_foo_dash, Metadata{"foo-bar", {}, {}, EMutability::startup, ESource::all},
                        EPresence::required, alternative<VariantEmpty>("empty"),
                        alternative<VariantNumbers>("numbers"))};
            }
        };

        TreeLayer layer;
        for (const std::string_view path: {"foo", "foobar", "foo-bar"}) {
            auto subtree = variantLayer(std::string{path}, "numbers");
            layer.m_groups.insert(layer.m_groups.end(), subtree.m_groups.begin(), subtree.m_groups.end());
            layer.m_scalars.m_assignments.insert(
                layer.m_scalars.m_assignments.end(), subtree.m_scalars.m_assignments.begin(),
                subtree.m_scalars.m_assignments.end());
            variantAssignment(layer, std::string{path} + ".numbers.amount", SourceValue::integer(3));
        }

        Diagnostics diagnostics;
        const auto valid = resolveRecursive<Prefixes>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(valid.has_value() and not diagnostics.hasErrors());

        variantAssignment(layer, "foobar.numbers.typo", SourceValue::integer(1), "sibling:1");
        variantAssignment(layer, "foo-bar.numbers.typo", SourceValue::integer(1), "sibling:2");
        const auto invalid = resolveRecursive<Prefixes>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not invalid and invalid.error() == make_error_code(EConfigErrc::unknown_property));
        PPR_TEST_ASSERT(diagnostics.size() == 2u);
        PPR_TEST_ASSERT(diagnostics.entries()[0u].m_property == "foobar.numbers.typo");
        PPR_TEST_ASSERT(diagnostics.entries()[0u].m_location == "sibling:1");
        PPR_TEST_ASSERT(diagnostics.entries()[1u].m_property == "foo-bar.numbers.typo");
        PPR_TEST_ASSERT(diagnostics.entries()[1u].m_location == "sibling:2");
    };

    PPR_UNIT_TEST (variant_nested_resolution_is_order_independent) {
        auto layer = variantLayer("choice", "nested");
        variantAssignment(layer, "choice.nested.inner.numbers.amount", SourceValue::integer(7));
        variantAssignment(layer, "choice.nested.inner.tag", SourceValue::text("numbers"));
        layer.m_groups.push_back({"choice.nested.inner.numbers", "variant:3"});
        layer.m_groups.push_back({"choice.nested.inner", "variant:4"});
        for (u32 order = 0u; order < 2u; ++order) {
            Diagnostics diagnostics;
            const auto result = resolveRecursive<VariantSettings<EPresence::required>>(
                std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(result.has_value() and not diagnostics.hasErrors());
            const auto &nested = std::get<VariantNested>(result->m_value.m_choice);
            PPR_TEST_ASSERT(std::get<VariantNumbers>(nested.m_inner).m_amount == 7u);
            PPR_TEST_ASSERT(result->find("choice.nested.inner.tag") != nullptr);
            std::ranges::reverse(layer.m_groups);
            std::ranges::reverse(layer.m_scalars.m_assignments);
        }
    };

    PPR_UNIT_TEST (variant_tag_and_payload_errors_report_logical_paths) {
        const auto check = [](const TreeLayer &layer, const EConfigErrc expected, const std::string_view path) {
            Diagnostics diagnostics;
            const auto result = resolveRecursive<VariantSettings<EPresence::required>>(
                std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(expected));
            PPR_TEST_ASSERT(std::ranges::any_of(diagnostics.entries(), [&](const auto &entry) {
                return entry.m_property == path and entry.m_code == make_error_code(expected);
            }));
        };
        check(variantLayer("choice", "unknown"), EConfigErrc::unknown_enum_value, "choice.tag");
        auto layer = variantLayer("choice", "empty");
        layer.m_scalars.m_assignments.front().m_value = SourceValue::integer(1);
        check(layer, EConfigErrc::type_mismatch, "choice.tag");
        layer.m_scalars.m_assignments.clear();
        check(layer, EConfigErrc::missing_value, "choice.tag");
        layer = variantLayer("choice", "empty");
        layer.m_groups.pop_back();
        check(layer, EConfigErrc::missing_value, "choice.empty");
        layer = variantLayer("choice", "numbers");
        check(layer, EConfigErrc::missing_value, "choice.numbers.amount");
        layer = variantLayer("choice", "empty");
        variantAssignment(layer, "choice.numbers.amount", SourceValue::integer(2), "inactive:7");
        check(layer, EConfigErrc::unknown_property, "choice.numbers.amount");
        layer = variantLayer("choice", "empty");
        layer.m_groups.push_back({"choice.numbers", "inactive:8"});
        check(layer, EConfigErrc::unknown_property, "choice.numbers");
        layer = variantLayer("choice", "empty");
        variantAssignment(layer, "choice.empty.typo", SourceValue::integer(2));
        check(layer, EConfigErrc::unknown_property, "choice.empty.typo");
        layer = variantLayer("choice", "empty");
        variantAssignment(layer, "choice.tag", SourceValue::text("empty"));
        check(layer, EConfigErrc::duplicate_key, "choice.tag");
        layer = variantLayer("choice", "empty");
        layer.m_groups.push_back({"choice.empty", "duplicate:4"});
        check(layer, EConfigErrc::duplicate_key, "choice.empty");
        layer = variantLayer("choice", "numbers");
        variantAssignment(layer, "choice.numbers.amount", SourceValue::integer(2));
        variantAssignment(layer, "choice.numbers.amount", SourceValue::integer(3));
        check(layer, EConfigErrc::duplicate_key, "choice.numbers.amount");
    };

    PPR_UNIT_TEST (variant_required_optional_and_defaulted_presence) {
        Diagnostics diagnostics;
        const auto required = resolveRecursive<VariantSettings<EPresence::required>>({}, diagnostics);
        PPR_TEST_ASSERT(not required and required.error() == make_error_code(EConfigErrc::missing_value));
        diagnostics.clear();
        const auto absent = resolveRecursive<VariantSettings<EPresence::optional>>({}, diagnostics);
        PPR_TEST_ASSERT(absent.has_value() and not absent->m_value.m_choice);
        auto layer = variantLayer("choice", "empty");
        const auto present = resolveRecursive<VariantSettings<EPresence::optional>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(present.has_value() and present->m_value.m_choice);
        PPR_TEST_ASSERT(std::holds_alternative<VariantEmpty>(*present->m_value.m_choice));
        const auto defaulted = resolveRecursive<VariantSettings<EPresence::defaulted>>({}, diagnostics);
        PPR_TEST_ASSERT(defaulted.has_value());
        const auto &numbers = std::get<VariantNumbers>(defaulted->m_value.m_choice);
        PPR_TEST_ASSERT(numbers.m_amount == 3u and numbers.m_offset == 6u and numbers.m_extra == 2u);
        PPR_TEST_ASSERT(defaulted->find("choice.tag")->m_location == "default");
        const auto authored = resolveRecursive<VariantSettings<EPresence::defaulted>>(
            std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(authored.has_value() and std::holds_alternative<VariantEmpty>(authored->m_value.m_choice));
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (variant_layers_replace_same_and_changed_tags_atomically) {
        Array<TreeLayer> layers{variantLayer("choice", "numbers", "base:1")};
        variantAssignment(layers.front(), "choice.numbers.amount", SourceValue::integer(2), "base:2");
        variantAssignment(layers.front(), "choice.numbers.offset", SourceValue::integer(8), "base:3");
        variantAssignment(layers.front(), "choice.numbers.extra", SourceValue::integer(9), "base:4");
        layers.push_back(variantLayer("choice", "numbers", "overlay:1"));
        variantAssignment(layers.back(), "choice.numbers.amount", SourceValue::integer(5), "overlay:2");
        Diagnostics diagnostics;
        const auto same = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(same.has_value());
        const auto &numbers = std::get<VariantNumbers>(same->m_value.m_choice);
        PPR_TEST_ASSERT(numbers.m_amount == 5u and numbers.m_offset == 4u and not numbers.m_extra);
        PPR_TEST_ASSERT(same->find("choice.numbers.offset")->m_location == "default");
        PPR_TEST_ASSERT(same->find("choice.numbers.amount")->m_location == "overlay:2");
        PPR_TEST_ASSERT(same->find("choice.tag")->m_location == "overlay:1");

        layers.push_back(variantLayer("choice", "empty", "changed:1"));
        const auto changed = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(changed.has_value() and std::holds_alternative<VariantEmpty>(changed->m_value.m_choice));
        PPR_TEST_ASSERT(changed->find("choice.numbers.amount") == nullptr);
        layers.push_back({singleLayer(ESource::none, "unrelated", SourceValue::integer(7)), {}});
        const auto untouched = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(untouched.has_value() and std::holds_alternative<VariantEmpty>(untouched->m_value.m_choice));
        PPR_TEST_ASSERT(untouched->find("choice.tag")->m_location == "changed:1");
        PPR_TEST_ASSERT(untouched->m_value.m_unrelated == 7u and not diagnostics.hasErrors());
    };

    PPR_UNIT_TEST (repeated_nested_replacements_preserve_origin_order_and_sources) {
        struct Sequence {
            u32 m_head{4u}, m_tail{4u};
            std::variant<VariantEmpty, VariantNested> m_choice;

            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &Sequence::m_head, Metadata{"head", {}, {}, EMutability::startup, ESource::all},
                        Range<u32>{1u, 9u}),
                    taggedVariant(
                        &Sequence::m_choice, Metadata{"choice", {}, {}, EMutability::startup, ESource::all},
                        EPresence::required, alternative<VariantEmpty>("empty"), alternative<VariantNested>("nested")),
                    field(
                        &Sequence::m_tail, Metadata{"tail", {}, {}, EMutability::startup, ESource::all},
                        Range<u32>{1u, 9u})};
            }
        };

        Array<TreeLayer> layers;
        for (u32 index = 0u; index < 4u; ++index) {
            const std::string origin = "nested-layer-" + std::to_string(index) + ":27";
            auto layer = variantLayer("choice", "nested", origin);
            layer.m_scalars.m_source = ESource::file;
            layer.m_groups.push_back({"choice.nested.inner", origin});
            layer.m_groups.push_back({"choice.nested.inner.numbers", origin});
            variantAssignment(layer, "choice.nested.inner.tag", SourceValue::text("numbers"), origin);
            variantAssignment(layer, "choice.nested.inner.numbers.amount", SourceValue::integer(index + 1u), origin);
            layers.push_back(std::move(layer));
        }

        variantAssignment(layers.front(), "head", SourceValue::integer(7), "retained-head:2");
        variantAssignment(layers.front(), "tail", SourceValue::integer(8), "retained-tail:2");
        variantAssignment(layers.front(), "choice.nested.inner.numbers.extra", SourceValue::integer(9));
        Diagnostics diagnostics;
        const auto result = resolveRecursive<Sequence>(layers, diagnostics);
        PPR_TEST_ASSERT(result.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(result->m_value.m_head == 7u and result->m_value.m_tail == 8u);
        const auto &numbers = std::get<VariantNumbers>(std::get<VariantNested>(result->m_value.m_choice).m_inner);
        PPR_TEST_ASSERT(numbers.m_amount == 4u and numbers.m_offset == 4u and not numbers.m_extra);

        constexpr std::string_view expected[] = {
            "head", "choice", "choice.tag", "choice.nested", "choice.nested.inner", "choice.nested.inner.tag",
            "choice.nested.inner.numbers", "choice.nested.inner.numbers.amount", "choice.nested.inner.numbers.offset",
            "choice.nested.inner.numbers.extra", "tail"};
        PPR_TEST_ASSERT(result->m_origins.size() == std::size(expected));
        for (std::size_t index = 0u; index < std::size(expected); ++index) {
            PPR_TEST_ASSERT(result->m_origins[index].m_name == expected[index]);
        }

        PPR_TEST_ASSERT(result->find("head")->m_location == "retained-head:2");
        PPR_TEST_ASSERT(result->find("tail")->m_location == "retained-tail:2");
        PPR_TEST_ASSERT(result->find("choice.nested.inner.numbers.amount")->m_location == "nested-layer-3:27");
        PPR_TEST_ASSERT(result->find("choice.nested.inner.numbers.amount")->m_source == ESource::file);
        PPR_TEST_ASSERT(result->find("choice.nested.inner.numbers.offset")->m_location == "default");
        PPR_TEST_ASSERT(result->find("choice.nested.inner.numbers.offset")->m_source == ESource::none);
    };

    PPR_UNIT_TEST (variant_replacements_do_not_inherit_and_validate_shadowed_layers) {
        Array<TreeLayer> layers{variantLayer("choice", "numbers", "base:1")};
        variantAssignment(layers.front(), "choice.numbers.amount", SourceValue::integer(2));
        layers.push_back(variantLayer("choice", "numbers", "partial:1"));
        Diagnostics diagnostics;
        const auto partial = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(not partial and partial.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.numbers.amount");
        layers.back() = TreeLayer{singleLayer(ESource::none, "choice.numbers.amount", SourceValue::integer(3)), {}};
        diagnostics.clear();
        const auto no_tag = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(not no_tag and no_tag.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.tag");

        layers.front().m_scalars.m_assignments.back().m_value = SourceValue::integer(20);
        layers.back() = variantLayer("choice", "empty", "valid:1");
        diagnostics.clear();
        const auto shadowed = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(not shadowed and shadowed.error() == make_error_code(EConfigErrc::out_of_range));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.numbers.amount");

        // A later active payload must not legitimize an earlier inactive path.
        layers.front() = variantLayer("choice", "empty", "inactive:1");
        variantAssignment(layers.front(), "choice.numbers.amount", SourceValue::integer(2));
        layers.back() = variantLayer("choice", "numbers", "active:1");
        variantAssignment(layers.back(), "choice.numbers.amount", SourceValue::integer(3));
        diagnostics.clear();
        const auto inactive_shadowed = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(
            not inactive_shadowed and inactive_shadowed.error() == make_error_code(EConfigErrc::unknown_property));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.numbers.amount");

        layers.front() = variantLayer("choice", "nested", "outer:1");
        variantAssignment(layers.front(), "choice.nested.inner.tag", SourceValue::text("numbers"));
        variantAssignment(layers.front(), "choice.nested.inner.numbers.amount", SourceValue::integer(2));
        layers.back() = variantLayer("choice", "nested", "outer:2");
        diagnostics.clear();
        const auto no_outer_inheritance = resolveRecursive<VariantSettings<EPresence::required>>(layers, diagnostics);
        PPR_TEST_ASSERT(
            not no_outer_inheritance and no_outer_inheritance.error() == make_error_code(EConfigErrc::missing_value));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.nested.inner");
    };

    PPR_UNIT_TEST (variant_schema_all_and_value_active_preorder_traversal) {
        using S = VariantSettings<EPresence::optional>;
        const auto member = std::get<0u>(S::describe());
        const auto descriptor = describeProperty(member);
        PPR_TEST_ASSERT(descriptor.m_kind == ENodeKind::variant and descriptor.m_optional);
        PPR_TEST_ASSERT(descriptor.m_choices == (Array<std::string_view>{"empty", "numbers", "nested"}));
        Array<std::string> schema_paths;
        describeRecursive<S>([&](const auto &path, const auto &, const auto &) { schema_paths.push_back(path); });
        PPR_TEST_ASSERT(schema_paths.front() == "choice");
        PPR_TEST_ASSERT(std::ranges::find(schema_paths, "choice.nested.inner.numbers.amount") != schema_paths.end());
        PPR_TEST_ASSERT(std::ranges::find(schema_paths, "choice.numbers.amount") != schema_paths.end());
        S settings;
        settings.m_choice = VariantNumbers{2u};
        PPR_TEST_ASSERT(variantTag(member, settings.m_choice) == "numbers");
        Array<std::string> value_paths;
        visitRecursive(settings, [&](const auto &path, const auto &, auto &) { value_paths.push_back(path); });
        PPR_TEST_ASSERT(value_paths[0u] == "choice" and value_paths[1u] == "choice.numbers");
        PPR_TEST_ASSERT(std::ranges::find(value_paths, "choice.empty") == value_paths.end());
        PPR_TEST_ASSERT(std::ranges::find(value_paths, "choice.nested") == value_paths.end());
        settings.m_choice.reset();
        value_paths.clear();
        visitRecursive(settings, [&](const auto &path, const auto &, auto &) { value_paths.push_back(path); });
        PPR_TEST_ASSERT(value_paths == (Array<std::string>{"choice", "unrelated"}));
        PPR_TEST_ASSERT(variantTag(member, settings.m_choice).empty());
    };

    PPR_UNIT_TEST (variant_schema_rejects_invalid_tags_types_presence_and_defaults) {
        struct InvalidTags {
            std::variant<VariantEmpty, VariantNumbers> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &InvalidTags::m_choice, Metadata{"choice"}, EPresence::required, alternative<VariantEmpty>("tag"),
                    alternative<VariantNumbers>("tag"))};
            }
        };
        struct InvalidSegment {
            std::variant<VariantEmpty> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &InvalidSegment::m_choice, Metadata{"choice"}, EPresence::required,
                    alternative<VariantEmpty>("a.b"))};
            }
        };
        struct MissingCoverage {
            std::variant<VariantEmpty, VariantNumbers> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &MissingCoverage::m_choice, Metadata{"choice"}, EPresence::required,
                    alternative<VariantEmpty>("empty"))};
            }
        };
        struct InvalidEmptyTag {
            std::variant<VariantEmpty> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &InvalidEmptyTag::m_choice, Metadata{"choice"}, EPresence::required,
                    alternative<VariantEmpty>(""))};
            }
        };
        struct InvalidPresence {
            std::variant<VariantEmpty> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &InvalidPresence::m_choice, Metadata{"choice"}, EPresence::optional,
                    alternative<VariantEmpty>("empty"))};
            }
        };
        struct DuplicateTypes {
            std::variant<VariantEmpty, VariantNumbers> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &DuplicateTypes::m_choice, Metadata{"choice"}, EPresence::required, alternative<VariantEmpty>("a"),
                    alternative<VariantEmpty>("b"))};
            }
        };
        struct EmptyDomain {
            std::variant<VariantEmpty> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(&EmptyDomain::m_choice, Metadata{"choice"}, EPresence::required)};
            }
        };
        struct MonostatePayload {
            std::variant<std::monostate, VariantEmpty> m_choice{};
            static constexpr auto describe() {
                return std::tuple{taggedVariant(
                    &MonostatePayload::m_choice, Metadata{"choice"}, EPresence::optional,
                    alternative<std::monostate>("none"), alternative<VariantEmpty>("empty"))};
            }
        };
        struct InvalidDefault : VariantSettings<EPresence::defaulted> {
            InvalidDefault() {
                m_choice = VariantNumbers{};
            }
        };
        const auto check = []<typename S>() {
            Diagnostics diagnostics;
            const auto result = validateSchema<S>(diagnostics);
            PPR_TEST_ASSERT(not result and result.error() == make_error_code(EConfigErrc::schema_invalid));
            PPR_TEST_ASSERT(diagnostics.hasErrors() and diagnostics.entries().front().m_property.starts_with("choice"));
        };
        check.template operator()<InvalidTags>();
        check.template operator()<InvalidSegment>();
        check.template operator()<InvalidEmptyTag>();
        check.template operator()<InvalidPresence>();
        check.template operator()<MissingCoverage>();
        check.template operator()<DuplicateTypes>();
        check.template operator()<EmptyDomain>();
        check.template operator()<MonostatePayload>();
        check.template operator()<InvalidDefault>();
        Diagnostics diagnostics;
        PPR_TEST_ASSERT(validateSchema<VariantSettings<EPresence::required>>(diagnostics).has_value());
        PPR_TEST_ASSERT(validateSchema<VariantSettings<EPresence::defaulted>>(diagnostics).has_value());
    };

    PPR_UNIT_TEST (environment_name_derivation) {
        PPR_TEST_ASSERT(environmentName("display.cap_hz") == "PPR_DISPLAY__CAP_HZ");
        PPR_TEST_ASSERT(environmentName("perf.pacing") == "PPR_PERF__PACING");
        PPR_TEST_ASSERT(environmentName("app.max-fps") == "PPR_APP__MAX_FPS");
    };

    PPR_UNIT_TEST (environment_reads_only_known_names) {
        Array<std::pair<std::string, std::string>> table{};
        table.emplace_back("PPR_PERF__CAP_HZ", "144");
        table.emplace_back("PPR_UNRELATED", "1");

        Array<std::string> queried{};
        auto lookup = [&](const std::string_view name) -> std::optional<std::string> {
            queried.push_back(std::string{name});
            for (const auto &row: table) {
                if (row.first == name) {
                    return row.second;
                }
            }
            return std::nullopt;
        };

        Diagnostics diagnostics{};
        const Array<Assignment> assignments = collectEnvironment<PerfSettings>(lookup, diagnostics);
        PPR_TEST_ASSERT(assignments.size() == 1u);
        PPR_TEST_ASSERT(assignments.front().m_name == "perf.cap_hz");
        PPR_TEST_ASSERT(assignments.front().m_location == "env:PPR_PERF__CAP_HZ");
        for (const auto &name: queried) {
            PPR_TEST_ASSERT(name != "PPR_UNRELATED");
        }

        Array<Assignment> owned{};
        for (const auto &assignment: assignments) {
            owned.push_back(assignment);
        }
        Array<Layer> layers{};
        layers.push_back(environmentLayer(std::move(owned)));
        Diagnostics resolve_diagnostics{};
        const auto result = resolve<PerfSettings>(layers, resolve_diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_cap_hz == 144u);
    };

    PPR_UNIT_TEST (environment_alias_names) {
        Array<std::pair<std::string, std::string>> table{};
        table.emplace_back("PPR_WORKERS", "8");
        auto lookup = [&](const std::string_view name) -> std::optional<std::string> {
            for (const auto &row: table) {
                if (row.first == name) {
                    return row.second;
                }
            }
            return std::nullopt;
        };

        Diagnostics diagnostics{};
        const Array<Assignment> assignments = collectEnvironment<AliasSettings>(lookup, diagnostics);
        PPR_TEST_ASSERT(assignments.size() == 1u);
        PPR_TEST_ASSERT(assignments.front().m_name == "job.workers");

        Diagnostics schema_diagnostics{};
        PPR_TEST_ASSERT(validateEnvironmentSchema<AliasSettings>(schema_diagnostics).has_value());
        Diagnostics perf_schema_diagnostics{};
        PPR_TEST_ASSERT(validateEnvironmentSchema<PerfSettings>(perf_schema_diagnostics).has_value());
    };
} // namespace pP::tests::detail::cfg

namespace pP::tests {
    const UnitTest config = UnitTest::Named("config") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::cfg::defaults_resolve,
            detail::cfg::bounded_diagnostics_preserve_failure_count_and_clear_contract,
            detail::cfg::precedence_across_layers,
            detail::cfg::source_subsets_are_valid,
            detail::cfg::last_assignment_wins_within_layer,
            detail::cfg::unknown_property_is_an_error,
            detail::cfg::integer_text_consumption_and_overflow,
            detail::cfg::file_integer_out_of_typed_range,
            detail::cfg::typed_integer_sources_check_destination_width,
            detail::cfg::typed_floating_sources_check_destination_range,
            detail::cfg::file_string_never_satisfies_integer,
            detail::cfg::float_rejects_garbage_and_non_finite,
            detail::cfg::boolean_token_set,
            detail::cfg::enum_named_domain_only,
            detail::cfg::cross_field_validation_runs_after_merge,
            detail::cfg::shadowed_invalid_values_still_error,
            detail::cfg::source_restrictions_are_enforced,
            detail::cfg::optional_absence_supply_and_reset,
            detail::cfg::runtime_mutability_and_atomicity,
            detail::cfg::runtime_rejects_unknown_properties,
            detail::cfg::sensitive_values_are_redacted,
            detail::cfg::diagnostics_carry_context,
            detail::cfg::schema_validation,
            detail::cfg::required_defaulted_optional_presence,
            detail::cfg::duplicate_and_unknown_keys,
            detail::cfg::recursive_source_order_preserves_collection_and_origin_winners,
            detail::cfg::recursive_prefix_neighbors_preserve_descendant_presence,
            detail::cfg::recursive_group_source_errors_preserve_authored_locations,
            detail::cfg::recursive_invalid_collection_keys_preserve_diagnostic_order,
            detail::cfg::recursive_schema_validation,
            detail::cfg::owner_reference_scope_is_metadata_and_validation_stays_syntax_only,
            detail::cfg::reference_scope_declarations_reject_invalid_targets_and_unknown_scope,
            detail::cfg::recursive_required_group_validates_defaulted_descendants,
            detail::cfg::recursive_absent_optional_validates_defaulted_descendants,
            detail::cfg::recursive_empty_collection_validates_defaulted_element_descendants,
            detail::cfg::variant_nested_resolution_is_order_independent,
            detail::cfg::recursive_owned_results_survive_sources_and_scratch_reuse,
            detail::cfg::variant_replacement_scopes_exclude_prefix_siblings,
            detail::cfg::variant_tag_and_payload_errors_report_logical_paths,
            detail::cfg::variant_required_optional_and_defaulted_presence,
            detail::cfg::variant_layers_replace_same_and_changed_tags_atomically,
            detail::cfg::repeated_nested_replacements_preserve_origin_order_and_sources,
            detail::cfg::variant_replacements_do_not_inherit_and_validate_shadowed_layers,
            detail::cfg::variant_schema_all_and_value_active_preorder_traversal,
            detail::cfg::variant_schema_rejects_invalid_tags_types_presence_and_defaults,
            detail::cfg::environment_name_derivation,
            detail::cfg::environment_reads_only_known_names,
            detail::cfg::environment_alias_names,
        });
    };

    const UnitTest &configTests() noexcept {
        return config;
    }
} // namespace pP::tests
