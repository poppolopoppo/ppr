module;

#include "pP/Macros.h"
#include <toml++/toml.hpp>

module engine.core;

import :config;
import :config.file;
import :hal;
import :hashing;
import :logger;
import :memory.arena;

import std;

namespace pP::config {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(ConfigFile, debug, none)
    namespace {
        // ------------------------------------------------------------------
        // TOML decoding
        // ------------------------------------------------------------------

        // toml++ owns TOML 1.0 syntax (tables, dotted keys, typed scalars,
        // comments, escapes). The codec only flattens the parsed tree into
        // the format-neutral document and rejects value kinds Phase 1 has
        // no settings for (arrays, dates/times). Duplicate keys, malformed
        // numbers, and redefined tables fail inside toml++ with source
        // positions that become diagnostics.
        template<typename K>
        [[nodiscard]] std::string_view keyText(const K &key) {
            if constexpr (requires { key.str(); }) {
                const auto view = key.str();
                return std::string_view{view.data(), view.size()};
            } else {
                return std::string_view{key};
            }
        }

        using ScratchText = std::basic_string<char, std::char_traits<char>, mem::STL<char, mem::ScratchPad>>;

        struct ScratchTextHash {
            [[nodiscard]] std::size_t operator()(const ScratchText &value) const noexcept {
                return static_cast<std::size_t>(hash::DefaultHash<ScratchText>{}(value).m_value);
            }
        };

        struct TomlFlattener {
            std::string m_origin{"config"};
            Diagnostics *m_diagnostics{nullptr};
            Document m_document{};
            std::unordered_set<
                ScratchText, ScratchTextHash, std::equal_to<ScratchText>, mem::STL<ScratchText, mem::ScratchPad>>
                m_keys{};
            std::error_code m_first_error{};
            EDecodeMode m_mode{EDecodeMode::scalar};
            bool m_retain_document{true};

            [[nodiscard]] std::string location(const toml::source_position &pos) const {
                if (pos.line == 0) {
                    return m_origin;
                }

                std::array<char, 16u> digits{};
                const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), pos.line);
                std::string result;
                result.reserve(m_origin.size() + 1u + static_cast<std::size_t>(converted.ptr - digits.data()));
                result.append(m_origin);
                result.push_back(':');
                result.append(digits.data(), converted.ptr);
                return result;
            }

            void fail(std::string where, const std::string_view key, std::string message, const EConfigErrc code) {
                m_diagnostics->add(std::move(where), std::string{key}, std::move(message), make_error_code(code));
                if (not m_first_error) {
                    m_first_error = make_error_code(code);
                }
            }

            void addEntry(const toml::node &node, const std::string_view key, FileValue value) {
                // Own keys independently of the document vector: its growth can
                // relocate short strings while recursive flattening is in progress.
                if (not m_keys.emplace(key).second) {
                    fail(
                        location(node.source().begin), key, configFormat("duplicate key '{}'", key),
                        EConfigErrc::duplicate_key);
                    return;
                }

                if (m_retain_document) {
                    m_document.m_entries.push_back(
                        Entry{std::string{key}, std::move(value), location(node.source().begin)});
                }
            }

