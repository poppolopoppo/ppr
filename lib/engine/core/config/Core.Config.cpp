module;

#include "pP/Macros.h"

module engine.core;

// ------------------------------------------------------------------
// config
// ------------------------------------------------------------------

import :config;
import :hal;
import :logger;

import std;

namespace pP::config {
    PPR_DEFINE_LOG_CATEGORY(Config, debug, none)

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

    // Slice 1a: canonical definition — collection/reference keys are not paths.
    bool isValidKey(const std::string_view key) noexcept {
        if (key.empty() or key.size() > 128u) {
            return false;
        }
        for (const char ch: key) {
            if (not ((ch >= 'a' and ch <= 'z') or (ch >= '0' and ch <= '9') or ch == '_' or ch == '-')) {
                return false;
            }
        }
        return true;
    }

    std::string details::recursivePath(const std::string_view prefix, const std::string_view name) {
        std::string result;
        result.reserve(prefix.size() + (prefix.empty() ? 0u : 1u) + name.size());
        result.append(prefix);
        if (not prefix.empty()) {
            result.push_back('.');
        }
        result.append(name);
        return result;
    }

    details::ScratchText details::scratchPath(const std::string_view prefix, const std::string_view name) {
        ScratchText result;
        result.reserve(prefix.size() + (prefix.empty() ? 0u : 1u) + name.size());
        result.append(prefix);
        if (not prefix.empty()) {
            result.push_back('.');
        }
        result.append(name);
        return result;
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
        std::string location, std::string property, std::string message, std::error_code code, std::string alias,
        Array<std::string> related_properties) {
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
                std::move(related_properties),
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
            const std::string_view location, const std::string_view property, const std::string_view message,
            std::error_code code, std::string alias) {
            if (m_diagnostics != nullptr) {
                m_diagnostics->add(
                    std::string{location}, std::string{property}, std::string{message}, code, std::move(alias));
            }
        }

        SourcePathIndex::SourcePathIndex(const std::span<const Assignment> assignments) {
            m_paths.reserve(assignments.size());
            for (std::size_t position = 0u; position < assignments.size(); ++position) {
                m_paths.push_back({assignments[position].m_name, position});
            }
            sort_();
        }

        SourcePathIndex::SourcePathIndex(const std::span<const GroupPresence> groups) {
            m_paths.reserve(groups.size());
            for (std::size_t position = 0u; position < groups.size(); ++position) {
                m_paths.push_back({groups[position].m_path, position});
            }
            sort_();
        }

        SourcePathIndex::SourcePathIndex(const std::span<const ScratchText> paths) {
            m_paths.reserve(paths.size());
            for (std::size_t position = 0u; position < paths.size(); ++position) {
                m_paths.push_back({std::string_view{paths[position]}, position});
            }

            sort_();
        }

        void SourcePathIndex::sort_() {
            std::ranges::sort(m_paths, [](const Entry &left, const Entry &right) {
                return left.m_path < right.m_path or
                       (left.m_path == right.m_path and left.m_position < right.m_position);
            });
        }

        std::span<const SourcePathIndex::Entry> SourcePathIndex::exact(const std::string_view path) const noexcept {
            const auto first = std::ranges::lower_bound(m_paths, path, {}, &Entry::m_path);
            const auto last = std::upper_bound(
                first, m_paths.end(), path, [](const auto key, const Entry &entry) { return key < entry.m_path; });
            return {first, last};
        }

        bool SourcePathIndex::supplied(const std::string_view path, const bool descendants) const {
            const hal::ProfileScope scope{"Config.Index.Supplied"};
            if (not exact(path).empty()) {
                return true;
            }
            if (not descendants) {
                return false;
            }

            // Prefix siblings such as a-b may sort between a and its a.b descendants.
            ScratchText prefix{path};
            prefix.push_back('.');
            const auto first = std::ranges::lower_bound(m_paths, std::string_view{prefix}, {}, &Entry::m_path);
            return first != m_paths.end() and first->m_path.starts_with(prefix);
        }

