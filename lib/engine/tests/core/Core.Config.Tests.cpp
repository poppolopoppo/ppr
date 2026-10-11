module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;

import std;

namespace pP::tests::detail::cfg {
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
            detail::cfg::environment_name_derivation,
            detail::cfg::environment_reads_only_known_names,
            detail::cfg::environment_alias_names,
        });
    };

    const UnitTest &configTests() noexcept {
        return config;
    }
} // namespace pP::tests
