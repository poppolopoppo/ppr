module;

#include "pP/Macros.h"

module engine.core;

import :config;

import std;

namespace pP::config {
    class ConfigErrorCategory final : public std::error_category {
    public:
        [[nodiscard]] const char *name() const noexcept override {
            return "pP.config";
        }

        [[nodiscard]] std::string message(const int ev) const override {
            switch (static_cast<EConfigErrc>(ev)) {
                case EConfigErrc::unknown_property: return "unknown property";
                case EConfigErrc::unknown_option: return "unknown option";
                case EConfigErrc::source_disallowed: return "source not allowed for this property";
                case EConfigErrc::type_mismatch: return "value type does not match the property type";
                case EConfigErrc::invalid_integer: return "invalid integer value";
                case EConfigErrc::invalid_float: return "invalid floating-point value";
                case EConfigErrc::invalid_boolean: return "invalid boolean value";
                case EConfigErrc::unknown_enum_value: return "unknown enum value";
                case EConfigErrc::out_of_range: return "value out of range";
                case EConfigErrc::non_finite_not_allowed: return "non-finite value not allowed";
                case EConfigErrc::duplicate_key: return "duplicate key";
                case EConfigErrc::missing_value: return "missing required value";
                case EConfigErrc::unexpected_value: return "unexpected value";
                case EConfigErrc::conflicting_command: return "conflicting command";
                case EConfigErrc::cross_field_invalid: return "invalid combination of properties";
                case EConfigErrc::schema_invalid: return "invalid schema";
                case EConfigErrc::codec_error: return "configuration codec error";
                default: return configFormat("unknown config result ({})", ev);
            }
        }

        [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
            switch (static_cast<EConfigErrc>(ev)) {
                case EConfigErrc::unknown_property:
                case EConfigErrc::unknown_option: return std::errc::invalid_argument;
                case EConfigErrc::missing_value: return std::errc::invalid_argument;
                case EConfigErrc::unexpected_value: return std::errc::invalid_argument;
                case EConfigErrc::invalid_integer:
                case EConfigErrc::invalid_float:
                case EConfigErrc::invalid_boolean: return std::errc::invalid_argument;
                case EConfigErrc::out_of_range: return std::errc::result_out_of_range;
                default: return {ev, *this};
            }
        }
    };

    static const ConfigErrorCategory g_config_error_category{};

    [[nodiscard]] const std::error_category &configCategory() noexcept {
        return g_config_error_category;
    }

    [[nodiscard]] std::error_code make_error_code(const EConfigErrc err) noexcept {
        return std::error_code{static_cast<int>(err), g_config_error_category};
    }

    [[nodiscard]] std::string_view sourceName(const ESource source) noexcept {
        switch (source) {
            case ESource::none: return "default";
            case ESource::file: return "file";
            case ESource::environment: return "environment";
            case ESource::cli: return "cli";
            case ESource::runtime: return "runtime";
            default: break;
        }
        // Combined masks (e.g. ESource::all) have no single name.
        return "mixed";
    }

    [[nodiscard]] bool isValidCanonicalName(const std::string_view name) noexcept {
        if (name.empty()) {
            return false;
        }
        bool last_was_dot = true;
        for (const char ch: name) {
            if (ch == '.') {
                if (last_was_dot) {
                    return false;
                }
                last_was_dot = true;
            } else if ((ch >= 'a' and ch <= 'z') or
                       (ch >= 'A' and ch <= 'Z') or
                       (ch >= '0' and ch <= '9') or
                       ch == '_' or
                       ch == '-') {
                last_was_dot = false;
            } else {
                return false;
            }
        }
        return not last_was_dot;
    }

    [[nodiscard]] std::string AnyDomain::describe() const {
        return "any";
    }

    [[nodiscard]] std::string BooleanDomain::describe() const {
        return "boolean";
    }

    [[nodiscard]] std::error_code StringDomain::validate(const std::string &value) const noexcept {
        if (not m_allow_empty and value.empty()) {
            return make_error_code(EConfigErrc::out_of_range);
        }
        if (value.size() > m_max_length) {
            return make_error_code(EConfigErrc::out_of_range);
        }
        return {};
    }

    [[nodiscard]] std::string StringDomain::describe() const {
        if (m_allow_empty) {
            return configFormat("string (max {} chars)", m_max_length);
        }
        return configFormat("non-empty string (max {} chars)", m_max_length);
    }

    [[nodiscard]] std::error_code PathDomain::validate(const std::string &value) const noexcept {
        if (not m_allow_empty and value.empty()) {
            return make_error_code(EConfigErrc::out_of_range);
        }
        if (value.size() > m_max_length) {
            return make_error_code(EConfigErrc::out_of_range);
        }
        return {};
    }

    [[nodiscard]] std::string PathDomain::describe() const {
        if (m_allow_empty) {
            return configFormat("path (max {} chars)", m_max_length);
        }
        return configFormat("non-empty path (max {} chars)", m_max_length);
    }

    [[nodiscard]] std::error_code FloatDomain::validate(const double &value) const noexcept {
        if (not m_allow_non_finite and not std::isfinite(value)) {
            return make_error_code(EConfigErrc::non_finite_not_allowed);
        }
        if (std::isfinite(value) and (value < m_min or (m_exclusive_min and value == m_min) or value > m_max)) {
            return make_error_code(EConfigErrc::out_of_range);
        }
        return {};
    }