        Array<std::size_t, mem::ScratchPad> SourcePathIndex::subtree(const std::string_view path) const {
            const hal::ProfileScope scope{"Config.Index.Subtree"};
            Array<std::size_t, mem::ScratchPad> positions;
            for (const auto &entry: exact(path)) {
                positions.push_back(entry.m_position);
            }

            ScratchText prefix{path};
            prefix.push_back('.');
            auto entry = std::ranges::lower_bound(m_paths, std::string_view{prefix}, {}, &Entry::m_path);
            while (entry != m_paths.end() and entry->m_path.starts_with(prefix)) {
                positions.push_back(entry->m_position);
                ++entry;
            }

            // Lookup order must not change layer-local winners or diagnostic ordering.
            std::ranges::sort(positions);
            return positions;
        }

        void ResolveState::indexSources() {
            const hal::ProfileScope scope{"Config.Resolve.IndexSources"};
            m_assignment_storage.clear();
            m_assignment_storage.reserve(m_layers.size());
            for (const auto *const layer: m_layers) {
                m_assignment_storage.emplace_back(std::span<const Assignment>{layer->m_assignments});
            }

            m_assignment_indices = m_assignment_storage;
        }

        void ResolveState::indexOrigins() {
            const hal::ProfileScope scope{"Config.Resolve.IndexOrigins"};
            if (m_origin_index_valid) {
                return;
            }

            m_origin_live.resize(m_origins->size(), 1u);
            for (std::size_t position = 0u; position < m_origins->size(); ++position) {
                m_origin_indices[ScratchText{(*m_origins)[position].m_name}].push_back(position);
            }

            m_origin_index_valid = true;
        }

        void ResolveState::finalizeOrigins() {
            if (m_origins == nullptr or not m_origin_index_valid) {
                return;
            }

            const hal::ProfileScope scope{"Config.Resolve.FinalizeOrigins"};
            std::size_t kept = 0u;
            for (std::size_t position = 0u; position < m_origins->size(); ++position) {
                if (m_origin_live[position] == 0u) {
                    continue;
                }

                if (kept != position) {
                    (*m_origins)[kept] = std::move((*m_origins)[position]);
                }
                ++kept;
            }

            m_origins->resize(kept);
            m_origin_indices.clear();
            m_origin_live.clear();
            m_origin_index_valid = false;
        }

        void ResolveState::upsertOrigin(
            const std::string_view name, const ESource source, const std::string_view location) {
            const hal::ProfileScope scope{"Config.Resolve.UpsertOrigin"};
            if (m_origins == nullptr) {
                return;
            }

            indexOrigins();
            const auto found = m_origin_indices.find(name);
            if (found != m_origin_indices.end()) {
                auto &origin = (*m_origins)[found->second.front()];
                origin.m_source = source;
                origin.m_location = location;
                return;
            }

            const auto position = m_origins->size();
            m_origins->push_back(PropertyOrigin{std::string{name}, source, std::string{location}});
            m_origin_live.push_back(1u);
            m_origin_indices[ScratchText{name}].push_back(position);
        }

