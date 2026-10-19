module;
#include "pP/Macros.h"
module game.colony.worldgen_panel;
import imgui;
import engine.app;
import engine.asset;
import engine.image;
import std;
import game.ui;

namespace pP::colony {
    // ------------------------------------------------------------------
    // owned descriptor documents
    // ------------------------------------------------------------------

    namespace {
        const std::array<std::string_view, kContentDocumentCount> &categories() {
            static const auto names = [] {
                std::array<std::string_view, kContentDocumentCount> result{};

                for (const auto &binding: contentDocumentBindings()) {
                    result[static_cast<std::size_t>(binding.m_document)] = binding.m_collection;
                }

                return result;
            }();
            return names;
        }

        void emit(Array<WorkshopIntent> &intents, const EWorkshopIntent kind) {
            intents.push_back({.m_kind = kind});
        }

        WorldgenPanelState::DescriptorDocument *findDocument(WorldgenPanelState &state, const std::string_view name) {
            const auto found = std::ranges::find(
                state.m_documents, name, &WorldgenPanelState::DescriptorDocument::m_name);
            return found == state.m_documents.end() ? nullptr : std::addressof(*found);
        }

        config::Document &document(WorldgenPanelState &state, WorldgenPanelState::DescriptorDocument &record) {
            if (not record.m_decoded) {
                record.m_decoded = true;
                const auto &sources = state.m_document_source->m_descriptors;
                const auto source = std::ranges::find(sources, record.m_path, &asset::DescriptorSource::m_path);
                if (source == sources.end()) {
                    return record.m_document;
                }

                record.m_cached_bytes = source->m_bytes;
                config::Diagnostics diagnostics{8u};
                auto decoded = config::decode(
                    source->m_bytes, source->m_path.string(), diagnostics, config::EDecodeMode::tree);
                if (decoded) {
                    record.m_document = std::move(*decoded);
                }
            }

            if (not record.m_projected and
                state.m_document_catalog and
                state.m_document_validated_revision == state.m_document_revision) {
                const auto append = [&](const auto &entries) {
                    const auto entry = std::ranges::find_if(entries, [&](const auto &value) {
                        return value.m_descriptor.m_source.m_path == record.m_path;
                    });
                    if (entry == entries.end()) {
                        return;
                    }

                    auto projection = config::documentFrom(entry->m_descriptor.m_value);
                    PPR_ASSERT(projection.has_value());

                    for (auto &value: projection->m_entries) {
                        if (std::ranges::find(record.m_document.m_entries, value.m_key, &config::Entry::m_key) ==
                            record.m_document.m_entries.end()) {
                            value.m_location = "production decoder default (omitted in source)";
                            record.m_document.m_entries.push_back(std::move(value));
                        }
                    }
                };

                const auto &catalog = *state.m_document_catalog;
                switch (record.m_category) {
                    case EContentDocument::materials: append(catalog.materials()); break;
                    case EContentDocument::elements: append(catalog.elements().definitions()); break;
                    case EContentDocument::fields: append(catalog.fields()); break;
                    case EContentDocument::biomes: append(catalog.biomes()); break;
                    case EContentDocument::features: append(catalog.features()); break;
                    case EContentDocument::rules: append(catalog.rules()); break;
                    case EContentDocument::worlds: append(catalog.worlds()); break;
                    case EContentDocument::textures: append(catalog.textures().definitions()); break;
                    case EContentDocument::backgrounds: append(catalog.backgrounds()); break;
                    default: break;
                }

                std::ranges::sort(record.m_document.m_entries, {}, &config::Entry::m_key);
                record.m_projected = true;
            }

            return record.m_document;
        }

        std::string_view text(const config::Document &value, const std::string_view key) {
            const auto entry = std::ranges::find(value.m_entries, key, &config::Entry::m_key);
            return entry != value.m_entries.end() and entry->m_value.m_kind == config::EFileKind::text
                       ? std::string_view{entry->m_value.m_text}
                       : std::string_view{};
        }

        Array<std::string> children(const config::Document &value, const std::string_view root) {
            const auto prefix = std::string{root} + ".";
            std::set<std::string> keys;
            const auto collect = [&](const std::string_view path) {
                if (path.starts_with(prefix)) {
                    const auto tail = path.substr(prefix.size());
                    keys.emplace(tail.substr(0u, tail.find('.')));
                }
            };

            for (const auto &entry: value.m_entries) {
                collect(entry.m_key);
            }

            for (const auto &group: value.m_groups) {
                collect(group.m_path);
            }

            return {keys.begin(), keys.end()};
        }

        WorldgenPanelState::DescriptorDocument *linkedDocument(
            WorldgenPanelState &state, const WorldgenPanelState::DescriptorDocument &owner,
            const std::string_view reference, const std::string_view kind) {
            config::Diagnostics diagnostics{1u};
            auto entry = state.m_document_index->resolve(
                std::string_view{owner.m_name}.substr(0u, owner.m_name.find(':')), reference, kind, diagnostics);
            return entry ? findDocument(state, (*entry)->m_name.string()) : nullptr;
        }

        WorldgenPanelState::DescriptorDocument *worldDocument(WorldgenPanelState &state) {
            std::string name{state.m_profile.data()};
            if (name.find(':') == std::string::npos) {
                if (not state.m_document_index or state.m_document_index->packs().size() != 1u) {
                    return nullptr;
                }

                name = state.m_document_index->packs().front().m_manifest.m_value.m_pack_id + ":" + name;
            }

            auto *const found = findDocument(state, name);
            return found and found->m_category == EContentDocument::worlds ? found : nullptr;
        }

        /// References are synchronous: finish consuming them before changing the profile and querying again.
        const Array<std::string> &sampleBiomes(WorldgenPanelState &state) {
            const std::string_view profile{state.m_profile.data()};
            if (state.m_sample_choices_current and state.m_sample_profile == profile) {
                return state.m_sample_biomes;
            }

            const hal::ProfileScope scope{"Workshop.UI.PreviewChoices"};
            auto *const world = worldDocument(state);
            Array<std::string> result;
            if (world) {
                auto &value = document(state, *world);
                for (const auto &key: children(value, "profile.biomes")) {
                    const auto alias = text(value, "profile.biomes." + key + ".biome");
                    if (not alias.empty()) {
                        result.emplace_back(alias);
                    }
                }
            }

            std::ranges::sort(result);
            result.erase(std::unique(result.begin(), result.end()), result.end());

            state.m_sample_biomes = std::move(result);
            state.m_sample_profile = profile;
            state.m_sample_choices_current = true;
            return state.m_sample_biomes;
        }

        // ------------------------------------------------------------------
        // contextual reference choices
        // ------------------------------------------------------------------

        std::string_view assetReferenceKind(const EContentDocument category, const std::string_view property) {
            std::string_view asset_kind;
            if (category == EContentDocument::materials and property == "element") {
                asset_kind = sim::asset::kElementKind;
            } else if (property.starts_with("appearance.") and property.ends_with("texture")) {
                asset_kind = image::kTextureAssetKind;
            } else if (category == EContentDocument::backgrounds and property.ends_with(".texture")) {
                asset_kind = image::kTextureAssetKind;
            } else if (category == EContentDocument::worlds and property == "appearance.background_stack") {
                asset_kind = kBackgroundAssetKind;
            } else if (category == EContentDocument::worlds and
                       property.starts_with("palette.") and
                       property.ends_with(".asset")) {
                const auto tail = property.substr(8u);
                const auto collection = tail.substr(0u, tail.find('.'));
                const auto binding = std::ranges::find(
                    contentDocumentBindings(), collection, &ContentDocumentBinding::m_collection);
                if (binding != contentDocumentBindings().end()) {
                    asset_kind = contentAssetKind(binding->m_document);
                }
            }

            return asset_kind;
        }

