module;

#include "pP/Macros.h"
#include <toml++/toml.hpp>

module engine.core;

import :config;
import :config.file;

import std;

namespace pP::config {
    namespace {
        // toml++ owns TOML 1.0 syntax (tables, dotted keys, typed scalars,
        // comments, escapes). The codec only flattens the parsed tree into
        // the format-neutral document and rejects value kinds Phase 1 has
        // no settings for (arrays, dates/times). Duplicate keys, malformed
        // numbers, and redefined tables fail inside toml++ with source
        // positions that become diagnostics.
        template<typename K>
        [[nodiscard]] std::string keyText(const K &key) {
            if constexpr (requires { key.str(); }) {
                const auto view = key.str();
                return std::string{view.data(), view.size()};
            } else {
                return std::string{std::string_view{key}};
            }
        }

        struct TomlFlattener {
            std::string m_origin{"config"};
            Diagnostics *m_diagnostics{nullptr};
            Document m_document{};
            std::error_code m_first_error{};

            [[nodiscard]] std::string location(const toml::source_position &pos) const {
                if (pos.line == 0) {
                    return m_origin;
                }
                return configFormat("{}:{}", m_origin, pos.line);
            }

            void fail(std::string where, std::string key, std::string message, const EConfigErrc code) {
                m_diagnostics->add(std::move(where), std::move(key), std::move(message), make_error_code(code));
                if (not m_first_error) {
                    m_first_error = make_error_code(code);
                }
            }

            void addEntry(const toml::node &node, const std::string &key, FileValue value) {
                for (const auto &entry: m_document.m_entries) {
                    if (entry.m_key == key) {
                        fail(
                            location(node.source().begin), key, configFormat("duplicate key '{}'", key),
                            EConfigErrc::duplicate_key);
                        return;
                    }
                }
                m_document.m_entries.push_back(Entry{key, std::move(value), location(node.source().begin)});
            }

            void walkTable(const toml::table &table, const std::string &prefix) {
                for (auto &&kv: table) {
                    const std::string segment = keyText(kv.first);
                    const std::string key = prefix.empty() ? segment : prefix + "." + segment;
                    const toml::node &node = kv.second;
                    switch (node.type()) {
                        case toml::node_type::table: {
                            const toml::table *nested = node.as_table();
                            PPR_ASSERT(nested != nullptr);
                            if (nested != nullptr) {
                                walkTable(*nested, key);
                            }
                            break;
                        }
                        case toml::node_type::array:
                            fail(
                                location(node.source().begin), key, "arrays are not supported",
                                EConfigErrc::codec_error);
                            break;
                        case toml::node_type::boolean: {
                            const std::optional<bool> value = node.value<bool>();
                            if (not value.has_value()) {
                                fail(location(node.source().begin), key, "malformed boolean", EConfigErrc::codec_error);
                            } else {
                                addEntry(node, key, FileValue::boolean(*value));
                            }
                            break;
                        }
                        case toml::node_type::integer: {
                            const std::optional<i64> value = node.value<i64>();
                            if (not value.has_value()) {
                                fail(location(node.source().begin), key, "malformed integer", EConfigErrc::codec_error);
                            } else {
                                addEntry(node, key, FileValue::integer(*value));
                            }
                            break;
                        }
                        case toml::node_type::floating_point: {
                            const std::optional<double> value = node.value<double>();
                            if (not value.has_value()) {
                                fail(
                                    location(node.source().begin), key, "malformed floating-point value",
                                    EConfigErrc::codec_error);
                            } else {
                                addEntry(node, key, FileValue::floating(*value));
                            }
                            break;
                        }
                        case toml::node_type::string: {
                            const std::optional<std::string> value = node.value<std::string>();
                            if (not value.has_value()) {
                                fail(location(node.source().begin), key, "malformed string", EConfigErrc::codec_error);
                            } else {
                                addEntry(node, key, FileValue::text(*value));
                            }
                            break;
                        }
                        default:
                            fail(
                                location(node.source().begin), key, "unsupported value kind", EConfigErrc::codec_error);
                            break;
                    }
                }
            }
        };

        [[nodiscard]] bool isBareKey(const std::string_view key) noexcept {
            if (key.empty()) {
                return false;
            }
            for (const char ch: key) {
                const bool bare = (ch >= 'a' and ch <= 'z') or
                                  (ch >= 'A' and ch <= 'Z') or
                                  (ch >= '0' and ch <= '9') or
                                  ch == '_' or
                                  ch == '-';
                if (not bare) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::string escapeTomlString(const std::string_view text) {
            std::string out{"\""};
            for (const char ch: text) {
                switch (ch) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    default:
                        if ((ch >= 0x00 and ch < 0x20) or ch == 0x7F) {
                            constexpr std::string_view hex = "0123456789ABCDEF";
                            const auto code = static_cast<unsigned char>(ch);
                            out += "\\u00";
                            out += hex[code >> 4];
                            out += hex[code & 0x0F];
                        } else {
                            out += ch;
                        }
                        break;
                }
            }
            out += '"';
            return out;
        }
    } // namespace