            void walkTable(const toml::table &table, const std::string_view prefix) {
                const hal::ProfileScope scope{"Config.File.WalkTable"};
                // Child calls borrow this frame's key only until they return.
                ScratchText key;

                for (auto &&kv: table) {
                    const std::string_view segment = keyText(kv.first);
                    key.clear();
                    key.reserve(prefix.size() + (prefix.empty() ? 0u : 1u) + segment.size());
                    key.append(prefix);
                    if (not prefix.empty()) {
                        key.push_back('.');
                    }
                    key.append(segment);

                    const toml::node &node = kv.second;
                    if (m_mode != EDecodeMode::scalar and (segment.empty() or segment.find('.') != std::string::npos)) {
                        fail(
                            location(node.source().begin), key,
                            m_mode == EDecodeMode::records ? "ambiguous record key segment"
                                                           : "ambiguous tree key segment",
                            EConfigErrc::codec_error);
                        continue;
                    }

                    switch (node.type()) {
                        case toml::node_type::table: {
                            const toml::table *nested = node.as_table();
                            PPR_ASSERT(nested != nullptr);
                            if (nested != nullptr) {
                                if (m_mode == EDecodeMode::tree and m_retain_document) {
                                    m_document.m_groups.push_back({std::string{key}, location(node.source().begin)});
                                }
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
                            const auto *const value = node.as_string();
                            if (value == nullptr) {
                                fail(location(node.source().begin), key, "malformed string", EConfigErrc::codec_error);
                            } else {
                                addEntry(node, key, m_retain_document ? FileValue::text(value->get()) : FileValue{});
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

        [[nodiscard]] Expected<toml::table> parseToml(
            const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics,
            const EDecodeMode mode) {
            const hal::ProfileScope scope{"Config.File.ParseToml"};
            const std::string origin_text = origin.empty() ? std::string{"config"} : std::string{origin};
            if (mode == EDecodeMode::tree and bytes.size() > 1024u * 1024u) {
                diagnostics.add(
                    origin_text, "", "tree source exceeds the 1 MiB document limit",
                    make_error_code(EConfigErrc::out_of_range));
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::out_of_range);
            }

            try {
                return toml::parse(bytes, origin_text);
            } catch (const toml::parse_error &err) {
                const u32 line = err.source().begin.line;
                const std::string where = line == 0 ? origin_text : configFormat("{}:{}", origin_text, line);
                diagnostics.add(where, "", std::string{err.description()}, make_error_code(EConfigErrc::codec_error));
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
                std::unreachable();
            }
        }

        [[nodiscard]] Expected<Document> flattenParsed(
            const toml::table &table, const std::string_view origin, Diagnostics &diagnostics, const EDecodeMode mode,
            const bool retain_document = true) {
            const hal::ProfileScope scope{"Config.File.Flatten"};
            const auto scratch_scope = mem::ScratchPad::open();
            TomlFlattener flattener{};
            flattener.m_origin = origin.empty() ? std::string{"config"} : std::string{origin};
            flattener.m_diagnostics = std::addressof(diagnostics);
            flattener.m_mode = mode;
            flattener.m_retain_document = retain_document;

            flattener.walkTable(table, {});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, flattener.m_first_error);
            return std::move(flattener.m_document);
        }

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
        const hal::ProfileScope scope{"Config.File.Decode"};
        auto table = parseToml(bytes, origin, diagnostics, mode);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, table);

        return flattenParsed(*table, origin, diagnostics, mode);
    }

    namespace {
        // ------------------------------------------------------------------
        // canonical writer validation and scalar emission
        // ------------------------------------------------------------------

        struct EncodingIndex {
            Array<const Entry *, mem::ScratchPad> m_entries;
            Array<const GroupPresence *, mem::ScratchPad> m_groups;

            explicit EncodingIndex(const Document &document) {
                m_entries.reserve(document.m_entries.size());
                for (const auto &entry: document.m_entries) {
                    m_entries.push_back(std::addressof(entry));
                }

                m_groups.reserve(document.m_groups.size());
                for (const auto &group: document.m_groups) {
                    m_groups.push_back(std::addressof(group));
                }

                std::ranges::sort(m_entries, {}, [](const Entry *entry) { return std::string_view{entry->m_key}; });
                std::ranges::sort(
                    m_groups, {}, [](const GroupPresence *group) { return std::string_view{group->m_path}; });
            }
        };

        [[nodiscard]] std::error_code validateEncoding(const Document &document, const EncodingIndex &index) {
            for (std::size_t position = 1u; position < index.m_entries.size(); ++position) {
                if (index.m_entries[position - 1u]->m_key == index.m_entries[position]->m_key) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::duplicate_key);
                }
            }

            // Retain source-order precedence between invalid and duplicate groups.
            for (const auto &group: document.m_groups) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ConfigFile,
                    isValidCanonicalName(group.m_path) ? std::error_code{} : make_error_code(EConfigErrc::codec_error));

                const auto equal = std::ranges::equal_range(
                    index.m_groups, std::string_view{group.m_path}, {},
                    [](const GroupPresence *value) { return std::string_view{value->m_path}; });
                for (const auto *other: equal) {
                    if (other < std::addressof(group)) {
                        PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::duplicate_key);
                    }
                }
            }

            if (document.m_groups.empty()) {
                return {};
            }

            for (const auto &entry: document.m_entries) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ConfigFile,
                    isValidCanonicalName(entry.m_key) ? std::error_code{} : make_error_code(EConfigErrc::codec_error));

                const auto exact = std::ranges::lower_bound(
                    index.m_groups, std::string_view{entry.m_key}, {},
                    [](const GroupPresence *value) { return std::string_view{value->m_path}; });
                if (exact != index.m_groups.end() and (*exact)->m_path == entry.m_key) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::type_mismatch);
                }

                ScratchText prefix{entry.m_key};
                prefix.push_back('.');
                const auto group = std::ranges::lower_bound(
                    index.m_groups, std::string_view{prefix}, {},
                    [](const GroupPresence *value) { return std::string_view{value->m_path}; });
                const auto scalar = std::ranges::lower_bound(
                    index.m_entries, std::string_view{prefix}, {},
                    [](const Entry *value) { return std::string_view{value->m_key}; });
                if ((group != index.m_groups.end() and (*group)->m_path.starts_with(prefix)) or
                    (scalar != index.m_entries.end() and (*scalar)->m_key.starts_with(prefix))) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::type_mismatch);
                }
            }
            return {};
        }

        [[nodiscard]] std::error_code validateScalarKind(const FileValue &value) {
            switch (value.m_kind) {
                case EFileKind::boolean:
                case EFileKind::integer:
                case EFileKind::floating:
                case EFileKind::text: return {};
                default: PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }
            std::unreachable();
        }

        [[nodiscard]] std::error_code appendScalarValue(std::string &out, const FileValue &value) {
            PPR_RETURN_ERROR_ON_FAIL(ConfigFile, validateScalarKind(value));

            switch (value.m_kind) {
                case EFileKind::boolean: out += value.m_boolean ? "true" : "false"; break;
                case EFileKind::integer: {
                    std::array<char, 32> buffer{};
                    const auto [end, error] = std::to_chars(
                        buffer.data(), buffer.data() + buffer.size(), value.m_integer);
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, std::make_error_code(error));

                    out.append(buffer.data(), end);
                    break;
                }
                case EFileKind::floating: {
                    if (std::isnan(value.m_floating)) {
                        out += "nan";
                    } else if (not std::isfinite(value.m_floating)) {
                        out += value.m_floating < 0.0 ? "-inf" : "inf";
                    } else {
                        std::array<char, 128> buffer{};
                        const auto [end, error] = std::to_chars(
                            buffer.data(), buffer.data() + buffer.size(), value.m_floating);
                        PPR_RETURN_ERROR_ON_FAIL(ConfigFile, std::make_error_code(error));

                        const std::string_view number{buffer.data(), static_cast<std::size_t>(end - buffer.data())};
                        out += number;
                        if (number.find_first_of(".eE") == std::string_view::npos) {
                            out += ".0";
                        }
                    }
                    break;
                }
                case EFileKind::text: out += escapeTomlString(value.m_text); break;
                default: PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }
            return {};
        }

        [[nodiscard]] bool equalScalar(const FileValue &left, const FileValue &right) {
            if (left.m_kind != right.m_kind) {
                return false;
            }

            switch (left.m_kind) {
                case EFileKind::boolean: return left.m_boolean == right.m_boolean;
                case EFileKind::integer: return left.m_integer == right.m_integer;
                case EFileKind::text: return left.m_text == right.m_text;
                case EFileKind::floating:
                    if (std::isnan(left.m_floating) and std::isnan(right.m_floating)) {
                        return true;
                    }

                    return left.m_floating == right.m_floating and
                           std::signbit(left.m_floating) == std::signbit(right.m_floating);
                default: return false;
            }
        }

        [[nodiscard]] std::error_code equalRetainedDocuments(const Document &left, const Document &right) {
            const EncodingIndex left_index{left};
            const EncodingIndex right_index{right};
            PPR_RETURN_ERROR_ON_FAIL(ConfigFile, validateEncoding(left, left_index));

            for (const auto *entry: left_index.m_entries) {
                PPR_RETURN_ERROR_ON_FAIL(ConfigFile, validateScalarKind(entry->m_value));
            }

            PPR_RETURN_ERROR_ON_FAIL(ConfigFile, validateEncoding(right, right_index));

            for (const auto *entry: right_index.m_entries) {
                PPR_RETURN_ERROR_ON_FAIL(ConfigFile, validateScalarKind(entry->m_value));
            }

            if (left_index.m_entries.size() != right_index.m_entries.size() or
                left_index.m_groups.size() != right_index.m_groups.size()) {
                PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }

            for (std::size_t position = 0u; position < left_index.m_entries.size(); ++position) {
                const auto &a = *left_index.m_entries[position];
                const auto &b = *right_index.m_entries[position];
                if (a.m_key != b.m_key or not equalScalar(a.m_value, b.m_value)) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
                }
            }

            for (std::size_t position = 0u; position < left_index.m_groups.size(); ++position) {
                if (left_index.m_groups[position]->m_path != right_index.m_groups[position]->m_path) {
                    PPR_RETURN_ERROR_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
                }
            }
            return {};
        }

        [[nodiscard]] std::string_view entrySection(const Entry &entry) {
            const std::string_view key{entry.m_key};
            const auto dot = key.rfind('.');
            return dot == std::string_view::npos ? std::string_view{} : key.substr(0u, dot);
        }
    }

    [[nodiscard]] Expected<std::string> encode(const Document &document) {
        const hal::ProfileScope scope{"Config.File.Encode"};
        const auto scratch_scope = mem::ScratchPad::open();
        const EncodingIndex index{document};
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, validateEncoding(document, index));

        Array<std::string_view, mem::ScratchPad> sections;
        std::map<
            std::string_view, std::size_t, std::less<>,
            mem::STL<std::pair<const std::string_view, std::size_t>, mem::ScratchPad>>
            section_index;
        const auto add_section = [&](const std::string_view section) {
            if (section_index.try_emplace(section, sections.size()).second) {
                sections.push_back(section);
            }
        };

        add_section({});
        for (const auto &group: document.m_groups) {
            add_section(group.m_path);
        }

        for (const auto &entry: document.m_entries) {
            add_section(entrySection(entry));
        }

        if (not document.m_groups.empty()) {
            std::ranges::sort(sections);
            for (std::size_t position = 0u; position < sections.size(); ++position) {
                section_index.find(sections[position])->second = position;
            }
        }

        // Sort section memberships once; each section retains original entry order.
        Array<std::pair<std::size_t, std::size_t>, mem::ScratchPad> memberships;
        memberships.reserve(document.m_entries.size());
        for (std::size_t position = 0u; position < document.m_entries.size(); ++position) {
            memberships.emplace_back(section_index.find(entrySection(document.m_entries[position]))->second, position);
        }

        std::ranges::sort(memberships);

        std::string out{"# Generated by PPR config; comments are not preserved.\n"};
        std::size_t cursor = 0u;
        for (std::size_t section_position = 0u; section_position < sections.size(); ++section_position) {
            const auto section = sections[section_position];
            if (section.empty()) {
                out += '\n';
            } else {
                out += configFormat("\n[{}]\n", section);
            }

            while (cursor < memberships.size() and memberships[cursor].first == section_position) {
                const auto &entry = document.m_entries[memberships[cursor].second];
                const std::string_view key{entry.m_key};
                const auto dot = key.rfind('.');
                const auto short_key = dot == std::string_view::npos ? key : key.substr(dot + 1u);
                out += isBareKey(short_key) ? std::string{short_key} : escapeTomlString(short_key);
                out += " = ";
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, appendScalarValue(out, entry.m_value));
                out += '\n';
                ++cursor;
            }
        }
        return out;
    }

    namespace {
        // ------------------------------------------------------------------
        // parser positions and scalar text
        // ------------------------------------------------------------------

        // toml++ increments source columns once per UTF-8 codepoint (including
        // CR), and advances source lines on LF. Its reader consumes an initial BOM.
        struct SourceCursor {
            std::size_t m_offset;
            u32 m_line{1u};
            u32 m_column{1u};
        };

        [[nodiscard]] Expected<std::size_t> sourceOffset(
            const std::string_view bytes, const toml::source_position position, SourceCursor &cursor) {
            while (cursor.m_offset < bytes.size() and
                   (cursor.m_line != position.line or cursor.m_column != position.column)) {
                const auto lead = static_cast<unsigned char>(bytes[cursor.m_offset]);
                const std::size_t width = lead < 0x80u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : 4u;
                if (bytes[cursor.m_offset] == '\n') {
                    ++cursor.m_line;
                    cursor.m_column = 1u;
                } else {
                    ++cursor.m_column;
                }
                cursor.m_offset += width;
            }

            if (cursor.m_offset > bytes.size() or
                cursor.m_line != position.line or
                cursor.m_column != position.column) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }
            return cursor.m_offset;
        }

        [[nodiscard]] Expected<std::size_t> sourceOffset(
            const std::string_view bytes, const toml::source_position position) {
            const hal::ProfileScope scope{"Config.File.SourceOffset"};
            SourceCursor cursor{bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u};
            return sourceOffset(bytes, position, cursor);
        }

        [[nodiscard]] Expected<std::string> scalarText(const FileValue &value) {
            const hal::ProfileScope scope{"Config.File.ScalarText"};
            std::string out;
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, appendScalarValue(out, value));
            return out;
        }
    }

    Expected<std::string> applySourcePatch(
        const std::string_view bytes, const ScalarSourcePatch &patch, const bool reverse) {
        const hal::ProfileScope scope{"Config.File.ApplyPatch"};
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
        const hal::ProfileScope scope{"Config.File.PatchScalar"};
        const auto fail = [&](const std::string_view message) {
            const auto error = make_error_code(EConfigErrc::codec_error);
            diagnostics.add(std::string{origin}, std::string{key}, std::string{message}, error);
            return error;
        };

        if (bytes.size() > 1024u * 1024u or not isValidCanonicalName(key)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Invalid scalar patch path or source size"));
        }

        auto table = parseToml(bytes, origin, diagnostics, EDecodeMode::scalar);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, table);
        auto decoded = flattenParsed(*table, origin, diagnostics, EDecodeMode::scalar, false);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);

        auto scalar = scalarText(value);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, scalar);

        const toml::table *parent = std::addressof(*table);
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

            const hal::ProfileScope position_scope{"Config.File.SourceOffset"};
            SourceCursor cursor{bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u};
            auto begin = sourceOffset(bytes, node->source().begin, cursor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, begin);
            auto end = sourceOffset(bytes, node->source().end, cursor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, end);
            if (*end < *begin) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail("Invalid parser source span"));
            }

            patch = {*begin, std::string{bytes.substr(*begin, *end - *begin)}, std::move(*scalar)};
        } else {
            auto begin = sourceOffset(bytes, parent->source().begin);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, begin);
            if (parent == std::addressof(*table) or
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

    namespace {
        // ------------------------------------------------------------------
        // validated source transactions
        // ------------------------------------------------------------------

        struct ValidatedSourcePatch {
            ScalarSourcePatch m_patch;
            std::string m_bytes;
            Document m_document;
        };

        Expected<ValidatedSourcePatch> insertSubtree(
            const std::string_view bytes, const std::string_view key, const Document &record,
            const std::string_view origin, Diagnostics &diagnostics, const EDecodeMode mode,
            const Document *const original = nullptr) {
            const hal::ProfileScope scope{"Config.File.InsertSubtree"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, bytes.size() <= 1024u * 1024u and
                                    isValidCanonicalName(key) and
                                    (mode == EDecodeMode::tree or not record.m_entries.empty())
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));

            Expected<Document> decoded;
            if (original == nullptr) {
                decoded = decode(bytes, origin, diagnostics, mode);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);
            }

            const auto &document = original == nullptr ? *decoded : *original;
            const auto prefix = std::string{key} + ".";
            if (std::ranges::any_of(
                    document.m_entries,
                    [&](const Entry &entry) { return entry.m_key == key or entry.m_key.starts_with(prefix); }) or
                std::ranges::any_of(document.m_groups, [&](const GroupPresence &group) {
                    return group.m_path == key or group.m_path.starts_with(prefix);
                })) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::duplicate_key);
            }
            if (mode == EDecodeMode::tree and std::ranges::any_of(document.m_entries, [&](const Entry &entry) {
                    return key.starts_with(entry.m_key + ".");
                })) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::type_mismatch);
            }

            Document qualified;
            if (mode == EDecodeMode::tree) {
                qualified.m_groups.push_back({std::string{key}, {}});
                for (const auto &group: record.m_groups) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ConfigFile, isValidCanonicalName(group.m_path)
                                        ? std::error_code{}
                                        : std::make_error_code(std::errc::invalid_argument));
                    qualified.m_groups.push_back({prefix + group.m_path, {}});
                }
            }

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
            auto checked = decode(*candidate, origin, diagnostics, mode);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, checked);
            return ValidatedSourcePatch{std::move(patch), std::move(*candidate), std::move(*checked)};
        }

        Expected<ValidatedSourcePatch> removeSubtree(
            const std::string_view bytes, const std::string_view key, const std::string_view origin,
            Diagnostics &diagnostics, const EDecodeMode mode) {
            const hal::ProfileScope scope{"Config.File.RemoveSubtree"};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, bytes.size() <= 1024u * 1024u and isValidCanonicalName(key)
                                ? std::error_code{}
                                : std::make_error_code(std::errc::invalid_argument));

            const auto scratch_scope = mem::ScratchPad::open();
            auto table = parseToml(bytes, origin, diagnostics, mode);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, table);
            auto decoded = flattenParsed(*table, origin, diagnostics, mode);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, decoded);

            const toml::node *node = std::addressof(*table);
            std::string_view remaining = key;
            while (not remaining.empty()) {
                // A descendant of an inline table shares its assignment line.
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ConfigFile, not node->as_table() or not node->as_table()->is_inline()
                                    ? std::error_code{}
                                    : std::make_error_code(std::errc::operation_not_supported));

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

            struct PositionRequest {
                std::size_t m_slot;
                toml::source_position m_position;
            };

            struct NodeSpan {
                const toml::node *m_node;
                std::size_t m_begin;
                std::size_t m_end;
            };

            Array<PositionRequest, mem::ScratchPad> requests;
            Array<std::size_t, mem::ScratchPad> offsets;
            Array<NodeSpan, mem::ScratchPad> nodes;
            const auto request = [&](const toml::source_position position) {
                const auto slot = offsets.size();
                offsets.push_back(0u);
                requests.push_back({slot, position});
                return slot;
            };

            const auto collect = [&](auto &&self, const toml::node &current) -> void {
                const auto begin = request(current.source().begin);
                if (const auto *nested = current.as_table(); nested and not nested->is_inline()) {
                    nodes.push_back({std::addressof(current), begin, begin});

                    for (const auto &[unused, child]: *nested) {
                        self(self, child);
                    }
                } else {
                    nodes.push_back({std::addressof(current), begin, request(current.source().end)});
                }
            };

            collect(collect, *node);

            std::ranges::sort(requests, [](const PositionRequest &left, const PositionRequest &right) {
                return std::tie(left.m_position.line, left.m_position.column) <
                       std::tie(right.m_position.line, right.m_position.column);
            });

            {
                const hal::ProfileScope position_scope{"Config.File.SourceOffset"};
                SourceCursor cursor{bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u};
                for (const auto &position: requests) {
                    auto offset = sourceOffset(bytes, position.m_position, cursor);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, offset);

                    offsets[position.m_slot] = *offset;
                }
            }

            using Span = std::pair<std::size_t, std::size_t>;
            Array<Span, mem::ScratchPad> spans;
            spans.reserve(nodes.size());
            for (const auto &current: nodes) {
                const auto begin = offsets[current.m_begin];
                if (const auto *nested = current.m_node->as_table(); nested and not nested->is_inline()) {
                    if (begin < bytes.size() and bytes[begin] == '[') {
                        const auto end = bytes.find('\n', begin);
                        spans.emplace_back(begin, end == std::string_view::npos ? bytes.size() : end + 1u);
                    }
                } else {
                    const auto end = offsets[current.m_end];
                    const auto line_begin = begin == 0u ? std::string_view::npos : bytes.rfind('\n', begin - 1u);
                    const auto line_end = bytes.find('\n', end);
                    spans.emplace_back(
                        line_begin == std::string_view::npos ? (bytes.starts_with("\xEF\xBB\xBF") ? 3u : 0u)
                                                             : line_begin + 1u,
                        line_end == std::string_view::npos ? bytes.size() : line_end + 1u);
                }
            }

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

            auto checked = decode(*candidate, origin, diagnostics, mode);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, checked);

            const auto prefix = std::string{key} + ".";
            if (std::ranges::any_of(
                    checked->m_entries,
                    [&](const Entry &entry) { return entry.m_key == key or entry.m_key.starts_with(prefix); }) or
                std::ranges::any_of(checked->m_groups, [&](const GroupPresence &group) {
                    return group.m_path == key or group.m_path.starts_with(prefix);
                })) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, EConfigErrc::codec_error);
            }

            // Parser source regions for implicit tables can overlap descendants.
            // Prove that removing the selected subtree preserved all other values
            // and group presence, including unrelated empty groups.
            Document retained;
            for (const auto &entry: decoded->m_entries) {
                if (entry.m_key != key and not entry.m_key.starts_with(prefix)) {
                    retained.m_entries.push_back(entry);
                }
            }

            Document missing_ancestors;
            const EncodingIndex checked_index{*checked};
            for (const auto &group: decoded->m_groups) {
                if (group.m_path != key and not group.m_path.starts_with(prefix)) {
                    retained.m_groups.push_back(group);

                    if (not std::ranges::binary_search(
                            checked_index.m_groups, std::string_view{group.m_path}, {},
                            [](const GroupPresence *value) { return std::string_view{value->m_path}; }) and
                        key.starts_with(group.m_path + ".")) {
                        missing_ancestors.m_groups.push_back(group);
                    }
                }
            }

            // Removing the last descendant can erase an implicit ancestor. Keep its
            // presence by materializing an empty header without rewriting other bytes.
            if (not missing_ancestors.m_groups.empty()) {
                auto headers = encode(missing_ancestors);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, headers);
                const std::string_view eol = bytes.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
                std::string appended{eol};
                for (const char ch: *headers) {
                    if (ch == '\n') {
                        appended += eol;
                    } else {
                        appended += ch;
                    }
                }
                ScalarSourcePatch addition{candidate->size(), {}, std::move(appended)};
                candidate = applySourcePatch(*candidate, addition);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, candidate);
                checked = decode(*candidate, origin, diagnostics, mode);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, checked);

                // Fold the appended headers into the single removal patch.
                patch.m_before += bytes.substr(last);
                patch.m_after += bytes.substr(last);
                patch.m_after += addition.m_after;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, equalRetainedDocuments(retained, *checked));
            return ValidatedSourcePatch{std::move(patch), std::move(*candidate), std::move(*checked)};
        }
    } // namespace

    Expected<ScalarSourcePatch> insertRecord(
        const std::string_view bytes, const std::string_view key, const Document &record, const std::string_view origin,
        Diagnostics &diagnostics) {
        auto patch = insertSubtree(bytes, key, record, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, patch);
        return std::move(patch->m_patch);
    }

    Expected<ScalarSourcePatch> removeRecord(
        const std::string_view bytes, const std::string_view key, const std::string_view origin,
        Diagnostics &diagnostics) {
        auto patch = removeSubtree(bytes, key, origin, diagnostics, EDecodeMode::records);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, patch);
        return std::move(patch->m_patch);
    }

    Expected<ScalarSourcePatch> patchTree(
        const std::string_view bytes, const std::string_view key, const ETreeSourceEdit edit, const Document &subtree,
        const std::string_view origin, Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Config.File.PatchTree"};
        const auto fail = [&](const std::error_code error, const std::string_view message) {
            diagnostics.add(std::string{origin}, std::string{key}, std::string{message}, error);
            return error;
        };

        if (bytes.size() > 1024u * 1024u) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, fail(make_error_code(EConfigErrc::out_of_range), "Tree source exceeds the 1 MiB limit"));
        }

        if (not isValidCanonicalName(key) or
            (edit != ETreeSourceEdit::insert and
                edit != ETreeSourceEdit::remove and
                edit != ETreeSourceEdit::replace) or
            (edit == ETreeSourceEdit::remove and (not subtree.m_entries.empty() or not subtree.m_groups.empty()))) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ConfigFile, fail(std::make_error_code(std::errc::invalid_argument), "Invalid tree edit or subtree"));
        }

        std::string scratch{bytes};
        std::optional<Document> checked;
        if (edit != ETreeSourceEdit::insert) {
            auto removal = removeSubtree(scratch, key, origin, diagnostics, EDecodeMode::tree);
            if (not removal) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail(removal.error(), "Cannot remove tree subtree"));
            }

            scratch = std::move(removal->m_bytes);
            checked.emplace(std::move(removal->m_document));
        }

        if (edit != ETreeSourceEdit::remove) {
            auto insertion = insertSubtree(
                scratch, key, subtree, origin, diagnostics, EDecodeMode::tree,
                checked ? std::addressof(*checked) : nullptr);
            if (not insertion) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ConfigFile, fail(insertion.error(), "Cannot insert tree subtree"));
            }

            scratch = std::move(insertion->m_bytes);
        }

        // The scratch transaction is returned atomically as one reversible patch.
        std::size_t first = 0u;
        while (first < bytes.size() and first < scratch.size() and bytes[first] == scratch[first]) {
            ++first;
        }

        std::size_t suffix = 0u;
        while (suffix < bytes.size() - first and
               suffix < scratch.size() - first and
               bytes[bytes.size() - suffix - 1u] == scratch[scratch.size() - suffix - 1u]) {
            ++suffix;
        }
        return ScalarSourcePatch{
            first, std::string{bytes.substr(first, bytes.size() - first - suffix)},
            scratch.substr(first, scratch.size() - first - suffix)};
    }

    [[nodiscard]] Layer toLayer(Document document, std::string origin) {
        const hal::ProfileScope scope{"Config.File.ToLayer"};
        Layer layer{};
        layer.m_source = ESource::file;
        layer.m_origin = std::move(origin);
        layer.m_assignments.reserve(document.m_entries.size());

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

    // Slice 1a: canonical definition — retain group locations alongside scalar conversion.
    TreeLayer toTreeLayer(Document document, std::string origin) {
        auto groups = std::move(document.m_groups);
        return {toLayer(std::move(document), std::move(origin)), std::move(groups)};
    }
} // namespace pP::config
