module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;

import std;

namespace pP::tests::detail::cfg_file {
    using namespace pP::config;

    // Full writer round-trips use SingleSettings below.
    struct SingleSettings {
        std::string m_host{"localhost"};
        u32 m_port{80};
        bool m_tls{false};
        double m_timeout{30.0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &SingleSettings::m_host,
                    Metadata{"net.host", "Host", "Net", EMutability::startup, ESource::all, true},
                    StringDomain{128, false}),
                field(
                    &SingleSettings::m_port,
                    Metadata{"net.port", "Port", "Net", EMutability::startup, ESource::all, true},
                    Range<u32>{1u, 65535u}),
                field(
                    &SingleSettings::m_tls, Metadata{"net.tls", "TLS", "Net", EMutability::startup, ESource::all, true},
                    boolean_domain),
                field(
                    &SingleSettings::m_timeout,
                    Metadata{"net.timeout", "Timeout", "Net", EMutability::startup, ESource::all, true},
                    FloatDomain{0.0, 3600.0, false}),
            };
        }
    };

    [[nodiscard]] const Entry *findEntry(const Document &document, const std::string_view key) {
        for (const auto &entry: document.m_entries) {
            if (entry.m_key == key) {
                return std::addressof(entry);
            }
        }
        return nullptr;
    }

    PPR_UNIT_TEST (decode_sections_comments_and_types) {
        constexpr std::string_view bytes = "# leading comment\n"
                                           "\n"
                                           "[net] # section comment\n"
                                           "host = \"example.com\" # trailing comment\n"
                                           "port = 8080\n"
                                           "tls = true\n"
                                           "timeout = 12.5\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "settings.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(document->m_entries.size() == 4u);

        const Entry *host = findEntry(*document, "net.host");
        PPR_TEST_ASSERT(host != nullptr);
        PPR_TEST_ASSERT(host->m_value.m_kind == EFileKind::text);
        PPR_TEST_ASSERT(host->m_value.m_text == "example.com");
        PPR_TEST_ASSERT(host->m_location == "settings.toml:4");

        const Entry *port = findEntry(*document, "net.port");
        PPR_TEST_ASSERT(port != nullptr);
        PPR_TEST_ASSERT(port->m_value.m_kind == EFileKind::integer);
        PPR_TEST_ASSERT(port->m_value.m_integer == 8080);

        const Entry *tls = findEntry(*document, "net.tls");
        PPR_TEST_ASSERT(tls != nullptr);
        PPR_TEST_ASSERT(tls->m_value.m_kind == EFileKind::boolean);
        PPR_TEST_ASSERT(tls->m_value.m_boolean);

        const Entry *timeout = findEntry(*document, "net.timeout");
        PPR_TEST_ASSERT(timeout != nullptr);
        PPR_TEST_ASSERT(timeout->m_value.m_kind == EFileKind::floating);
        PPR_TEST_ASSERT(timeout->m_value.m_floating == 12.5);
    };

    PPR_UNIT_TEST (hash_inside_strings_is_not_a_comment) {
        constexpr std::string_view bytes = "[net]\nhost = \"a#b\"\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "s.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        const Entry *host = findEntry(*document, "net.host");
        PPR_TEST_ASSERT(host != nullptr);
        PPR_TEST_ASSERT(host->m_value.m_text == "a#b");
    };

    PPR_UNIT_TEST (duplicate_keys_are_errors) {
        constexpr std::string_view bytes = "[net]\nport = 1\nport = 2\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "dup.toml", diagnostics);
        PPR_TEST_ASSERT(not document.has_value());
        PPR_TEST_ASSERT(document.error() == make_error_code(EConfigErrc::codec_error));
        PPR_TEST_ASSERT(diagnostics.hasErrors());

        const std::string text = diagnostics.format();
        PPR_TEST_ASSERT(text.find("dup.toml:3") != std::string::npos);
    };

    PPR_UNIT_TEST (dotted_keys_are_relative_to_table) {
        // TOML dotted keys inside a table are relative: "net.port" under
        // "[net]" defines "net.net.port", so this is not a duplicate.
        constexpr std::string_view bytes = "[net]\nport = 1\nnet.port = 2\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "rel.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(document->m_entries.size() == 2u);

        const Entry *port = findEntry(*document, "net.port");
        PPR_TEST_ASSERT(port != nullptr);
        PPR_TEST_ASSERT(port->m_value.m_integer == 1);
        const Entry *nested = findEntry(*document, "net.net.port");
        PPR_TEST_ASSERT(nested != nullptr);
        PPR_TEST_ASSERT(nested->m_value.m_integer == 2);
    };

    PPR_UNIT_TEST (typed_value_integrity) {
        // Quoted numbers stay strings; integers stay integers; booleans
        // stay booleans. The strict conversion boundary (not the codec)
        // rejects cross-kind assignments.
        constexpr std::string_view bytes = "[net]\nport = \"8080\"\ntls = true\ntimeout = 5\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "s.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(findEntry(*document, "net.port")->m_value.m_kind == EFileKind::text);
        PPR_TEST_ASSERT(findEntry(*document, "net.tls")->m_value.m_kind == EFileKind::boolean);
        PPR_TEST_ASSERT(findEntry(*document, "net.timeout")->m_value.m_kind == EFileKind::integer);

        Array<Layer> layers{};
        layers.push_back(toLayer(std::move(*document), "s.toml"));
        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<SingleSettings>(layers, resolve_diagnostics);
        PPR_TEST_ASSERT(not resolved.has_value());
        PPR_TEST_ASSERT(resolved.error() == make_error_code(EConfigErrc::type_mismatch));
    };

    PPR_UNIT_TEST (malformed_input_reports_location) {
        Diagnostics diagnostics{};
        PPR_TEST_ASSERT(not decode("[net\nport = 1\n", "bad.toml", diagnostics).has_value());
        PPR_TEST_ASSERT(diagnostics.hasErrors());
        PPR_TEST_ASSERT(diagnostics.format().find("bad.toml:1") != std::string::npos);

        Diagnostics bare{};
        PPR_TEST_ASSERT(not decode("just some words\n", "bad.toml", bare).has_value());

        Diagnostics unterminated{};
        PPR_TEST_ASSERT(not decode("[net]\nhost = \"abc\n", "bad.toml", unterminated).has_value());
    };

    PPR_UNIT_TEST (unsupported_tables_and_arrays_rejected) {
        Diagnostics array_table{};
        PPR_TEST_ASSERT(not decode("[[net]]\nport = 1\n", "s.toml", array_table).has_value());

        Diagnostics array_value{};
        PPR_TEST_ASSERT(not decode("[net]\nport = [1, 2]\n", "s.toml", array_value).has_value());

        // Inline tables are tables: they flatten into dotted keys.
        Diagnostics inline_table{};
        const auto inlined = decode("[net]\nhost = {a = 1}\n", "s.toml", inline_table);
        PPR_TEST_ASSERT(inlined.has_value());
        const Entry *nested = findEntry(*inlined, "net.host.a");
        PPR_TEST_ASSERT(nested != nullptr);
        PPR_TEST_ASSERT(nested->m_value.m_integer == 1);
    };

    PPR_UNIT_TEST (record_mode_rejects_erased_structure) {
        for (const auto source:
            {"[materials.empty]\n", "[materials.rock]\nunknown = {}\n", "\"materials.rock\" = { id = 16 }\n",
                "\"\" = 16\n"}) {
            Diagnostics scalar_diagnostics{};
            PPR_TEST_ASSERT(decode(source, "legacy.toml", scalar_diagnostics).has_value());
            Diagnostics diagnostics{};
            const auto records = decode(source, "records.toml", diagnostics, EDecodeMode::records);
            PPR_TEST_ASSERT(not records.has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_location.starts_with("records.toml:"));
        }
        Diagnostics diagnostics{};
        const auto valid = decode("materials.rock.id = 16\n", "records.toml", diagnostics, EDecodeMode::records);
        PPR_TEST_ASSERT(valid.has_value());
        PPR_TEST_ASSERT(findEntry(*valid, "materials.rock.id")->m_value.m_integer == 16);
    };

    PPR_UNIT_TEST (strict_toml_numbers_and_keys) {
        for (const std::string_view bad: {"x = 1__0\n", "x = 01\n", "x = \"a\" \"b\"\n", "[t]\n[t]\n"}) {
            Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decode(bad, "s.toml", diagnostics).has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }

        // Spaced dotted keys are valid TOML.
        Diagnostics spaced{};
        const auto document = decode("a . b = 1\n", "s.toml", spaced);
        PPR_TEST_ASSERT(document.has_value());
        const Entry *entry = findEntry(*document, "a.b");
        PPR_TEST_ASSERT(entry != nullptr);
        PPR_TEST_ASSERT(entry->m_value.m_integer == 1);
    };

    PPR_UNIT_TEST (number_forms) {
        constexpr std::string_view bytes = "[n]\n"
                                           "dec = 1_000\n"
                                           "neg = -42\n"
                                           "hex = 0xFF\n"
                                           "oct = 0o17\n"
                                           "bin = 0b101\n"
                                           "flt = 1.5\n"
                                           "exp = 1e3\n"
                                           "big = 9223372036854775807\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "n.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(findEntry(*document, "n.dec")->m_value.m_integer == 1000);
        PPR_TEST_ASSERT(findEntry(*document, "n.neg")->m_value.m_integer == -42);
        PPR_TEST_ASSERT(findEntry(*document, "n.hex")->m_value.m_integer == 255);
        PPR_TEST_ASSERT(findEntry(*document, "n.oct")->m_value.m_integer == 15);
        PPR_TEST_ASSERT(findEntry(*document, "n.bin")->m_value.m_integer == 5);
        PPR_TEST_ASSERT(findEntry(*document, "n.flt")->m_value.m_floating == 1.5);
        PPR_TEST_ASSERT(findEntry(*document, "n.exp")->m_value.m_floating == 1000.0);
        PPR_TEST_ASSERT(findEntry(*document, "n.big")->m_value.m_integer == 9223372036854775807LL);

        Diagnostics overflow{};
        PPR_TEST_ASSERT(not decode("[n]\nv = 9223372036854775808\n", "n.toml", overflow).has_value());
    };

    PPR_UNIT_TEST (string_escapes_and_literals) {
        constexpr std::string_view bytes = "[s]\n"
                                           "a = \"x\\n\\t\\\"q\\\"\\\\\"\n"
                                           "b = 'C:\\path\\n'\n"
                                           "c = \"\\u00E9\"\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "s.toml", diagnostics);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(findEntry(*document, "s.a")->m_value.m_text == "x\n\t\"q\"\\");
        PPR_TEST_ASSERT(findEntry(*document, "s.b")->m_value.m_text == "C:\\path\\n");
        PPR_TEST_ASSERT(findEntry(*document, "s.c")->m_value.m_text == "\u00E9");
    };

    PPR_UNIT_TEST (encode_decode_round_trip) {
        Document document{};
        document.m_entries.push_back(Entry{"net.host", FileValue::text("example.com"), "t:1"});
        document.m_entries.push_back(Entry{"net.port", FileValue::integer(8080), "t:2"});
        document.m_entries.push_back(Entry{"net.tls", FileValue::boolean(true), "t:3"});
        document.m_entries.push_back(Entry{"net.timeout", FileValue::floating(12.5), "t:4"});
        document.m_entries.push_back(Entry{"top", FileValue::text("a\nb\"q\""), "t:5"});

        const auto bytes = encode(document);
        PPR_TEST_ASSERT(bytes.has_value());

        Diagnostics diagnostics{};
        const auto decoded = decode(*bytes, "round.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(decoded->m_entries.size() == document.m_entries.size());
        for (const auto &entry: document.m_entries) {
            const Entry *back = findEntry(*decoded, entry.m_key);
            PPR_TEST_ASSERT(back != nullptr);
            PPR_TEST_ASSERT(back->m_value.m_kind == entry.m_value.m_kind);
            if (entry.m_value.m_kind == EFileKind::text) {
                PPR_TEST_ASSERT(back->m_value.m_text == entry.m_value.m_text);
            } else if (entry.m_value.m_kind == EFileKind::integer) {
                PPR_TEST_ASSERT(back->m_value.m_integer == entry.m_value.m_integer);
            } else if (entry.m_value.m_kind == EFileKind::boolean) {
                PPR_TEST_ASSERT(back->m_value.m_boolean == entry.m_value.m_boolean);
            } else {
                PPR_TEST_ASSERT(back->m_value.m_floating == entry.m_value.m_floating);
            }
        }

        // Encoding a document with duplicate keys is a writer error.
        Document dup = document;
        dup.m_entries.push_back(Entry{"net.port", FileValue::integer(1), "t:9"});
        PPR_TEST_ASSERT(not encode(dup).has_value());
    };

    PPR_UNIT_TEST (control_characters_round_trip) {
        std::string text{};
        for (char code = 0; code < 0x20; ++code) {
            text += code;
        }
        text += '\x7F';
        text += "\"\\\u00E9";

        Document document{};
        document.m_entries.push_back(Entry{"text", FileValue::text(text), "t:1"});
        const auto bytes = encode(document);
        PPR_TEST_ASSERT(bytes.has_value());
        PPR_TEST_ASSERT(bytes->find("\\u0001") != std::string::npos);
        PPR_TEST_ASSERT(bytes->find("\\u007F") != std::string::npos);

        Diagnostics diagnostics{};
        const auto decoded = decode(*bytes, "controls.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        const Entry *back = findEntry(*decoded, "text");
        PPR_TEST_ASSERT(back != nullptr);
        PPR_TEST_ASSERT(back->m_value.m_kind == EFileKind::text);
        PPR_TEST_ASSERT(back->m_value.m_text == text);
    };

    PPR_UNIT_TEST (high_precision_doubles_round_trip) {
        const std::array values{
            1.2345678901234567, std::nextafter(1.0, 2.0), 1.2345678901234567e-100, -1.2345678901234567e100,
            std::numeric_limits<double>::min(), std::numeric_limits<double>::max(), 1.0, 0.0, -0.0};
        for (const double value: values) {
            Document document{};
            document.m_entries.push_back(Entry{"number", FileValue::floating(value), "t:1"});
            const auto bytes = encode(document);
            PPR_TEST_ASSERT(bytes.has_value());

            Diagnostics diagnostics{};
            const auto decoded = decode(*bytes, "precision.toml", diagnostics);
            PPR_TEST_ASSERT(decoded.has_value());
            PPR_TEST_ASSERT(not diagnostics.hasErrors());
            const Entry *back = findEntry(*decoded, "number");
            PPR_TEST_ASSERT(back != nullptr);
            PPR_TEST_ASSERT(back->m_value.m_kind == EFileKind::floating);
            PPR_TEST_ASSERT(back->m_value.m_floating == value);
            PPR_TEST_ASSERT(std::signbit(back->m_value.m_floating) == std::signbit(value));
        }
    };

    // A custom global locale is process-wide: isolate it from other test leaves.
    PPR_UNIT_TEST (numeric_encoding_is_locale_independent, UnitTest::fork) {
        struct NumericPunctuation final : std::numpunct<char> {
            char do_decimal_point() const override {
                return ',';
            }
            char do_thousands_sep() const override {
                return '_';
            }
            std::string do_grouping() const override {
                return "\3";
            }
        };
        struct RestoreLocale {
            std::locale m_previous{std::locale()};
            ~RestoreLocale() {
                std::locale::global(m_previous);
            }
        } restore{};

        Document document{};
        document.m_entries.push_back(Entry{"integer", FileValue::integer(123456789), "t:1"});
        document.m_entries.push_back(Entry{"floating", FileValue::floating(1234.5678901234567), "t:2"});
        std::locale::global(std::locale::classic());
        const auto classic = encode(document);
        PPR_TEST_ASSERT(classic.has_value());

        // The locale takes ownership of the facet; no installed OS locale is required.
        std::locale::global(std::locale(std::locale::classic(), std::make_unique<NumericPunctuation>().release()));
        const auto localized = encode(document);
        PPR_TEST_ASSERT(localized.has_value());
        PPR_TEST_ASSERT(*localized == *classic);

        Diagnostics diagnostics{};
        const auto decoded = decode(*localized, "locale.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        const Entry *integer = findEntry(*decoded, "integer");
        const Entry *floating = findEntry(*decoded, "floating");
        PPR_TEST_ASSERT(integer != nullptr);
        PPR_TEST_ASSERT(floating != nullptr);
        PPR_TEST_ASSERT(integer->m_value.m_kind == EFileKind::integer);
        PPR_TEST_ASSERT(floating->m_value.m_kind == EFileKind::floating);
        PPR_TEST_ASSERT(integer->m_value.m_integer == 123456789);
        PPR_TEST_ASSERT(floating->m_value.m_floating == 1234.5678901234567);
    };

    PPR_UNIT_TEST (non_finite_round_trip) {
        Document document{};
        document.m_entries.push_back(
            Entry{"n.inf", FileValue::floating(std::numeric_limits<double>::infinity()), "t:1"});
        document.m_entries.push_back(
            Entry{"n.nan", FileValue::floating(std::numeric_limits<double>::quiet_NaN()), "t:2"});

        const auto bytes = encode(document);
        PPR_TEST_ASSERT(bytes.has_value());

        Diagnostics diagnostics{};
        const auto decoded = decode(*bytes, "n.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(not std::isfinite(findEntry(*decoded, "n.inf")->m_value.m_floating));
        PPR_TEST_ASSERT(std::isnan(findEntry(*decoded, "n.nan")->m_value.m_floating));
    };

    PPR_UNIT_TEST (settings_writer_round_trip) {
        SingleSettings settings{};
        settings.m_host = "example.com";
        settings.m_port = 8080;
        settings.m_tls = true;
        settings.m_timeout = 12.5;

        const auto serialized = documentFrom(settings);
        PPR_TEST_ASSERT(serialized.has_value());
        const auto bytes = encode(*serialized);
        PPR_TEST_ASSERT(bytes.has_value());

        Diagnostics decode_diagnostics{};
        auto decoded = decode(*bytes, "round.toml", decode_diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());

        Array<Layer> layers{};
        layers.push_back(toLayer(std::move(*decoded), "round.toml"));
        Diagnostics resolve_diagnostics{};
        const auto resolved = resolve<SingleSettings>(layers, resolve_diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_host == "example.com");
        PPR_TEST_ASSERT(resolved->m_value.m_port == 8080u);
        PPR_TEST_ASSERT(resolved->m_value.m_tls);
        PPR_TEST_ASSERT(resolved->m_value.m_timeout == 12.5);
    };

    struct OptionalSettings {
        std::optional<i32> m_seed{};
        std::optional<std::string> m_name{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &OptionalSettings::m_seed,
                    Metadata{"opt.seed", "Seed", "T", EMutability::startup, ESource::all, false},
                    Range<i32>{-1000, 1000}),
                field(
                    &OptionalSettings::m_name,
                    Metadata{"opt.name", "Name", "T", EMutability::startup, ESource::all, false},
                    StringDomain{32, true}),
            };
        }
    };

    struct WideSettings {
        u64 m_big{0};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                field(
                    &WideSettings::m_big, Metadata{"opt.big", "Big", "T", EMutability::startup, ESource::all, false},
                    any_domain),
            };
        }
    };

    PPR_UNIT_TEST (optional_serialization) {
        const OptionalSettings disengaged{};
        const auto empty = documentFrom(disengaged);
        PPR_TEST_ASSERT(empty.has_value());
        PPR_TEST_ASSERT(empty->m_entries.empty());

        OptionalSettings engaged{};
        engaged.m_seed = 42;
        engaged.m_name = std::string{"ada"};
        const auto filled = documentFrom(engaged);
        PPR_TEST_ASSERT(filled.has_value());
        PPR_TEST_ASSERT(filled->m_entries.size() == 2u);

        const Entry *seed = findEntry(*filled, "opt.seed");
        PPR_TEST_ASSERT(seed != nullptr);
        PPR_TEST_ASSERT(seed->m_value.m_kind == EFileKind::integer);
        PPR_TEST_ASSERT(seed->m_value.m_integer == 42);
        const Entry *name = findEntry(*filled, "opt.name");
        PPR_TEST_ASSERT(name != nullptr);
        PPR_TEST_ASSERT(name->m_value.m_kind == EFileKind::text);
        PPR_TEST_ASSERT(name->m_value.m_text == "ada");
    };

    PPR_UNIT_TEST (large_unsigned_serialization) {
        WideSettings settings{};
        settings.m_big = std::numeric_limits<u64>::max();
        const auto wide = documentFrom(settings);
        PPR_TEST_ASSERT(not wide.has_value());
        PPR_TEST_ASSERT(wide.error() == make_error_code(EConfigErrc::out_of_range));

        settings.m_big = static_cast<u64>(std::numeric_limits<i64>::max());
        const auto narrow = documentFrom(settings);
        PPR_TEST_ASSERT(narrow.has_value());
        PPR_TEST_ASSERT(narrow->m_entries.size() == 1u);
        PPR_TEST_ASSERT(findEntry(*narrow, "opt.big")->m_value.m_integer == std::numeric_limits<i64>::max());
    };
} // namespace pP::tests::detail::cfg_file

