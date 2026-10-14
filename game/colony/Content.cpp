module;
#include "pP/Macros.h"
module game.colony.content;
import engine.core;
import engine.sim;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ColonyContent, info, none)

    // ------------------------------------------------------------------
    // decode helpers
    // ------------------------------------------------------------------

    namespace {
        using namespace config;
        constexpr Metadata metadata(const std::string_view name) {
            return {name, "World content", "Content", EMutability::startup, ESource::file};
        }
        constexpr std::array<ContentDocumentBinding, kContentDocumentCount> kBindings{
            {{EContentDocument::materials, "materials", "Materials", "materials", false},
                {EContentDocument::biomes, "biomes", "Biomes", "biomes", false},
                {EContentDocument::fields, "fields", "Fields", "fields", false},
                {EContentDocument::features, "features", "Features", "features", false},
                {EContentDocument::worlds, "worlds", "Worlds", "worlds", false},
                {EContentDocument::rules, "rules", "Rules", "worlds.{key}.rules", true, 2u},
                {EContentDocument::appearance, "appearance", "Appearance", "appearance", false, 2u}}};
        struct DocumentEnvelope {
            u32 m_schema_version{};
            static constexpr auto describe() {
                return std::tuple{field(
                    &DocumentEnvelope::m_schema_version, metadata("schema_version"),
                    Range{kContentSchemaVersion, kContentSchemaVersion}, EPresence::required)};
            }
        };
        struct ManifestFiles {
            std::string m_materials{}, m_biomes{}, m_fields{}, m_features{}, m_worlds{}, m_rules{}, m_appearance{};
            static constexpr auto describe() {
                return std::tuple{
                    field(
                        &ManifestFiles::m_materials, metadata("materials"), StringDomain{1024u, false},
                        EPresence::required),
                    field(
                        &ManifestFiles::m_biomes, metadata("biomes"), StringDomain{1024u, false}, EPresence::required),
                    field(
                        &ManifestFiles::m_fields, metadata("fields"), StringDomain{1024u, false}, EPresence::required),
                    field(
                        &ManifestFiles::m_features, metadata("features"), StringDomain{1024u, false},
                        EPresence::required),
                    field(
                        &ManifestFiles::m_worlds, metadata("worlds"), StringDomain{1024u, false}, EPresence::required),
                    field(&ManifestFiles::m_rules, metadata("rules"), StringDomain{1024u, false}, EPresence::required),
                    field(
                        &ManifestFiles::m_appearance, metadata("appearance"), StringDomain{1024u, false},
                        EPresence::required)};
            }
        };
        struct Manifest : DocumentEnvelope {
            std::string m_pack_id{};
            ManifestFiles m_files{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    DocumentEnvelope::describe(),
                    std::tuple{
                        field(
                            &Manifest::m_pack_id, metadata("pack_id"), StringDomain{128u, false}, EPresence::required),
                        group(&Manifest::m_files, metadata("files"), EPresence::required)});
            }
        };
        struct WorldGenDefinitionEnvelope : sim::definition::Definition {
            u32 m_definition_schema_version{};
            static constexpr auto describe() {
                return std::tuple_cat(
                    std::tuple{field(
                        &WorldGenDefinitionEnvelope::m_definition_schema_version, metadata("definition_schema_version"),
                        Range{sim::definition::kDefinitionSchemaVersion, sim::definition::kDefinitionSchemaVersion},
                        EPresence::required)},
                    sim::definition::Definition::describe());
            }
        };
        std::error_code reject(
            Diagnostics &diagnostics, const std::string_view location, const std::string_view path,
            const std::string_view message, const EConfigErrc code = EConfigErrc::cross_field_invalid) {
            const auto error = config::make_error_code(code);
            diagnostics.add(std::string{location}, std::string{path}, std::string{message}, error);
            return error;
        }
        bool validName(const std::string_view name) noexcept {
            return not name.empty() and
                   name.front() >= 'a' and
                   name.front() <= 'z' and
                   std::ranges::all_of(name, [](const char c) {
                       return (c >= 'a' and c <= 'z') or (c >= '0' and c <= '9') or c == '_';
                   });
        }
        bool validTexturePath(const std::string_view path) noexcept {
            if (path.empty() or path.front() == '/' or path.find_first_of("\\:\0", 0u, 3u) != std::string_view::npos) {
                return false;
            }
            std::size_t start = 0u;
            while (start < path.size()) {
                const auto end = path.find('/', start);
                const auto segment = path.substr(
                    start, end == std::string_view::npos ? path.size() - start : end - start);
                if (segment.empty() or segment == "." or segment == "..") {
                    return false;
                }
                if (end == std::string_view::npos) {
                    return true;
                }
                start = end + 1u;
            }
            return false;
        }
        std::string_view location(const Document &document, std::string_view path) {
            while (not path.empty()) {
                const auto entry = std::ranges::find(document.m_entries, path, &Entry::m_key);
                if (entry != document.m_entries.end()) {
                    return entry->m_location;
                }
                const auto group = std::ranges::find(document.m_groups, path, &GroupPresence::m_path);
                if (group != document.m_groups.end()) {
                    return group->m_location;
                }
                const auto dot = path.rfind('.');
                path = dot == std::string_view::npos ? std::string_view{} : path.substr(0u, dot);
            }
            return "pack";
        }
        void appendDiagnostics(Diagnostics &to, const Diagnostics &from, const Document &document) {
            for (const auto &entry: from.entries()) {
                to.add(
                    std::string{location(document, entry.m_property)}, entry.m_property, entry.m_message, entry.m_code,
                    entry.m_alias, entry.m_related_properties);
            }
            to.addOmitted(from.omittedCount());
        }
        std::string_view documentText(const Document &document, const std::string_view path) {
            const auto entry = std::ranges::find(document.m_entries, path, &Entry::m_key);
            return entry != document.m_entries.end() and entry->m_value.m_kind == EFileKind::text
                       ? std::string_view{entry->m_value.m_text}
                       : std::string_view{};
        }
        Array<std::string> keys(const Document &document, const std::string_view root) {
            const std::string prefix = std::string{root} + ".";
            Array<std::string> result;
            const auto add = [&](const std::string_view path) {
                if (path.starts_with(prefix)) {
                    const auto tail = path.substr(prefix.size());
                    const auto dot = tail.find('.');
                    const auto key = tail.substr(0u, dot);
                    if (not key.empty()) {
                        result.emplace_back(key);
                    }
                }
            };
            for (const auto &entry: document.m_entries) {
                add(entry.m_key);
            }
            for (const auto &group: document.m_groups) {
                add(group.m_path);
            }
            std::ranges::sort(result);
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }
        bool schemaMatches(std::string_view schema, std::string_view path) {
            while (not schema.empty() and not path.empty()) {
                const auto schema_dot = schema.find('.');
                const auto path_dot = path.find('.');
                const auto a = schema.substr(0u, schema_dot);
                const auto b = path.substr(0u, path_dot);
                if (a != "{key}" and a != b) {
                    return false;
                }
                if (schema_dot == std::string_view::npos or path_dot == std::string_view::npos) {
                    return schema_dot == path_dot;
                }
                schema.remove_prefix(schema_dot + 1u);
                path.remove_prefix(path_dot + 1u);
            }
            return schema.empty() and path.empty();
        }
        template<typename T>
        const T *findEntry(const KeyedCollection<T> &collection, const std::string_view key) {
            const auto entry = std::ranges::find(collection.m_entries, key, &KeyedEntry<T>::m_key);
            return entry == collection.m_entries.end() ? nullptr : std::addressof(entry->m_value);
        }
        template<typename T>
        Array<const KeyedEntry<T> *> sorted(const KeyedCollection<T> &collection) {
            Array<const KeyedEntry<T> *> result;
            for (const auto &entry: collection.m_entries) {
                result.push_back(std::addressof(entry));
            }
            std::ranges::sort(result, {}, [](const auto *entry) { return entry->m_key; });
            return result;
        }
        Expected<u32> color(
            const std::string &value, const std::string &path, const Document &document, Diagnostics &diagnostics) {
            u32 result{};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result, 16);
            if (value.size() != 8u or error != std::errc{} or end != value.data() + value.size()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(document, path), path,
                                       "Expected exactly eight hexadecimal RRGGBBAA digits"));
            }
            return result;
        }
    }

    // ------------------------------------------------------------------
    // document bindings
    // ------------------------------------------------------------------

    std::span<const ContentDocumentBinding> contentDocumentBindings() noexcept {
        return kBindings;
    }
    std::string contentDocumentPath(
        const EContentDocument category, const std::string_view key, const std::string_view relative) {
        const auto index = static_cast<std::size_t>(category);
        if (index >= kBindings.size()) {
            return {};
        }
        if (not key.empty()) {
            std::size_t depth = 0u;
            std::string_view remaining = key;
            while (not remaining.empty()) {
                const auto dot = remaining.find('.');
                if (not config::isValidKey(remaining.substr(0u, dot))) {
                    return {};
                }
                ++depth;
                if (dot == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(dot + 1u);
                if (remaining.empty()) {
                    return {};
                }
            }
            if (depth != kBindings[index].m_definition_depth) {
                return {};
            }
        }
        std::string result{kBindings[index].m_collection};
        if (not key.empty()) {
            result += ".";
            result += key;
        }
        if (not relative.empty()) {
            result += ".";
            result += relative;
        }
        return result;
    }
    std::string contentCanonicalPath(const EContentDocument category, const std::string_view path) {
        if (category != EContentDocument::rules) {
            return std::string{path};
        }
        if (path == "rules") {
            return "worlds";
        }
        if (not path.starts_with("rules.")) {
            return {};
        }
        const auto tail = path.substr(6u);
        const auto dot = tail.find('.');
        std::string result = "worlds." + std::string{tail.substr(0u, dot)};
        if (dot != std::string_view::npos) {
            result += ".rules.";
            result += tail.substr(dot + 1u);
        }
        return result;
    }

    std::optional<std::pair<EContentDocument, std::string>> contentSourcePath(const std::string_view canonical_path) {
        const auto match = [&](const ContentDocumentBinding &binding) -> std::optional<std::string> {
            std::string_view schema = binding.m_schema_root;
            std::string_view remaining = canonical_path;
            std::string result{binding.m_collection};
            while (not schema.empty()) {
                if (remaining.empty()) {
                    return std::nullopt;
                }

                const auto schema_dot = schema.find('.');
                const auto path_dot = remaining.find('.');
                const auto expected = schema.substr(0u, schema_dot);
                const auto actual = remaining.substr(0u, path_dot);

                if (expected == "{key}") {
                    if (not config::isValidKey(actual)) {
                        return std::nullopt;
                    }
                    result += ".";
                    result += actual;
                } else if (expected != actual) {
                    return std::nullopt;
                }

                schema = schema_dot == std::string_view::npos ? std::string_view{} : schema.substr(schema_dot + 1u);
                remaining = path_dot == std::string_view::npos ? std::string_view{} : remaining.substr(path_dot + 1u);
            }

            if (not remaining.empty()) {
                result += ".";
                result += remaining;
            }

            return result;
        };

        // An attachment owns its subtree even when a broader document owns its ancestor.
        for (const bool attachments: {true, false}) {
            for (const auto &binding: kBindings) {
                if (binding.m_attached_rules == attachments) {
                    if (auto path = match(binding)) {
                        return std::pair{binding.m_document, std::move(*path)};
                    }
                }
            }
        }
        return std::nullopt;
    }

    // ------------------------------------------------------------------
    // document merge
    // ------------------------------------------------------------------

    Expected<Document> mergeContentDocuments(
        const std::array<Document, kContentDocumentCount> &documents, Diagnostics &diagnostics) {
        Document merged;
        const auto &worlds = documents[static_cast<std::size_t>(EContentDocument::worlds)];
        const auto owners = keys(worlds, "worlds");
        const auto &appearance = documents[static_cast<std::size_t>(EContentDocument::appearance)];
        for (const auto &binding: kBindings) {
            if (binding.m_attached_rules or binding.m_document == EContentDocument::appearance) {
                continue;
            }
            const auto declared = keys(documents[static_cast<std::size_t>(binding.m_document)], binding.m_collection);
            const std::string root = "appearance." + std::string{binding.m_collection};
            for (const auto &key: keys(appearance, root)) {
                if (std::ranges::find(declared, key) == declared.end()) {
                    const std::string path = root + "." + key;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(diagnostics, location(appearance, path), path, "Appearance owner does not exist"));
                }
            }
        }

        for (std::size_t index = 0u; index < documents.size(); ++index) {
            const auto &binding = kBindings[index];
            const auto &document = documents[index];
            Document envelope;
            for (const auto &entry: document.m_entries) {
                if (entry.m_key == "schema_version") {
                    envelope.m_entries.push_back(entry);
                }
            }
            const TreeLayer envelope_layers[] = {toTreeLayer(envelope, "pack")};
            auto version = resolveRecursive<DocumentEnvelope>(envelope_layers, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, version);

            const auto validate = [&](const std::string_view path, const std::string_view source) -> std::error_code {
                const std::string prefix = std::string{binding.m_collection} + ".";
                if (path != binding.m_collection and not path.starts_with(prefix)) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, source, path, "Document does not own this schema root",
                                           EConfigErrc::unknown_property));
                }
                if (binding.m_attached_rules and path.starts_with(prefix)) {
                    const auto tail = path.substr(prefix.size());
                    const auto owner = tail.substr(0u, tail.find('.'));
                    const std::string declaration = "worlds." + std::string{owner};
                    // A rule cannot synthesize a profile: require its explicit worlds.toml declaration.
                    if (std::ranges::find(owners, owner) == owners.end() or
                        std::ranges::none_of(
                            worlds.m_groups, [&](const auto &group) { return group.m_path == declaration; })) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            ColonyContent,
                            reject(diagnostics, source, path, "Rule owner must be explicitly declared in worlds.toml"));
                    }
                }
                if (binding.m_document == EContentDocument::worlds and path.starts_with("worlds.")) {
                    const auto tail = path.substr(7u);
                    const auto dot = tail.find('.');
                    if (dot != std::string_view::npos and
                        (tail.substr(dot + 1u) == "rules" or tail.substr(dot + 1u).starts_with("rules."))) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            ColonyContent, reject(
                                               diagnostics, source, path, "Profile rules are owned by rules.toml",
                                               EConfigErrc::unknown_property));
                    }
                }
                return {};
            };
            for (const auto &entry: document.m_entries) {
                if (entry.m_key == "schema_version") {
                    continue;
                }
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, validate(entry.m_key, entry.m_location));
                Entry mapped = entry;
                mapped.m_key = contentCanonicalPath(binding.m_document, entry.m_key);
                if (std::ranges::find(merged.m_entries, mapped.m_key, &Entry::m_key) != merged.m_entries.end() or
                    std::ranges::find(merged.m_groups, mapped.m_key, &GroupPresence::m_path) != merged.m_groups.end()) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, entry.m_location, mapped.m_key,
                                           "Multiple sources target the same destination", EConfigErrc::duplicate_key));
                }
                merged.m_entries.push_back(std::move(mapped));
            }
            for (const auto &group: document.m_groups) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, validate(group.m_path, group.m_location));
                // Rule document framing is not a second declaration of the profile/world collection.
                if (binding.m_attached_rules and
                    (group.m_path == "rules" or group.m_path.find('.', 6u) == std::string::npos)) {
                    continue;
                }
                GroupPresence mapped{contentCanonicalPath(binding.m_document, group.m_path), group.m_location};
                if (std::ranges::find(merged.m_groups, mapped.m_path, &GroupPresence::m_path) !=
                        merged.m_groups.end() or
                    std::ranges::find(merged.m_entries, mapped.m_path, &Entry::m_key) != merged.m_entries.end()) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, group.m_location, mapped.m_path,
                            "Multiple sources declare the same destination", EConfigErrc::duplicate_key));
                }
                merged.m_groups.push_back(std::move(mapped));
            }
        }
        return merged;
    }

    Expected<PackDefinition> decodePackDefinition(const ContentPackSource &source, Diagnostics &diagnostics) {
        std::array<Document, kContentDocumentCount> documents;
        for (std::size_t index = 0u; index < documents.size(); ++index) {
            const auto &input = source.m_documents[index];
            if (input.m_bytes.size() > kMaxMaterialDocumentBytes) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, input.m_path.string(), "", "Content document exceeds 1 MiB",
                                       EConfigErrc::out_of_range));
            }
            auto document = decode(input.m_bytes, input.m_path.string(), diagnostics, EDecodeMode::tree);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
            documents[index] = std::move(*document);
        }
        auto document = mergeContentDocuments(documents, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        return resolvePackDefinition(*document, diagnostics);
    }

    Expected<PackDefinition> resolvePackDefinition(const Document &document, Diagnostics &diagnostics) {
        const TreeLayer layers[] = {toTreeLayer(document, "pack")};
        Diagnostics local{128u};
        auto resolved = resolveRecursive<PackDefinition>(layers, local);
        if (resolved) {
            const auto error = sim::definition::validateDefinition(resolved->m_value, local);
            appendDiagnostics(diagnostics, local, document);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        } else {
            appendDiagnostics(diagnostics, local, document);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
        }
        return std::move(resolved->m_value);
    }

    // ------------------------------------------------------------------
    // property metadata
    // ------------------------------------------------------------------

    std::optional<ContentPropertyDescriptor> contentPropertyDescriptor(
        const EContentDocument category, const std::string_view relative, const Document &,
        const std::string_view key) {
        const auto path = contentCanonicalPath(category, contentDocumentPath(category, key, relative));
        std::optional<ContentPropertyDescriptor> result;
        config::describeRecursive<PackDefinition>([&](const std::string &schema_path, const auto &member, const auto) {
            const auto descriptor = config::describeProperty(member);
            const bool tag = descriptor.m_kind == ENodeKind::variant and
                             path.ends_with(".tag") and
                             schemaMatches(schema_path, std::string_view{path}.substr(0u, path.size() - 4u));
            if (schemaMatches(schema_path, path) or tag) {
                ContentPropertyDescriptor value;
                static_cast<PropertyDescriptor &>(value) = descriptor;
                if (tag) {
                    value.m_kind = ENodeKind::scalar;
                }
                for (const auto &binding: kBindings) {
                    if (value.m_reference_target and value.m_reference_target->m_collection == binding.m_collection) {
                        value.m_reference_category = binding.m_document;
                    }
                }
                result = std::move(value);
            }
        });
        return result;
    }

    Array<std::string> contentPropertyReferenceChoices(
        const EContentDocument category, const std::string_view relative,
        const std::array<Document, kContentDocumentCount> &documents, const std::string_view key) {
        const auto index = static_cast<std::size_t>(category);
        if (index >= documents.size()) {
            return {};
        }
        const auto descriptor = contentPropertyDescriptor(category, relative, documents[index], key);
        if (not descriptor or not descriptor->m_reference_target) {
            return {};
        }
        // Merge raw paths only: incomplete/invalid drafts must retain reference editing.
        Document raw;
        for (std::size_t i = 0u; i < documents.size(); ++i) {
            for (const auto &entry: documents[i].m_entries) {
                auto copy = entry;
                copy.m_key = contentCanonicalPath(kBindings[i].m_document, copy.m_key);
                raw.m_entries.push_back(std::move(copy));
            }
            for (const auto &group: documents[i].m_groups) {
                raw.m_groups.push_back({contentCanonicalPath(kBindings[i].m_document, group.m_path), group.m_location});
            }
        }
        const auto path = contentCanonicalPath(category, contentDocumentPath(category, key, relative));
        const auto &target = *descriptor->m_reference_target;
        std::string root{target.m_collection};
        const auto profile_end = path.find('.', 7u);
        const std::string profile = path.starts_with("worlds.") ? path.substr(0u, profile_end) : std::string{};
        if (target.m_scope == EReferenceScope::owner) {
            if (target.m_collection == "rules") {
                root = profile + ".rules";
            }
            if (target.m_collection == "markers") {
                std::string feature;
                if (path.starts_with(profile + ".rules.")) {
                    const auto rule_end = path.find('.', profile.size() + 7u);
                    const std::string rule = path.substr(0u, rule_end);
                    const auto placement = documentText(raw, rule + ".payload.matter_fill.placement");
                    feature = documentText(
                        raw, profile + ".rules." + std::string{placement} + ".payload.feature.feature");
                } else {
                    feature = documentText(raw, profile + ".start.feature");
                }
                if (feature.empty()) {
                    return {};
                }
                root = "features." + feature + ".markers";
            }
        }
        auto result = keys(raw, root);
        std::erase_if(result, [&](const auto &choice) {
            const auto base = root + "." + choice;
            if (target.m_collection == "rules") {
                const auto enabled = std::ranges::find(raw.m_entries, base + ".enabled", &Entry::m_key);
                return documentText(raw, base + ".payload.tag") != "feature" or
                       (enabled != raw.m_entries.end() and
                           enabled->m_value.m_kind == EFileKind::boolean and
                           not enabled->m_value.m_boolean);
            }
            if (target.m_collection == "markers") {
                return documentText(raw, base + ".role") != target.m_suitability;
            }
            if (target.m_collection == "materials") {
                const auto representation = documentText(raw, base + ".representation");
                return target.m_suitability == "particle" ? representation != "particle" : representation != "terrain";
            }
            if (target.m_collection == "biomes" and
                not profile.empty() and
                not path.starts_with(profile + ".biomes.")) {
                for (const auto &enabled: keys(raw, profile + ".biomes")) {
                    if (documentText(raw, profile + ".biomes." + enabled + ".biome") == choice) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        });
        if (target.m_suitability == "terrain_or_vacuum") {
            result.insert(result.begin(), "vacuum");
        }
        return result;
    }

    // ------------------------------------------------------------------
    // pack manifest and source
    // ------------------------------------------------------------------

    Expected<ContentPackManifest> decodePackManifest(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) {
        if (bytes.size() > kMaxPackManifestBytes) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent,
                reject(diagnostics, origin, "", "Pack manifest exceeds 64 KiB", EConfigErrc::out_of_range));
        }
        auto document = decode(bytes, origin, diagnostics, EDecodeMode::tree);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        const TreeLayer layers[] = {toTreeLayer(*document, std::string{origin})};
        auto resolved = resolveRecursive<Manifest>(layers, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
        if (not validName(resolved->m_value.m_pack_id)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ColonyContent, reject(diagnostics, origin, "pack_id", "Expected a lowercase stable pack name"));
        }
        const auto &files = resolved->m_value.m_files;
        ContentPackManifest result{
            .m_pack_id = resolved->m_value.m_pack_id,
            .m_files = {
                files.m_materials, files.m_biomes, files.m_fields, files.m_features, files.m_worlds, files.m_rules,
                files.m_appearance}};
        for (std::size_t index = 0u; index < result.m_files.size(); ++index) {
            const auto &path = result.m_files[index];
            const std::string property = "files." + std::string{kBindings[index].m_collection};
            result.m_file_locations[index] = location(*document, property);
            if (not validTexturePath(path) or not path.ends_with(".toml") or path == "pack.toml") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(*document, property), property,
                                       "Expected a distinct pack-relative TOML file without dot segments"));
            }
            for (std::size_t previous = 0u; previous < index; ++previous) {
                if (path == result.m_files[previous]) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(*document, property), property,
                                           "Manifest file is listed more than once", EConfigErrc::duplicate_key));
                }
            }
        }
        return result;
    }
    Expected<ContentPackSource> loadContentPackSource(
        const std::filesystem::path &root, Diagnostics &diagnostics, const IContext *const context) {
        const auto cancelled = [&]() -> std::error_code { return context ? context->error() : std::error_code{}; };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        std::error_code error{};
        const auto canonical_root = std::filesystem::canonical(root, error);
        if (error) {
            diagnostics.add(root.string(), "pack", "Cannot resolve pack root", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        }
        const auto origin = (canonical_root / "pack.toml").string();
        const auto read = [&](const std::string &relative, const std::string_view key, const std::string_view location,
                              const std::size_t limit) -> Expected<ContentSourceDocument> {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            std::error_code file_error{};
            const auto path = std::filesystem::canonical(canonical_root / relative, file_error);
            if (file_error) {
                diagnostics.add(
                    std::string{location}, std::string{key}, "Cannot resolve pack file " + relative, file_error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            }
            const auto within = path.lexically_relative(canonical_root);
            if (within.empty() or within.is_absolute() or *within.begin() == "..") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location, key, "Resolved file escapes the pack root"));
            }
            const bool regular = std::filesystem::is_regular_file(path, file_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            if (not regular) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location, key, "Pack entry must be a regular file"));
            }
            const auto size = std::filesystem::file_size(path, file_error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, file_error);
            if (size > limit) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(diagnostics, location, key, "Pack file exceeds its byte limit", EConfigErrc::out_of_range));
            }
            ContentSourceDocument source{.m_path = path};
            if (size == 0u) {
                return source;
            }
            auto mapped = io::mapFile(path);
            // Core mapping translates allocator failure; this application's
            // allocation policy is fatal, including at this I/O boundary.
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not mapped) {
                diagnostics.add(
                    std::string{location}, std::string{key}, "Cannot map pack file " + relative, mapped.error());
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, mapped);
            }
            if (mapped->size() != size) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::resource_unavailable_try_again);
            }
            source.m_bytes.assign(mapped->c_str(), mapped->size());
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            return source;
        };
        auto manifest_source = read("pack.toml", "pack", origin, kMaxPackManifestBytes);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest_source);
        auto manifest = decodePackManifest(manifest_source->m_bytes, origin, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest);
        ContentPackSource source{.m_manifest = std::move(*manifest)};
        for (std::size_t index = 0u; index < kContentDocumentCount; ++index) {
            const auto &location = source.m_manifest.m_file_locations[index];
            const std::string property = "files." + std::string{kBindings[index].m_collection};
            auto document = read(source.m_manifest.m_files[index], property, location, kMaxMaterialDocumentBytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
            if (std::filesystem::equivalent(document->m_path, manifest_source->m_path, error)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location, property, "A content document aliases pack.toml",
                                       EConfigErrc::duplicate_key));
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
            for (std::size_t previous = 0u; previous < index; ++previous) {
                if (std::filesystem::equivalent(document->m_path, source.m_documents[previous].m_path, error)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location, property, "Manifest entries resolve to the same file",
                                           EConfigErrc::duplicate_key));
                }
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
            }
            source.m_documents[index] = std::move(*document);
        }
        source.m_manifest_source = std::move(*manifest_source);
        return source;
    }

    // ------------------------------------------------------------------
    // worldgen definition envelope
    // ------------------------------------------------------------------

    Expected<sim::definition::Definition> decodeWorldGenDefinition(
        const std::string_view bytes, const std::string_view origin, Diagnostics &diagnostics) try {
        auto document = decode(bytes, origin, diagnostics, EDecodeMode::tree);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        const TreeLayer layers[] = {toTreeLayer(*document, std::string{origin})};
        Diagnostics local{64u};
        auto resolved = resolveRecursive<WorldGenDefinitionEnvelope>(layers, local);
        const auto error = resolved ? sim::definition::validateDefinition(resolved->m_value, local) : resolved.error();

        // Sim reports logical paths only. Prefer the exact scalar's location,
        // then the nearest authored table, then the supplied document origin.
        for (const auto &diagnostic: local.entries()) {
            std::string_view location = origin;
            std::string_view path = diagnostic.m_property;
            while (not path.empty()) {
                const auto entry = std::ranges::find(document->m_entries, path, &Entry::m_key);
                if (entry != document->m_entries.end()) {
                    location = entry->m_location;
                    break;
                }
                const auto group = std::ranges::find(document->m_groups, path, &GroupPresence::m_path);
                if (group != document->m_groups.end()) {
                    location = group->m_location;
                    break;
                }
                const auto dot = path.rfind('.');
                path = dot == std::string_view::npos ? std::string_view{} : path.substr(0u, dot);
            }
            diagnostics.add(
                std::string{location}, diagnostic.m_property, diagnostic.m_message, diagnostic.m_code,
                diagnostic.m_alias, diagnostic.m_related_properties);
        }
        diagnostics.addOmitted(local.omittedCount());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        return std::move(static_cast<sim::definition::Definition &>(resolved->m_value));
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }

    // ------------------------------------------------------------------
    // document projection
    // ------------------------------------------------------------------

    Array<std::string> contentDefinitionKeys(const EContentDocument category, const Document &document) {
        const auto index = static_cast<std::size_t>(category);
        if (index >= kBindings.size()) {
            return {};
        }
        const auto &binding = kBindings[index];
        Array<std::string> result;
        for (const auto &first: keys(document, binding.m_collection)) {
            if (binding.m_definition_depth == 1u) {
                result.push_back(first);
            } else {
                for (const auto &second: keys(document, std::string{binding.m_collection} + "." + first)) {
                    result.push_back(first + "." + second);
                }
            }
        }
        return result;
    }

    Expected<Document> projectContentDocument(const EContentDocument category, const PackDefinition &definition) {
        const auto index = static_cast<std::size_t>(category);
        if (index >= kBindings.size()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        auto all = config::documentFrom(definition);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, all);

        Document result;
        result.m_entries.push_back({"schema_version", FileValue::integer(kContentSchemaVersion), "default"});

        const auto project = [&](const std::string_view path) -> std::optional<std::string> {
            if (category == EContentDocument::rules or category == EContentDocument::worlds) {
                if (not path.starts_with("worlds.")) {
                    return category == EContentDocument::worlds and path == "worlds"
                               ? std::optional<std::string>{"worlds"}
                               : std::nullopt;
                }

                const auto end = path.find('.', 7u);
                const auto tail = end == std::string_view::npos ? std::string_view{} : path.substr(end + 1u);
                const bool rule = tail == "rules" or tail.starts_with("rules.");

                if (category == EContentDocument::worlds) {
                    return rule ? std::nullopt : std::optional<std::string>{path};
                }
                if (not rule) {
                    return std::nullopt;
                }

                const auto owner = path.substr(7u, end - 7u);
                std::string mapped = "rules." + std::string{owner};
                if (tail.size() > 5u) {
                    mapped += tail.substr(5u);
                }
                return mapped;
            }

            const auto root = kBindings[index].m_collection;
            if (path != root and not path.starts_with(std::string{root} + ".")) {
                return std::nullopt;
            }
            return std::string{path};
        };

        for (const auto &entry: all->m_entries) {
            if (auto path = project(entry.m_key)) {
                auto copy = entry;
                copy.m_key = std::move(*path);
                result.m_entries.push_back(std::move(copy));
            }
        }

        for (const auto &group: all->m_groups) {
            if (auto path = project(group.m_path)) {
                result.m_groups.push_back({std::move(*path), group.m_location});
            }
        }

        std::ranges::sort(result.m_entries, {}, &Entry::m_key);
        std::ranges::sort(result.m_groups, {}, &GroupPresence::m_path);
        return result;
    }

    // ------------------------------------------------------------------
    // content catalog
    // ------------------------------------------------------------------

    const PackDefinition &ContentCatalog::definition() const noexcept {
        return m_definition;
    }
    std::span<const MaterialDefinition> ContentCatalog::materials() const noexcept {
        return m_materials;
    }
    const MaterialDefinition *ContentCatalog::findMaterial(const u16 id) const noexcept {
        const auto found = std::ranges::find(m_materials, id, &MaterialDefinition::m_numeric_id);
        return found == m_materials.end() ? nullptr : std::addressof(*found);
    }
    const MaterialDefinition *ContentCatalog::findMaterial(const std::string_view key) const noexcept {
        const auto found = std::ranges::find(m_materials, key, &MaterialDefinition::m_key);
        return found == m_materials.end() ? nullptr : std::addressof(*found);
    }

    Expected<ContentCatalog> projectContentCatalog(
        PackDefinition definition, const Document &document, Diagnostics &diagnostics) {
        // Typed callers receive the same appearance domains/cardinality checks as decoded files.
        auto visual_document = config::documentFrom(definition.m_appearance);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, visual_document);
        const TreeLayer visual_layers[] = {toTreeLayer(*visual_document, "appearance")};
        Diagnostics visual_diagnostics{64u};
        auto resolved_appearance = resolveRecursive<Appearance>(visual_layers, visual_diagnostics);
        for (const auto &entry: visual_diagnostics.entries()) {
            const std::string path = "appearance." + entry.m_property;
            diagnostics.add(std::string{location(document, path)}, path, entry.m_message, entry.m_code);
        }
        diagnostics.addOmitted(visual_diagnostics.omittedCount());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved_appearance);

        Diagnostics compile_diagnostics{128u};
        auto compiled = sim::definition::compileDefinition(definition, 1.0 / 3.0, compile_diagnostics);
        appendDiagnostics(diagnostics, compile_diagnostics, document);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled);
        ContentCatalog catalog;
        catalog.m_definition = std::move(definition);
        const auto &source = catalog.m_definition;
        const auto &appearance = source.m_appearance;
        const auto &bindings = compiled->m_bindings;

        // Appearance entries must name a real simulation owner; every owner needs its visual projection.
        const auto owners = [&](const auto &visuals, const auto &simulation,
                                const std::string_view root) -> std::error_code {
            for (const auto &entry: visuals.m_entries) {
                if (findEntry(simulation, entry.m_key) == nullptr) {
                    const std::string path = "appearance." + std::string{root} + "." + entry.m_key;
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent,
                        reject(diagnostics, location(document, path), path, "Appearance owner does not exist"));
                }
            }
            for (const auto &entry: simulation.m_entries) {
                if (findEntry(visuals, entry.m_key) == nullptr) {
                    const std::string path = "appearance." + std::string{root} + "." + entry.m_key;
                    PPR_RETURN_ERROR_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, path), path,
                                           "Simulation owner requires appearance", EConfigErrc::missing_value));
                }
            }
            return {};
        };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_materials, source.m_materials, "materials"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_biomes, source.m_biomes, "biomes"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_features, source.m_features, "features"));
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, owners(appearance.m_worlds, source.m_worlds, "worlds"));

        for (std::size_t i = 0u; i < bindings.m_materials.size(); ++i) {
            const auto &binding = bindings.m_materials[i];
            const auto &numeric = compiled->m_materials[i];
            const auto &visual = *findEntry(appearance.m_materials, binding.m_key);
            const auto &semantic = *findEntry(source.m_materials, binding.m_key);
            const std::string root = "appearance.materials." + binding.m_key;
            auto tint = color(visual.m_color_rgba, root + ".color_rgba", document, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
            if (numeric.m_terrain and (not visual.m_albedo_texture or not visual.m_texture_period_m)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    reject(
                        diagnostics, location(document, root), root,
                        "Terrain appearance requires albedo_texture and texture_period_m", EConfigErrc::missing_value));
            }
            if (not numeric.m_terrain and visual.m_roundness) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(document, root + ".roundness"), root + ".roundness",
                                       "Particle appearance cannot declare terrain roundness"));
            }
            if ((visual.m_albedo_texture or visual.m_normal_texture) and not visual.m_texture_period_m) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(
                                       diagnostics, location(document, root), root + ".texture_period_m",
                                       "Textured materials require a world-space period"));
            }
            for (const auto &name: {"albedo_texture", "normal_texture"}) {
                const auto &path = std::string_view{name} == "albedo_texture" ? visual.m_albedo_texture
                                                                              : visual.m_normal_texture;
                if (path and not validTexturePath(*path)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, root + "." + name), root + "." + name,
                                           "Expected a portable pack-relative path without dot segments"));
                }
            }
            catalog.m_materials.push_back(
                {binding.m_key, binding.m_id, visual.m_display_name, static_cast<EMaterialPhase>(numeric.m_matter),
                    numeric.m_terrain ? EMaterialRepresentation::terrain : EMaterialRepresentation::particle, *tint,
                    visual.m_albedo_texture, visual.m_texture_period_m, semantic.m_diggable,
                    numeric.m_reference_density_kg_m3, std::string{location(document, root + ".albedo_texture")},
                    visual.m_roundness.value_or(1.0), visual.m_metallic, visual.m_roughness,
                    {visual.m_emissive_r, visual.m_emissive_g, visual.m_emissive_b}, visual.m_emissive_strength,
                    visual.m_alpha_mode, visual.m_ior, visual.m_normal_texture, visual.m_normal_scale,
                    visual.m_refraction_depth_m, std::string{location(document, root + ".normal_texture")},
                    visual.m_texture_sampling});
        }
        for (const auto &binding: bindings.m_fields) {
            catalog.m_fields.push_back({binding.m_key, compiled->m_fields[binding.m_index].m_noise});
        }
        const auto fieldKey = [&](const u32 index) -> const std::string & { return bindings.m_fields[index].m_key; };
        const auto biomeKey = [&](const u32 index) -> const std::string & { return bindings.m_biomes[index].m_key; };
        const auto featureKey = [&](const u32 index) -> const std::string & {
            return bindings.m_features[index].m_key;
        };
        for (const auto &binding: bindings.m_biomes) {
            const auto &numeric = compiled->m_biomes[binding.m_index];
            const auto &semantic = *findEntry(source.m_biomes, binding.m_key);
            const auto &visual = *findEntry(appearance.m_biomes, binding.m_key);
            auto tint = color(
                visual.m_preview_color_rgba, "appearance.biomes." + binding.m_key + ".preview_color_rgba", document,
                diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
            BiomeDefinition biome{
                .m_key = binding.m_key,
                .m_display_name = visual.m_display_name,
                .m_preview_color_rgba = *tint,
                .m_temperature_base_k = numeric.m_temperature_base_k,
                .m_temperature_variation_k = numeric.m_temperature_variation_k,
                .m_temperature_field = fieldKey(numeric.m_temperature_field)};
            biome.m_lining_material_id = numeric.m_lining_material;
            biome.m_lining_thickness_fine = numeric.m_lining_thickness_fine;
            biome.m_relaxation_passes = numeric.m_relaxation_passes;
            biome.m_relaxation_adoption = numeric.m_relaxation_adoption;
            biome.m_sprinkle_probability = numeric.m_sprinkle_probability;
            if (semantic.m_pore) {
                biome.m_pore_field = fieldKey(numeric.m_pore_field);
            }
            biome.m_pore_probability = numeric.m_pore_probability;
            biome.m_pore_radius_min_m = numeric.m_pore_radius_min_m;
            biome.m_pore_radius_max_m = numeric.m_pore_radius_max_m;
            biome.m_pore_affinity_min = numeric.m_pore_affinity_min;
            biome.m_pore_passes = numeric.m_pore_passes;
            biome.m_finish_passes = numeric.m_finish_passes;
            biome.m_finish_adoption = numeric.m_finish_adoption;
            biome.m_erosion_passes = numeric.m_erosion_passes;
            biome.m_soil_material_id = numeric.m_soil_material;
            biome.m_weathered_material_id = numeric.m_weathered_material;
            biome.m_soil_depth_m = numeric.m_soil_depth_m;
            biome.m_weathered_depth_m = numeric.m_weathered_depth_m;
            biome.m_formation_material_id = numeric.m_formation_material;
            biome.m_rind_material_id = numeric.m_rind_material;
            biome.m_formation_probability = numeric.m_formation_probability;
            biome.m_formation_length_m = numeric.m_formation_length_m;
            for (const auto &band: numeric.m_bands) {
                const auto entry = std::ranges::find_if(semantic.m_composition.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == band.m_order;
                });
                BiomeCompositionBand target{
                    entry->m_key, band.m_order, band.m_depth_min, band.m_depth_max, fieldKey(band.m_field), {}, 0.0,
                    band.m_bed_period_m, band.m_bed_warp_m, band.m_bed_dip};
                const auto choices = sorted(entry->m_value.m_materials);
                for (std::size_t i = 0u; i < choices.size(); ++i) {
                    target.m_materials.push_back(
                        {choices[i]->m_key, band.m_choices[i].m_material, band.m_choices[i].m_weight});
                    target.m_total_weight += band.m_choices[i].m_weight;
                }
                biome.m_composition.push_back(std::move(target));
            }
            for (const auto &deposit: numeric.m_deposits) {
                const auto entry = std::ranges::find_if(semantic.m_deposits.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == deposit.m_order;
                });
                biome.m_deposits.push_back(
                    {entry->m_key, deposit.m_order, fieldKey(deposit.m_field), deposit.m_material, deposit.m_host,
                        deposit.m_depth_min, deposit.m_depth_max, deposit.m_threshold_min, deposit.m_threshold_max,
                        deposit.m_shape, deposit.m_stage, deposit.m_blob_count, deposit.m_radius_min_m,
                        deposit.m_radius_max_m, deposit.m_stretch_min, deposit.m_stretch_max, deposit.m_distortion,
                        deposit.m_core_coverage, deposit.m_fringe_coverage, deposit.m_sprinkle_probability,
                        deposit.m_orientation_min_degrees, deposit.m_orientation_max_degrees});
            }
            catalog.m_biomes.push_back(std::move(biome));
        }
        for (const auto &binding: bindings.m_features) {
            const auto &numeric = compiled->m_features[binding.m_index];
            const auto &semantic = *findEntry(source.m_features, binding.m_key);
            const auto &visual = *findEntry(appearance.m_features, binding.m_key);
            FeatureDefinition feature{
                .m_key = binding.m_key,
                .m_display_name = visual.m_display_name,
                .m_allow_horizontal_reflection = numeric.m_allow_reflection,
                .m_reservation = {
                    numeric.m_bounds.m_min_x, numeric.m_bounds.m_min_y, numeric.m_bounds.m_max_x,
                    numeric.m_bounds.m_max_y}};
            for (const auto &operation: numeric.m_operations) {
                const auto entry = std::ranges::find_if(semantic.m_operations.m_entries, [&](const auto &item) {
                    return item.m_value.m_order == operation.m_order;
                });
                feature.m_operations.push_back(
                    {entry->m_key, operation.m_order, operation.m_carve ? EFeatureAction::carve : EFeatureAction::paint,
                        static_cast<EFeatureShape>(operation.m_shape), operation.m_x, operation.m_y, operation.m_width,
                        operation.m_height, operation.m_end_x, operation.m_end_y, operation.m_radius_m,
                        operation.m_material});
            }
            for (const auto &marker: binding.m_markers) {
                const auto &value = numeric.m_markers[marker.m_index];
                feature.m_markers.push_back(
                    {marker.m_key, static_cast<EFeatureMarkerRole>(value.m_role), value.m_x, value.m_y});
            }
            catalog.m_features.push_back(std::move(feature));
        }
        for (const auto *entry: sorted(source.m_worlds)) {
            Diagnostics recipe_diagnostics{128u};
            auto compiled_world = sim::definition::compileRecipe(
                source, {entry->m_key}, {1.0 / 3.0, 1.0, 0.4, 0.9}, recipe_diagnostics);
            appendDiagnostics(diagnostics, recipe_diagnostics, document);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, compiled_world);
            const auto &recipe = compiled_world->m_recipe;
            const auto &visual = *findEntry(appearance.m_worlds, entry->m_key);
            WorldProfileDefinition world{
                .m_key = entry->m_key,
                .m_display_name = visual.m_display_name,
                .m_default_wall_material_id = recipe.m_boundary_material,
                .m_boundary_fine = recipe.m_boundary_fine,
                .m_min_sites = recipe.m_min_sites,
                .m_max_sites = recipe.m_max_sites,
                .m_site_attempts = recipe.m_site_attempts,
                .m_site_spacing_m = recipe.m_site_spacing_m,
                .m_boundary_warp_amplitude_m = recipe.m_boundary_warp_amplitude_m,
                .m_boundary_warp_x_field = fieldKey(recipe.m_warp_x_field),
                .m_boundary_warp_y_field = fieldKey(recipe.m_warp_y_field),
                .m_start_biome = biomeKey(recipe.m_start_biome),
                .m_start_feature = featureKey(recipe.m_start_feature),
                .m_start_marker = bindings.m_features[recipe.m_start_feature].m_markers[recipe.m_start_marker].m_key,
                .m_start_x_fine = recipe.m_start_x_fine,
                .m_start_y_fine = recipe.m_start_y_fine,
                .m_start_clearance_m = recipe.m_start_clearance_m,
                .m_start_route_width_m = recipe.m_start_route_width_m,
                .m_background_stack = visual.m_background_stack};
            if (findEntry(appearance.m_backgrounds, visual.m_background_stack) == nullptr) {
                const std::string path = "appearance.worlds." + entry->m_key + ".background_stack";
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, reject(diagnostics, location(document, path), path, "Unknown background stack"));
            }
            if (recipe.m_open_surface) {
                world.m_surface_field = fieldKey(recipe.m_surface_field);
                world.m_bedrock_field = fieldKey(recipe.m_bedrock_field);
            }
            world.m_surface_elevation_m = recipe.m_surface_elevation_m;
            world.m_surface_amplitude_m = recipe.m_surface_amplitude_m;
            world.m_bedrock_thickness_m = recipe.m_bedrock_thickness_m;
            world.m_bedrock_variation_m = recipe.m_bedrock_variation_m;
            world.m_bedrock_material_id = recipe.m_bedrock_material;
            world.m_start_depth_m = recipe.m_start_depth_m;
            const auto biome_choices = sorted(entry->m_value.m_biomes);
            for (std::size_t i = 0u; i < recipe.m_constraints.size(); ++i) {
                const auto &value = recipe.m_constraints[i];
                world.m_biomes.push_back(
                    {biome_choices[i]->m_key, biomeKey(value.m_biome), value.m_weight, value.m_required_count,
                        value.m_depth_min, value.m_depth_max, value.m_distance_min_m, value.m_distance_max_m});
            }
            const auto resources = sorted(entry->m_value.m_resources);
            for (std::size_t i = 0u; i < recipe.m_resources.size(); ++i) {
                const auto &value = recipe.m_resources[i];
                world.m_start_resources.push_back(
                    {resources[i]->m_key, bindings.m_features[recipe.m_start_feature].m_markers[value.m_marker].m_key,
                        value.m_material, value.m_minimum_fine_cells, value.m_max_distance_m});
            }
            world.m_rule_keys.resize(recipe.m_rules.size());
            for (const auto &binding: compiled_world->m_bindings.m_rules) {
                world.m_rule_keys[binding.m_index] = binding.m_key;
            }
            catalog.m_worlds.push_back(std::move(world));
        }
        for (const auto *entry: sorted(appearance.m_backgrounds)) {
            BackgroundStackDefinition stack{.m_key = entry->m_key};
            for (const auto *layer: sorted(entry->m_value.m_layers)) {
                const auto &value = layer->m_value;
                const std::string path = "appearance.backgrounds." + entry->m_key + ".layers." + layer->m_key;
                auto tint = color(value.m_color_rgba, path + ".color_rgba", document, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, tint);
                if (not validTexturePath(value.m_texture)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, reject(
                                           diagnostics, location(document, path + ".texture"), path + ".texture",
                                           "Expected a portable texture path without dot segments"));
                }
                stack.m_layers.push_back(
                    {layer->m_key, value.m_texture, std::string{location(document, path + ".texture")}, value.m_order,
                        *tint, value.m_repeat_period_m, value.m_parallax_factor});
            }
            std::ranges::sort(stack.m_layers, {}, &BackgroundLayerDefinition::m_order);
            for (u32 i = 0u; i < 4u; ++i) {
                if (stack.m_layers[i].m_order != i or (i == 0u and (stack.m_layers[i].m_color_rgba & 255u) != 255u)) {
                    const std::string path = "appearance.backgrounds." + entry->m_key + ".layers";
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent,
                        reject(
                            diagnostics, location(document, path), path,
                            "Layer orders must be unique zero through three; the farthest layer tint must be opaque"));
                }
            }
            catalog.m_backgrounds.push_back(std::move(stack));
        }
        return catalog;
    }
    Expected<Document> contentVariantSubtree(
        const EContentDocument category, const std::string_view key, const std::string_view relative,
        const std::string_view tag) {
        const auto path = contentCanonicalPath(category, contentDocumentPath(category, key, relative));
        const Document draft;
        const auto descriptor = contentPropertyDescriptor(category, relative, draft, key);
        if (not descriptor or
            descriptor->m_kind != ENodeKind::variant or
            std::ranges::find(descriptor->m_choices, tag) == descriptor->m_choices.end()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        const std::string selected = path + "." + std::string{tag};
        std::optional<Document> result;
        std::error_code error;
        config::describeRecursive<PackDefinition>(
            [&]<typename T>(const std::string &schema_path, const auto &, const std::type_identity<T>) {
                if constexpr (config::details::TDescribed<T>) {
                    if (schemaMatches(schema_path, selected)) {
                        auto payload = config::documentFrom(T{});
                        if (not payload) {
                            error = payload.error();
                            return;
                        }
                        Document subtree;
                        subtree.m_entries.push_back({"tag", FileValue::text(std::string{tag}), "default"});
                        subtree.m_groups.push_back({std::string{tag}, "default"});
                        for (auto &entry: payload->m_entries) {
                            entry.m_key = std::string{tag} + "." + entry.m_key;
                            subtree.m_entries.push_back(std::move(entry));
                        }
                        for (auto &group: payload->m_groups) {
                            group.m_path = std::string{tag} + "." + group.m_path;
                            subtree.m_groups.push_back(std::move(group));
                        }
                        result = std::move(subtree);
                    }
                }
            });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        if (not result) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        return std::move(*result);
    }

    Expected<ContentCatalog> projectContentCatalog(PackDefinition definition, Diagnostics &diagnostics) {
        auto document = config::documentFrom(definition);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, document);
        auto resolved = resolvePackDefinition(*document, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, resolved);
        return projectContentCatalog(std::move(*resolved), *document, diagnostics);
    }
}