        Array<std::string> referenceChoices(
            WorldgenPanelState &state, WorldgenPanelState::DescriptorDocument &owner, const std::string_view property,
            const ContentPropertyDescriptor &descriptor) {
            const auto asset_kind = assetReferenceKind(owner.m_category, property);

            if (not asset_kind.empty()) {
                const auto declaring = std::string_view{owner.m_name}.substr(0u, owner.m_name.find(':'));
                const auto packs = state.m_document_index->packs();
                const auto pack = std::ranges::find_if(
                    packs, [&](const auto &value) { return value.m_manifest.m_value.m_pack_id == declaring; });
                Array<std::string> result;
                for (const auto &entry: state.m_document_index->assets()) {
                    const bool dependency = pack != packs.end() and
                                            std::ranges::any_of(
                                                pack->m_manifest.m_value.m_requires.m_entries,
                                                [&](const auto &required) {
                                                    return required.m_value.m_pack_id == entry.m_name.m_pack;
                                                });
                    if (entry.m_kind == asset_kind and (entry.m_name.m_pack == declaring or dependency)) {
                        result.push_back(entry.m_name.string());
                    }
                }

                return result;
            }

            if (not descriptor.m_reference_target) {
                return {};
            }

            auto *const world = owner.m_category == EContentDocument::worlds ? &owner : worldDocument(state);
            if (not world) {
                return {};
            }

            auto &world_value = document(state, *world);
            const auto &target = *descriptor.m_reference_target;
            const auto palette = "palette." + std::string{target.m_collection};
            const auto bound = [&](const std::string_view collection, const std::string_view alias,
                                   const std::string_view kind) {
                return linkedDocument(
                    state, *world,
                    text(world_value, "palette." + std::string{collection} + "." + std::string{alias} + ".asset"),
                    kind);
            };

            if (target.m_collection == "markers") {
                std::string_view feature;
                if (owner.m_category == EContentDocument::rules) {
                    const auto placement = text(document(state, owner), "definition.payload.matter_fill.placement");
                    auto *const rule = bound("rules", placement, sim::asset::kRuleKind);
                    if (not rule) {
                        return {};
                    }

                    feature = text(document(state, *rule), "definition.payload.feature.feature");
                } else {
                    feature = text(world_value, "profile.start.feature");
                }

                auto *const selected = bound("features", feature, sim::asset::kFeatureKind);
                if (not selected) {
                    return {};
                }

                auto &value = document(state, *selected);
                auto result = children(value, "definition.markers");
                std::erase_if(result, [&](const auto &key) {
                    return text(value, "definition.markers." + key + ".role") != target.m_suitability;
                });
                return result;
            }

            auto result = children(world_value, palette);
            std::erase_if(result, [&](const auto &alias) {
                if (target.m_collection == "materials") {
                    auto *const material = bound("materials", alias, sim::asset::kMaterialKind);
                    if (not material) {
                        return true;
                    }

                    const auto representation = text(document(state, *material), "representation");
                    return target.m_suitability == "particle" ? representation != "particle"
                                                              : representation != "terrain";
                }

                if (target.m_collection == "rules") {
                    auto *const rule = bound("rules", alias, sim::asset::kRuleKind);
                    if (not rule) {
                        return true;
                    }

                    const auto &value = document(state, *rule);
                    const auto enabled = std::ranges::find(
                        value.m_entries, "definition.enabled", &config::Entry::m_key);
                    return text(value, "definition.payload.tag") != "feature" or
                           (enabled != value.m_entries.end() and
                               enabled->m_value.m_kind == config::EFileKind::boolean and
                               not enabled->m_value.m_boolean);
                }

                if (target.m_collection == "biomes" and
                    not (owner.m_category == EContentDocument::worlds and property.starts_with("profile.biomes."))) {
                    for (const auto &enabled: children(world_value, "profile.biomes")) {
                        if (text(world_value, "profile.biomes." + enabled + ".biome") == alias) {
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
        // validated reverse references
        // ------------------------------------------------------------------

        void collectUsedBy(WorldgenPanelState &state) {
            state.m_uses.clear();
            state.m_use_selection = state.m_selected_key;
            const auto &catalog = *state.m_document_catalog;
            const auto assets = catalog.packs().assets();
            const auto selected = std::ranges::find_if(
                assets, [&](const auto &entry) { return entry.m_name.string() == state.m_selected_key; });
            if (selected == assets.end()) {
                return;
            }

            const auto scan = [&](const auto &entries, const EContentDocument category) {
                for (const auto &entry: entries) {
                    const auto owner = std::ranges::find(
                        assets, asset::AssetKey{*entry.m_id}, &asset::IndexedAsset::m_key);
                    PPR_ASSERT(owner != assets.end());
                    const auto append = [&](const std::string &property, const std::string &world) {
                        state.m_uses.push_back({owner->m_name.string(), property, world, category});
                    };

                    const auto aliasMatches = [&](const auto &palette, const std::string_view alias,
                                                  const std::string_view kind, const std::string_view pack,
                                                  const asset::AssetKey key) {
                        const auto binding = std::ranges::find_if(palette.m_entries, [&](const auto &value) {
                            return alias.empty() or value.m_key == alias;
                        });
                        // Empty aliases are used only for the ownership membership query below.
                        for (auto current = binding; current != palette.m_entries.end(); ++current) {
                            if (not alias.empty() and current->m_key != alias) {
                                break;
                            }

                            config::Diagnostics diagnostics{1u};
                            auto linked = catalog.packs().resolve(pack, current->m_value.m_asset, kind, diagnostics);
                            PPR_ASSERT(linked.has_value());
                            if ((*linked)->m_key == key) {
                                return true;
                            }
                        }

                        return false;
                    };

                    const auto bound = [&](const auto &palette, const std::string_view collection,
                                           const std::string_view alias, const std::string_view pack,
                                           const asset::AssetKey key) {
                        if (collection == "materials") {
                            return aliasMatches(palette.m_materials, alias, sim::asset::kMaterialKind, pack, key);
                        }

                        if (collection == "fields") {
                            return aliasMatches(palette.m_fields, alias, sim::asset::kFieldKind, pack, key);
                        }

                        if (collection == "biomes") {
                            return aliasMatches(palette.m_biomes, alias, sim::asset::kBiomeKind, pack, key);
                        }

                        if (collection == "features") {
                            return aliasMatches(palette.m_features, alias, sim::asset::kFeatureKind, pack, key);
                        }

                        if (collection == "rules") {
                            return aliasMatches(palette.m_rules, alias, sim::asset::kRuleKind, pack, key);
                        }

                        return false;
                    };

                    config::visitRecursive(
                        entry.m_descriptor.m_value,
                        [&](const std::string &property, const auto &member, const auto &value) {
                            const auto reference = [&]() -> std::string_view {
                                using T = std::remove_cvref_t<decltype(value)>;
                                if constexpr (std::same_as<T, std::string>) {
                                    return value;
                                } else if constexpr (std::same_as<T, std::optional<std::string>>) {
                                    return value ? std::string_view{*value} : std::string_view{};
                                } else if constexpr (requires { value.m_key; }) {
                                    return value.m_key;
                                } else if constexpr (requires { value->m_key; }) {
                                    return value ? std::string_view{value->m_key} : std::string_view{};
                                } else {
                                    return {};
                                }
                            }();
                            if (reference.empty()) {
                                return;
                            }

                            const auto kind = assetReferenceKind(category, property);
                            if (not kind.empty()) {
                                config::Diagnostics diagnostics{1u};
                                auto linked = catalog.packs().resolve(
                                    owner->m_name.m_pack, reference, kind, diagnostics);
                                PPR_ASSERT(linked.has_value());
                                if ((*linked)->m_key == selected->m_key) {
                                    append(property, {});
                                }

                                return;
                            }

                            if constexpr (requires { member.m_constraint.m_target; }) {
                                const auto &target = member.m_constraint.m_target;
                                for (const auto &world: catalog.worlds()) {
                                    const auto world_owner = std::ranges::find(
                                        assets, asset::AssetKey{*world.m_id}, &asset::IndexedAsset::m_key);
                                    PPR_ASSERT(world_owner != assets.end());
                                    const auto &palette = world.m_descriptor.m_value.m_palette;
                                    const bool belongs = category == EContentDocument::worlds
                                                             ? owner->m_key == world_owner->m_key
                                                             : bound(
                                                                   palette,
                                                                   categories()[static_cast<std::size_t>(category)], {},
                                                                   world_owner->m_name.m_pack, owner->m_key);
                                    if (belongs and bound(
                                                        palette, target.m_collection, reference,
                                                        world_owner->m_name.m_pack, selected->m_key)) {
                                        append(property, world_owner->m_name.string());
                                    }
                                }
                            }
                        });
                }
            };

            scan(catalog.elements().definitions(), EContentDocument::elements);
            scan(catalog.materials(), EContentDocument::materials);
            scan(catalog.fields(), EContentDocument::fields);
            scan(catalog.biomes(), EContentDocument::biomes);
            scan(catalog.features(), EContentDocument::features);
            scan(catalog.rules(), EContentDocument::rules);
            scan(catalog.worlds(), EContentDocument::worlds);
            scan(catalog.textures().definitions(), EContentDocument::textures);
            scan(catalog.backgrounds(), EContentDocument::backgrounds);
        }

        // ------------------------------------------------------------------
        // navigation and descriptor cache
        // ------------------------------------------------------------------

        bool matches(const std::string_view text, const std::string_view search) {
            if (search.empty()) {
                return true;
            }

            return std::search(text.begin(), text.end(), search.begin(), search.end(), [](const char a, const char b) {
                const auto lower = [](const char ch) { return ch >= 'A' and ch <= 'Z' ? ch + ('a' - 'A') : ch; };
                return lower(a) == lower(b);
            }) != text.end();
        }

        std::span<const config::Diagnostic> currentDiagnostics(const WorldgenPanelView &view) {
            return view.m_diagnostics_revision == view.m_revision ? view.m_property_diagnostics
                                                                  : std::span<const config::Diagnostic>{};
        }

        bool contentErrors(const WorldgenPanelView &view) {
            return view.m_draft_invalid;
        }

        void disabledReason(const std::string_view reason) {
            if (not reason.empty() and ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%.*s", static_cast<int>(reason.size()), reason.data());
            }
        }

        /// One workshop action. The reason text drives the disabled state and
        /// its hover tooltip; a primary action carries the filled accent with
        /// its dark text token, and the disabled alpha still applies over it.
        void actionButton(
            const char *label, const std::string_view reason, const EWorkshopIntent kind,
            Array<WorkshopIntent> &intents, const bool primary = false) {
            ImGui::BeginDisabled(not reason.empty());
            if (primary) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui::theme::kAccent.vec4());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::theme::kAccentHover.vec4());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::theme::kAccentActive.vec4());
                ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::kAccentText.vec4());
            }
            const bool pressed = ImGui::Button(label);
            if (primary) {
                ImGui::PopStyleColor(4);
            }
            ImGui::EndDisabled();
            disabledReason(reason);
            if (pressed) {
                emit(intents, kind);
            }
        }

        /// One workshop action as a menu item. The reason text shares the
        /// actionButton contract: it both disables the item and rides its hover
        /// tooltip, so a menu entry and a button for one intent stay in step.
        void actionMenuItem(
            const char *label, const std::string_view reason, const EWorkshopIntent kind,
            Array<WorkshopIntent> &intents) {
            if (ImGui::MenuItem(label, nullptr, false, reason.empty())) {
                emit(intents, kind);
            }

            disabledReason(reason);
        }

        void selectDefinition(WorldgenPanelState &state, const std::string_view category, const std::string_view key) {
            state.m_pending_category = category;
            state.m_pending_key = key;
            state.m_pending_property.clear();
            state.m_selection_pending = true;
            state.m_show_properties = true;
        }

        void flushPendingEdits(WorldgenPanelState &state, Array<WorkshopIntent> &intents) {
            auto *const record = findDocument(state, state.m_selected_key);
            if (not record) {
                return;
            }

            const auto commit = [&](const std::string &property, const config::FileValue &value) {
                WorkshopIntent intent{.m_kind = EWorkshopIntent::edit_definition};
                intent.m_document = record->m_category;
                intent.m_key = record->m_name;
                intent.m_property = property;
                intent.m_value = value;
                intents.push_back(std::move(intent));
            };

            if (not state.m_numeric_edit_key.empty()) {
                commit(state.m_numeric_edit_key, state.m_numeric_edit);
                state.m_numeric_edit_key.clear();
            }

            if (not state.m_edit_key.empty() and not state.m_text_edit.empty()) {
                const auto &entries = document(state, *record).m_entries;
                const auto entry = std::ranges::find(entries, state.m_edit_key, &config::Entry::m_key);
                if (entry != entries.end() and entry->m_value.m_text != state.m_text_edit.data()) {
                    auto value = entry->m_value;
                    value.m_text = state.m_text_edit.data();
                    commit(state.m_edit_key, value);
                }

                state.m_edit_key.clear();
            }
        }

        bool navigateDiagnostic(WorldgenPanelState &state, const config::Diagnostic &diagnostic) {
            for (const auto &record: state.m_documents) {
                const auto path = record.m_path.string();
                if (diagnostic.m_location == path or diagnostic.m_location.starts_with(path + ":")) {
                    selectDefinition(state, categories()[static_cast<std::size_t>(record.m_category)], record.m_name);
                    state.m_pending_property = diagnostic.m_property;
                    state.m_property_search[0u] = '\0';
                    state.m_show_properties = true;
                    return true;
                }
            }

            return false;
        }

        void refreshDocuments(WorldgenPanelState &state, const WorldgenPanelView &view) {
            if (view.m_source == state.m_document_source and
                view.m_index == state.m_document_index and
                view.m_revision == state.m_document_revision and
                view.m_validated_revision == state.m_document_validated_revision and
                view.m_catalog == state.m_document_catalog) {
                return;
            }

            const hal::ProfileScope scope{"Workshop.UI.Documents.Refresh"};
            auto previous = std::move(state.m_documents);
            state.m_document_source = view.m_source;
            state.m_document_index = view.m_index;
            state.m_document_revision = view.m_revision;
            state.m_document_validated_revision = view.m_validated_revision;
            state.m_document_catalog = view.m_catalog;
            state.m_tree_selection.clear();
            state.m_use_selection.clear();
            state.m_sample_choices_current = false;
            for (auto &keys: state.m_record_keys) {
                keys.clear();
            }

            if (not view.m_source or not view.m_index) {
                return;
            }

            if (state.m_selected_asset) {
                const auto assets = view.m_index->assets();
                const auto active = std::ranges::find(assets, *state.m_selected_asset, &asset::IndexedAsset::m_key);
                if (active == assets.end()) {
                    state.m_selected_asset.reset();
                    state.m_browser.m_active_key.reset();
                    state.m_selected_key.clear();
                    state.m_edit_key.clear();
                    state.m_numeric_edit_key.clear();
                }
            }

            std::map<std::filesystem::path, const asset::DescriptorSource *> sources;
            for (const auto &source: view.m_source->m_descriptors) {
                sources.emplace(source.m_path, std::addressof(source));
            }

            state.m_documents.reserve(view.m_index->assets().size());
            for (const auto &entry: view.m_index->assets()) {
                const auto binding = std::ranges::find_if(contentDocumentBindings(), [&](const auto &value) {
                    return contentAssetKind(value.m_document) == entry.m_kind;
                });
                if (binding == contentDocumentBindings().end()) {
                    continue;
                }

                WorldgenPanelState::DescriptorDocument record;
                record.m_name = entry.m_name.string();
                record.m_path = entry.m_descriptor;
                record.m_category = binding->m_document;
                const auto old = std::ranges::find(
                    previous, record.m_name, &WorldgenPanelState::DescriptorDocument::m_name);
                const auto source = sources.find(record.m_path);
                if (old != previous.end() and
                    old->m_path == record.m_path and
                    old->m_decoded and
                    source != sources.end() and
                    old->m_cached_bytes == source->second->m_bytes) {
                    record.m_document = std::move(old->m_document);
                    record.m_cached_bytes = std::move(old->m_cached_bytes);
                    record.m_decoded = true;
                    record.m_projected = old->m_projected;
                }

                state.m_record_keys[static_cast<std::size_t>(record.m_category)].push_back(record.m_name);
                state.m_documents.push_back(std::move(record));
            }

            const std::string_view requested_world = state.m_profile.data();
            if (requested_world.find(':') == std::string_view::npos and view.m_index->packs().size() == 1u) {
                const auto qualified = view.m_index->packs().front().m_manifest.m_value.m_pack_id +
                                       ":" +
                                       std::string{requested_world};
                if (findDocument(state, qualified)) {
                    std::snprintf(state.m_profile.data(), state.m_profile.size(), "%s", qualified.c_str());
                }
            }
        }

        void refreshPropertyTree(WorldgenPanelState &state) {
            const auto selection =
                state.m_selected_category + "." + state.m_selected_key + "@" + state.m_profile.data();
            if (selection == state.m_tree_selection) {
                return;
            }

            const hal::ProfileScope scope{"Workshop.UI.PropertyTree.Refresh"};
            state.m_tree_selection = selection;
            state.m_property_tree.clear();

            const auto category = std::ranges::find(categories(), state.m_selected_category);
            if (category == categories().end()) {
                return;
            }

            const auto index = static_cast<std::size_t>(category - categories().begin());
            auto *const record = findDocument(state, state.m_selected_key);
            if (not record) {
                return;
            }

            const auto &selected_document = document(state, *record);

            for (std::size_t entry_index = 0u; entry_index < selected_document.m_entries.size(); ++entry_index) {
                const auto &entry = selected_document.m_entries[entry_index];
                auto remaining = std::string_view{entry.m_key};
                const auto property = remaining;
                auto *children = &state.m_property_tree;
                std::string path;
                while (not remaining.empty()) {
                    const auto dot = remaining.find('.');
                    const auto name = remaining.substr(0u, dot);
                    path += name;
                    auto node = std::ranges::find(*children, name, &WorldgenPanelState::PropertyNode::m_name);
                    if (node == children->end()) {
                        children->push_back({std::string{name}, path});
                        node = children->end() - 1;
                    }

                    if (dot == std::string_view::npos) {
                        node->m_entry = entry_index;
                        node->m_descriptor = contentPropertyDescriptor(
                            static_cast<EContentDocument>(index), property, selected_document, state.m_selected_key);
                        if (node->m_descriptor) {
                            for (const auto choice: node->m_descriptor->m_choices) {
                                node->m_choices.emplace_back(choice);
                            }

                            auto references = referenceChoices(state, *record, property, *node->m_descriptor);
                            if (node->m_descriptor->m_reference_target or not references.empty()) {
                                node->m_choices = std::move(references);
                            }
                        }

                        break;
                    }

                    path += '.';
                    remaining.remove_prefix(dot + 1u);
                    children = &node->m_children;
                }
            }
        }

        // ------------------------------------------------------------------
        // descriptor property controls
        // ------------------------------------------------------------------

        bool textControl(
            WorldgenPanelState &state, const config::Entry &entry, config::FileValue &value,
            const std::optional<ContentPropertyDescriptor> &descriptor) {
            bool changed = false;
            if (state.m_edit_key != entry.m_key) {
                const auto scratch = mem::ScratchPad::open();
                const auto maximum = descriptor and descriptor->m_text_length ? *descriptor->m_text_length
                                                                              : asset::kMaxDescriptorBytes;
                Array<char, mem::ScratchPad> temporary(std::max(maximum, value.m_text.size()) + 1u, '\0');
                std::snprintf(temporary.data(), temporary.size(), "%s", value.m_text.c_str());

                if (ImGui::InputText(
                        "##value", temporary.data(), temporary.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    value.m_text = temporary.data();
                    changed = true;
                }

                if (ImGui::IsItemActive()) {
                    state.m_edit_key = entry.m_key;
                    state.m_text_edit.assign(temporary.begin(), temporary.end());
                }
            } else {
                const bool entered = ImGui::InputText(
                    "##value", state.m_text_edit.data(), state.m_text_edit.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                if (entered or ImGui::IsItemDeactivatedAfterEdit()) {
                    value.m_text = state.m_text_edit.data();
                    changed = value.m_text != entry.m_value.m_text;
                }

                if (not ImGui::IsItemActive()) {
                    state.m_edit_key.clear();
                }
            }

            return changed;
        }

        /// One numeric property: the metadata-hinted range slider plus an
        /// exact-entry field clamped to the authoritative schema domain rather
        /// than the slider's display hint range. Returns a live edit and reports
        /// a completed one through `finished` for the caller's pending-edit commit.
        bool numericControl(
            const WorldgenPanelState::PropertyNode &node, config::FileValue &value, bool &finished,
            const bool can_reset) {
            if (not node.m_descriptor) {
                bool changed = false;
                if (value.m_kind == config::EFileKind::integer) {
                    changed = ImGui::InputScalar("##value", ImGuiDataType_S64, &value.m_integer);
                } else {
                    changed = ImGui::InputDouble("##value", &value.m_floating, 0.0, 0.0, "%.8g");
                }

                finished = ImGui::IsItemDeactivatedAfterEdit();
                return changed;
            }

            const auto &descriptor = *node.m_descriptor;
            double lower = descriptor.m_slider_min, upper = descriptor.m_slider_max;

            if (descriptor.m_min) {
                lower = (std::max) (lower,
                    descriptor.m_exclusive_min
                        ? std::nextafter(*descriptor.m_min, std::numeric_limits<double>::infinity())
                        : *descriptor.m_min);
            }

            if (descriptor.m_max) {
                upper = (std::min) (upper,
                    descriptor.m_exclusive_max
                        ? std::nextafter(*descriptor.m_max, -std::numeric_limits<double>::infinity())
                        : *descriptor.m_max);
            }

            const auto toInteger = [](const double number) {
                if (number >= static_cast<double>((std::numeric_limits<i64>::max)())) {
                    return (std::numeric_limits<i64>::max)();
                }

                if (number <= static_cast<double>((std::numeric_limits<i64>::lowest)())) {
                    return (std::numeric_limits<i64>::lowest)();
                }

                return static_cast<i64>(number);
            };

            // The authoritative schema bounds, which may be wider or narrower
            // than the slider's display hint range computed above.
            double domain_lower = -(std::numeric_limits<double>::infinity());
            double domain_upper = std::numeric_limits<double>::infinity();
            if (descriptor.m_min) {
                domain_lower = *descriptor.m_min;
                if (descriptor.m_exclusive_min) {
                    domain_lower = std::nextafter(domain_lower, std::numeric_limits<double>::infinity());
                }
            }

            if (descriptor.m_max) {
                domain_upper = *descriptor.m_max;
                if (descriptor.m_exclusive_max) {
                    domain_upper = std::nextafter(domain_upper, -std::numeric_limits<double>::infinity());
                }
            }

            const float exact_width = ImGui::GetFontSize() * 7.0f;
            const float trailing = can_reset ? 88.0f : 38.0f;
            ImGui::SetNextItemWidth(-(exact_width + ImGui::GetStyle().ItemSpacing.x + trailing));

            bool changed = false;
            if (value.m_kind == config::EFileKind::integer) {
                i64 minimum = (std::min) (toInteger(std::ceil(lower)), value.m_integer);
                i64 maximum = (std::max) (toInteger(std::floor(upper)), value.m_integer);
                changed = ImGui::SliderScalar(
                    "##value", ImGuiDataType_S64, &value.m_integer, &minimum, &maximum, "%lld");
            } else {
                if (std::isfinite(value.m_floating)) {
                    lower = (std::min) (lower, value.m_floating);
                    upper = (std::max) (upper, value.m_floating);
                }

                changed = ImGui::SliderScalar(
                    "##value", ImGuiDataType_Double, &value.m_floating, &lower, &upper, "%.6g");
            }
            finished = ImGui::IsItemDeactivatedAfterEdit();

            // The exact entry commits through the same pending-edit path as the
            // slider; only its clamp target differs (the schema domain, not the
            // slider hint), and a cleared or unparsable entry writes nothing.
            ImGui::SameLine();
            ImGui::SetNextItemWidth(exact_width);
            if (value.m_kind == config::EFileKind::integer) {
                const auto previous = value.m_integer;
                i64 typed = previous;
                const bool entered = ImGui::InputScalar("##exact", ImGuiDataType_S64, &typed, nullptr, nullptr, "%lld");
                if (entered or ImGui::IsItemDeactivatedAfterEdit()) {
                    i64 minimum = (std::numeric_limits<i64>::lowest)();
                    i64 maximum = (std::numeric_limits<i64>::max)();
                    if (descriptor.m_min) {
                        minimum = toInteger(std::ceil(domain_lower));
                    }

                    if (descriptor.m_max) {
                        maximum = toInteger(std::floor(domain_upper));
                    }

                    value.m_integer = std::clamp(typed, minimum, maximum);
                    changed = changed or value.m_integer != previous;
                }
            } else {
                const auto previous = value.m_floating;
                double typed = previous;
                const bool entered = ImGui::InputScalar(
                    "##exact", ImGuiDataType_Double, &typed, nullptr, nullptr, "%.6g");
                if (entered or ImGui::IsItemDeactivatedAfterEdit()) {
                    value.m_floating = std::clamp(typed, domain_lower, domain_upper);
                    changed = changed or value.m_floating != previous;
                }
            }

            finished = finished or ImGui::IsItemDeactivatedAfterEdit();
            return changed;
        }

        bool choiceControl(
            const WorldgenPanelState::PropertyNode &node, config::FileValue &value, const bool authored,
            bool &cleared) {
            bool changed = false;
            if (ImGui::BeginCombo("##value", value.m_text.empty() ? "<unset>" : value.m_text.c_str())) {
                if (node.m_descriptor and
                    node.m_descriptor->m_optional and
                    ImGui::Selectable("<unset / default>", not authored)) {
                    changed = authored;
                    cleared = authored;
                }

                for (const auto &choice: node.m_choices) {
                    if (ImGui::Selectable(choice.c_str(), choice == value.m_text) and choice != value.m_text) {
                        value.m_text = choice;
                        changed = true;
                    }
                }

                if (node.m_choices.empty()) {
                    ImGui::TextDisabled("No definitions available in this context");
                }

                ImGui::EndCombo();
            }
            return changed;
        }

        void propertyErrors(WorldgenPanelState &state, const std::string &key, const WorldgenPanelView &view) {
            const auto diagnostics = currentDiagnostics(view);
            const auto *const record = findDocument(state, state.m_selected_key);
            const auto owns = [&](const config::Diagnostic &diagnostic) {
                if (not record) {
                    return false;
                }

                const auto path = record->m_path.string();
                return diagnostic.m_property == key and
                       (diagnostic.m_location == path or diagnostic.m_location.starts_with(path + ":"));
            };

            const bool has_error = std::ranges::any_of(
                diagnostics, [&](const config::Diagnostic &diagnostic) { return owns(diagnostic); });

            if (not has_error) {
                if (state.m_focus_property == key) {
                    ImGui::SetScrollHereY(0.35f);
                    state.m_focus_property.clear();
                }
                return;
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.45f, 0.35f, 1.0f});
            const bool clicked = ImGui::SmallButton("!##error");
            ImGui::PopStyleColor();
            const auto anchor = ImGui::GetItemRectMax();
            const bool focused = state.m_focus_property == key;

            if (focused and not ImGui::IsItemVisible()) {
                ImGui::SetScrollHereY(0.35f);
            } else if (clicked or focused) {
                ImGui::OpenPopup("Property error");
                if (focused) {
                    state.m_focus_property.clear();
                }
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Show errors for %s", key.c_str());
            }

            ImGui::SetNextWindowPos(ImVec2{anchor.x + 6.0f, anchor.y + 4.0f}, ImGuiCond_Appearing);
            if (ImGui::BeginPopup("Property error")) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
                ImGui::TextUnformatted(key.c_str());
                ImGui::Separator();
                for (const auto &diagnostic: diagnostics) {
                    if (owns(diagnostic)) {
                        ImGui::TextWrapped("%s", diagnostic.m_message.c_str());
                        ImGui::TextDisabled("%s", diagnostic.m_location.c_str());
                    }
                }

                ImGui::PopTextWrapPos();
                ImGui::EndPopup();
            }
        }

        void propertyControl(
            WorldgenPanelState &state, const WorldgenPanelState::PropertyNode &node, const std::size_t category,
            const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            auto *const record = findDocument(state, state.m_selected_key);
            PPR_ASSERT(record);
            const auto &entry = document(state, *record).m_entries[*node.m_entry];
            const auto &definition_key = record->m_name;
            const std::string_view property = entry.m_key;
            const bool editable = not view.m_closing and (not node.m_descriptor or not node.m_descriptor->m_read_only);

            ImGui::PushID(entry.m_key.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(node.m_name.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(entry.m_key.c_str());
                ImGui::TextUnformatted(entry.m_location.c_str());
                if (node.m_descriptor) {
                    const auto &descriptor = *node.m_descriptor;
                    ImGui::TextWrapped(
                        "%.*s", static_cast<int>(descriptor.m_description.size()), descriptor.m_description.data());
                    if (descriptor.m_min and descriptor.m_max) {
                        ImGui::Text(
                            "Expected %s%.8g, %.8g%s %.*s", descriptor.m_exclusive_min ? "(" : "[", *descriptor.m_min,
                            *descriptor.m_max, descriptor.m_exclusive_max ? ")" : "]",
                            static_cast<int>(descriptor.m_units.size()), descriptor.m_units.data());
                    }
                }

                ImGui::TextUnformatted("Ctrl-click a slider to enter an exact value.");
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(1);
            const bool choice_control = entry.m_value.m_kind == config::EFileKind::text and
                                        (not node.m_choices.empty() or
                                            (node.m_descriptor and node.m_descriptor->m_reference_target));
            const bool can_reset = node.m_descriptor and
                                   node.m_descriptor->m_optional and
                                   not choice_control and
                                   entry.m_location != "production decoder default (omitted in source)";
            ImGui::SetNextItemWidth(can_reset ? -88.0f : -38.0f);
            auto value = entry.m_value;
            const bool numeric = value.m_kind == config::EFileKind::integer or
                                 value.m_kind == config::EFileKind::floating;
            if (numeric and state.m_numeric_edit_key == entry.m_key) {
                value = state.m_numeric_edit;
            }
            bool changed = false;
            bool cleared = false;
            bool numeric_finished = false;

            ImGui::BeginDisabled(not editable);
            switch (value.m_kind) {
                case config::EFileKind::boolean: changed = ImGui::Checkbox("##value", &value.m_boolean); break;
                case config::EFileKind::integer:
                case config::EFileKind::floating:
                    changed = numericControl(node, value, numeric_finished, can_reset);
                    break;
                case config::EFileKind::text:
                    changed = choice_control
                                  ? choiceControl(
                                        node, value,
                                        entry.m_location != "production decoder default (omitted in source)", cleared)
                                  : textControl(state, entry, value, node.m_descriptor);
                    break;
            }

            ImGui::EndDisabled();

            if (numeric) {
                if (changed) {
                    state.m_numeric_edit_key = entry.m_key;
                    state.m_numeric_edit = value;
                }

                changed = numeric_finished and state.m_numeric_edit_key == entry.m_key;
                if (changed) {
                    value = state.m_numeric_edit;
                    state.m_numeric_edit_key.clear();
                }
            }

            if (can_reset) {
                ImGui::SameLine();
                ImGui::BeginDisabled(not editable);
                if (ImGui::SmallButton("Reset##optional")) {
                    changed = true;
                    cleared = true;
                    if (state.m_numeric_edit_key == entry.m_key) {
                        state.m_numeric_edit_key.clear();
                    }

                    if (state.m_edit_key == entry.m_key) {
                        state.m_edit_key.clear();
                    }
                }

                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Unset this optional property and use its default.");
                }
            }

            if (changed) {
                WorkshopIntent intent{
                    .m_kind = cleared ? EWorkshopIntent::clear_property : EWorkshopIntent::edit_definition};
                intent.m_document = static_cast<EContentDocument>(category);
                intent.m_key = definition_key;
                intent.m_property = property;
                intent.m_value = std::move(value);
                intents.push_back(std::move(intent));
            }

            propertyErrors(state, entry.m_key, view);
            if (node.m_descriptor and entry.m_value.m_kind == config::EFileKind::text) {
                WorldgenPanelState::DescriptorDocument *referenced{};
                const auto kind = assetReferenceKind(record->m_category, property);
                if (not kind.empty()) {
                    referenced = linkedDocument(state, *record, entry.m_value.m_text, kind);
                } else if (node.m_descriptor->m_reference_category) {
                    auto *const world = record->m_category == EContentDocument::worlds ? record : worldDocument(state);
                    if (world) {
                        const auto target_category = *node.m_descriptor->m_reference_category;
                        const auto reference = text(
                            document(state, *world),
                            "palette." +
                                std::string{categories()[static_cast<std::size_t>(target_category)]} +
                                "." +
                                entry.m_value.m_text +
                                ".asset");
                        referenced = linkedDocument(state, *world, reference, contentAssetKind(target_category));
                    }
                }

                if (referenced) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("->##reference")) {
                        selectDefinition(
                            state, categories()[static_cast<std::size_t>(referenced->m_category)], referenced->m_name);
                    }
                }
            }

            ImGui::PopID();
        }