namespace pP::tests {
    const UnitTest config_file = UnitTest::Named("config_file") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::cfg_file::decode_sections_comments_and_types,
            detail::cfg_file::hash_inside_strings_is_not_a_comment,
            detail::cfg_file::duplicate_keys_are_errors,
            detail::cfg_file::dotted_keys_are_relative_to_table,
            detail::cfg_file::typed_value_integrity,
            detail::cfg_file::malformed_input_reports_location,
            detail::cfg_file::unsupported_tables_and_arrays_rejected,
            detail::cfg_file::strict_toml_numbers_and_keys,
            detail::cfg_file::record_mode_rejects_erased_structure,
            detail::cfg_file::number_forms,
            detail::cfg_file::string_escapes_and_literals,
            detail::cfg_file::encode_decode_round_trip,
            detail::cfg_file::control_characters_round_trip,
            detail::cfg_file::high_precision_doubles_round_trip,
            detail::cfg_file::numeric_encoding_is_locale_independent,
            detail::cfg_file::non_finite_round_trip,
            detail::cfg_file::settings_writer_round_trip,
            detail::cfg_file::optional_serialization,
            detail::cfg_file::large_unsigned_serialization,
        });
    };

    const UnitTest &configFileTests() noexcept {
        return config_file;
    }
} // namespace pP::tests
