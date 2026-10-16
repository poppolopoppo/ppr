module;
#include "pP/Macros.h"

module game.colony.content;

import engine.asset;
import engine.core;
import engine.sim;
import engine.image;
import std;

namespace pP::colony {
    using pP::make_error_code;

    PPR_DEFINE_LOG_CATEGORY(ContentSources, debug, none)

    // ------------------------------------------------------------------
    // complete source budgets and physical ownership
    // ------------------------------------------------------------------

    namespace {
        [[nodiscard]] std::error_code admitText(
            std::size_t &retained, const u64 bytes, const u64 per_file, const std::filesystem::path &path,
            config::Diagnostics &diagnostics) {
            if (bytes > per_file or bytes > ContentSourceLimits::maximum_text_bytes - retained) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentSources,
                    asset::reject(
                        diagnostics, path.string(), "source", "Content source text exceeds its file or aggregate limit",
                        config::EConfigErrc::out_of_range));
            }

            retained += static_cast<std::size_t>(bytes);
            return {};
        }

        [[nodiscard]] std::error_code checkTextBudget(
            const ContentSourceSet &source, config::Diagnostics &diagnostics) {
            std::size_t retained{};

            for (const auto &pack: source.m_packs) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentSources, admitText(
                                        retained, pack.m_manifest.m_bytes.size(), asset::kMaxManifestBytes,
                                        pack.m_manifest.m_path, diagnostics));
            }

            for (const auto &descriptor: source.m_descriptors) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentSources, admitText(
                                        retained, descriptor.m_bytes.size(), asset::kMaxDescriptorBytes,
                                        descriptor.m_path, diagnostics));
            }

            return {};
        }

        struct PhysicalSource {
            std::filesystem::path m_path{}, m_parent{}, m_suffix{};
            std::uintmax_t m_links{};
            bool m_exists{}, m_network{};
        };

        [[nodiscard]] Expected<PhysicalSource> inspectPhysicalSource(
            const std::filesystem::path &path, config::Diagnostics &diagnostics) {
            PhysicalSource result;
            result.m_path = path;
            result.m_network = path.root_name().generic_string().starts_with("//");

            std::error_code error;
            const auto status = std::filesystem::status(path, error);
            if (error and error != std::errc::no_such_file_or_directory) {
                diagnostics.add(path.string(), "source", "Cannot inspect source ownership", error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, error);
            }

            result.m_exists = std::filesystem::exists(status);
            if (result.m_exists) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources,
                    not std::filesystem::is_regular_file(status)
                        ? asset::reject(diagnostics, path.string(), "source", "Source target must be a regular file")
                        : std::error_code{});

                result.m_links = std::filesystem::hard_link_count(path, error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, error);
                return result;
            }

            auto parent = path.parent_path();
            while (not parent.empty()) {
                error.clear();
                const auto parent_status = std::filesystem::status(parent, error);
                if (error and error != std::errc::no_such_file_or_directory) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, error);
                }

                if (std::filesystem::exists(parent_status)) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ContentSources,
                        not std::filesystem::is_directory(parent_status)
                            ? asset::reject(diagnostics, parent.string(), "source", "Source parent is not a directory")
                            : std::error_code{});

                    result.m_parent = std::filesystem::canonical(parent, error);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, error);
                    result.m_suffix = path.lexically_relative(parent);
                    return result;
                }

                const auto next = parent.parent_path();
                if (next == parent) {
                    break;
                }

                parent = next;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, std::errc::no_such_file_or_directory);
            std::unreachable();
        }

        [[nodiscard]] std::error_code validatePhysicalOwners(
            const std::span<const PhysicalSource> sources, config::Diagnostics &diagnostics) {
            std::set<std::filesystem::path> canonical;
            Array<std::size_t> multilink, network;

            for (std::size_t index = 0u; index < sources.size(); ++index) {
                const auto &source = sources[index];
                if (not canonical.insert(source.m_path).second) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        ContentSources, asset::reject(
                                            diagnostics, source.m_path.string(), "source",
                                            "Two manifests or descriptors own the same canonical target",
                                            config::EConfigErrc::duplicate_key));
                }

                // Canonical names cover ordinary local spelling/symlink aliases.
                // Only multilink files and exceptional network namespaces need
                // equivalent checks; their worst case remains quadratic.
                const auto compare = [&](const PhysicalSource &previous) -> std::error_code {
                    const bool namespace_alias = source.m_network or previous.m_network;
                    const bool hard_links = source.m_links > 1u and previous.m_links > 1u;
                    if (not namespace_alias and not hard_links) {
                        return {};
                    }

                    std::error_code error;
                    bool same{};
                    if (source.m_exists and previous.m_exists) {
                        same = std::filesystem::equivalent(source.m_path, previous.m_path, error);
                    } else if (not source.m_exists and not previous.m_exists and source.m_suffix == previous.m_suffix) {
                        same = std::filesystem::equivalent(source.m_parent, previous.m_parent, error);
                    }

                    PPR_RETURN_ERROR_ON_FAIL(ContentSources, error);
                    if (same) {
                        PPR_RETURN_ERROR_ON_FAIL(
                            ContentSources, asset::reject(
                                                diagnostics, source.m_path.string(), "source",
                                                "Two manifests or descriptors alias one physical target",
                                                config::EConfigErrc::duplicate_key));
                    }

                    return {};
                };

                if (source.m_network) {
                    for (std::size_t previous = 0u; previous < index; ++previous) {
                        PPR_RETURN_ERROR_ON_FAIL(ContentSources, compare(sources[previous]));
                    }
                } else {
                    for (const auto previous: network) {
                        PPR_RETURN_ERROR_ON_FAIL(ContentSources, compare(sources[previous]));
                    }

                    if (source.m_links > 1u) {
                        for (const auto previous: multilink) {
                            if (not sources[previous].m_network) {
                                PPR_RETURN_ERROR_ON_FAIL(ContentSources, compare(sources[previous]));
                            }
                        }
                    }
                }

                if (source.m_links > 1u) {
                    multilink.push_back(index);
                }

                if (source.m_network) {
                    network.push_back(index);
                }
            }

            return {};
        }
    }

    // ------------------------------------------------------------------
    // deterministic new authoring destinations
    // ------------------------------------------------------------------

    std::string_view contentAssetKind(const EContentDocument category) noexcept {
        switch (category) {
            case EContentDocument::materials: return sim::asset::kMaterialKind;
            case EContentDocument::biomes: return sim::asset::kBiomeKind;
            case EContentDocument::fields: return sim::asset::kFieldKind;
            case EContentDocument::features: return sim::asset::kFeatureKind;
            case EContentDocument::worlds: return sim::asset::kWorldKind;
            case EContentDocument::rules: return sim::asset::kRuleKind;
            case EContentDocument::elements: return sim::asset::kElementKind;
            case EContentDocument::textures: return image::kTextureAssetKind;
            case EContentDocument::backgrounds: return kBackgroundAssetKind;
            default: return {};
        }
    }

    Expected<std::filesystem::path> newContentDescriptorPath(
        const asset::PackSet &packs, const asset::QualifiedName &name, const std::string_view kind,
        config::Diagnostics &diagnostics) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources, not asset::validLocalName(name.m_local) ? std::make_error_code(std::errc::invalid_argument)
                                                                    : std::error_code{});

        constexpr std::pair<std::string_view, std::string_view> directories[] = {
            {sim::asset::kElementKind, "elements"}, {sim::asset::kMaterialKind, "materials"},
            {sim::asset::kFieldKind, "fields"}, {sim::asset::kBiomeKind, "biomes"},
            {sim::asset::kFeatureKind, "features"}, {sim::asset::kRuleKind, "rules"},
            {sim::asset::kWorldKind, "worlds"}, {image::kTextureAssetKind, "textures"},
            {kBackgroundAssetKind, "backgrounds"}};

        const auto directory = std::ranges::find(directories, kind, [](const auto &entry) { return entry.first; });
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources,
            directory == std::end(directories) ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        return packs.containedPath(
            name.m_pack, std::string{directory->second} + "/asset-" + name.m_local + ".toml", diagnostics);
    }

    // ------------------------------------------------------------------
    // immutable complete source snapshots
    // ------------------------------------------------------------------

    namespace {
        [[nodiscard]] Expected<std::shared_ptr<const asset::PackSet>> validateSourceSet(
            const ContentSourceSet &source, std::shared_ptr<const asset::PackSet> indexed,
            const std::span<const ContentSourceSnapshot::Restoration> restorations, config::Diagnostics &diagnostics,
            const IContext *const context) {
            std::map<std::filesystem::path, const asset::DescriptorSource *> records;

            for (const auto &descriptor: source.m_descriptors) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources, not records.emplace(descriptor.m_path, std::addressof(descriptor)).second
                                        ? asset::reject(
                                              diagnostics, descriptor.m_path.string(), "source",
                                              "Duplicate descriptor source record", config::EConfigErrc::duplicate_key)
                                        : std::error_code{});
            }

            Array<PhysicalSource> physical_sources;
            physical_sources.reserve(source.m_packs.size() + source.m_descriptors.size());

            for (const auto &pack: indexed->packs()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});

                auto manifest = indexed->containedPath(pack.m_manifest.m_value.m_pack_id, "pack.toml", diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, manifest);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources,
                    *manifest != pack.m_manifest.m_source.m_path
                        ? asset::reject(
                              diagnostics, pack.m_root.string(), "manifest", "Manifest provenance is not pack.toml")
                        : std::error_code{});

                auto physical = inspectPhysicalSource(*manifest, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, physical);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources, not physical->m_exists ? std::make_error_code(std::errc::no_such_file_or_directory)
                                                           : std::error_code{});
                physical_sources.push_back(std::move(*physical));
            }

            std::set<std::filesystem::path> declared;

            for (const auto &entry: indexed->assets()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources,
                    not declared.insert(entry.m_descriptor).second
                        ? asset::reject(
                              diagnostics, entry.m_descriptor.string(), "descriptor",
                              "Multiple assets declare one descriptor target", config::EConfigErrc::duplicate_key)
                        : std::error_code{});

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentSources, records.find(entry.m_descriptor) == records.end()
                                        ? asset::reject(
                                              diagnostics, entry.m_descriptor.string(), "descriptor",
                                              "Complete source snapshot is missing a declared descriptor",
                                              config::EConfigErrc::missing_value)
                                        : std::error_code{});

                auto physical = inspectPhysicalSource(entry.m_descriptor, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, physical);
                if (not physical->m_exists) {
                    auto generated = newContentDescriptorPath(*indexed, entry.m_name, entry.m_kind, diagnostics);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, generated);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ContentSources,
                        *generated != entry.m_descriptor and
                                std::ranges::none_of(
                                    restorations, [&](const auto &proof) { return proof.matches(*indexed, entry); })
                            ? asset::reject(
                                  diagnostics, entry.m_descriptor.string(), "descriptor",
                                  "Missing authored descriptor must use its deterministic new-asset destination")
                            : std::error_code{});
                }

                physical_sources.push_back(std::move(*physical));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources, declared.size() != source.m_descriptors.size()
                                    ? asset::reject(
                                          diagnostics, "content snapshot", "source",
                                          "Snapshot contains undeclared or duplicate descriptor records")
                                    : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, validatePhysicalOwners(physical_sources, diagnostics));

            return indexed;
        }
    }

    ContentSourceSnapshot::ContentSourceSnapshot(ContentSourceSet source, std::shared_ptr<const asset::PackSet> index)
        : m_source{std::move(source)}, m_index{std::move(index)} {
    }

    Expected<ContentSourceSnapshot> ContentSourceSnapshot::index(
        ContentSourceSet source, config::Diagnostics &diagnostics, const IContext *const context) {
        return index(std::move(source), {}, diagnostics, context);
    }

    Expected<ContentSourceSnapshot> ContentSourceSnapshot::index(
        ContentSourceSet source, const std::span<const Restoration> restorations, config::Diagnostics &diagnostics,
        const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, checkTextBudget(source, diagnostics));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources,
            source.m_packs.empty() ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        auto indexed = asset::PackSet::index(source.m_packs, kContentAssetKinds, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, indexed);

        auto checked = validateSourceSet(
            source, std::make_shared<const asset::PackSet>(std::move(*indexed)), restorations, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, checked);

        return ContentSourceSnapshot{std::move(source), std::move(*checked)};
    }

    Expected<ContentSourceSnapshot> ContentSourceSnapshot::index(
        Array<asset::PackSource> manifests, const asset::DescriptorReader reader,
        const std::span<const Restoration> restorations, config::Diagnostics &diagnostics,
        const IContext *const context) {
        ContentSourceSet source;
        source.m_packs = std::move(manifests);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, checkTextBudget(source, diagnostics));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources,
            source.m_packs.empty() ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        auto indexed = asset::PackSet::index(source.m_packs, kContentAssetKinds, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, indexed);
        auto owner = std::make_shared<const asset::PackSet>(std::move(*indexed));
        source.m_descriptors.reserve(owner->assets().size());
        std::size_t retained{};

        for (const auto &pack: source.m_packs) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources, admitText(
                                    retained, pack.m_manifest.m_bytes.size(), asset::kMaxManifestBytes,
                                    pack.m_manifest.m_path, diagnostics));
        }

        for (const auto &entry: owner->assets()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
            auto descriptor = owner->readDescriptor(entry.m_key, entry.m_kind, reader, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources,
                admitText(
                    retained, descriptor->m_bytes.size(), asset::kMaxDescriptorBytes, descriptor->m_path, diagnostics));

            source.m_descriptors.push_back(std::move(*descriptor));
        }

        auto checked = validateSourceSet(source, std::move(owner), restorations, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, checked);

        return ContentSourceSnapshot{std::move(source), std::move(*checked)};
    }

    // ------------------------------------------------------------------
    // scoped proof of previously owned descriptor targets
    // ------------------------------------------------------------------

    ContentSourceSnapshot::Restoration::Restoration(
        asset::QualifiedName name, std::string kind, std::filesystem::path root, std::filesystem::path path)
        : m_name{std::move(name)}, m_kind{std::move(kind)}, m_root{std::move(root)}, m_path{std::move(path)} {
    }

    ContentSourceSnapshot::Restoration::Restoration(Restoration &&other) noexcept
        : m_name{std::move(other.m_name)}, m_kind{std::move(other.m_kind)}, m_root{std::move(other.m_root)},
          m_path{std::move(other.m_path)}, m_valid{std::exchange(other.m_valid, false)} {
    }

    ContentSourceSnapshot::Restoration &ContentSourceSnapshot::Restoration::operator=(Restoration &&other) noexcept {
        if (this != std::addressof(other)) {
            m_name = std::move(other.m_name);
            m_kind = std::move(other.m_kind);
            m_root = std::move(other.m_root);
            m_path = std::move(other.m_path);
            m_valid = std::exchange(other.m_valid, false);
        }

        return *this;
    }

    const asset::QualifiedName &ContentSourceSnapshot::Restoration::name() const noexcept {
        return m_name;
    }

    std::string_view ContentSourceSnapshot::Restoration::kind() const noexcept {
        return m_kind;
    }

    const std::filesystem::path &ContentSourceSnapshot::Restoration::root() const noexcept {
        return m_root;
    }

    const std::filesystem::path &ContentSourceSnapshot::Restoration::path() const noexcept {
        return m_path;
    }

    bool ContentSourceSnapshot::Restoration::matches(
        const asset::PackSet &packs, const asset::IndexedAsset &entry) const noexcept {
        return m_valid and
               entry.m_pack_index < packs.packs().size() and
               m_name.m_pack == entry.m_name.m_pack and
               m_name.m_local == entry.m_name.m_local and
               m_kind == entry.m_kind and
               m_path == entry.m_descriptor and
               m_root == packs.packs()[entry.m_pack_index].m_root;
    }

    Expected<ContentSourceSnapshot::Restoration> ContentSourceSnapshot::ownedTarget(
        const asset::AssetKey key, config::Diagnostics &diagnostics) const {
        const auto entries = m_index->assets();
        const auto found = std::ranges::find(entries, key, &asset::IndexedAsset::m_key);
        if (found == entries.end()) {
            diagnostics.add(
                "content snapshot", "asset", "Cannot retain ownership of an asset outside this checked snapshot",
                std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, std::errc::invalid_argument);
        }

        return Restoration{
            found->m_name, found->m_kind, m_index->packs()[found->m_pack_index].m_root, found->m_descriptor};
    }

    const ContentSourceSet &ContentSourceSnapshot::source() const noexcept {
        return m_source;
    }

    std::shared_ptr<const asset::PackSet> ContentSourceSnapshot::packOwner() const noexcept {
        return m_index;
    }

    Expected<asset::DescriptorSource> ContentSourceSet::readDescriptor(
        const asset::IndexedAsset &entry, config::Diagnostics &diagnostics) const {
        const auto found = std::ranges::find(m_descriptors, entry.m_descriptor, &asset::DescriptorSource::m_path);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources,
            found == m_descriptors.end()
                ? asset::reject(
                      diagnostics, entry.m_descriptor.string(), "descriptor",
                      "Complete source snapshot is missing a declared descriptor", config::EConfigErrc::missing_value)
                : std::error_code{});

        return *found;
    }

    bool ContentSourceSet::sameMounts(const ContentSourceSet &other) const noexcept {
        if (m_packs.size() != other.m_packs.size()) {
            return false;
        }

        return std::ranges::all_of(m_packs, [&](const auto &pack) {
            return std::ranges::any_of(other.m_packs, [&](const auto &candidate) {
                return pack.m_root == candidate.m_root and pack.m_manifest.m_path == candidate.m_manifest.m_path;
            });
        });
    }

    bool ContentSourceSet::sameMounts(const asset::PackSet &other) const noexcept {
        if (m_packs.size() != other.packs().size()) {
            return false;
        }

        return std::ranges::all_of(m_packs, [&](const auto &pack) {
            return std::ranges::any_of(other.packs(), [&](const auto &candidate) {
                return pack.m_root == candidate.m_root and
                       pack.m_manifest.m_path == candidate.m_manifest.m_source.m_path;
            });
        });
    }

    // ------------------------------------------------------------------
    // bounded disk snapshots and definition-only import
    // ------------------------------------------------------------------

    Expected<ContentSourceSnapshot> loadContentSourceSet(
        const std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
        const IContext *const context) {
        const hal::ProfileScope scope{"Content.Sources.Load"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentSources, roots.empty() ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});

        ContentSourceSet result;
        std::size_t retained{};
        const auto admit_file = [&](const std::filesystem::path &path, const u64 maximum) -> Expected<u64> {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
            std::error_code error;
            const auto bytes = std::filesystem::file_size(path, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, admitText(retained, bytes, maximum, path, diagnostics));
            return bytes;
        };

        for (const auto &root: roots) {
            auto size = admit_file(root / "pack.toml", asset::kMaxManifestBytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, size);
            auto pack = asset::readPackSource(root, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, pack);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources, pack->m_manifest.m_bytes.size() != *size
                                    ? std::make_error_code(std::errc::resource_unavailable_try_again)
                                    : std::error_code{});
            result.m_packs.push_back(std::move(*pack));
        }

        auto index = asset::PackSet::index(result.m_packs, kContentAssetKinds, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, index);
        auto indexed = std::make_shared<const asset::PackSet>(std::move(*index));

        for (const auto &entry: indexed->assets()) {
            auto size = admit_file(entry.m_descriptor, asset::kMaxDescriptorBytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, size);
            auto descriptor = indexed->readDescriptor(entry.m_key, entry.m_kind, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources, descriptor->m_bytes.size() != *size
                                    ? std::make_error_code(std::errc::resource_unavailable_try_again)
                                    : std::error_code{});
            result.m_descriptors.push_back(std::move(*descriptor));
        }

        auto checked = validateSourceSet(result, std::move(indexed), {}, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, checked);
        return ContentSourceSnapshot{std::move(result), std::move(*checked)};
    }

    Expected<ContentDescriptorCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *const context) {
        const std::filesystem::path roots[] = {root};
        return loadContentCatalog(roots, diagnostics, context);
    }

    Expected<ContentDescriptorCatalog> loadContentCatalog(
        const std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
        const IContext *const context) {
        auto snapshot = loadContentSourceSet(roots, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, snapshot);

        return loadContentCatalog(*snapshot, diagnostics, context);
    }

    Expected<ContentDescriptorCatalog> loadContentCatalog(
        const ContentSourceSet &source, config::Diagnostics &diagnostics, const IContext *const context) {
        auto snapshot = ContentSourceSnapshot::index(source, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, snapshot);

        return loadContentCatalog(*snapshot, diagnostics, context);
    }

    Expected<ContentDescriptorCatalog> loadContentCatalog(
        const ContentSourceSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *const context) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
        std::map<std::filesystem::path, const asset::DescriptorSource *> records;

        for (const auto &descriptor: snapshot.source().m_descriptors) {
            records.emplace(descriptor.m_path, std::addressof(descriptor));
        }

        const auto reader = [&](const asset::IndexedAsset &entry,
                                config::Diagnostics &local) -> Expected<asset::DescriptorSource> {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
            const auto found = records.find(entry.m_descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentSources, found == records.end()
                                    ? asset::reject(
                                          local, entry.m_descriptor.string(), "descriptor",
                                          "Complete source snapshot is missing a declared descriptor",
                                          config::EConfigErrc::missing_value)
                                    : std::error_code{});

            return *found->second;
        };

        auto catalog = ContentDescriptorCatalog::importDefinitions(snapshot.packOwner(), reader, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, catalog);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentSources, context ? context->error() : std::error_code{});
        return catalog;
    }
}
