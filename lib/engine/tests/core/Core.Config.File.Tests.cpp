module;

#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;

import std;

namespace pP::tests::detail::cfg_file {
    // ------------------------------------------------------------------
    // config file
    // ------------------------------------------------------------------

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

    PPR_UNIT_TEST (flattened_key_collisions_preserve_duplicate_diagnostics) {
        // Both keys are legal TOML, but scalar flattening gives them the same path.
        // The nested table sorts before the literal dotted key, so line 1 is the duplicate.
        constexpr std::string_view bytes = "\"a.b\" = 1\n[a]\nb = 2\n";

        Diagnostics diagnostics{};
        const auto document = decode(bytes, "collision.toml", diagnostics);
        PPR_TEST_ASSERT(not document.has_value());
        PPR_TEST_ASSERT(document.error() == make_error_code(EConfigErrc::duplicate_key));
        PPR_TEST_ASSERT(diagnostics.size() == 1u);

        const std::string text = diagnostics.format();
        PPR_TEST_ASSERT(text.find("collision.toml:1") != std::string::npos);
        PPR_TEST_ASSERT(text.find("duplicate key 'a.b'") != std::string::npos);
    };

    PPR_UNIT_TEST (many_unique_short_and_long_keys_preserve_values_and_order) {
        constexpr u32 count = 256u;
        const std::string long_prefix = "z" + std::string(128u, 'x');
        const auto key = [&](const bool long_key, const u32 index) {
            return std::format("{}{:04}", long_key ? std::string_view{long_prefix} : std::string_view{"k"}, index);
        };

        std::string bytes;
        for (u32 remaining = count; remaining > 0u; --remaining) {
            const u32 index = remaining - 1u;
            bytes += std::format("{} = {}\n", key(true, index), count + index);
            bytes += std::format("{} = {}\n", key(false, index), index);
        }

        for (const auto mode: {EDecodeMode::scalar, EDecodeMode::records, EDecodeMode::tree}) {
            Diagnostics diagnostics{};
            const auto document = decode(bytes, "unique.toml", diagnostics, mode);
            PPR_TEST_ASSERT(document.has_value());
            PPR_TEST_ASSERT(not diagnostics.hasErrors());
            PPR_TEST_ASSERT(document->m_entries.size() == count * 2u);

            for (u32 index = 0; index < count; ++index) {
                const Entry &short_entry = document->m_entries[index];
                PPR_TEST_ASSERT(short_entry.m_key == key(false, index));
                PPR_TEST_ASSERT(short_entry.m_value.m_kind == EFileKind::integer);
                PPR_TEST_ASSERT(short_entry.m_value.m_integer == static_cast<i64>(index));

                const Entry &long_entry = document->m_entries[count + index];
                PPR_TEST_ASSERT(long_entry.m_key == key(true, index));
                PPR_TEST_ASSERT(long_entry.m_value.m_kind == EFileKind::integer);
                PPR_TEST_ASSERT(long_entry.m_value.m_integer == static_cast<i64>(count + index));
            }

            PPR_TEST_ASSERT(document->m_entries.front().m_location == "unique.toml:512");
            PPR_TEST_ASSERT(document->m_entries[count].m_location == "unique.toml:511");
        }
    };

    PPR_UNIT_TEST (decoded_document_owns_long_keys_values_and_locations_after_scratch_reuse) {
        const std::string key(128u, 'k');
        const std::string value(1024u, 'v');
        const std::string origin = "an_owned_configuration_source_location_that_outlives_parsing.toml";
        Diagnostics diagnostics;
        const auto document = [&]() {
            const std::string bytes = key + " = \"" + value + "\"\n";
            return decode(bytes, origin, diagnostics);
        }();
        PPR_TEST_ASSERT(document.has_value() and not diagnostics.hasErrors());

        {
            const auto scratch_scope = mem::ScratchPad::open();
            Array<std::byte, mem::ScratchPad> overwrite(1024u * 1024u, std::byte{0x79});
            PPR_TEST_ASSERT(overwrite.back() == std::byte{0x79});
        }

        PPR_TEST_ASSERT(document->m_entries.size() == 1u);
        PPR_TEST_ASSERT(document->m_entries.front().m_key == key);
        PPR_TEST_ASSERT(document->m_entries.front().m_value.m_text == value);
        PPR_TEST_ASSERT(document->m_entries.front().m_location == origin + ":1");
    };