    [[nodiscard]] Expected<Document> decode(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) {
        const std::string origin_text = origin.empty() ? std::string{"config"} : std::string{origin};
        toml::table table;
        try {
            table = toml::parse(bytes, origin_text);
        } catch (const toml::parse_error &err) {
            const std::string_view description = err.description();
            const u32 line = err.source().begin.line;
            const std::string where = line == 0 ? origin_text : configFormat("{}:{}", origin_text, line);
            diagnostics.add(where, "", std::string{description}, make_error_code(EConfigErrc::codec_error));
            return std::unexpected{make_error_code(EConfigErrc::codec_error)};
        }
        TomlFlattener flattener{};
        flattener.m_origin = origin_text;
        flattener.m_diagnostics = std::addressof(diagnostics);
        flattener.walkTable(table, {});
        if (flattener.m_first_error) {
            return std::unexpected{flattener.m_first_error};
        }
        return std::move(flattener.m_document);
    }

    [[nodiscard]] Expected<std::string> encode(const Document &document) {
        // Duplicate keys are writer errors, reported without diagnostics.
        for (std::size_t i = 0; i < document.m_entries.size(); ++i) {
            for (std::size_t j = i + 1; j < document.m_entries.size(); ++j) {
                if (document.m_entries[i].m_key == document.m_entries[j].m_key) {
                    return std::unexpected{make_error_code(EConfigErrc::duplicate_key)};
                }
            }
        }

        Array<std::string> sections{std::string{}};
        for (const auto &entry: document.m_entries) {
            std::string section{};
            if (const std::size_t dot = entry.m_key.rfind('.'); dot != std::string::npos) {
                section = entry.m_key.substr(0, dot);
            }
            bool known = false;
            for (const auto &existing: sections) {
                if (existing == section) {
                    known = true;
                    break;
                }
            }
            if (not known) {
                sections.push_back(section);
            }
        }

        std::string out{"# Generated by PPR config; comments are not preserved.\n"};
        for (const auto &section: sections) {
            // Root keys first, without a header.
            if (not section.empty()) {
                out += configFormat("\n[{}]\n", section);
            } else if (not out.empty()) {
                out += "\n";
            }
            for (const auto &entry: document.m_entries) {
                std::string entry_section{};
                std::string short_key = entry.m_key;
                if (const std::size_t dot = entry.m_key.rfind('.'); dot != std::string::npos) {
                    entry_section = entry.m_key.substr(0, dot);
                    short_key = entry.m_key.substr(dot + 1);
                }
                if (entry_section != section) {
                    continue;
                }
                const std::string key_text = isBareKey(short_key) ? short_key : escapeTomlString(short_key);
                const FileValue &value = entry.m_value;
                switch (value.m_kind) {
                    case EFileKind::boolean:
                        out += configFormat("{} = {}\n", key_text, value.m_boolean ? "true" : "false");
                        break;
                    case EFileKind::integer: {
                        std::array<char, 32> buffer{};
                        const auto [end, error] = std::to_chars(
                            buffer.data(), buffer.data() + buffer.size(), value.m_integer);
                        if (error != std::errc{}) {
                            return std::unexpected{std::make_error_code(error)};
                        }
                        out += key_text + " = ";
                        out.append(buffer.data(), end);
                        out += '\n';
                        break;
                    }
                    case EFileKind::floating: {
                        if (std::isnan(value.m_floating)) {
                            out += configFormat("{} = nan\n", key_text);
                        } else if (not std::isfinite(value.m_floating)) {
                            out += configFormat("{} = {}inf\n", key_text, value.m_floating < 0.0 ? "-" : "");
                        } else {
                            // Shortest locale-independent representation that round-trips to the same double.
                            std::array<char, 128> buffer{};
                            const auto [end, error] = std::to_chars(
                                buffer.data(), buffer.data() + buffer.size(), value.m_floating);
                            if (error != std::errc{}) {
                                return std::unexpected{std::make_error_code(error)};
                            }
                            std::string number(buffer.data(), end);
                            if (number.find_first_of(".eE") == std::string::npos) {
                                number += ".0";
                            }
                            out += configFormat("{} = {}\n", key_text, number);
                        }
                        break;
                    }
                    case EFileKind::text:
                        out += configFormat("{} = {}\n", key_text, escapeTomlString(value.m_text));
                        break;
                    default: return std::unexpected{make_error_code(EConfigErrc::codec_error)};
                }
            }
        }
        return out;
    }

    [[nodiscard]] Layer toLayer(Document document, std::string origin) {
        Layer layer{};
        layer.m_source = ESource::file;
        layer.m_origin = origin;
        std::size_t order = 0;
        for (auto &entry: document.m_entries) {
            SourceValue value{};
            switch (entry.m_value.m_kind) {
                case EFileKind::boolean: value = SourceValue::boolean(entry.m_value.m_boolean); break;
                case EFileKind::integer: value = SourceValue::integer(entry.m_value.m_integer); break;
                case EFileKind::floating: value = SourceValue::floating(entry.m_value.m_floating); break;
                case EFileKind::text: value = SourceValue::text(std::move(entry.m_value.m_text)); break;
                default: value = SourceValue::text({}); break;
            }
            layer.m_assignments.push_back(
                Assignment{std::move(entry.m_key), {}, std::move(value), std::move(entry.m_location), order++});
        }
        return layer;
    }
} // namespace pP::config
