module;

#include "pP/Macros.h"
#include <toml++/toml.hpp>

module engine.core;

import :config;
import :config.file;
import :logger;

import std;

namespace pP::config {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(ConfigFile, debug, none)
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
            EDecodeMode m_mode{EDecodeMode::scalar};

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
                    if (m_mode == EDecodeMode::records and
                        (segment.empty() or segment.find('.') != std::string::npos)) {
                        fail(
                            location(node.source().begin), key, "ambiguous record key segment",
                            EConfigErrc::codec_error);
                        continue;
                    }
                    switch (node.type()) {
                        case toml::node_type::table: {
                            const toml::table *nested = node.as_table();
                            PPR_ASSERT(nested != nullptr);
                            if (nested != nullptr) {
                                if (m_mode == EDecodeMode::records and nested->empty()) {
                                    fail(
                                        location(node.source().begin), key, "empty record tables are not supported",
                                        EConfigErrc::missing_value);
                                    break;
                                }
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
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics, const EDecodeMode mode) {
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
        flattener.m_mode = mode;

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

    namespace {
        // toml++ increments source columns once per UTF-8 codepoint (including
        // CR), and advances source lines on LF. Its reader consumes an initial BOM.
        [[nodiscard]] Expected<std::size_t> sourceOffset(
            const std::string_view bytes, const toml::source_position position) {
            std::size_t offset = bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u;
            u32 line = 1u;
            u32 column = 1u;
            while (offset < bytes.size() and (line != position.line or column != position.column)) {
                const auto lead = static_cast<unsigned char>(bytes[offset]);
                const std::size_t width = lead < 0x80u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : 4u;
                if (bytes[offset] == '\n') {
                    ++line;
                    column = 1u;
                } else {
                    ++column;
                }
                offset += width;
            }
            if (offset > bytes.size() or line != position.line or column != position.column) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }
            return offset;
        }

        [[nodiscard]] Expected<std::string> scalarText(const FileValue &value) {
            Document document;
            document.m_entries.push_back(Entry{"value", value, {}});
            auto encoded = encode(document);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, encoded);

            constexpr std::string_view marker = "value = ";
            const auto begin = encoded->find(marker);
            if (begin == std::string::npos or not encoded->ends_with('\n')) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }
            return encoded->substr(begin + marker.size(), encoded->size() - begin - marker.size() - 1u);
        }
    }