    PPR_UNIT_TEST (scalar_patch_preserves_original_decode_failures_and_diagnostic_order) {
        for (const std::string_view bytes: {"[a\nb = 1\n", "\"a.b\" = 1\n[a]\nb = 2\n", "[a]\nb = [1, 2]\n"}) {
            Diagnostics decode_diagnostics;
            const auto decoded = decode(bytes, "invalid-patch.toml", decode_diagnostics);
            PPR_TEST_ASSERT(not decoded.has_value());

            Diagnostics patch_diagnostics;
            const auto patch = patchScalar(
                bytes, "a.b", FileValue::integer(3), "invalid-patch.toml", patch_diagnostics);
            PPR_TEST_ASSERT(not patch.has_value() and patch.error() == decoded.error());
            PPR_TEST_ASSERT(patch_diagnostics.format() == decode_diagnostics.format());
        }
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

    PPR_UNIT_TEST (source_patch_preserves_utf8_comments_crlf_and_quoted_values) {
        const std::string source = "\xEF\xBB\xBF# untouched\r\n[net]\r\n"
                                   "inline = { \"\xC3\xA9\xF0\x9F\x8C\x8D\" = 'unchanged', port = 8_080 } # tail\r\n"
                                   "host = 'old # text' # retain this\r\n";
        Diagnostics diagnostics;
        auto patch = patchScalar(source, "net.inline.port", FileValue::integer(9000), "source.toml", diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        PPR_TEST_ASSERT(patch->m_before == "8_080");
        auto changed = applySourcePatch(source, *patch);
        PPR_TEST_ASSERT(changed.has_value());
        auto expected = source;
        expected.replace(expected.find("8_080"), 5u, "9000");
        PPR_TEST_ASSERT(*changed == expected);
        const auto restored = applySourcePatch(*changed, *patch, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);

        auto string_patch = patchScalar(
            *changed, "net.host", FileValue::text("quote \" slash \\ newline\n"), "source.toml", diagnostics);
        PPR_TEST_ASSERT(string_patch.has_value() and string_patch->m_before == "'old # text'");
        auto string_changed = applySourcePatch(*changed, *string_patch);
        PPR_TEST_ASSERT(string_changed.has_value());
        PPR_TEST_ASSERT(string_changed->ends_with("\"quote \\\" slash \\\\ newline\\n\" # retain this\r\n"));
        auto decoded = decode(*string_changed, "source.toml", diagnostics);
        PPR_TEST_ASSERT(decoded.has_value());
        PPR_TEST_ASSERT(findEntry(*decoded, "net.host")->m_value.m_text == "quote \" slash \\ newline\n");
    };

    PPR_UNIT_TEST (source_patch_insertion_preserves_headers_final_comments_and_newline_style) {
        for (const std::string source:
            {"[net] # header\r\nport = 8080 # final", "[net] # header\nport = 8080 # final\n",
                "[net] # final header"}) {
            Diagnostics diagnostics;
            const auto patch = patchScalar(source, "net.tls", FileValue::boolean(true), "source.toml", diagnostics);
            PPR_TEST_ASSERT(patch.has_value() and patch->m_before.empty());
            const auto changed = applySourcePatch(source, *patch);
            PPR_TEST_ASSERT(changed.has_value());
            const auto restored = applySourcePatch(*changed, *patch, true);
            PPR_TEST_ASSERT(restored.has_value() and *restored == source);
            auto expected = source;
            const auto newline = source.find('\n');
            if (newline == std::string::npos) {
                expected += "\ntls = true";
            } else {
                expected.insert(
                    newline + 1u, source.find("\r\n") == std::string::npos ? "tls = true\n" : "tls = true\r\n");
            }
            PPR_TEST_ASSERT(*changed == expected);
        }
    };

    PPR_UNIT_TEST (source_patch_rejects_missing_implicit_parents_type_changes_and_conflicts) {
        Diagnostics diagnostics;
        constexpr std::string_view source = "[net.inner]\nport = 8080\n";
        PPR_TEST_ASSERT(not patchScalar(source, "missing.port", FileValue::integer(9), "source", diagnostics));
        PPR_TEST_ASSERT(not patchScalar(source, "net.extra", FileValue::integer(9), "source", diagnostics));
        PPR_TEST_ASSERT(not patchScalar(source, "net..port", FileValue::integer(9), "source", diagnostics));
        PPR_TEST_ASSERT(not patchScalar(source, "net.inner.port", FileValue::text("9"), "source", diagnostics));
        PPR_TEST_ASSERT(
            not patchScalar("net.port = 1\n", "net.extra", FileValue::boolean(true), "source", diagnostics));
        PPR_TEST_ASSERT(
            not patchScalar("net = { port = 1 }\n", "net.extra", FileValue::boolean(true), "source", diagnostics));
        auto patch = patchScalar(source, "net.inner.port", FileValue::integer(9), "source", diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        const auto conflict = applySourcePatch("[net.inner]\nport = 8081\n", *patch);
        PPR_TEST_ASSERT(not conflict and conflict.error() == std::errc::invalid_argument);
        const std::string oversized(1024u * 1024u + 1u, ' ');
        PPR_TEST_ASSERT(not patchScalar(oversized, "x", FileValue::integer(9), "source", diagnostics));
    };

    PPR_UNIT_TEST (record_source_patches_preserve_interleaved_tables_comments_and_undo) {
        const std::string source =
            "\xEF\xBB\xBFschema_version = 2\r\n# keep header\r\n"
            "[records.first]\r\nvalue = '''multiline\r\n[not.a.header]\r\ntext''' # own value\r\n"
            "# keep between tables\r\n[records.other]\r\nvalue = 7 # keep unrelated\r\n"
            "[records.first.child]\r\nname = 'caf\xc3\xa9'\r\n";
        Diagnostics diagnostics;
        const auto removal = removeRecord(source, "records.first", "source.toml", diagnostics);
        PPR_TEST_ASSERT(removal.has_value());
        const auto removed = applySourcePatch(source, *removal);
        PPR_TEST_ASSERT(removed.has_value());
        PPR_TEST_ASSERT(
            *removed == "\xEF\xBB\xBFschema_version = 2\r\n# keep header\r\n"
                        "# keep between tables\r\n[records.other]\r\nvalue = 7 # keep unrelated\r\n");
        const auto restored = applySourcePatch(*removed, *removal, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);
        Document record;
        record.m_entries.push_back({"value", FileValue::integer(8), {}});
        record.m_entries.push_back({"child.name", FileValue::text("new"), {}});
        const auto insertion = insertRecord(source, "records.new", record, "source.toml", diagnostics);
        PPR_TEST_ASSERT(insertion.has_value());
        const auto inserted = applySourcePatch(source, *insertion);
        PPR_TEST_ASSERT(inserted.has_value() and inserted->starts_with(source));
        PPR_TEST_ASSERT(inserted->find("[records.new.child]\r\nname = \"new\"\r\n") != std::string::npos);
        const auto undone = applySourcePatch(*inserted, *insertion, true);
        PPR_TEST_ASSERT(undone.has_value() and *undone == source);
        PPR_TEST_ASSERT(not insertRecord(source, "records.first", record, "source.toml", diagnostics));
        PPR_TEST_ASSERT(not removeRecord(source, "records.missing", "source.toml", diagnostics));
        PPR_TEST_ASSERT(not removeRecord(
            "records = { first = { value = 1 }, other = { value = 2 } }\n", "records.first", "source.toml",
            diagnostics));
    };

    PPR_UNIT_TEST (schema_property_projection_preserves_domains_hints_choices_and_optionality) {
        struct Settings {
            enum class Sampling : u8 {
                point,
                smooth
            };
            std::optional<double> m_frequency{1.0e20};
            std::optional<std::string> m_source{};
            Sampling m_sampling{};
            std::optional<Sampling> m_alternate{};
            i32 m_offset{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &Settings::m_frequency,
                        Metadata{
                            "frequency", "Noise frequency", "Noise", EMutability::startup, ESource::file, true, false,
                            {}, "1/m", 0.0, 0.1},
                        FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}),
                    field(
                        &Settings::m_source,
                        Metadata{
                            "source", "Source field", "Noise", EMutability::startup, ESource::file, true, false,
                            "fields"},
                        StringDomain{128u, false}),
                    field(
                        &Settings::m_sampling, Metadata{"sampling", "Sampling", "Noise"},
                        EnumDomain<Sampling, 2u>{
                            {EnumEntry<Sampling>{Sampling::point, "point"},
                                EnumEntry<Sampling>{Sampling::smooth, "smooth"}}}),
                    field(
                        &Settings::m_alternate, Metadata{"alternate", "Alternate sampling", "Noise"},
                        EnumDomain<Sampling, 2u>{
                            {EnumEntry<Sampling>{Sampling::point, "nearest"},
                                EnumEntry<Sampling>{Sampling::smooth, "linear"}}}),
                    field(&Settings::m_offset, Metadata{"offset", "Offset", "Noise"}, Range<i32>{-8, 8})};
            }
        } settings;
        const auto schema = Settings::describe();
        const auto frequency = describeProperty(std::get<0u>(schema));
        PPR_TEST_ASSERT(frequency.m_min == 0.0 and frequency.m_max == (std::numeric_limits<double>::max)());
        PPR_TEST_ASSERT(frequency.m_optional and frequency.m_exclusive_min and not frequency.m_exclusive_max);
        PPR_TEST_ASSERT(frequency.m_slider_min > 0.0 and frequency.m_slider_max == 0.1);
        PPR_TEST_ASSERT(frequency.m_description == "Noise frequency" and frequency.m_units == "1/m");
        PPR_TEST_ASSERT(settings.m_frequency == 1.0e20);
        const auto source = describeProperty(std::get<1u>(schema));
        PPR_TEST_ASSERT(source.m_optional and source.m_reference == "fields");
        const auto sampling = describeProperty(std::get<2u>(schema));
        const auto alternate = describeProperty(std::get<3u>(schema));
        PPR_TEST_ASSERT(not sampling.m_optional and alternate.m_optional);
        PPR_TEST_ASSERT(sampling.m_choices == (Array<std::string_view>{"point", "smooth"}));
        PPR_TEST_ASSERT(alternate.m_choices == (Array<std::string_view>{"nearest", "linear"}));
        const auto offset = describeProperty(std::get<4u>(schema));
        PPR_TEST_ASSERT(offset.m_min == -8.0 and offset.m_max == 8.0 and not offset.m_optional);
    };