    [[nodiscard]] std::string FloatDomain::describe() const {
        if (m_exclusive_min) {
            return configFormat("number in ({}, {}]", m_min, m_max);
        }
        return configFormat("number in [{}, {}]", m_min, m_max);
    }

    [[nodiscard]] std::string SourceValue::describe(const bool redact) const {
        if (redact) {
            return "<redacted>";
        }
        switch (m_kind) {
            case ESourceKind::boolean: return m_boolean ? "true" : "false";
            case ESourceKind::integer: return configFormat("{}", m_integer);
            case ESourceKind::floating: return configFormat("{}", m_floating);
            case ESourceKind::text: return configFormat("\"{}\"", m_text);
            case ESourceKind::reset: return "<reset>";
            default: return "<unknown>";
        }
    }

    Diagnostics::Diagnostics(const std::size_t max_entries) noexcept : m_max_entries(max_entries) {
    }

    void Diagnostics::add(
        std::string location, std::string property, std::string message, std::error_code code, std::string alias) {
        addOmitted(1u);
        if (m_entries.size() >= m_max_entries) {
            return;
        }
        m_entries.push_back(
            Diagnostic{
                std::move(location),
                std::move(property),
                std::move(alias),
                std::move(message),
                code,
            });
    }

    void Diagnostics::addOmitted(const std::size_t count) noexcept {
        const auto available = (std::numeric_limits<std::size_t>::max)() - m_total_count;
        m_total_count += std::min(count, available);
    }

    [[nodiscard]] std::string Diagnostics::format() const {
        std::string result{};
        for (const auto &entry: m_entries) {
            if (not entry.m_location.empty()) {
                result += entry.m_location;
                result += ": ";
            }
            if (not entry.m_property.empty()) {
                result += entry.m_property;
                result += ": ";
            }
            result += entry.m_message;
            if (not entry.m_alias.empty()) {
                result += configFormat(" (as '{}')", entry.m_alias);
            }
            if (entry.m_code) {
                result += configFormat(" [{}]", entry.m_code.message());
            }
            result += '\n';
        }
        if (omittedCount() != 0u) {
            result += configFormat("{} further diagnostics omitted (record limit reached).\n", omittedCount());
        }
        return result;
    }

    namespace details {
        [[nodiscard]] std::string_view trim(const std::string_view text) noexcept {
            std::size_t begin = 0;
            while (begin < text.size() and
                   (text[begin] == ' ' or text[begin] == '\t' or text[begin] == '\n' or text[begin] == '\r')) {
                ++begin;
            }
            std::size_t end = text.size();
            while (end > begin and
                   (text[end - 1] == ' ' or text[end - 1] == '\t' or text[end - 1] == '\n' or text[end - 1] == '\r')) {
                --end;
            }
            return text.substr(begin, end - begin);
        }

        [[nodiscard]] bool iequals(const std::string_view lhs, const std::string_view rhs) noexcept {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                char l = lhs[i];
                char r = rhs[i];
                if (l >= 'A' and l <= 'Z') {
                    l = static_cast<char>(l + ('a' - 'A'));
                }
                if (r >= 'A' and r <= 'Z') {
                    r = static_cast<char>(r + ('a' - 'A'));
                }
                if (l != r) {
                    return false;
                }
            }
            return true;
        }

        // Documented CLI/environment boolean token set. File booleans stay
        // typed and never pass through here.
        [[nodiscard]] std::error_code parseBooleanText(const std::string_view text, bool &out) noexcept {
            const std::string_view token = trim(text);
            for (const std::string_view word: {"1", "true", "yes", "y", "on", "enable", "enabled"}) {
                if (iequals(token, word)) {
                    out = true;
                    return {};
                }
            }
            for (const std::string_view word: {"0", "false", "no", "n", "off", "disable", "disabled"}) {
                if (iequals(token, word)) {
                    out = false;
                    return {};
                }
            }
            return make_error_code(EConfigErrc::invalid_boolean);
        }

        void ResolveState::report(
            std::string location, std::string property, std::string message, std::error_code code, std::string alias) {
            if (m_diagnostics != nullptr) {
                m_diagnostics->add(
                    std::move(location), std::move(property), std::move(message), code, std::move(alias));
            }
        }

        void ResolveState::upsertOrigin(std::string name, const ESource source, std::string location) {
            if (m_origins == nullptr) {
                return;
            }
            for (auto &origin: *m_origins) {
                if (origin.m_name == name) {
                    origin.m_source = source;
                    origin.m_location = std::move(location);
                    return;
                }
            }
            m_origins->push_back(PropertyOrigin{std::move(name), source, std::move(location)});
        }

        void ResolveState::rejectUnknown(const std::span<const std::string_view> names) {
            for (const auto &layer: m_layers) {
                for (const auto &assignment: layer.m_assignments) {
                    bool known = false;
                    for (const std::string_view name: names) {
                        if (assignment.m_name == name) {
                            known = true;
                            break;
                        }
                    }
                    if (not known) {
                        report(
                            assignment.m_location, assignment.m_name,
                            configFormat("unknown property '{}'", assignment.m_name),
                            make_error_code(EConfigErrc::unknown_property), assignment.m_alias_used);
                        retain(make_error_code(EConfigErrc::unknown_property));
                    }
                }
            }
        }
    } // namespace details
} // namespace pP::config