    Expected<std::string> applySourcePatch(
        const std::string_view bytes, const ScalarSourcePatch &patch, const bool reverse) {
        const auto &before = reverse ? patch.m_after : patch.m_before;
        const auto &after = reverse ? patch.m_before : patch.m_after;
        // Match the established content document source-byte limit, including growth.
        constexpr std::size_t max_source_bytes = 1024u * 1024u;
        if (bytes.size() > max_source_bytes or
            patch.m_offset > bytes.size() or
            before.size() > bytes.size() - patch.m_offset or
            bytes.substr(patch.m_offset, before.size()) != before) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, std::errc::invalid_argument);
        }
        if (after.size() > max_source_bytes - (bytes.size() - before.size())) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::out_of_range);
        }

        std::string result{bytes};
        result.replace(patch.m_offset, before.size(), after);
        return result;
    }

    Expected<ScalarSourcePatch> patchScalar(
        const std::string_view bytes, const std::string_view key, const FileValue &value, const std::string_view origin,
        Diagnostics &diagnostics) {
        const auto fail = [&](const std::string_view message) {
            const auto error = make_error_code(EConfigErrc::codec_error);
            diagnostics.add(std::string{origin}, std::string{key}, std::string{message}, error);
            return error;
        };

        if (bytes.size() > 1024u * 1024u or not isValidCanonicalName(key)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Invalid scalar patch path or source size"));
        }

        auto decoded = decode(bytes, origin, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);

        auto scalar = scalarText(value);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, scalar);

        toml::table table;
        try {
            table = toml::parse(bytes, std::string{origin});
        } catch (const toml::parse_error &) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Source cannot be parsed for patching"));
        }

        const toml::table *parent = std::addressof(table);
        std::string_view remaining = key;
        while (remaining.find('.') != std::string_view::npos) {
            const auto dot = remaining.find('.');
            const auto *node = parent->get(remaining.substr(0u, dot));
            if (node == nullptr or node->as_table() == nullptr) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Scalar parent table does not exist"));
            }
            parent = node->as_table();
            remaining.remove_prefix(dot + 1u);
        }

        ScalarSourcePatch patch;
        if (const auto *node = parent->get(remaining)) {
            const bool compatible = (node->is_boolean() and value.m_kind == EFileKind::boolean) or
                                    (node->is_integer() and value.m_kind == EFileKind::integer) or
                                    (node->is_floating_point() and value.m_kind == EFileKind::floating) or
                                    (node->is_string() and value.m_kind == EFileKind::text);
            if (not compatible) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Scalar patch changes the authored value kind"));
            }

            auto begin = sourceOffset(bytes, node->source().begin);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, begin);
            auto end = sourceOffset(bytes, node->source().end);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, end);
            if (*end < *begin) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Invalid parser source span"));
            }

            patch = {*begin, std::string{bytes.substr(*begin, *end - *begin)}, std::move(*scalar)};
        } else {
            auto begin = sourceOffset(bytes, parent->source().begin);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, begin);
            if (parent == std::addressof(table) or
                parent->is_inline() or
                *begin >= bytes.size() or
                bytes[*begin] != '[') {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Insertion requires an existing explicit table"));
            }

            const auto newline = bytes.find('\n', *begin);
            const std::string_view eol = bytes.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
            patch.m_offset = newline == std::string_view::npos ? bytes.size() : newline + 1u;
            if (newline == std::string_view::npos) {
                patch.m_after += eol;
            }
            patch.m_after += std::string{remaining} + " = " + *scalar;
            if (newline != std::string_view::npos) {
                patch.m_after += eol;
            }
        }

        auto patched = applySourcePatch(bytes, patch);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, patched);
        auto reparsed = decode(*patched, origin, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, reparsed);

        const auto entry = std::ranges::find(reparsed->m_entries, key, &Entry::m_key);
        // An implicit parent can share a descendant's header source region.
        // Require that insertion actually created the requested qualified key.
        if (entry == reparsed->m_entries.end() or entry->m_value.m_kind != value.m_kind) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Insertion requires the requested explicit table"));
        }
        return patch;
    }

    Expected<ScalarSourcePatch> insertRecord(
        const std::string_view bytes, const std::string_view key, const Document &record, const std::string_view origin,
        Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ConfigFile, bytes.size() <= 1024u * 1024u and isValidCanonicalName(key) and not record.m_entries.empty()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        auto decoded = decode(bytes, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);
        const auto prefix = std::string{key} + ".";
        if (std::ranges::any_of(decoded->m_entries, [&](const Entry &entry) {
                return entry.m_key == key or entry.m_key.starts_with(prefix);
            })) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::duplicate_key);
        }

        Document qualified;
        for (const auto &entry: record.m_entries) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, isValidCanonicalName(entry.m_key) ? std::error_code{}
                                                              : std::make_error_code(std::errc::invalid_argument));
            qualified.m_entries.push_back({prefix + entry.m_key, entry.m_value, {}});
        }

        auto encoded = encode(qualified);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, encoded);

        const std::string_view eol = bytes.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
        std::string appended;
        if (not bytes.empty() and not bytes.ends_with('\n')) {
            appended += eol;
        }
        appended += eol;
        for (const char ch: *encoded) {
            if (ch == '\n') {
                appended += eol;
            } else {
                appended += ch;
            }
        }

        ScalarSourcePatch patch{bytes.size(), {}, std::move(appended)};
        auto candidate = applySourcePatch(bytes, patch);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, candidate);
        auto checked = decode(*candidate, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, checked);
        return patch;
    }

    Expected<ScalarSourcePatch> removeRecord(
        const std::string_view bytes, const std::string_view key, const std::string_view origin,
        Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ConfigFile, bytes.size() <= 1024u * 1024u and isValidCanonicalName(key)
                            ? std::error_code{}
                            : std::make_error_code(std::errc::invalid_argument));

        auto decoded = decode(bytes, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);

        toml::table table;
        try {
            table = toml::parse(bytes, std::string{origin});
        } catch (const toml::parse_error &) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
        }

        const toml::node *node = std::addressof(table);
        std::string_view remaining = key;
        while (not remaining.empty()) {
            const auto dot = remaining.find('.');
            node = node->as_table() ? node->as_table()->get(remaining.substr(0u, dot)) : nullptr;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, node ? std::error_code{} : make_error_code(EConfigErrc::unknown_property));
            if (dot == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(dot + 1u);
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ConfigFile, not node->as_table() or not node->as_table()->is_inline()
                            ? std::error_code{}
                            : std::make_error_code(std::errc::operation_not_supported));

        using Span = std::pair<std::size_t, std::size_t>;
        Array<Span> spans;
        const auto collect = [&](auto &&self, const toml::node &current) -> std::error_code {
            auto begin = sourceOffset(bytes, current.source().begin);
            PPR_RETURN_ERROR_ON_FAIL(ConfigFile, begin);
            if (const auto *nested = current.as_table(); nested and not nested->is_inline()) {
                if (*begin < bytes.size() and bytes[*begin] == '[') {
                    const auto end = bytes.find('\n', *begin);
                    spans.emplace_back(*begin, end == std::string_view::npos ? bytes.size() : end + 1u);
                }
                for (const auto &[unused, child]: *nested) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, self(self, child));
                }
            } else {
                auto end = sourceOffset(bytes, current.source().end);
                PPR_RETURN_ERROR_ON_FAIL(ConfigFile, end);
                const auto line_begin = *begin == 0u ? std::string_view::npos : bytes.rfind('\n', *begin - 1u);
                const auto line_end = bytes.find('\n', *end);
                spans.emplace_back(
                    line_begin == std::string_view::npos ? 0u : line_begin + 1u,
                    line_end == std::string_view::npos ? bytes.size() : line_end + 1u);
            }
            return {};
        };

        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, collect(collect, *node));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ConfigFile, not spans.empty() ? std::error_code{} : make_error_code(EConfigErrc::codec_error));

        std::ranges::sort(spans);
        const auto first = spans.front().first;
        const auto last = std::ranges::max(spans, {}, &Span::second).second;
        ScalarSourcePatch patch{first, std::string{bytes.substr(first, last - first)}, {}};
        auto cursor = first;
        for (const auto &[begin, end]: spans) {
            if (begin > cursor) {
                patch.m_after += bytes.substr(cursor, begin - cursor);
            }
            cursor = (std::max) (cursor, end);
        }

        auto candidate = applySourcePatch(bytes, patch);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, candidate);
        auto checked = decode(*candidate, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, checked);

        const auto prefix = std::string{key} + ".";
        if (std::ranges::any_of(checked->m_entries, [&](const Entry &entry) {
                return entry.m_key == key or entry.m_key.starts_with(prefix);
            })) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
        }

        // Parser source regions for implicit tables can overlap descendants.
        // Prove that removing the selected record preserved every other value.
        Document retained;
        for (const auto &entry: decoded->m_entries) {
            if (entry.m_key != key and not entry.m_key.starts_with(prefix)) {
                retained.m_entries.push_back(entry);
            }
        }

        std::ranges::sort(retained.m_entries, {}, &Entry::m_key);
        std::ranges::sort(checked->m_entries, {}, &Entry::m_key);
        auto expected = encode(retained);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, expected);
        auto actual = encode(*checked);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, actual);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ConfigFile, *expected == *actual ? std::error_code{} : make_error_code(EConfigErrc::codec_error));
        return patch;
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
