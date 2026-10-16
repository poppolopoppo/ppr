module;
#include "pP/Macros.h"

module game.colony.content.authoring;
import engine.core;
import engine.asset;
import game.colony.content;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(ContentAuthoring, debug, none)

    // ------------------------------------------------------------------
    // owned source and ledger queries
    // ------------------------------------------------------------------

    namespace {
        using Restoration = ContentSourceSnapshot::Restoration;

        template<typename Source>
        auto sourceBytes(Source &source, const std::filesystem::path &path) {
            using String = std::conditional_t<std::is_const_v<Source>, const std::string, std::string>;
            const auto pack = std::ranges::find(
                source.m_packs, path, [](const auto &value) -> const auto & { return value.m_manifest.m_path; });
            if (pack != source.m_packs.end()) {
                return static_cast<String *>(std::addressof(pack->m_manifest.m_bytes));
            }

            const auto record = std::ranges::find(source.m_descriptors, path, &asset::DescriptorSource::m_path);
            return record == source.m_descriptors.end() ? static_cast<String *>(nullptr)
                                                        : std::addressof(record->m_bytes);
        }

        template<typename Baseline>
        auto ledgerRecord(Baseline &baseline, const std::filesystem::path &path) {
            return std::ranges::find(baseline.m_descriptors, path, [](const auto &record) -> const auto & {
                return record.m_target->path();
            });
        }

        bool sameSource(const ContentSourceSet &left, const ContentSourceSet &right) noexcept {
            if (not left.sameMounts(right) or left.m_descriptors.size() != right.m_descriptors.size()) {
                return false;
            }

            for (const auto &pack: left.m_packs) {
                const auto *bytes = sourceBytes(right, pack.m_manifest.m_path);
                if (not bytes or *bytes != pack.m_manifest.m_bytes) {
                    return false;
                }
            }

            for (const auto &record: left.m_descriptors) {
                const auto *bytes = sourceBytes(right, record.m_path);
                if (not bytes or *bytes != record.m_bytes) {
                    return false;
                }
            }

            return true;
        }

        Array<Restoration> restorationProofs(const ContentDiskBaseline &baseline) {
            Array<Restoration> result;
            result.reserve(baseline.m_descriptors.size());

            for (const auto &record: baseline.m_descriptors) {
                result.push_back(*record.m_target);
            }

            return result;
        }

        std::error_code checkLedger(const ContentDiskBaseline &baseline) {
            std::set<std::filesystem::path> paths;

            for (const auto &record: baseline.m_descriptors) {
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentAuthoring, not record.m_target or not paths.insert(record.m_target->path()).second
                                          ? std::make_error_code(std::errc::invalid_argument)
                                          : std::error_code{});
            }

            return {};
        }

        Expected<const asset::IndexedAsset *> authoredEntry(
            const asset::PackSet &packs, const EContentDocument category, const std::string_view key,
            config::Diagnostics &diagnostics) {
            const auto kind = contentAssetKind(category);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                kind.empty() or (key.find(':') == std::string_view::npos and packs.packs().size() != 1u)
                    ? asset::reject(
                          diagnostics, "authoring", key,
                          "Select a qualified asset name and a supported descriptor kind")
                    : std::error_code{});

            const auto name = asset::qualifyName(key, packs.packs().front().m_manifest.m_value.m_pack_id);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, name);

            return packs.resolve(name->m_pack, name->string(), kind, diagnostics);
        }

        Expected<config::Document> authoredDocument(
            const ContentSourceSet &source, const asset::IndexedAsset &entry, config::Diagnostics &diagnostics) {
            const auto *bytes = sourceBytes(source, entry.m_descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                not bytes ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{});

            return config::decode(*bytes, entry.m_descriptor.string(), diagnostics, config::EDecodeMode::tree);
        }

        std::error_code conflict(config::Diagnostics &diagnostics, const std::filesystem::path &path) {
            return asset::reject(
                diagnostics, path.string(), "save",
                "Owned source changed on disk; explicitly reload to adopt external changes",
                config::EConfigErrc::cross_field_invalid);
        }

        Expected<std::optional<std::string>> readDisk(
            const std::filesystem::path &path, const std::size_t limit, config::Diagnostics &diagnostics) {
            std::error_code error;
            const auto status = std::filesystem::status(path, error);
            if (error == std::errc::no_such_file_or_directory or (not error and not std::filesystem::exists(status))) {
                return std::optional<std::string>{};
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                not std::filesystem::is_regular_file(status)
                    ? asset::reject(diagnostics, path.string(), "save", "Source target is not a regular file")
                    : std::error_code{});

            const auto size = std::filesystem::file_size(path, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, size > limit ? std::make_error_code(std::errc::file_too_large) : std::error_code{});

            std::ifstream stream{path, std::ios::binary};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, not stream ? std::make_error_code(std::errc::io_error) : std::error_code{});
            std::string bytes(static_cast<std::size_t>(size), '\0');
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, not stream or stream.peek() != std::char_traits<char>::eof()
                                      ? std::make_error_code(std::errc::io_error)
                                      : std::error_code{});

            return std::optional<std::string>{std::move(bytes)};
        }

        bool sameMounts(const ContentDiskBaseline &baseline, const ContentSourceSet &source) noexcept {
            if (baseline.m_packs.size() != source.m_packs.size()) {
                return false;
            }

            return std::ranges::all_of(baseline.m_packs, [&](const auto &pack) {
                return std::ranges::any_of(source.m_packs, [&](const auto &candidate) {
                    return pack.m_root == candidate.m_root and pack.m_manifest.m_path == candidate.m_manifest.m_path;
                });
            });
        }
    }

    // ------------------------------------------------------------------
    // session lifetime and immutable validation views
    // ------------------------------------------------------------------

    Expected<ContentAuthoringSession> ContentAuthoringSession::open(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *const context,
        const sim::definition::GenerationCompileParameters parameters, ContentSourceEpochToken source_epoch) {
        const std::filesystem::path roots[] = {root};
        return open(roots, diagnostics, context, parameters, std::move(source_epoch));
    }

    Expected<ContentAuthoringSession> ContentAuthoringSession::open(
        const std::span<const std::filesystem::path> roots, config::Diagnostics &diagnostics,
        const IContext *const context, const sim::definition::GenerationCompileParameters parameters,
        ContentSourceEpochToken source_epoch) {
        auto snapshot = loadContentSourceSet(roots, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, snapshot);

        auto catalog = validateContentDraft(*snapshot, parameters, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, catalog);

        ContentAuthoringSession session;
        session.m_root = snapshot->source().m_packs.front().m_root;
        session.m_draft = snapshot->source();
        session.m_index = snapshot->packOwner();
        session.m_source_epoch = source_epoch ? std::move(source_epoch) : makeContentSourceEpoch();
        session.m_parameters = parameters;
        session.m_baseline.m_packs = session.m_draft.m_packs;
        session.m_baseline.m_descriptors.reserve(session.m_draft.m_descriptors.size());

        for (const auto &entry: session.m_index->assets()) {
            auto proof = snapshot->ownedTarget(entry.m_key, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, proof);

            session.m_baseline.m_descriptors.push_back(
                {std::make_shared<const Restoration>(std::move(*proof)),
                    *sourceBytes(session.m_draft, entry.m_descriptor)});
        }

        session.m_catalog = std::move(catalog->m_definitions);
        return session;
    }

    const std::filesystem::path &ContentAuthoringSession::root() const noexcept {
        return m_root;
    }

    const ContentDiskBaseline &ContentAuthoringSession::baselineSource() const noexcept {
        return m_baseline;
    }

    const ContentSourceSet &ContentAuthoringSession::draftSource() const noexcept {
        return m_draft;
    }

    const asset::PackSet &ContentAuthoringSession::sourceIndex() const noexcept {
        return *m_index;
    }

    const ContentSourceEpochToken &ContentAuthoringSession::sourceEpoch() const noexcept {
        return m_source_epoch;
    }

    u64 ContentAuthoringSession::revision() const noexcept {
        return m_revision;
    }

    u64 ContentAuthoringSession::validatedRevision() const noexcept {
        return m_validated_revision;
    }

    const std::shared_ptr<const ContentDescriptorCatalog> &ContentAuthoringSession::validatedCatalog() const noexcept {
        return m_catalog;
    }

    bool ContentAuthoringSession::dirty() const noexcept {
        for (const auto &pack: m_draft.m_packs) {
            const auto baseline = std::ranges::find(
                m_baseline.m_packs, pack.m_manifest.m_path,
                [](const auto &value) -> const auto & { return value.m_manifest.m_path; });
            if (baseline == m_baseline.m_packs.end() or baseline->m_manifest.m_bytes != pack.m_manifest.m_bytes) {
                return true;
            }
        }

        for (const auto &record: m_draft.m_descriptors) {
            const auto baseline = ledgerRecord(m_baseline, record.m_path);
            if (baseline == m_baseline.m_descriptors.end() or
                not baseline->m_bytes or
                *baseline->m_bytes != record.m_bytes) {
                return true;
            }
        }

        return std::ranges::any_of(m_baseline.m_descriptors, [&](const auto &record) {
            return record.m_bytes.has_value() and not sourceBytes(m_draft, record.m_target->path());
        });
    }

    bool ContentAuthoringSession::canUndo() const noexcept {
        return m_history.canUndo();
    }

    bool ContentAuthoringSession::canRedo() const noexcept {
        return m_history.canRedo();
    }

    void ContentAuthoringSession::pruneTombstones() noexcept {
        // Ledger, command history and dispatched value snapshots share proofs.
        // Worker release can delay pruning; no other thread adds ledger owners.
        std::erase_if(m_baseline.m_descriptors, [&](const auto &record) {
            return not record.m_bytes and
                   record.m_target.use_count() == 1 and
                   not sourceBytes(m_draft, record.m_target->path());
        });
    }

    // ------------------------------------------------------------------
    // atomic source commands and bounded history
    // ------------------------------------------------------------------

    std::error_code ContentAuthoringSession::applyPatch(
        const SourceCommand &command, const bool reverse, std::shared_ptr<const asset::PackSet> prepared) {
        Array<asset::DescriptorSource> replacements;
        replacements.reserve(command.m_changes.size());
        bool topology{};

        for (const auto &change: command.m_changes) {
            const auto &before = reverse ? change.m_after : change.m_before;
            const auto &after = reverse ? change.m_before : change.m_after;
            const auto *current = sourceBytes(m_draft, change.m_path);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, (before.has_value() != (current != nullptr) or (before and *before != *current))
                                      ? std::make_error_code(std::errc::state_not_recoverable)
                                      : std::error_code{});

            topology = topology or
                       not before or
                       not after or
                       std::ranges::any_of(
                           m_draft.m_packs, [&](const auto &pack) { return pack.m_manifest.m_path == change.m_path; });
            if (after) {
                replacements.push_back({change.m_path, *after});
            }
        }

        // Scalar edits retain the manifest index. Topology changes validate one
        // complete candidate before committing any of the grouped file changes.
        if (topology and not prepared) {
            auto candidate = m_draft;

            for (const auto &change: command.m_changes) {
                const auto &after = reverse ? change.m_before : change.m_after;
                if (auto *bytes = sourceBytes(candidate, change.m_path)) {
                    if (after) {
                        *bytes = *after;
                    } else {
                        std::erase_if(candidate.m_descriptors, [&](const auto &record) {
                            return record.m_path == change.m_path;
                        });
                    }
                } else if (after) {
                    candidate.m_descriptors.push_back({change.m_path, *after});
                }
            }

            config::Diagnostics diagnostics;
            const auto proofs = restorationProofs(m_baseline);
            auto checked = ContentSourceSnapshot::index(std::move(candidate), proofs, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, checked);
            prepared = checked->packOwner();
        }

        std::size_t retained{};
        const auto admit = [&](const std::filesystem::path &path, const std::string &bytes, const std::size_t maximum) {
            const auto replacement = std::ranges::find(command.m_changes, path, &FileChange::m_path);
            const auto *selected = std::addressof(bytes);
            if (replacement != command.m_changes.end()) {
                const auto &after = reverse ? replacement->m_before : replacement->m_after;
                if (not after) {
                    return std::error_code{};
                }

                selected = std::addressof(*after);
            }

            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring,
                selected->size() > maximum or selected->size() > ContentSourceLimits::maximum_text_bytes - retained
                    ? std::make_error_code(std::errc::file_too_large)
                    : std::error_code{});

            retained += selected->size();
            return std::error_code{};
        };

        for (const auto &pack: m_draft.m_packs) {
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, admit(pack.m_manifest.m_path, pack.m_manifest.m_bytes, asset::kMaxManifestBytes));
        }

        for (const auto &record: m_draft.m_descriptors) {
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, admit(record.m_path, record.m_bytes, asset::kMaxDescriptorBytes));
        }

        for (const auto &change: command.m_changes) {
            const auto &after = reverse ? change.m_before : change.m_after;
            if (after and not sourceBytes(m_draft, change.m_path)) {
                PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, admit(change.m_path, *after, asset::kMaxDescriptorBytes));
            }
        }

        m_draft.m_descriptors.reserve(m_draft.m_descriptors.size() + replacements.size());

        for (const auto &change: command.m_changes) {
            const auto &after = reverse ? change.m_before : change.m_after;
            if (not after) {
                std::erase_if(
                    m_draft.m_descriptors, [&](const auto &record) { return record.m_path == change.m_path; });
                continue;
            }

            const auto replacement = std::ranges::find(replacements, change.m_path, &asset::DescriptorSource::m_path);
            if (auto *bytes = sourceBytes(m_draft, change.m_path)) {
                *bytes = std::move(replacement->m_bytes);
            } else {
                m_draft.m_descriptors.push_back(std::move(*replacement));
            }
        }

        if (prepared) {
            m_index = std::move(prepared);
        }

        ++m_revision;
        return {};
    }

    std::error_code ContentAuthoringSession::executePatch(
        SourceCommand command, std::shared_ptr<const asset::PackSet> prepared) {
        u64 payload{};

        for (const auto &change: command.m_changes) {
            payload += (change.m_before ? change.m_before->size() : 0u) +
                       (change.m_after ? change.m_after->size() : 0u);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, m_history.execute(std::move(command), payload, [&](const auto &value) {
                return applyPatch(value, false, prepared);
            }));

        pruneTombstones();
        return {};
    }

    std::error_code ContentAuthoringSession::editSource(
        const asset::IndexedAsset &entry, config::ScalarSourcePatch patch) {
        if (patch.m_before == patch.m_after) {
            return {};
        }

        const auto *before = sourceBytes(m_draft, entry.m_descriptor);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            not before ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{});
        auto after = config::applySourcePatch(*before, patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, after);

        SourceCommand command;
        command.m_changes.push_back({entry.m_descriptor, *before, std::move(*after)});
        return executePatch(std::move(command));
    }

    // ------------------------------------------------------------------
    // descriptor editing
    // ------------------------------------------------------------------

    std::error_code ContentAuthoringSession::editBiome(
        const std::string_view key, const std::string_view relative, const config::FileValue &value,
        config::Diagnostics &diagnostics) {
        return editDefinition(EContentDocument::biomes, key, relative, value, diagnostics);
    }

    std::error_code ContentAuthoringSession::editDefinition(
        const EContentDocument category, const std::string_view key, const std::string_view relative,
        const config::FileValue &value, config::Diagnostics &diagnostics) {
        auto entry = authoredEntry(*m_index, category, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, entry);
        auto document = authoredDocument(m_draft, **entry, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, document);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, not config::isValidCanonicalName(relative)
                                  ? std::make_error_code(std::errc::invalid_argument)
                                  : std::error_code{});

        if (relative.ends_with(".tag")) {
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, value.m_kind != config::EFileKind::text
                                      ? config::make_error_code(config::EConfigErrc::type_mismatch)
                                      : std::error_code{});
            const auto current = std::ranges::find(document->m_entries, relative, &config::Entry::m_key);
            if (current != document->m_entries.end() and
                current->m_value.m_kind == config::EFileKind::text and
                current->m_value.m_text == value.m_text) {
                return {};
            }

            const auto root = relative.substr(0u, relative.size() - 4u);
            auto subtree = contentVariantSubtree(category, key, root, value.m_text);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, subtree);
            return replaceDefinitionSubtree(category, key, root, *subtree, diagnostics);
        }

        const auto *bytes = sourceBytes(m_draft, (*entry)->m_descriptor);
        auto patch = config::patchScalar(*bytes, relative, value, (*entry)->m_descriptor.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);

        return editSource(**entry, std::move(*patch));
    }

    std::error_code ContentAuthoringSession::clearDefinitionProperty(
        const EContentDocument category, const std::string_view key, const std::string_view relative,
        config::Diagnostics &diagnostics) {
        auto entry = authoredEntry(*m_index, category, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, entry);
        auto document = authoredDocument(m_draft, **entry, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, document);

        const auto descriptor = contentPropertyDescriptor(category, relative, *document, key);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            not descriptor or not descriptor->m_optional
                ? asset::reject(
                      diagnostics, (*entry)->m_descriptor.string(), relative,
                      "Only optional descriptor properties can be unset", config::EConfigErrc::cross_field_invalid)
                : std::error_code{});

        const auto prefix = std::string{relative} + ".";
        if (std::ranges::none_of(
                document->m_entries,
                [&](const auto &value) { return value.m_key == relative or value.m_key.starts_with(prefix); }) and
            std::ranges::none_of(document->m_groups, [&](const auto &value) {
                return value.m_path == relative or value.m_path.starts_with(prefix);
            })) {
            return {};
        }

        const auto *bytes = sourceBytes(m_draft, (*entry)->m_descriptor);
        auto patch = config::patchTree(
            *bytes, relative, config::ETreeSourceEdit::remove, {}, (*entry)->m_descriptor.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);

        return editSource(**entry, std::move(*patch));
    }

    std::error_code ContentAuthoringSession::replaceDefinitionSubtree(
        const EContentDocument category, const std::string_view key, const std::string_view relative,
        const config::Document &subtree, config::Diagnostics &diagnostics) {
        auto entry = authoredEntry(*m_index, category, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, entry);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, not config::isValidCanonicalName(relative)
                                  ? std::make_error_code(std::errc::invalid_argument)
                                  : std::error_code{});

        const auto *bytes = sourceBytes(m_draft, (*entry)->m_descriptor);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            not bytes ? std::make_error_code(std::errc::no_such_file_or_directory) : std::error_code{});
        auto patch = config::patchTree(
            *bytes, relative, config::ETreeSourceEdit::replace, subtree, (*entry)->m_descriptor.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);

        return editSource(**entry, std::move(*patch));
    }

    // ------------------------------------------------------------------
    // descriptor topology and history navigation
    // ------------------------------------------------------------------

    std::error_code ContentAuthoringSession::cloneDefinition(
        const EContentDocument category, const std::string_view source_key, const std::string_view new_key,
        config::Diagnostics &diagnostics) {
        auto source = authoredEntry(*m_index, category, source_key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, source);
        auto name = asset::qualifyName(new_key, (*source)->m_name.m_pack);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, name);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring,
            name->m_pack != (*source)->m_name.m_pack or std::ranges::any_of(
                                                            m_index->assets(),
                                                            [&](const auto &entry) {
                                                                return entry.m_name.m_pack == name->m_pack and
                                                                       entry.m_name.m_local == name->m_local;
                                                            })
                ? std::make_error_code(std::errc::file_exists)
                : std::error_code{});

        auto path = newContentDescriptorPath(*m_index, *name, (*source)->m_kind, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, path);
        auto disk = readDisk(*path, asset::kMaxDescriptorBytes, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, disk);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, disk->has_value() ? conflict(diagnostics, *path) : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, ledgerRecord(m_baseline, *path) != m_baseline.m_descriptors.end()
                                  ? std::make_error_code(std::errc::file_exists)
                                  : std::error_code{});

        const auto pack_index = (*source)->m_pack_index;
        const auto &manifest_path = m_index->packs()[pack_index].m_manifest.m_source.m_path;
        const auto *manifest = sourceBytes(m_draft, manifest_path);
        const auto relative = path->lexically_relative(m_index->packs()[pack_index].m_root).generic_string();
        config::Document declaration;
        declaration.m_entries.push_back({"kind", config::FileValue::text((*source)->m_kind), "authoring"});
        declaration.m_entries.push_back({"descriptor", config::FileValue::text(relative), "authoring"});
        auto patch = config::patchTree(
            *manifest, "assets." + name->m_local, config::ETreeSourceEdit::insert, declaration, manifest_path.string(),
            diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        auto modified_manifest = config::applySourcePatch(*manifest, *patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, modified_manifest);

        auto candidate = m_draft;
        *sourceBytes(candidate, manifest_path) = *modified_manifest;
        candidate.m_descriptors.push_back({*path, *sourceBytes(m_draft, (*source)->m_descriptor)});
        const auto proofs = restorationProofs(m_baseline);
        auto checked = ContentSourceSnapshot::index(std::move(candidate), proofs, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, checked);
        auto target = checked->packOwner()->resolve(name->m_pack, name->string(), (*source)->m_kind, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, target);
        auto proof = checked->ownedTarget((*target)->m_key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, proof);

        auto authority = std::make_shared<const Restoration>(std::move(*proof));
        m_baseline.m_descriptors.reserve(m_baseline.m_descriptors.size() + 1u);
        SourceCommand command;
        command.m_target = authority;
        command.m_changes.push_back({manifest_path, *manifest, std::move(*modified_manifest)});
        command.m_changes.push_back({*path, std::nullopt, *sourceBytes(m_draft, (*source)->m_descriptor)});
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, executePatch(std::move(command), checked->packOwner()));

        m_baseline.m_descriptors.push_back({std::move(authority), std::nullopt});
        return {};
    }

    std::error_code ContentAuthoringSession::deleteDefinition(
        const EContentDocument category, const std::string_view key, config::Diagnostics &diagnostics) {
        auto entry = authoredEntry(*m_index, category, key, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, entry);
        const auto &manifest_path = m_index->packs()[(*entry)->m_pack_index].m_manifest.m_source.m_path;
        const auto *manifest = sourceBytes(m_draft, manifest_path);
        auto patch = config::patchTree(
            *manifest, "assets." + (*entry)->m_name.m_local, config::ETreeSourceEdit::remove, {},
            manifest_path.string(), diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, patch);
        auto modified = config::applySourcePatch(*manifest, *patch);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, modified);

        const auto baseline = ledgerRecord(m_baseline, (*entry)->m_descriptor);
        PPR_RETURN_ERROR_ON_FAIL(
            ContentAuthoring, baseline == m_baseline.m_descriptors.end()
                                  ? std::make_error_code(std::errc::state_not_recoverable)
                                  : std::error_code{});
        SourceCommand command;
        command.m_target = baseline->m_target;
        command.m_changes.push_back({manifest_path, *manifest, std::move(*modified)});
        command.m_changes.push_back(
            {(*entry)->m_descriptor, *sourceBytes(m_draft, (*entry)->m_descriptor), std::nullopt});

        return executePatch(std::move(command));
    }

    Expected<bool> ContentAuthoringSession::undo() {
        auto result = m_history.undo([&](const auto &command) { return applyPatch(command, true); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, result);

        pruneTombstones();
        return *result;
    }

    Expected<bool> ContentAuthoringSession::redo() {
        auto result = m_history.redo([&](const auto &command) { return applyPatch(command, false); });
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, result);

        pruneTombstones();
        return *result;
    }

    std::error_code ContentAuthoringSession::revert() {
        config::Diagnostics diagnostics;
        const auto reader = [&](const asset::IndexedAsset &entry,
                                config::Diagnostics &local) -> Expected<asset::DescriptorSource> {
            const auto record = ledgerRecord(m_baseline, entry.m_descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                record == m_baseline.m_descriptors.end() or not record->m_bytes
                    ? asset::reject(
                          local, entry.m_descriptor.string(), "revert", "Baseline has no owned descriptor bytes")
                    : std::error_code{});

            return asset::DescriptorSource{entry.m_descriptor, *record->m_bytes};
        };

        const auto proofs = restorationProofs(m_baseline);
        auto restored = ContentSourceSnapshot::index(m_baseline.m_packs, reader, proofs, diagnostics);
        PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, restored);
        auto source = restored->source();

        m_draft = std::move(source);
        m_index = restored->packOwner();
        m_history.clear();
        ++m_revision;
        pruneTombstones();
        return {};
    }

    ContentAuthoringSnapshot ContentAuthoringSession::draftSnapshot() const {
        return {m_root, m_draft, m_baseline, m_parameters, m_revision, m_source_epoch};
    }

    // ------------------------------------------------------------------
    // definition validation and save reconciliation
    // ------------------------------------------------------------------

    bool ContentAuthoringSession::acceptValidation(
        const u64 revision, std::shared_ptr<const ContentDescriptorCatalog> catalog) noexcept {
        if (revision != m_revision or not catalog or not m_draft.sameMounts(catalog->packs())) {
            return false;
        }

        m_catalog = std::move(catalog);
        m_validated_revision = revision;
        return true;
    }

    bool ContentAuthoringSession::acceptSave(ContentAuthoringSaveResult result) {
        if (result.m_snapshot.m_root != m_root or not result.m_submitted_source.sameMounts(m_draft)) {
            return false;
        }

        const bool matching = result.m_snapshot.m_revision == m_revision and
                              sameSource(result.m_submitted_source, m_draft);
        const bool successful = matching and not result.m_error and result.m_catalog != nullptr;
        auto baseline = m_baseline;

        // Merge only the dispatched ledger's observed outcomes. Entries created
        // by newer edits are absent from that snapshot and remain owned locally.
        for (auto &record: result.m_snapshot.m_baseline.m_descriptors) {
            const auto current = ledgerRecord(baseline, record.m_target->path());
            if (current != baseline.m_descriptors.end()) {
                current->m_bytes = std::move(record.m_bytes);
            } else {
                baseline.m_descriptors.push_back(std::move(record));
            }
        }

        baseline.m_packs = std::move(result.m_snapshot.m_baseline.m_packs);
        m_baseline = std::move(baseline);
        ++m_revision;

        if (successful) {
            m_catalog = std::move(result.m_catalog);
            m_validated_revision = m_revision;
        }

        pruneTombstones();
        return successful;
    }

    Expected<ContentDraftValidation> validateContentDraft(
        const ContentAuthoringSnapshot &snapshot, config::Diagnostics &diagnostics, const IContext *const context,
        const std::optional<std::string_view> selected_world) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checkLedger(snapshot.m_baseline));

        const auto proofs = restorationProofs(snapshot.m_baseline);
        auto checked = ContentSourceSnapshot::index(snapshot.m_source, proofs, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checked);

        return validateContentDraft(*checked, snapshot.m_parameters, diagnostics, context, selected_world);
    }

    Expected<ContentDraftValidation> validateContentDraft(
        const ContentSourceSnapshot &snapshot, const sim::definition::GenerationCompileParameters parameters,
        config::Diagnostics &diagnostics, const IContext *const context,
        const std::optional<std::string_view> selected_world) {
        auto catalog = loadContentCatalog(snapshot, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, catalog);

        ContentDraftValidation result;
        result.m_definitions = std::make_shared<const ContentDescriptorCatalog>(std::move(*catalog));
        std::optional<sim::asset::WorldId> selected;
        if (selected_world) {
            auto id = resolveContentWorld(*result.m_definitions, *selected_world, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, id);

            selected = *id;
        }

        for (const auto &world: result.m_definitions->worlds()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, context ? context->error() : std::error_code{});
            if (selected == world.m_id) {
                auto view = selectWorldCatalog(result.m_definitions, world.m_id, parameters, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, view);

                result.m_selected = std::make_shared<const ContentCatalog>(std::move(*view));
            } else {
                auto assembled = assembleWorld(*result.m_definitions, world.m_id, parameters, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, assembled);
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    // staged physical saves and partial disk ownership
    // ------------------------------------------------------------------

    namespace {
        struct SaveFile {
            std::filesystem::path m_path;
            std::optional<std::string> m_before, m_after;
            bool m_manifest{};
        };

        std::error_code checkTarget(
            const asset::PackSet &packs, const Restoration &proof, config::Diagnostics &diagnostics) {
            const auto relative = proof.path().lexically_relative(proof.root());
            auto checked = packs.containedPath(proof.name().m_pack, relative.generic_string(), diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, checked);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, *checked != proof.path() ? conflict(diagnostics, proof.path()) : std::error_code{});

            return {};
        }

        std::error_code replaceFile(const SaveFile &file, config::Diagnostics &diagnostics) {
            auto current = readDisk(
                file.m_path, file.m_manifest ? asset::kMaxManifestBytes : asset::kMaxDescriptorBytes, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, current);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, *current != file.m_before ? conflict(diagnostics, file.m_path) : std::error_code{});
            if (file.m_before == file.m_after) {
                return {};
            }

            std::error_code error;
            if (not file.m_after) {
                const auto removed = std::filesystem::remove(file.m_path, error);
                PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, error);
                PPR_RETURN_ERROR_ON_FAIL(
                    ContentAuthoring, not removed ? conflict(diagnostics, file.m_path) : std::error_code{});

                return {};
            }

            std::filesystem::create_directories(file.m_path.parent_path(), error);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, error);
            auto temporary = file.m_path;
            temporary += ".workshop.tmp";
            bool acquired{};
            PPR_DEFER {
                if (acquired) {
                    std::error_code cleanup;
                    std::filesystem::remove(temporary, cleanup);
                    // Best effort staging cleanup must not replace the save error.
                    PPR_LOG_WARNING_ON_FAIL(ContentAuthoring, cleanup);
                }
            };

            std::ofstream stream{temporary, std::ios::binary | std::ios::out | std::ios::noreplace};
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, not stream ? std::make_error_code(std::errc::io_error) : std::error_code{});
            acquired = true;
            stream.write(file.m_after->data(), static_cast<std::streamsize>(file.m_after->size()));
            stream.flush();
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, not stream ? std::make_error_code(std::errc::io_error) : std::error_code{});
            stream.close();
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, not stream ? std::make_error_code(std::errc::io_error) : std::error_code{});

            auto staged = readDisk(
                temporary, file.m_manifest ? asset::kMaxManifestBytes : asset::kMaxDescriptorBytes, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, staged);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring,
                *staged != file.m_after ? std::make_error_code(std::errc::io_error) : std::error_code{});

            auto checked = readDisk(
                file.m_path, file.m_manifest ? asset::kMaxManifestBytes : asset::kMaxDescriptorBytes, diagnostics);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, checked);
            PPR_RETURN_ERROR_ON_FAIL(
                ContentAuthoring, *checked != file.m_before ? conflict(diagnostics, file.m_path) : std::error_code{});

            std::filesystem::rename(temporary, file.m_path, error);
            PPR_RETURN_ERROR_ON_FAIL(ContentAuthoring, error);
            acquired = false;
            return {};
        }
    }

    Expected<ContentAuthoringSaveResult> saveContentDraft(
        ContentAuthoringSnapshot snapshot, config::Diagnostics &diagnostics, const IContext *const context) try {
        const hal::ProfileScope scope{"Content.Authoring.Save"};
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, context ? context->error() : std::error_code{});
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checkLedger(snapshot.m_baseline));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAuthoring, not sameMounts(snapshot.m_baseline, snapshot.m_source)
                                  ? std::make_error_code(std::errc::invalid_argument)
                                  : std::error_code{});

        const auto proofs = restorationProofs(snapshot.m_baseline);
        auto desired = ContentSourceSnapshot::index(snapshot.m_source, proofs, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, desired);

        auto definitions = validateContentDraft(*desired, snapshot.m_parameters, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, definitions);

        auto catalog = std::move(definitions->m_definitions);

        // The baseline may contain owned orphans. Its manifest index identifies
        // which records still have published owners and cannot be removed early.
        auto published = asset::PackSet::index(snapshot.m_baseline.m_packs, kContentAssetKinds, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, published);
        const auto desired_index = desired->packOwner();
        Array<SaveFile> files;
        files.reserve(snapshot.m_source.m_packs.size() + snapshot.m_baseline.m_descriptors.size());

        for (const auto &pack: snapshot.m_source.m_packs) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, context ? context->error() : std::error_code{});
            const auto baseline = std::ranges::find(
                snapshot.m_baseline.m_packs, pack.m_manifest.m_path,
                [](const auto &value) -> const auto & { return value.m_manifest.m_path; });
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, baseline == snapshot.m_baseline.m_packs.end()
                                      ? std::make_error_code(std::errc::invalid_argument)
                                      : std::error_code{});
            auto disk = readDisk(pack.m_manifest.m_path, asset::kMaxManifestBytes, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, disk);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                not *disk or (**disk != baseline->m_manifest.m_bytes and **disk != pack.m_manifest.m_bytes)
                    ? conflict(diagnostics, pack.m_manifest.m_path)
                    : std::error_code{});

            files.push_back({pack.m_manifest.m_path, std::move(*disk), pack.m_manifest.m_bytes, true});
        }

        for (const auto &entry: desired_index->assets()) {
            const auto record = ledgerRecord(snapshot.m_baseline, entry.m_descriptor);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring,
                record == snapshot.m_baseline.m_descriptors.end() or
                        not record->m_target->matches(*desired_index, entry)
                    ? asset::reject(
                          diagnostics, entry.m_descriptor.string(), "save", "Descriptor has no matching owned target")
                    : std::error_code{});
        }

        bool fresh_targets{};

        for (const auto &record: snapshot.m_baseline.m_descriptors) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, context ? context->error() : std::error_code{});
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, checkTarget(*published, *record.m_target, diagnostics));
            const auto *requested = sourceBytes(snapshot.m_source, record.m_target->path());
            auto disk = readDisk(record.m_target->path(), asset::kMaxDescriptorBytes, diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, disk);

            // Known absence never authorizes takeover, even for equal bytes.
            const bool matches = record.m_bytes
                                     ? (*disk == record.m_bytes or (requested and *disk and **disk == *requested))
                                     : not disk->has_value();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentAuthoring, not matches ? conflict(diagnostics, record.m_target->path()) : std::error_code{});

            fresh_targets = fresh_targets or (requested and not disk->has_value());
            files.push_back(
                {record.m_target->path(), std::move(*disk),
                    requested ? std::optional<std::string>{*requested} : std::nullopt, false});
        }

        auto submitted = snapshot.m_source;
        std::error_code first_error;
        const auto commit = [&](const SaveFile &file) {
            try {
                const auto result = replaceFile(file, diagnostics);
                if (not result) {
                    if (file.m_manifest) {
                        const auto pack = std::ranges::find(
                            snapshot.m_baseline.m_packs, file.m_path,
                            [](const auto &value) -> const auto & { return value.m_manifest.m_path; });
                        pack->m_manifest.m_bytes = *file.m_after;
                    } else {
                        ledgerRecord(snapshot.m_baseline, file.m_path)->m_bytes = file.m_after;
                    }
                }

                PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, result);
            } catch (const std::bad_alloc &) {
                throw;
            } catch (const std::system_error &error) {
                // Commit has started: keep every earlier observed outcome and
                // attempt the remaining independent files in this phase.
                PPR_RETAIN_ERROR_ON_FAIL(
                    ContentAuthoring, first_error,
                    error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
            } catch (...) {
                PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, std::errc::state_not_recoverable);
            }
        };

        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, context ? context->error() : std::error_code{});

        // Obsolete unpublished targets must not accumulate across changing
        // failed saves. Desired unpublished targets remain reusable candidates.
        for (const auto &file: files) {
            if (file.m_manifest or file.m_after or not file.m_before) {
                continue;
            }

            const bool referenced = std::ranges::any_of(
                published->assets(), [&](const auto &entry) { return entry.m_descriptor == file.m_path; });
            if (not referenced) {
                commit(file);
            }
        }

        // Once physical commit begins, complete independent operations instead
        // of abandoning their ownership ledger on cancellation.
        if (not first_error or not fresh_targets) {
            for (const auto &file: files) {
                if (not file.m_manifest and file.m_after) {
                    commit(file);
                }
            }
        }

        // No manifest may expose a failed new or changed descriptor.
        if (not first_error) {
            for (const auto &file: files) {
                if (file.m_manifest) {
                    commit(file);
                }
            }
        }

        // No removal can invalidate an old manifest that failed replacement.
        if (not first_error) {
            for (const auto &file: files) {
                if (file.m_manifest or file.m_after or not file.m_before) {
                    continue;
                }

                const auto record = ledgerRecord(snapshot.m_baseline, file.m_path);
                if (record->m_bytes) {
                    commit(file);
                }
            }
        }

        PPR_RETAIN_ERROR_ON_FAIL(ContentAuthoring, first_error, context ? context->error() : std::error_code{});
        if (first_error) {
            catalog.reset();
        }

        return ContentAuthoringSaveResult{std::move(snapshot), std::move(catalog), std::move(submitted), first_error};
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ContentAuthoring, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentAuthoring, std::errc::state_not_recoverable);
        std::unreachable();
    }
}