        /// The text the Value column renders for one property, so the nested
        /// search can match authored values as well as keys.
        std::string propertyValueText(const WorldgenPanelState::PropertyNode &node, const config::FileValue &value) {
            switch (value.m_kind) {
                case config::EFileKind::boolean: return value.m_boolean ? "true" : "false";
                case config::EFileKind::integer: {
                    std::array<char, 32u> text{};
                    std::snprintf(text.data(), text.size(), "%lld", static_cast<long long>(value.m_integer));
                    return text.data();
                }
                case config::EFileKind::floating: {
                    std::array<char, 48u> text{};
                    std::snprintf(text.data(), text.size(), node.m_descriptor ? "%.6g" : "%.8g", value.m_floating);
                    return text.data();
                }
                case config::EFileKind::text: return value.m_text;
            }
            return {};
        }

        bool treeMatches(
            const WorldgenPanelState::PropertyNode &node, const Array<config::Entry> &entries,
            const std::string_view search) {
            if (matches(node.m_path, search)) {
                return true;
            }

            if (node.m_entry and matches(propertyValueText(node, entries[*node.m_entry].m_value), search)) {
                return true;
            }

            return std::ranges::any_of(
                node.m_children, [&](const auto &child) { return treeMatches(child, entries, search); });
        }

        void propertyTree(
            WorldgenPanelState &state, const Array<WorldgenPanelState::PropertyNode> &nodes, const std::size_t category,
            const WorldgenPanelView &view, Array<WorkshopIntent> &intents, const Array<config::Entry> &entries) {
            const std::string_view search = state.m_property_search.data();

            for (const auto &node: nodes) {
                if (not treeMatches(node, entries, search)) {
                    continue;
                }

                if (node.m_entry) {
                    propertyControl(state, node, category, view, intents);
                    continue;
                }

                ImGui::PushID(node.m_path.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (not search.empty() or state.m_focus_property.starts_with(node.m_path + ".")) {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                }

                const bool open = ImGui::TreeNodeEx(node.m_name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
                propertyErrors(state, node.m_path, view);
                if (open) {
                    propertyTree(state, node.m_children, category, view, intents, entries);
                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        }

        bool keyCombo(
            const char *label, char *const value, const std::size_t capacity, const Array<std::string> &keys) {
            bool changed = false;
            if (ImGui::BeginCombo(label, value[0u] == '\0' ? "Choose a definition" : value)) {
                for (const auto &key: keys) {
                    if (ImGui::Selectable(key.c_str(), key == value)) {
                        std::snprintf(value, capacity, "%s", key.c_str());
                        changed = true;
                    }
                }

                ImGui::EndCombo();
            }
            return changed;
        }

        std::string_view previewReason(WorldgenPanelState &state, const WorldgenPanelView &view) {
            if (view.m_closing) {
                return "Workshop is closing.";
            }

            if (not view.m_source) {
                return "Open a content pack first.";
            }

            if (view.m_busy) {
                return "Wait for the current operation or cancel it before preparing another preview.";
            }

            if (contentErrors(view)) {
                return "Fix highlighted property errors before preparing another preview.";
            }

            if (std::ranges::find(state.m_record_keys[4u], state.m_profile.data()) == state.m_record_keys[4u].end()) {
                return "Choose an existing world profile.";
            }

            const auto &biomes = sampleBiomes(state);
            if (state.m_preview_mode == EWorldgenPreviewMode::biome_sample and
                std::ranges::find(biomes, state.m_biome.data()) == biomes.end()) {
                return "Choose an existing sample biome.";
            }

            constexpr int maximum_origin = static_cast<int>(sim::kWorldEdge - kWorldgenPreviewEdge);
            if (state.m_origin[0u] < 0 or
                state.m_origin[1u] < 0 or
                state.m_origin[0u] > maximum_origin or
                state.m_origin[1u] > maximum_origin) {
                return "Keep the entire 256-cell crop inside the world; adjust the origin sliders.";
            }
            return {};
        }

        // ------------------------------------------------------------------
        // workshop chrome
        // ------------------------------------------------------------------

        /// The menu-bar copy of every file, history and view action. Each entry
        /// shares its enable reason and intent with the toolbar button of the
        /// same name, so a menu item and its button never drift apart.
        void workshopMenuBar(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            if (not ImGui::BeginMenuBar()) {
                return;
            }

            if (ImGui::BeginMenu("File")) {
                const auto unavailable = view.m_closing      ? "Workshop is closing."
                                         : not view.m_source ? "Open a content pack first."
                                         : view.m_busy       ? "Wait for the current operation or cancel it first."
                                                             : "";
                actionMenuItem(
                    ui::icons::kSaveDocumentsLabel.data(),
                    unavailable[0u] != '\0' ? unavailable
                    : not view.m_dirty      ? "No authored changes to save."
                    : contentErrors(view)   ? "Fix highlighted definition errors before saving."
                                            : "",
                    EWorkshopIntent::save, intents);
                actionMenuItem(ui::icons::kReloadLabel.data(), unavailable, EWorkshopIntent::reload, intents);
                actionMenuItem(
                    "Revert",
                    unavailable[0u] != '\0' ? unavailable
                    : not view.m_dirty      ? "The draft already matches the saved baseline."
                                            : "",
                    EWorkshopIntent::revert, intents);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                actionMenuItem(
                    ui::icons::kUndoLabel.data(),
                    view.m_closing        ? "Workshop is closing."
                    : not view.m_can_undo ? "No command to undo."
                                          : "",
                    EWorkshopIntent::undo, intents);
                actionMenuItem(
                    ui::icons::kRedoLabel.data(),
                    view.m_closing        ? "Workshop is closing."
                    : not view.m_can_redo ? "No command to redo."
                                          : "",
                    EWorkshopIntent::redo, intents);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {
                ImGui::BeginDisabled(not view.m_source or view.m_closing);
                if (ImGui::MenuItem("Live preview", nullptr, &state.m_automatic_live)) {
                    emit(intents, EWorkshopIntent::preview_settings_changed);
                }

                ImGui::EndDisabled();
                if (ImGui::MenuItem("Show errors", nullptr, false, contentErrors(view))) {
                    state.m_show_diagnostics = true;
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + ImGui::GetFontSize() * 24.0f);
                ImGui::TextUnformatted(
                    "File saves, reloads or reverts the draft. Edit undoes or redoes authored changes. View toggles the live preview and the error list. The preview settings column controls the counted crop.");
                ImGui::PopTextWrapPos();
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        /// The single status line under the menu bar: the draft state, the
        /// revision counters and the worker message. It carries the draft
        /// heading that the window title no longer duplicates.
        void statusLine(const WorldgenPanelView &view) {
            ImGui::TextUnformatted(view.m_dirty ? "Modified draft" : "Saved draft");
            ImGui::SameLine();
            ImGui::TextDisabled(
                "| draft %llu / validated %llu%s", static_cast<unsigned long long>(view.m_revision),
                static_cast<unsigned long long>(view.m_validated_revision), view.m_busy ? " | worker busy" : "");
            ImGui::TextWrapped("%.*s", static_cast<int>(view.m_status.size()), view.m_status.data());

            if (contentErrors(view)) {
                ImGui::TextColored(ui::theme::kError.vec4(), "This draft has property errors.");
            }
        }

        void toolbar(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            const auto continue_row = [](const char *const label) {
                const float width = ImGui::CalcTextSize(label, nullptr, true).x +
                                    2.0f * ImGui::GetStyle().FramePadding.x;
                const float remaining = ImGui::GetCursorScreenPos().x +
                                        ImGui::GetContentRegionAvail().x -
                                        ImGui::GetItemRectMax().x;
                if (remaining >= width + ImGui::GetStyle().ItemSpacing.x) {
                    ImGui::SameLine();
                }
            };

            ImGui::TextUnformatted("Pack folder");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##pack-root", state.m_root.data(), state.m_root.size());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enter a pack root, then Load pack to replace the active source.");
            }

            const bool load_path_pending = std::filesystem::path{state.m_root.data()}.lexically_normal().string() !=
                                           view.m_loaded_root;

            if (view.m_source and not view.m_source->m_packs.empty()) {
                const auto &active_root = view.m_source->m_packs.front().m_root;
                ImGui::TextDisabled(
                    "Loaded source: %s%s", active_root.string().c_str(),
                    view.m_source->m_packs.size() > 1u ? " (dependency pack set)" : "");
            }

            const bool empty_root = std::string_view{state.m_root.data()}.find_first_not_of(" \t\r\n") ==
                                    std::string_view::npos;
            actionButton(
                "Load pack",
                view.m_closing ? "Workshop is closing."
                : view.m_busy  ? "Wait for the current operation or cancel it first."
                : empty_root   ? "Enter a content pack root before loading."
                               : "",
                EWorkshopIntent::open, intents);

            const char *const game_label = view.m_has_game ? "Return to game###workshop_game"
                                                           : "Start game###workshop_game";
            continue_row(game_label);
            const char *const game_reason = view.m_closing or view.m_starting ? "Wait for the current transition."
                                            : view.m_has_game                 ? ""
                                            : not view.m_source               ? "Load a pack first."
                                            : view.m_busy                     ? "Wait for validation and current work."
                                            : load_path_pending ? "Load the selected source before starting."
                                            : not view.m_can_start_game
                                                ? "Select a world and validate the current draft before starting."
                                                : "";
            actionButton(
                game_label, game_reason,
                view.m_has_game ? EWorkshopIntent::return_to_game : EWorkshopIntent::start_game, intents, true);

            if (view.m_has_game) {
                continue_row("New game###workshop_new_game");
                const char *const new_game_reason =
                    view.m_closing or view.m_starting ? "Wait for the current transition."
                    : not view.m_source               ? "Load a pack first."
                    : view.m_busy                     ? "Wait for validation and current work."
                    : load_path_pending               ? "Load the selected source before starting."
                    : not view.m_can_start_game       ? "Select a world and validate the current draft before starting."
                                                      : "";
                actionButton(
                    "New game###workshop_new_game", new_game_reason, EWorkshopIntent::start_game, intents, true);
            }

            continue_row("Quit app");
            actionButton(
                "Quit app###workshop_quit", view.m_closing ? "Workshop is closing." : "", EWorkshopIntent::close,
                intents);

            if (empty_root and not view.m_source) {
                ImGui::TextDisabled("Enter an authored pack directory to enable Load pack.");
            }

            continue_row("Prepare preview");
            actionButton("Prepare preview", previewReason(state, view), EWorkshopIntent::preview, intents, true);
            continue_row("Cancel work");
            actionButton(
                "Cancel work",
                view.m_closing or (not view.m_busy and not state.m_automatic_live)
                    ? "No active operation or automatic preview to cancel."
                    : "",
                EWorkshopIntent::cancel, intents);

            ImGui::Separator();
        }

        void definitionActions(
            WorldgenPanelState &state, const std::size_t category, const WorldgenPanelView &view,
            Array<WorkshopIntent> &intents) {
            if (not ImGui::CollapsingHeader("Create / delete definition")) {
                return;
            }

            ImGui::SameLine();
            ui::helpMarker(
                "Cloning keeps the selected settings and references. Deletion is undoable; existing references may need repair.");

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##new-key", "New unique definition key", state.m_new_key.data(), state.m_new_key.size());

            const std::string_view key = state.m_new_key.data();
            const bool valid_key = not key.empty() and
                                   key.find('.') == std::string_view::npos and
                                   asset::validLocalName(key);
            const auto owner_end = state.m_selected_key.find(':');
            const auto qualified_key = owner_end == std::string::npos
                                           ? std::string{key}
                                           : state.m_selected_key.substr(0u, owner_end + 1u) + std::string{key};
            const bool duplicate = std::ranges::find(state.m_record_keys[category], qualified_key) !=
                                   state.m_record_keys[category].end();
            const auto reason = view.m_closing  ? "Workshop is closing."
                                : not valid_key ? "Enter a canonical key without dots."
                                : duplicate     ? "That definition key already exists."
                                                : "";

            ImGui::BeginDisabled(reason[0u] != '\0');
            if (ImGui::Button("Create from selected")) {
                WorkshopIntent intent{.m_kind = EWorkshopIntent::clone_definition};
                intent.m_document = static_cast<EContentDocument>(category);
                intent.m_source_key = state.m_selected_key;
                intent.m_key = qualified_key;
                intents.push_back(std::move(intent));
            }

            ImGui::EndDisabled();
            disabledReason(reason);
            ImGui::SameLine();
            ImGui::BeginDisabled(view.m_closing);
            if (ImGui::Button("Delete selected")) {
                WorkshopIntent intent{.m_kind = EWorkshopIntent::delete_definition};
                intent.m_document = static_cast<EContentDocument>(category);
                intent.m_key = state.m_selected_key;
                intents.push_back(std::move(intent));
            }

            ImGui::EndDisabled();
        }

        void inspector(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            const hal::ProfileScope scope{"Workshop.UI.Properties"};
            const auto category = std::ranges::find(categories(), state.m_selected_category);

            if (state.m_selected_key.empty()) {
                ImGui::TextWrapped("Choose a definition in the browser to inspect its properties.");
                return;
            }

            if (state.m_selected_key == "vacuum" and state.m_selected_category == "materials") {
                ImGui::TextWrapped("Reserved Material ID 0 represents empty terrain. Its identity is fixed.");
                return;
            }

            if (category == categories().end()) {
                return;
            }

            const auto index = static_cast<std::size_t>(category - categories().begin());
            if (state.m_property_tree.empty()) {
                ImGui::TextWrapped(
                    "This definition is no longer in the draft. Choose another definition or Undo deletion.");
                return;
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##property-search", "Search nested properties", state.m_property_search.data(),
                state.m_property_search.size());
            if (index == 1u) {
                ui::helpMarker(
                    "Linked constraints: lining thickness needs a lining material; enabled pore passes need a pore field and ordered radii. Soil, weathering and formations need their materials. The sum of participant-wide pore, relaxation, finish and erosion pass maxima must be at most 58.");
            }

            ui::helpMarker("Hover labels for source, units and schema bounds. Ctrl-click sliders for exact values.");

            auto *const record = findDocument(state, state.m_selected_key);
            PPR_ASSERT(record);

            if (ImGui::BeginTable(
                    "Property tree", 2,
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 230.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                propertyTree(state, state.m_property_tree, index, view, intents, document(state, *record).m_entries);
                ImGui::EndTable();
            }

            definitionActions(state, index, view, intents);

            if (ImGui::CollapsingHeader("Used by")) {
                if (not view.m_catalog or view.m_validated_revision != view.m_revision) {
                    ImGui::TextWrapped("Reverse references are available after the complete draft validates.");
                } else {
                    if (state.m_use_selection != state.m_selected_key) {
                        collectUsedBy(state);
                    }

                    for (const auto &use: state.m_uses) {
                        const auto label = use.m_name +
                                           "." +
                                           use.m_property +
                                           (use.m_world.empty() ? std::string{} : " (World " + use.m_world + ")");
                        if (ImGui::Selectable(label.c_str())) {
                            selectDefinition(state, categories()[static_cast<std::size_t>(use.m_category)], use.m_name);
                            state.m_focus_property = use.m_property;
                        }
                    }
                }
            }
        }

        /// The preview settings column: the counted crop, the profile, scope,
        /// sample biome, seed, crop origin and the diagnostic view. Every
        /// control here drives one preview-settings change intent.
        void previewSettings(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            ImGui::TextUnformatted("256 x 256 fine cells | 85.33 m square");
            ImGui::SameLine();
            ui::helpMarker(
                "The cyan outline marks the counted and exported interior. Extra rendered cells provide contour support.");

            bool changed = keyCombo(
                "World profile", state.m_profile.data(), state.m_profile.size(), state.m_record_keys[4u]);
            constexpr const char *scopes[]{"Biome sample", "Exact whole-world crop"};
            int scope = static_cast<int>(state.m_preview_mode);
            if (ImGui::Combo("Preview scope", &scope, scopes, 2)) {
                state.m_preview_mode = static_cast<EWorldgenPreviewMode>(scope);
                changed = true;
            }

            ImGui::BeginDisabled(state.m_preview_mode != EWorldgenPreviewMode::biome_sample);
            const auto &biomes = sampleBiomes(state);
            changed = keyCombo("Sample biome", state.m_biome.data(), state.m_biome.size(), biomes) or changed;
            ImGui::EndDisabled();
            changed = ImGui::InputScalar("Full 64-bit seed", ImGuiDataType_U64, &state.m_seed) or changed;

            constexpr int maximum_origin = static_cast<int>(sim::kWorldEdge - kWorldgenPreviewEdge);
            changed = ImGui::SliderInt(
                          "Origin X (fine cells)", &state.m_origin[0u], 0, maximum_origin, "%d",
                          ImGuiSliderFlags_AlwaysClamp) or
                      changed;
            changed = ImGui::SliderInt(
                          "Origin Y (fine cells)", &state.m_origin[1u], 0, maximum_origin, "%d",
                          ImGuiSliderFlags_AlwaysClamp) or
                      changed;
            ImGui::SameLine();
            ui::helpMarker("Global +Y is height; depth follows the selected profile surface.");

            constexpr const char *modes[]{"Textured", "Material", "Biome", "Temperature (200-1000 K)"};
            int mode = static_cast<int>(state.m_mode);
            if (ImGui::Combo("Diagnostic view", &mode, modes, 4)) {
                state.m_mode = static_cast<EGridDiagnosticMode>(mode);
                changed = true;
            }

            if (changed) {
                emit(intents, EWorkshopIntent::preview_settings_changed);
            }

            const auto reason = previewReason(state, view);
            actionButton("Prepare preview", reason, EWorkshopIntent::preview, intents, true);
            if (not reason.empty()) {
                ImGui::TextWrapped("%.*s", static_cast<int>(reason.size()), reason.data());
            }
        }

        /// The preview report: the publishing status, the crop statistics, the
        /// participating rules and omissions, and the fresh diagnostic export
        /// directory. It lives in the editor's Preview tab.
        void previewReport(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            if (view.m_publishing) {
                ImGui::TextWrapped("Preparing the render preview; the previous picture remains visible.");
            }

            if (view.m_preview) {
                const auto &preview = *view.m_preview;
                ImGui::Separator();
                ImGui::Text(
                    "Preview draft %llu: %s", static_cast<unsigned long long>(preview.m_spec.m_revision),
                    view.m_preview_current ? "current" : "OUT OF DATE");
                ImGui::Text(
                    "Solid %.2f%% | vacuum %.2f%% | checksum %016llx",
                    100.0 * static_cast<double>(preview.m_solid_cells) / (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                    100.0 *
                        static_cast<double>(kWorldgenPreviewEdge * kWorldgenPreviewEdge - preview.m_solid_cells) /
                        (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                    static_cast<unsigned long long>(preview.m_checksum));
                ImGui::Text(
                    "Visible [%u,%u) x [%u,%u)", preview.m_visible.m_left, preview.m_visible.m_right,
                    preview.m_visible.m_bottom, preview.m_visible.m_top);

                if (ImGui::TreeNode("Material counts")) {
                    for (const auto &count: preview.m_material_counts) {
                        const auto *material = preview.m_catalog->findMaterial(count.m_material);
                        ImGui::Text(
                            "%u %s: %llu", count.m_material, material ? material->m_key.c_str() : "vacuum",
                            static_cast<unsigned long long>(count.m_cells));
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Participating profile rules")) {
                    if (ImGui::SmallButton("Inspect profile")) {
                        selectDefinition(state, "worlds", preview.m_spec.m_profile);
                        state.m_show_properties = true;
                    }

                    for (const auto &rule: preview.m_participating_rules) {
                        ImGui::BulletText("%s", rule.c_str());
                    }

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Sample scope / omissions")) {
                    for (const auto &omission: preview.m_omissions) {
                        ImGui::BulletText("%s", omission.c_str());
                    }

                    ImGui::TreePop();
                }
            }

            ImGui::Separator();
            ImGui::InputText(
                "Fresh diagnostic export directory", state.m_export_path.data(), state.m_export_path.size());
            const auto export_reason = view.m_closing          ? "Workshop is closing."
                                       : view.m_busy           ? "Wait for the current operation."
                                       : not view.m_can_export ? "Prepare a current preview before exporting."
                                       : std::string_view{state.m_export_path.data()}.find_first_not_of(" \t\r\n") ==
                                               std::string_view::npos
                                           ? "Enter a new export directory."
                                           : "";
            actionButton("Export diagnostics", export_reason, EWorkshopIntent::export_preview, intents);
        }

        void diagnosticsPanel(WorldgenPanelState &state, const WorldgenPanelView &view) {
            const auto diagnostics = currentDiagnostics(view);

            if (diagnostics.empty()) {
                ImGui::TextWrapped("No property diagnostics for the current draft.");
            } else {
                ImGui::TextUnformatted("Property errors");
                ImGui::SameLine();
                ui::helpMarker(
                    "Select an error to open its definition and property. The ! badge shows the complete message beside that control.");
                for (std::size_t index = 0u; index < diagnostics.size(); ++index) {
                    const auto &diagnostic = diagnostics[index];
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(
                            diagnostic.m_property.empty() ? "Pack operation" : diagnostic.m_property.c_str())) {
                        static_cast<void>(navigateDiagnostic(state, diagnostic));
                    }

                    ImGui::PushTextWrapPos();
                    ImGui::TextWrapped("%s", diagnostic.m_message.c_str());
                    ImGui::TextDisabled("%s", diagnostic.m_location.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }

            if (not view.m_diagnostics.empty() and
                ImGui::CollapsingHeader("Operation log (may describe an older draft)")) {
                ImGui::TextWrapped("%.*s", static_cast<int>(view.m_diagnostics.size()), view.m_diagnostics.data());
            }
        }
    }

    // ------------------------------------------------------------------
    // panel
    // ------------------------------------------------------------------

    Array<WorkshopIntent> drawWorldgenPanel(WorldgenPanelState &state, const WorldgenPanelView &view) {
        Array<WorkshopIntent> intents{};
        refreshDocuments(state, view);

        game::ui::prepareUtilityWindow(1100.0f, 820.0f, false);
        ImGui::SetNextWindowBgAlpha(ui::theme::kFloatingPanelAlpha);
        ui::WindowTitleFont title_font{};
        const bool visible = ImGui::Begin("Authoring Workspace###Worldgen Workshop", nullptr, ImGuiWindowFlags_MenuBar);
        title_font.close();
        if (not visible) {
            ImGui::End();
            return intents;
        }

        workshopMenuBar(state, view, intents);
        statusLine(view);
        toolbar(state, view, intents);
        std::optional<asset::AssetKey> activated{};
        const auto navigation = [&]() {
            const hal::ProfileScope scope{"Workshop.UI.AssetBrowser"};

            if (ImGui::Selectable("Empty terrain (reserved vacuum)", state.m_selected_key == "vacuum")) {
                selectDefinition(state, "materials", "vacuum");
            }

            activated = drawAssetBrowserPanel(
                state.m_browser, {.m_index = view.m_index, .m_index_revision = view.m_revision});
        };

        const auto editor = [&]() {
            ImGui::TextUnformatted(state.m_selected_key.empty() ? "Choose an asset" : state.m_selected_key.c_str());
            refreshPropertyTree(state);

            const float secondary_height = std::min(
                ImGui::GetContentRegionAvail().y * 0.35f, ImGui::GetFontSize() * 17.0f);
            ImGui::BeginChild("Properties", ImVec2{0.0f, -secondary_height}, ImGuiChildFlags_Borders);
            if (state.m_selected_asset and state.m_selected_category.empty()) {
                const auto assets = view.m_index ? view.m_index->assets() : std::span<const asset::IndexedAsset>{};
                const auto entry = std::ranges::find(assets, *state.m_selected_asset, &asset::IndexedAsset::m_key);
                if (entry != assets.end()) {
                    ImGui::TextWrapped("Kind: %s", entry->m_kind.c_str());
                    ImGui::TextWrapped("Descriptor: %s", entry->m_descriptor.string().c_str());
                    ImGui::TextWrapped("Pack: %s", entry->m_name.m_pack.c_str());
                }

                ImGui::TextWrapped("Read-only asset: this kind has no property editor.");
            } else {
                inspector(state, view, intents);
            }

            ImGui::EndChild();
            if (ImGui::BeginTabBar("Workspace details")) {
                if (ImGui::BeginTabItem("Preview")) {
                    ImGui::BeginChild("Preview report");
                    previewReport(state, view, intents);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                const auto flags = state.m_show_diagnostics ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Diagnostics", nullptr, flags)) {
                    state.m_show_diagnostics = false;
                    ImGui::BeginChild("Workspace diagnostics");
                    diagnosticsPanel(state, view);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        };

        ImGui::BeginDisabled(view.m_starting);
        const bool narrow = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 48.0f;
        if (narrow and ImGui::BeginTabBar("Workspace panes")) {
            if (ImGui::BeginTabItem("Assets")) {
                ImGui::BeginChild("Asset navigation");
                navigation();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem(
                    "Editor", nullptr,
                    state.m_show_properties ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                state.m_show_properties = false;
                editor();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Preview")) {
                ImGui::BeginChild("Preview settings");
                previewSettings(state, view, intents);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        } else if (not narrow) {
            const float strip_height = std::clamp(
                ImGui::GetContentRegionAvail().y * 0.28f, ImGui::GetFontSize() * 8.0f, ImGui::GetFontSize() * 20.0f);
            const float top_height = std::max(
                ImGui::GetContentRegionAvail().y - strip_height - ImGui::GetStyle().ItemSpacing.y,
                ImGui::GetFontSize() * 6.0f);
            const float settings_width = std::min(
                ImGui::GetContentRegionAvail().x * 0.32f, ImGui::GetFontSize() * 26.0f);
            state.m_show_properties = false;

            ImGui::SetNextWindowSizeConstraints(
                ImVec2{ImGui::GetFontSize() * 16.0f, 0.0f},
                ImVec2{ImGui::GetContentRegionAvail().x, (std::numeric_limits<float>::max)()});
            ImGui::BeginChild(
                "Preview settings", ImVec2{settings_width, top_height},
                ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
            previewSettings(state, view, intents);
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("Editor", ImVec2{0.0f, top_height}, ImGuiChildFlags_Borders);
            editor();
            ImGui::EndChild();

            ImGui::BeginChild("Asset strip", ImVec2{0.0f, strip_height}, ImGuiChildFlags_Borders);
            navigation();
            ImGui::EndChild();
        }

        ImGui::EndDisabled();

        const bool operation_requested = std::ranges::any_of(intents, [](const auto &intent) {
            return intent.m_kind != EWorkshopIntent::edit_definition and
                   intent.m_kind != EWorkshopIntent::clear_property;
        });
        if (activated or state.m_selection_pending or operation_requested) {
            flushPendingEdits(state, intents);
        }

        // Selection is adopted only after the old inspector has processed deactivation.
        // The host applies these owning edit intents before starting any transition.
        if (activated and view.m_index) {
            const auto assets = view.m_index->assets();
            const auto entry = std::ranges::find(assets, *activated, &asset::IndexedAsset::m_key);
            if (entry != assets.end()) {
                const auto bindings = contentDocumentBindings();
                const auto binding = std::ranges::find_if(bindings, [&](const auto &candidate) {
                    return contentAssetKind(candidate.m_document) == entry->m_kind;
                });
                selectDefinition(
                    state, binding == bindings.end() ? std::string_view{} : binding->m_collection,
                    entry->m_name.string());
                state.m_selected_asset = entry->m_key;
                state.m_browser.m_active_key = entry->m_key;
            }
        }

        if (state.m_selection_pending) {
            state.m_selected_category = std::move(state.m_pending_category);
            state.m_selected_key = std::move(state.m_pending_key);
            state.m_focus_property = std::move(state.m_pending_property);
            state.m_edit_key.clear();
            state.m_numeric_edit_key.clear();
            state.m_selection_pending = false;

            if (view.m_index) {
                const auto assets = view.m_index->assets();
                const auto found = std::ranges::find_if(assets, [&](const auto &entry) {
                    if (entry.m_name.string() != state.m_selected_key) {
                        return false;
                    }

                    if (state.m_selected_category.empty()) {
                        return state.m_selected_asset == entry.m_key;
                    }

                    const auto bindings = contentDocumentBindings();
                    const auto binding = std::ranges::find(
                        bindings, state.m_selected_category, &ContentDocumentBinding::m_collection);
                    return binding != bindings.end() and contentAssetKind(binding->m_document) == entry.m_kind;
                });
                state.m_selected_asset = found == assets.end() ? std::nullopt : std::optional{found->m_key};
                state.m_browser.m_active_key = state.m_selected_asset;
            }
        }

        ImGui::End();
        return intents;
    }
}