        void ResolveState::rejectUnknown(const std::span<const std::string_view> names) {
            const hal::ProfileScope scope{"Config.Resolve.RejectUnknown"};
            for (const auto *const layer: m_layers) {
                for (const auto &assignment: layer->m_assignments) {
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

        void ResolveState::replaceOrigins(const std::string_view path, Array<PropertyOrigin> &replacement) {
            const hal::ProfileScope scope{"Config.Resolve.ReplaceOrigins"};
            PPR_ASSERT(m_origins != nullptr and m_origins != std::addressof(replacement));
            indexOrigins();

            const auto discard = [&](const auto iterator) {
                for (const auto position: iterator->second) {
                    m_origin_live[position] = 0u;
                }

                return m_origin_indices.erase(iterator);
            };

            if (const auto exact = m_origin_indices.find(path); exact != m_origin_indices.end()) {
                discard(exact);
            }

            ScratchText prefix{path};
            prefix.push_back('.');
            auto descendant = m_origin_indices.lower_bound(std::string_view{prefix});
            while (descendant != m_origin_indices.end() and descendant->first.starts_with(prefix)) {
                descendant = discard(descendant);
            }

            // Keep survivors in place until finalization, then append replacement duplicates in source order.
            for (auto &origin: replacement) {
                const auto position = m_origins->size();
                m_origins->push_back(std::move(origin));
                m_origin_live.push_back(1u);
                m_origin_indices[ScratchText{m_origins->back().m_name}].push_back(position);
            }
        }

        void TreeResolveState::report(
            const std::string_view location, const std::string_view path, const std::string_view message,
            const std::error_code error) {
            m_scalars.report(location, path, message, error);
            m_scalars.retain(error);
        }

        bool TreeResolveState::supplied(const std::string_view path, const bool descendants) const {
            const hal::ProfileScope scope{"Config.Tree.Supplied"};
            for (std::size_t layer = 0u; layer < m_layers.size(); ++layer) {
                if (m_scalars.m_assignment_indices[layer].supplied(path, descendants) or
                    (descendants and m_group_indices[layer].supplied(path, true))) {
                    return true;
                }
            }
            return false;
        }

        void TreeResolveState::indexSources() {
            const hal::ProfileScope scope{"Config.Tree.IndexSources"};
            m_scalars.indexSources();
            m_group_storage.clear();
            m_group_storage.reserve(m_layers.size());
            for (const auto &layer: m_layers) {
                m_group_storage.emplace_back(std::span<const GroupPresence>{layer.m_groups});
            }

            m_group_indices = m_group_storage;
        }

        void TreeResolveState::presence(const std::string_view path, const Metadata &metadata, const bool required) {
            const hal::ProfileScope scope{"Config.Tree.Presence"};
            m_group_paths.emplace_back(path);
            m_scalars.upsertOrigin(path, ESource::none, "default");
            bool found = false;
            for (std::size_t layer_index = 0u; layer_index < m_layers.size(); ++layer_index) {
                const auto &layer = m_layers[layer_index];
                const auto source = layer.m_scalars.m_source;
                const bool allowed = source == ESource::none or any(metadata.m_sources & source);
                const std::string *winning_location = nullptr;
                const auto note = [&](const std::string &location) {
                    found = true;
                    if (not allowed) {
                        report(
                            location, path, "group does not accept this source",
                            make_error_code(EConfigErrc::source_disallowed));
                    } else {
                        winning_location = std::addressof(location);
                    }
                };
                // Both explicit empty tables and descendant assignments supply presence.
                for (const auto position: m_group_indices[layer_index].subtree(path)) {
                    note(layer.m_groups[position].m_location);
                }
                for (const auto position: m_scalars.m_assignment_indices[layer_index].subtree(path)) {
                    note(layer.m_scalars.m_assignments[position].m_location);
                }

                if (winning_location != nullptr) {
                    m_scalars.upsertOrigin(path, source, *winning_location);
                }
            }
            if (required and not found) {
                report("resolve", path, "required group was not supplied", make_error_code(EConfigErrc::missing_value));
            }
        }

        Array<std::string_view, mem::ScratchPad> TreeResolveState::entryKeys(const std::string_view path) {
            const hal::ProfileScope scope{"Config.Tree.EntryKeys"};
            Array<std::string_view, mem::ScratchPad> keys;
            std::set<std::string_view, std::less<>, mem::STL<std::string_view, mem::ScratchPad>> membership;
            ScratchText prefix{path};
            prefix.push_back('.');

            const auto add = [&](const std::string &name, const std::string &location) {
                if (not name.starts_with(prefix)) {
                    return;
                }
                const std::string_view tail{std::string_view{name}.substr(prefix.size())};
                const auto key = tail.substr(0u, tail.find('.'));
                if (not isValidKey(key)) {
                    report(
                        location, recursivePath(path, key), "invalid collection key ([a-z0-9_-], 1..128)",
                        make_error_code(EConfigErrc::out_of_range));
                } else if (membership.insert(key).second) {
                    keys.emplace_back(key);
                }
            };
            for (std::size_t layer_index = 0u; layer_index < m_layers.size(); ++layer_index) {
                const auto &layer = m_layers[layer_index];
                for (const auto position: m_group_indices[layer_index].subtree(path)) {
                    const auto &group = layer.m_groups[position];
                    add(group.m_path, group.m_location);
                }
                for (const auto position: m_scalars.m_assignment_indices[layer_index].subtree(path)) {
                    const auto &assignment = layer.m_scalars.m_assignments[position];
                    add(assignment.m_name, assignment.m_location);
                }
            }
            return keys;
        }

        void TreeResolveState::rejectUnknown() {
            const hal::ProfileScope scope{"Config.Tree.RejectUnknown"};
            const SourcePathIndex scalar_paths{m_scalar_paths};
            const SourcePathIndex group_paths{m_group_paths};
            const SourcePathIndex atomic_paths{m_atomic_paths};

            const auto delegated = [&](std::string_view path) {
                for (;;) {
                    if (not atomic_paths.exact(path).empty()) {
                        return true;
                    }

                    const auto dot = path.rfind('.');
                    if (dot == std::string_view::npos) {
                        return false;
                    }

                    path = path.substr(0u, dot);
                }
            };

            for (std::size_t layer_index = 0u; layer_index < m_layers.size(); ++layer_index) {
                const auto &layer = m_layers[layer_index];
                const auto scoped_groups = m_scope.empty() ? Array<std::size_t, mem::ScratchPad>{}
                                                           : m_group_indices[layer_index].subtree(m_scope);
                const auto group_count = m_scope.empty() ? layer.m_groups.size() : scoped_groups.size();
                for (std::size_t position = 0u; position < group_count; ++position) {
                    const auto i = m_scope.empty() ? position : scoped_groups[position];
                    const auto &group = layer.m_groups[i];

                    if (delegated(group.m_path)) {
                        continue;
                    }

                    if (group_paths.exact(group.m_path).empty()) {
                        report(
                            group.m_location, group.m_path, "unknown group",
                            make_error_code(EConfigErrc::unknown_property));
                    }

                    const auto matches = m_group_indices[layer_index].exact(group.m_path);
                    if (not matches.empty() and matches.front().m_position < i) {
                        report(
                            group.m_location, group.m_path, "duplicate group declaration in layer",
                            make_error_code(EConfigErrc::duplicate_key));
                    }
                }

                const auto scoped_scalars = m_scope.empty()
                                                ? Array<std::size_t, mem::ScratchPad>{}
                                                : m_scalars.m_assignment_indices[layer_index].subtree(m_scope);
                const auto scalar_count = m_scope.empty() ? layer.m_scalars.m_assignments.size()
                                                          : scoped_scalars.size();
                for (std::size_t position = 0u; position < scalar_count; ++position) {
                    const auto i = m_scope.empty() ? position : scoped_scalars[position];
                    const auto &assignment = layer.m_scalars.m_assignments[i];

                    if (delegated(assignment.m_name)) {
                        continue;
                    }

                    if (m_atomic_replacement) {
                        const auto matches = m_scalars.m_assignment_indices[layer_index].exact(assignment.m_name);
                        if (not matches.empty() and matches.front().m_position < i) {
                            report(
                                assignment.m_location, assignment.m_name, "duplicate key in variant replacement",
                                make_error_code(EConfigErrc::duplicate_key));
                        }
                    }

                    if (not scalar_paths.exact(assignment.m_name).empty()) {
                        continue;
                    }

                    const bool group = not group_paths.exact(assignment.m_name).empty();
                    report(
                        assignment.m_location, assignment.m_name,
                        group ? "group/collection scalar assignment or reset is not supported" : "unknown property",
                        make_error_code(group ? EConfigErrc::type_mismatch : EConfigErrc::unknown_property));
                }
            }
        }
    } // namespace details
} // namespace pP::config
