module;

#include "pP/Macros.h"

export module engine.core:config.file;

import :config;
import :containers.stl;
import :types;
import :utility;

import std;

export namespace pP::config {
    // Format-neutral document: canonical dotted keys plus strictly typed
    // scalar values. No TOML types escape the codec implementation.
    // (Phase 1 has no list-valued settings; arrays are rejected by the
    // codec and left for a future extension of this seam.)
    enum class EFileKind : u8 {
        boolean,
        integer,
        floating,
        text,
    };

    struct FileValue {
        EFileKind m_kind{EFileKind::text};
        bool m_boolean{false};
        i64 m_integer{0};
        double m_floating{0.0};
        std::string m_text{};

        [[nodiscard]] static FileValue boolean(const bool value) noexcept {
            FileValue result{};
            result.m_kind = EFileKind::boolean;
            result.m_boolean = value;
            return result;
        }

        [[nodiscard]] static FileValue integer(const i64 value) noexcept {
            FileValue result{};
            result.m_kind = EFileKind::integer;
            result.m_integer = value;
            return result;
        }

        [[nodiscard]] static FileValue floating(const double value) noexcept {
            FileValue result{};
            result.m_kind = EFileKind::floating;
            result.m_floating = value;
            return result;
        }

        [[nodiscard]] static FileValue text(std::string value) noexcept {
            FileValue result{};
            result.m_kind = EFileKind::text;
            result.m_text = std::move(value);
            return result;
        }
    };

    struct Entry {
        std::string m_key{}; // canonical dotted key ("perf.cap_hz")
        FileValue m_value{};
        std::string m_location{}; // "origin:line"
    };

    struct Document {
        Array<Entry> m_entries{};
    };

    // One codec seam: bytes in, owned document out (and back). Both return
    // expected results with contextual diagnostics. Duplicate keys are
    // errors; "#" comments are supported; I/O stays injected (callers
    // read/write the bytes).
    // Record consumers must reject empty tables and literal dotted segments:
    // flattening either would erase required authoring information.
    enum class EDecodeMode : u8 {
        scalar,
        records
    };

    [[nodiscard]] Expected<Document> decode(
        std::string_view bytes, std::string_view origin, Diagnostics &diagnostics,
        EDecodeMode mode = EDecodeMode::scalar);

    [[nodiscard]] Expected<std::string> encode(const Document &document);

    // Wraps a decoded document as a file precedence layer.
    [[nodiscard]] Layer toLayer(Document document, std::string origin);

    namespace details {
        template<typename Owner, typename M, typename Constraint, typename V>
        [[nodiscard]] std::error_code appendEntry(
            Document &document, const Member<Owner, M, Constraint> &member, const V &value) {
            if constexpr (IsOptional<V>::value) {
                if (value.has_value()) {
                    return appendEntry(document, member, *value);
                }
                return {};
            } else if constexpr (std::is_same_v<V, bool>) {
                document.m_entries.push_back(
                    Entry{std::string{member.m_metadata.m_name}, FileValue::boolean(value), "default"});
                return {};
            } else if constexpr (std::is_integral_v<V>) {
                // TOML integers are signed 64-bit; wider unsigned values
                // cannot round-trip and are reported, never asserted.
                if (not std::in_range<i64>(value)) {
                    return make_error_code(EConfigErrc::out_of_range);
                }
                document.m_entries.push_back(
                    Entry{
                        std::string{member.m_metadata.m_name},
                        FileValue::integer(static_cast<i64>(value)),
                        "default",
                    });
                return {};
            } else if constexpr (std::is_floating_point_v<V>) {
                document.m_entries.push_back(
                    Entry{
                        std::string{member.m_metadata.m_name},
                        FileValue::floating(static_cast<double>(value)),
                        "default",
                    });
                return {};
            } else if constexpr (std::is_same_v<V, std::string>) {
                document.m_entries.push_back(
                    Entry{std::string{member.m_metadata.m_name}, FileValue::text(value), "default"});
                return {};
            } else if constexpr (std::is_enum_v<V>) {
                std::string name{};
                if constexpr (requires { member.m_constraint.nameOf(value); }) {
                    name = std::string{member.m_constraint.nameOf(value)};
                }
                if (name.empty()) {
                    name = configFormat("#{}", static_cast<std::int64_t>(value));
                }
                document.m_entries.push_back(
                    Entry{std::string{member.m_metadata.m_name}, FileValue::text(std::move(name)), "default"});
                return {};
            } else {
                static_assert(
                    always_false_v<V>, "documentFrom supports bool/integer/floating/string/enum/optional only");
            }
        }
    } // namespace details

    // Serializes typed settings to a document (canonical keys + typed
    // values) for writer round-trips and the generated user overlay.
    // Values outside TOML's signed-64-bit integer range fail instead of
    // truncating.
    template<typename Settings>
    [[nodiscard]] Expected<Document> documentFrom(const Settings &settings) {
        Document document{};
        std::error_code failure{};
        visit(settings, [&](const auto &member, const auto &field) {
            if (failure) {
                return;
            }
            failure = details::appendEntry(document, member, field);
        });
        if (failure) {
            return std::unexpected{failure};
        }
        return document;
    }
} // namespace pP::config