    PPR_UNIT_TEST (scalar_removal_preserves_comments_crlf_siblings_and_reversible_proof) {
        const std::string source =
            "# keep header\r\n[settings]\r\n# keep explanation\r\noptional = 2.0 # own assignment\r\n"
            "sibling = 7 # keep sibling\r\n[settings.child]\r\nvalue = 'keep child'\r\n";
        Diagnostics diagnostics;
        const auto patch = removeRecord(source, "settings.optional", "optional.toml", diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        const auto removed = applySourcePatch(source, *patch);
        PPR_TEST_ASSERT(removed.has_value());
        PPR_TEST_ASSERT(
            *removed == "# keep header\r\n[settings]\r\n# keep explanation\r\nsibling = 7 # keep sibling\r\n"
                        "[settings.child]\r\nvalue = 'keep child'\r\n");
        const auto decoded = decode(*removed, "optional.toml", diagnostics, EDecodeMode::records);
        PPR_TEST_ASSERT(decoded.has_value() and findEntry(*decoded, "settings.optional") == nullptr);
        const auto *const sibling = findEntry(*decoded, "settings.sibling");
        const auto *const child = findEntry(*decoded, "settings.child.value");
        PPR_TEST_ASSERT(sibling != nullptr and sibling->m_value.m_integer == 7);
        PPR_TEST_ASSERT(child != nullptr and child->m_value.m_text == "keep child");
        const auto restored = applySourcePatch(*removed, *patch, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);
        auto changed = source;
        changed.replace(changed.find("optional = 2.0"), std::string_view{"optional = 2.0"}.size(), "optional = 3.0");
        const auto conflict = applySourcePatch(changed, *patch);
        PPR_TEST_ASSERT(not conflict and conflict.error() == std::errc::invalid_argument);
        const auto inline_removal = removeRecord(
            "settings = { optional = 2.0, sibling = 7 }\r\n", "settings.optional", "inline.toml", diagnostics);
        PPR_TEST_ASSERT(not inline_removal);
    };

    PPR_UNIT_TEST (tree_source_insert_qualifies_descendants_and_empty_groups) {
        for (const std::string source:
            {"\xEF\xBB\xBF# caf\xc3\xa9\r\n[unrelated.empty]\r\n# final", "# untouched\n[unrelated.empty]\n"}) {
            Document subtree;
            subtree.m_entries.push_back({"value", FileValue::integer(8), {}});
            subtree.m_entries.push_back({"child.name", FileValue::text("new"), {}});
            subtree.m_groups.push_back({"child", {}});
            subtree.m_groups.push_back({"child.empty", {}});
            Diagnostics diagnostics;
            const auto patch = patchTree(
                source, "records.new", ETreeSourceEdit::insert, subtree, "tree.toml", diagnostics);
            PPR_TEST_ASSERT(patch.has_value() and patch->m_offset == source.size() and patch->m_before.empty());
            const auto changed = applySourcePatch(source, *patch);
            PPR_TEST_ASSERT(changed.has_value() and changed->starts_with(source));
            const auto document = decode(*changed, "tree.toml", diagnostics, EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(findEntry(*document, "records.new.value")->m_value.m_integer == 8);
            PPR_TEST_ASSERT(findEntry(*document, "records.new.child.name")->m_value.m_text == "new");
            for (const auto path: {"unrelated.empty", "records.new", "records.new.child", "records.new.child.empty"}) {
                PPR_TEST_ASSERT(
                    std::ranges::find(document->m_groups, path, &GroupPresence::m_path) != document->m_groups.end());
            }
            if (source.find("\r\n") != std::string::npos) {
                for (std::size_t i = source.size(); i < changed->size(); ++i) {
                    PPR_TEST_ASSERT((*changed)[i] != '\n' or (i != 0u and (*changed)[i - 1u] == '\r'));
                }
            }
            const auto restored = applySourcePatch(*changed, *patch, true);
            PPR_TEST_ASSERT(restored.has_value() and *restored == source);
        }

        Diagnostics diagnostics;
        const auto empty = patchTree("# keep", "empty", ETreeSourceEdit::insert, Document{}, "empty.toml", diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        const auto changed = applySourcePatch("# keep", *empty);
        PPR_TEST_ASSERT(changed.has_value() and changed->starts_with("# keep\n"));
        const auto decoded = decode(*changed, "empty.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value() and decoded->m_entries.empty() and decoded->m_groups.size() == 1u);
        PPR_TEST_ASSERT(decoded->m_groups.front().m_path == "empty");
        // Record APIs keep their stricter records-mode contract.
        PPR_TEST_ASSERT(not insertRecord("", "empty", Document{}, "empty.toml", diagnostics));
        PPR_TEST_ASSERT(not removeRecord("[empty]\n", "empty", "empty.toml", diagnostics));
    };

    PPR_UNIT_TEST (tree_source_remove_preserves_interleaved_bytes_comments_and_group_presence) {
        const std::string source = "\xEF\xBB\xBF# keep header\r\n[records]\r\n[records.first]\r\n"
                                   "value = '''multiline\r\n[not.a.header]\r\ntext''' # own value\r\n"
                                   "# keep between tables\r\n[records.other.empty]\r\n"
                                   "[records.first.child]\r\nname = 'caf\xc3\xa9'\r\n"
                                   "[unrelated]\r\nvalue = 7 # final";
        Diagnostics diagnostics;
        const auto patch = patchTree(
            source, "records.first", ETreeSourceEdit::remove, Document{}, "tree.toml", diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        const auto changed = applySourcePatch(source, *patch);
        PPR_TEST_ASSERT(changed.has_value());
        PPR_TEST_ASSERT(
            *changed == "\xEF\xBB\xBF# keep header\r\n[records]\r\n"
                        "# keep between tables\r\n[records.other.empty]\r\n"
                        "[unrelated]\r\nvalue = 7 # final");
        const auto decoded = decode(*changed, "tree.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(findEntry(*decoded, "unrelated.value")->m_value.m_integer == 7);
        for (const auto path: {"records", "records.other", "records.other.empty", "unrelated"}) {
            PPR_TEST_ASSERT(
                std::ranges::find(decoded->m_groups, path, &GroupPresence::m_path) != decoded->m_groups.end());
        }
        PPR_TEST_ASSERT(std::ranges::none_of(decoded->m_groups, [](const auto &group) {
            return group.m_path == "records.first" or group.m_path.starts_with("records.first.");
        }));
        const auto restored = applySourcePatch(*changed, *patch, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);

        const auto empty = patchTree(
            "[empty]\n# keep\n[other]\n", "empty", ETreeSourceEdit::remove, Document{}, "empty.toml", diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        const auto removed = applySourcePatch("[empty]\n# keep\n[other]\n", *empty);
        PPR_TEST_ASSERT(removed.has_value() and *removed == "# keep\n[other]\n");
    };

    PPR_UNIT_TEST (tree_source_removal_retains_implicit_ancestors_and_bom) {
        Diagnostics diagnostics;
        constexpr std::string_view source = "\xEF\xBB\xBF# keep\r\n[outer.records.only.empty]\r\n# final";
        const auto patch = patchTree(
            source, "outer.records.only", ETreeSourceEdit::remove, Document{}, "implicit.toml", diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        const auto changed = applySourcePatch(source, *patch);
        PPR_TEST_ASSERT(changed.has_value() and changed->starts_with("\xEF\xBB\xBF# keep\r\n# final"));
        const auto decoded = decode(*changed, "implicit.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value() and decoded->m_entries.empty() and decoded->m_groups.size() == 2u);
        PPR_TEST_ASSERT(
            std::ranges::find(decoded->m_groups, "outer", &GroupPresence::m_path) != decoded->m_groups.end());
        PPR_TEST_ASSERT(
            std::ranges::find(decoded->m_groups, "outer.records", &GroupPresence::m_path) != decoded->m_groups.end());
        const auto restored = applySourcePatch(*changed, *patch, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);

        constexpr std::string_view scalar_source = "\xEF\xBB\xBFvalue = 1\n# keep\n[empty]\n";
        const auto scalar = patchTree(
            scalar_source, "value", ETreeSourceEdit::remove, Document{}, "bom.toml", diagnostics);
        PPR_TEST_ASSERT(scalar.has_value());
        const auto scalar_removed = applySourcePatch(scalar_source, *scalar);
        PPR_TEST_ASSERT(scalar_removed.has_value() and *scalar_removed == "\xEF\xBB\xBF# keep\n[empty]\n");
    };

    PPR_UNIT_TEST (tree_source_replace_is_one_atomic_reversible_revision_checked_patch) {
        const std::string source = "# keep\n[choice]\ntag = 'old'\n[other.empty]\n# between\n"
                                   "[choice.old]\nvalue = 3\n# final\n";
        Document subtree;
        subtree.m_entries.push_back({"tag", FileValue::text("new"), {}});
        subtree.m_groups.push_back({"new", {}});
        Diagnostics diagnostics;
        const auto patch = patchTree(source, "choice", ETreeSourceEdit::replace, subtree, "replace.toml", diagnostics);
        PPR_TEST_ASSERT(patch.has_value() and not patch->m_before.empty() and not patch->m_after.empty());
        const auto changed = applySourcePatch(source, *patch);
        PPR_TEST_ASSERT(changed.has_value() and changed->starts_with("# keep\n[other.empty]\n# between\n# final\n"));
        const auto document = decode(*changed, "replace.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(document.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(findEntry(*document, "choice.tag")->m_value.m_text == "new");
        PPR_TEST_ASSERT(findEntry(*document, "choice.old.value") == nullptr);
        PPR_TEST_ASSERT(
            std::ranges::find(document->m_groups, "choice.new", &GroupPresence::m_path) != document->m_groups.end());
        PPR_TEST_ASSERT(
            std::ranges::find(document->m_groups, "other.empty", &GroupPresence::m_path) != document->m_groups.end());
        const auto restored = applySourcePatch(*changed, *patch, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);

        auto stale = source;
        stale.replace(stale.find("value = 3"), 9u, "value = 4");
        const auto conflict = applySourcePatch(stale, *patch);
        PPR_TEST_ASSERT(not conflict and conflict.error() == std::errc::invalid_argument);
        auto invalid = subtree;
        invalid.m_entries.push_back({"new", FileValue::integer(1), {}});
        const auto failed = patchTree(source, "choice", ETreeSourceEdit::replace, invalid, "replace.toml", diagnostics);
        PPR_TEST_ASSERT(not failed and failed.error() == make_error_code(EConfigErrc::type_mismatch));
        const auto original = decode(source, "replace.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(original.has_value() and findEntry(*original, "choice.old.value")->m_value.m_integer == 3);

        const auto empty = patchTree(
            source, "choice", ETreeSourceEdit::replace, Document{}, "replace.toml", diagnostics);
        PPR_TEST_ASSERT(empty.has_value());
        const auto emptied = applySourcePatch(source, *empty);
        PPR_TEST_ASSERT(emptied.has_value());
        const auto empty_tree = decode(*emptied, "replace.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(empty_tree.has_value() and findEntry(*empty_tree, "choice.tag") == nullptr);
        PPR_TEST_ASSERT(
            std::ranges::find(empty_tree->m_groups, "choice", &GroupPresence::m_path) != empty_tree->m_groups.end());
    };

    PPR_UNIT_TEST (tree_source_edits_reject_collisions_invalid_structure_and_inline_removal) {
        Document subtree;
        subtree.m_entries.push_back({"value", FileValue::integer(2), {}});
        for (const auto source: {"target = 1\n", "[target]\n", "[target.child]\n"}) {
            Diagnostics diagnostics;
            const auto duplicate = patchTree(
                source, "target", ETreeSourceEdit::insert, subtree, "collision.toml", diagnostics);
            PPR_TEST_ASSERT(not duplicate and duplicate.error() == make_error_code(EConfigErrc::duplicate_key));
            PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "target");
            PPR_TEST_ASSERT(diagnostics.entries().back().m_location == "collision.toml");
        }
        Diagnostics diagnostics;
        const auto ancestor = patchTree(
            "target = 1\n", "target.child", ETreeSourceEdit::insert, subtree, "collision.toml", diagnostics);
        PPR_TEST_ASSERT(not ancestor and ancestor.error() == make_error_code(EConfigErrc::type_mismatch));
        for (const auto edit: {ETreeSourceEdit::remove, ETreeSourceEdit::replace}) {
            const auto absent = patchTree("[other]\n", "target", edit, Document{}, "missing.toml", diagnostics);
            PPR_TEST_ASSERT(not absent and absent.error() == make_error_code(EConfigErrc::unknown_property));
            for (const auto key: {"target", "target.child", "target.child.value"}) {
                const auto inlined = patchTree(
                    "target = { child = { value = 1 } }\n", key, edit, Document{}, "inline.toml", diagnostics);
                PPR_TEST_ASSERT(not inlined and inlined.error() == std::errc::operation_not_supported);
            }
        }
        PPR_TEST_ASSERT(
            not patchTree("[target]\n", "target", ETreeSourceEdit::remove, subtree, "invalid.toml", diagnostics));
        Document group_only;
        group_only.m_groups.push_back({"child", {}});
        PPR_TEST_ASSERT(
            not patchTree("[target]\n", "target", ETreeSourceEdit::remove, group_only, "invalid.toml", diagnostics));
        PPR_TEST_ASSERT(
            not patchTree("", "bad..key", ETreeSourceEdit::insert, Document{}, "invalid.toml", diagnostics));
        PPR_TEST_ASSERT(
            not patchTree("", "target", static_cast<ETreeSourceEdit>(255u), Document{}, "invalid.toml", diagnostics));
        for (const auto source: {"\"a.b\" = 1\n", "values = [1]\n", "[broken\n"}) {
            PPR_TEST_ASSERT(
                not patchTree(source, "target", ETreeSourceEdit::insert, subtree, "invalid.toml", diagnostics));
        }

        Document scalar_table = subtree;
        scalar_table.m_groups.push_back({"value", {}});
        const auto collision = patchTree(
            "", "target", ETreeSourceEdit::insert, scalar_table, "collision.toml", diagnostics);
        PPR_TEST_ASSERT(not collision and collision.error() == make_error_code(EConfigErrc::type_mismatch));
        Document scalar_ancestor = subtree;
        scalar_ancestor.m_entries.push_back({"value.child", FileValue::integer(1), {}});
        const auto nested = patchTree(
            "", "target", ETreeSourceEdit::insert, scalar_ancestor, "collision.toml", diagnostics);
        PPR_TEST_ASSERT(not nested and nested.error() == make_error_code(EConfigErrc::type_mismatch));
        group_only.m_groups.push_back({"child", {}});
        const auto duplicate_group = patchTree(
            "", "target", ETreeSourceEdit::insert, group_only, "collision.toml", diagnostics);
        PPR_TEST_ASSERT(not duplicate_group and duplicate_group.error() == make_error_code(EConfigErrc::duplicate_key));
        Document bad_path;
        bad_path.m_groups.push_back({"bad..path", {}});
        PPR_TEST_ASSERT(not patchTree("", "target", ETreeSourceEdit::insert, bad_path, "invalid.toml", diagnostics));
    };

    PPR_UNIT_TEST (tree_source_edits_enforce_revision_safe_size_limits) {
        constexpr std::size_t limit = 1024u * 1024u;
        const std::string tail = "[target]\nvalue = '" + std::string(512u, 'x') + "'\n";
        // Keep parser source columns bounded as well as document bytes.
        std::string source;
        const std::string comment = "#" + std::string(62u, ' ') + "\n";
        while (source.size() + comment.size() <= limit - tail.size() - 2u) {
            source += comment;
        }
        source += "#" + std::string(limit - tail.size() - source.size() - 2u, ' ') + "\n" + tail;
        PPR_TEST_ASSERT(source.size() == limit);
        Diagnostics diagnostics;
        const auto growth = patchTree(source, "extra", ETreeSourceEdit::insert, Document{}, "limit.toml", diagnostics);
        PPR_TEST_ASSERT(not growth and growth.error() == make_error_code(EConfigErrc::out_of_range));
        const auto oversized = patchTree(
            source + " ", "target", ETreeSourceEdit::remove, Document{}, "limit.toml", diagnostics);
        PPR_TEST_ASSERT(not oversized and oversized.error() == make_error_code(EConfigErrc::out_of_range));
        const auto shrinking = patchTree(
            source, "target", ETreeSourceEdit::replace, Document{}, "limit.toml", diagnostics);
        PPR_TEST_ASSERT(shrinking.has_value());
        const auto changed = applySourcePatch(source, *shrinking);
        PPR_TEST_ASSERT(changed.has_value() and changed->size() < limit);
        const auto restored = applySourcePatch(*changed, *shrinking, true);
        PPR_TEST_ASSERT(restored.has_value() and *restored == source);
        auto stale = source;
        stale[stale.find("value = '") + 9u] = 'y';
        const auto conflict = applySourcePatch(stale, *shrinking);
        PPR_TEST_ASSERT(not conflict and conflict.error() == std::errc::invalid_argument);
        Document huge;
        huge.m_entries.push_back({"text", FileValue::text(std::string(limit, 'x')), {}});
        const auto too_large = patchTree("", "target", ETreeSourceEdit::insert, huge, "limit.toml", diagnostics);
        PPR_TEST_ASSERT(not too_large and too_large.error() == make_error_code(EConfigErrc::out_of_range));
        const auto reverse_stale = applySourcePatch(source, *shrinking, true);
        PPR_TEST_ASSERT(not reverse_stale and reverse_stale.error() == std::errc::invalid_argument);
    };

    struct FileReference {
        std::string m_key{};
        static constexpr auto describe() {
            return std::tuple{field(
                &FileReference::m_key, Metadata{"key", {}, {}, EMutability::startup, ESource::file},
                StringDomain{128u, false}, EPresence::required)};
        }
    };
    struct ReferenceWriterSettings {
        FileReference m_target{"stone"};
        std::optional<FileReference> m_optional{};
        static constexpr auto describe() {
            constexpr ReferenceDomain<FileReference> reference{{"records", "usable", EReferenceScope::owner}};
            return std::tuple{
                field(
                    &ReferenceWriterSettings::m_target, Metadata{"target", {}, {}, EMutability::startup, ESource::file},
                    reference, EPresence::required),
                field(
                    &ReferenceWriterSettings::m_optional,
                    Metadata{"optional", {}, {}, EMutability::startup, ESource::file}, reference, EPresence::optional)};
        }
    };
    PPR_UNIT_TEST (reference_writer_exports_scalar_keys_and_round_trips_optional_presence) {
        for (const bool present: {false, true}) {
            ReferenceWriterSettings settings;
            if (present) {
                settings.m_optional = FileReference{"ore"};
            }
            const auto document = documentFrom(settings);
            PPR_TEST_ASSERT(document.has_value() and document->m_groups.empty());
            PPR_TEST_ASSERT(document->m_entries.size() == (present ? 2u : 1u));
            PPR_TEST_ASSERT(findEntry(*document, "target")->m_value.m_kind == EFileKind::text);
            PPR_TEST_ASSERT(findEntry(*document, "target")->m_value.m_text == "stone");
            PPR_TEST_ASSERT(findEntry(*document, "target.key") == nullptr);

            const auto bytes = encode(*document);
            PPR_TEST_ASSERT(bytes.has_value());
            Diagnostics diagnostics;
            const auto decoded = decode(*bytes, "reference.toml", diagnostics, EDecodeMode::tree);
            PPR_TEST_ASSERT(decoded.has_value());
            const auto layer = toTreeLayer(*decoded, "reference.toml");
            const auto resolved = resolveRecursive<ReferenceWriterSettings>(std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
            PPR_TEST_ASSERT(resolved->m_value.m_target.m_key == "stone");
            PPR_TEST_ASSERT(resolved->m_value.m_optional.has_value() == present);
            if (present) {
                PPR_TEST_ASSERT(resolved->m_value.m_optional->m_key == "ore");
            }
        }
    };

    struct TreeRecord {
        u32 m_value{7u};
        static constexpr auto describe() {
            return std::tuple{field(
                &TreeRecord::m_value, Metadata{"value", "Value", "Tree", EMutability::startup, ESource::file},
                Range<u32>{0u, 9u})};
        }
    };

    struct TreeSettings {
        u32 m_scalar{1u};
        std::optional<TreeRecord> m_empty{};
        KeyedCollection<TreeRecord> m_records{};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &TreeSettings::m_scalar, Metadata{"scalar", "Scalar", "Tree", EMutability::startup, ESource::file},
                    Range<u32>{0u, 9u}),
                group(
                    &TreeSettings::m_empty,
                    Metadata{"empty", "Empty group", "Tree", EMutability::startup, ESource::file}, EPresence::optional),
                collection(
                    &TreeSettings::m_records,
                    Metadata{"records", "Records", "Tree", EMutability::startup, ESource::file}, {0u, 8u})};
        }
    };

    PPR_UNIT_TEST (tree_decode_encode_round_trip) {
        constexpr std::string_view source = "scalar = 3\n[empty]\n[records.rock]\n";
        Diagnostics diagnostics;
        const auto document = decode(source, "tree.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(document.has_value() and not diagnostics.hasErrors());
        const auto empty = std::ranges::find(document->m_groups, "empty", &GroupPresence::m_path);
        PPR_TEST_ASSERT(empty != document->m_groups.end() and empty->m_location == "tree.toml:2");
        PPR_TEST_ASSERT(
            std::ranges::find(document->m_groups, "records.rock", &GroupPresence::m_path) != document->m_groups.end());

        const auto layer = toTreeLayer(*document, "tree.toml");
        PPR_TEST_ASSERT(layer.m_scalars.m_source == ESource::file and layer.m_scalars.m_origin == "tree.toml");
        const auto resolved = resolveRecursive<TreeSettings>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(resolved.has_value());
        PPR_TEST_ASSERT(resolved->m_value.m_empty and resolved->m_value.m_empty->m_value == 7u);
        PPR_TEST_ASSERT(resolved->m_value.m_records.m_entries.size() == 1u);
        PPR_TEST_ASSERT(resolved->find("empty") != nullptr and resolved->find("empty")->m_source == ESource::file);
        PPR_TEST_ASSERT(resolved->find("empty")->m_location == "tree.toml:2");
        PPR_TEST_ASSERT(resolved->find("scalar")->m_location == "tree.toml:1");

        const auto encoded = encode(*document);
        PPR_TEST_ASSERT(encoded.has_value() and encoded->find("[empty]") != std::string::npos);
        PPR_TEST_ASSERT(encoded->find("[records.rock]") != std::string::npos);
        const auto decoded = decode(*encoded, "round-trip.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value());
        const auto round_trip_layer = toTreeLayer(*decoded, "round-trip.toml");
        const auto round_trip = resolveRecursive<TreeSettings>(std::span{&round_trip_layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(round_trip.has_value() and round_trip->m_value.m_empty.has_value());
        PPR_TEST_ASSERT(round_trip->m_value.m_scalar == 3u and round_trip->m_value.m_records.m_entries.size() == 1u);

        const auto scalar = decode("[net]\nport = 8080\n", "scalar.toml", diagnostics);
        PPR_TEST_ASSERT(scalar.has_value() and scalar->m_groups.empty());
        const auto scalar_encoded = encode(*scalar);
        PPR_TEST_ASSERT(scalar_encoded.has_value());
        const auto scalar_decoded = decode(*scalar_encoded, "scalar-round-trip.toml", diagnostics);
        PPR_TEST_ASSERT(scalar_decoded.has_value() and scalar_decoded->m_groups.empty());
        PPR_TEST_ASSERT(findEntry(*scalar_decoded, "net.port")->m_value.m_integer == 8080);
    };

    PPR_UNIT_TEST (key_syntax_edges) {
        for (const auto &key: {std::string{"a"}, std::string{"az09_-"}, std::string(128u, 'z')}) {
            PPR_TEST_ASSERT(isValidKey(key));
            Diagnostics diagnostics;
            const auto document = decode("[records.\"" + key + "\"]\n", "keys.toml", diagnostics, EDecodeMode::tree);
            PPR_TEST_ASSERT(document.has_value());
            const auto layer = toTreeLayer(*document, "keys.toml");
            const auto resolved = resolveRecursive<TreeSettings>(std::span{&layer, 1u}, diagnostics);
            PPR_TEST_ASSERT(resolved.has_value() and resolved->m_value.m_records.m_entries.front().m_key == key);
        }

        for (const auto &key: {std::string{}, std::string{"a.b"}, std::string{"Rock"}, std::string(129u, 'z')}) {
            PPR_TEST_ASSERT(not isValidKey(key));
            Diagnostics diagnostics;
            const auto document = decode("[records.\"" + key + "\"]\n", "keys.toml", diagnostics, EDecodeMode::tree);
            if (document) {
                const auto layer = toTreeLayer(*document, "keys.toml");
                PPR_TEST_ASSERT(not resolveRecursive<TreeSettings>(std::span{&layer, 1u}, diagnostics));
            }
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }

        Diagnostics duplicates;
        PPR_TEST_ASSERT(
            not decode("[records.rock]\n[records.rock]\n", "duplicate.toml", duplicates, EDecodeMode::tree));
        PPR_TEST_ASSERT(duplicates.hasErrors());
        PPR_TEST_ASSERT(duplicates.format().find("duplicate.toml:2") != std::string::npos);
    };

    struct FileVariantEmpty {
        static constexpr auto describe() {
            return std::tuple{};
        }
    };

    struct FileVariantValue {
        u32 m_value{2u};
        std::optional<TreeRecord> m_child{};
        static constexpr auto describe() {
            return std::tuple{
                field(
                    &FileVariantValue::m_value, Metadata{"value", {}, {}, EMutability::startup, ESource::file},
                    Range<u32>{1u, 9u}, EPresence::required),
                group(
                    &FileVariantValue::m_child, Metadata{"child", {}, {}, EMutability::startup, ESource::file},
                    EPresence::optional)};
        }
    };

    struct FileVariantNested {
        std::variant<FileVariantEmpty, FileVariantValue> m_inner{FileVariantValue{5u, TreeRecord{7u}}};
        static constexpr auto describe() {
            return std::tuple{taggedVariant(
                &FileVariantNested::m_inner, Metadata{"inner", {}, {}, EMutability::startup, ESource::file},
                EPresence::required, alternative<FileVariantEmpty>("empty"), alternative<FileVariantValue>("value"))};
        }
    };

    template<bool Reverse = false>
    struct FileVariantSettings {
        using Payload = std::variant<FileVariantEmpty, FileVariantValue, FileVariantNested>;
        Payload m_choice{FileVariantNested{}};
        std::optional<Payload> m_absent{};
        std::optional<Payload> m_present{Payload{FileVariantEmpty{}}};
        Payload m_defaulted{FileVariantValue{3u}};
        static constexpr auto describe() {
            const auto choice = [] {
                if constexpr (Reverse) {
                    return taggedVariant(
                        &FileVariantSettings::m_choice, Metadata{"choice", {}, {}, EMutability::startup, ESource::file},
                        EPresence::required, alternative<FileVariantNested>("nested"),
                        alternative<FileVariantValue>("value"), alternative<FileVariantEmpty>("empty"));
                } else {
                    return taggedVariant(
                        &FileVariantSettings::m_choice, Metadata{"choice", {}, {}, EMutability::startup, ESource::file},
                        EPresence::required, alternative<FileVariantEmpty>("empty"),
                        alternative<FileVariantValue>("value"), alternative<FileVariantNested>("nested"));
                }
            }();
            return std::tuple{
                choice,
                taggedVariant(
                    &FileVariantSettings::m_absent, Metadata{"absent", {}, {}, EMutability::startup, ESource::file},
                    EPresence::optional, alternative<FileVariantEmpty>("empty"), alternative<FileVariantValue>("value"),
                    alternative<FileVariantNested>("nested")),
                taggedVariant(
                    &FileVariantSettings::m_present, Metadata{"present", {}, {}, EMutability::startup, ESource::file},
                    EPresence::optional, alternative<FileVariantEmpty>("empty"), alternative<FileVariantValue>("value"),
                    alternative<FileVariantNested>("nested")),
                taggedVariant(
                    &FileVariantSettings::m_defaulted,
                    Metadata{"defaulted", {}, {}, EMutability::startup, ESource::file}, EPresence::defaulted,
                    alternative<FileVariantEmpty>("empty"), alternative<FileVariantValue>("value"),
                    alternative<FileVariantNested>("nested"))};
        }
    };

    PPR_UNIT_TEST (variant_document_exports_active_payload_and_explicit_empty_groups) {
        const FileVariantSettings<> settings;
        const auto document = documentFrom(settings);
        PPR_TEST_ASSERT(document.has_value());
        PPR_TEST_ASSERT(findEntry(*document, "choice.tag")->m_value.m_text == "nested");
        PPR_TEST_ASSERT(findEntry(*document, "choice.nested.inner.tag")->m_value.m_text == "value");
        PPR_TEST_ASSERT(findEntry(*document, "choice.nested.inner.value.value")->m_value.m_integer == 5);
        PPR_TEST_ASSERT(findEntry(*document, "present.tag")->m_value.m_text == "empty");
        PPR_TEST_ASSERT(findEntry(*document, "absent.tag") == nullptr);
        PPR_TEST_ASSERT(findEntry(*document, "choice.value.value") == nullptr);
        PPR_TEST_ASSERT(findEntry(*document, "choice.empty.tag") == nullptr);
        PPR_TEST_ASSERT(
            std::ranges::find(document->m_groups, "present.empty", &GroupPresence::m_path) != document->m_groups.end());
        PPR_TEST_ASSERT(std::ranges::none_of(document->m_groups, [](const auto &group) {
            return group.m_path == "absent" or group.m_path.starts_with("absent.") or group.m_path == "choice.empty";
        }));
        const auto encoded = encode(*document);
        PPR_TEST_ASSERT(encoded.has_value() and encoded->find("[present.empty]") != std::string::npos);
        Diagnostics diagnostics;
        const auto decoded = decode(*encoded, "variant-export.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value());
        const auto layer = toTreeLayer(*decoded, "variant-export.toml");
        const auto resolved = resolveRecursive<FileVariantSettings<>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(resolved.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(not resolved->m_value.m_absent and resolved->m_value.m_present);
        PPR_TEST_ASSERT(std::holds_alternative<FileVariantEmpty>(*resolved->m_value.m_present));
        const auto &inner = std::get<FileVariantValue>(std::get<FileVariantNested>(resolved->m_value.m_choice).m_inner);
        PPR_TEST_ASSERT(inner.m_value == 5u and inner.m_child and inner.m_child->m_value == 7u);
        const auto exported = documentFrom(resolved->m_value);
        PPR_TEST_ASSERT(exported.has_value());
        const auto reencoded = encode(*exported);
        PPR_TEST_ASSERT(reencoded.has_value() and *reencoded == *encoded);
    };

    PPR_UNIT_TEST (variant_export_decode_is_stable_under_document_and_schema_reordering) {
        auto document = documentFrom(FileVariantSettings<>{});
        PPR_TEST_ASSERT(document.has_value());
        const auto original = encode(*document);
        PPR_TEST_ASSERT(original.has_value());
        std::ranges::reverse(document->m_entries);
        std::ranges::reverse(document->m_groups);
        auto layer = toTreeLayer(*document, "reordered.toml");
        Diagnostics diagnostics;
        const auto reordered = resolveRecursive<FileVariantSettings<true>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value() and not diagnostics.hasErrors());
        const auto exported = documentFrom(reordered->m_value);
        PPR_TEST_ASSERT(exported.has_value());
        const auto encoded = encode(*exported);
        PPR_TEST_ASSERT(encoded.has_value() and *encoded == *original);

        const auto decoded = decode(*encoded, "reordered-round-trip.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(decoded.has_value());
        layer = toTreeLayer(*decoded, "reordered-round-trip.toml");
        const auto restored = resolveRecursive<FileVariantSettings<>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(restored.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(
            std::get<FileVariantValue>(std::get<FileVariantNested>(restored->m_value.m_choice).m_inner).m_value == 5u);
    };

    PPR_UNIT_TEST (variant_file_presence_and_duplicate_paths_survive_tree_decode) {
        constexpr std::string_view source =
            "[choice]\ntag = 'empty'\n[choice.empty]\n[present]\ntag = 'empty'\n[present.empty]\n";
        Diagnostics diagnostics;
        const auto document = decode(source, "presence.toml", diagnostics, EDecodeMode::tree);
        PPR_TEST_ASSERT(document.has_value());
        const auto layer = toTreeLayer(*document, "presence.toml");
        const auto result = resolveRecursive<FileVariantSettings<>>(std::span{&layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(result.has_value() and not result->m_value.m_absent and result->m_value.m_present);
        PPR_TEST_ASSERT(std::get<FileVariantValue>(result->m_value.m_defaulted).m_value == 3u);
        PPR_TEST_ASSERT(result->find("present.empty") != nullptr);

        const auto duplicate = decode(
            "[choice]\ntag = 'empty'\ntag = 'empty'\n[choice.empty]\n", "duplicate-variant.toml", diagnostics,
            EDecodeMode::tree);
        // Parser-level TOML duplicates retain the existing codec error contract.
        PPR_TEST_ASSERT(not duplicate and duplicate.error() == make_error_code(EConfigErrc::codec_error));
        PPR_TEST_ASSERT(diagnostics.format().find("duplicate-variant.toml:3") != std::string::npos);
        auto repeated = *document;
        repeated.m_entries.push_back(*findEntry(repeated, "choice.tag"));
        const auto repeated_layer = toTreeLayer(std::move(repeated), "duplicate-document");
        diagnostics.clear();
        const auto repeated_result = resolveRecursive<FileVariantSettings<>>(
            std::span{&repeated_layer, 1u}, diagnostics);
        PPR_TEST_ASSERT(not repeated_result and repeated_result.error() == make_error_code(EConfigErrc::duplicate_key));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "choice.tag");
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
            detail::cfg_file::flattened_key_collisions_preserve_duplicate_diagnostics,
            detail::cfg_file::many_unique_short_and_long_keys_preserve_values_and_order,
            detail::cfg_file::decoded_document_owns_long_keys_values_and_locations_after_scratch_reuse,
            detail::cfg_file::scalar_patch_preserves_original_decode_failures_and_diagnostic_order,
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
            detail::cfg_file::reference_writer_exports_scalar_keys_and_round_trips_optional_presence,
            detail::cfg_file::tree_decode_encode_round_trip,
            detail::cfg_file::variant_document_exports_active_payload_and_explicit_empty_groups,
            detail::cfg_file::variant_export_decode_is_stable_under_document_and_schema_reordering,
            detail::cfg_file::variant_file_presence_and_duplicate_paths_survive_tree_decode,
            detail::cfg_file::key_syntax_edges,
            detail::cfg_file::source_patch_preserves_utf8_comments_crlf_and_quoted_values,
            detail::cfg_file::source_patch_insertion_preserves_headers_final_comments_and_newline_style,
            detail::cfg_file::source_patch_rejects_missing_implicit_parents_type_changes_and_conflicts,
            detail::cfg_file::record_source_patches_preserve_interleaved_tables_comments_and_undo,
            detail::cfg_file::schema_property_projection_preserves_domains_hints_choices_and_optionality,
            detail::cfg_file::scalar_removal_preserves_comments_crlf_siblings_and_reversible_proof,
            detail::cfg_file::tree_source_insert_qualifies_descendants_and_empty_groups,
            detail::cfg_file::tree_source_remove_preserves_interleaved_bytes_comments_and_group_presence,
            detail::cfg_file::tree_source_removal_retains_implicit_ancestors_and_bom,
            detail::cfg_file::tree_source_replace_is_one_atomic_reversible_revision_checked_patch,
            detail::cfg_file::tree_source_edits_reject_collisions_invalid_structure_and_inline_removal,
            detail::cfg_file::tree_source_edits_enforce_revision_safe_size_limits,
        });
    };

    const UnitTest &configFileTests() noexcept {
        return config_file;
    }
} // namespace pP::tests
