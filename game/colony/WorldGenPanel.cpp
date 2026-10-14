module;
module game.colony.worldgen_panel;
import imgui;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // helpers
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
            intents.push_back({kind});
        }
        std::string recordKey(const config::Entry &entry, const std::string_view category) {
            const auto bindings = contentDocumentBindings();
            const auto binding = std::ranges::find(bindings, category, &ContentDocumentBinding::m_collection);
            if (binding == bindings.end()) {
                return {};
            }
            const auto prefix = std::string{category} + ".";
            if (not entry.m_key.starts_with(prefix)) {
                return {};
            }
            auto end = prefix.size();
            for (std::size_t depth = 0u; depth < binding->m_definition_depth; ++depth) {
                end = entry.m_key.find('.', end);
                if (end == std::string::npos) {
                    return depth + 1u == binding->m_definition_depth ? entry.m_key.substr(prefix.size())
                                                                     : std::string{};
                }
                if (depth + 1u < binding->m_definition_depth) {
                    ++end;
                }
            }
            return entry.m_key.substr(prefix.size(), end - prefix.size());
        }
        void appendResolvedDefaults(
            config::Document &document, const EContentDocument category, const ContentCatalog &catalog) {
            const auto keys = contentDefinitionKeys(category, document);
            const auto projection = projectContentDocument(category, catalog.definition());
            if (not projection) {
                return;
            }

            for (const auto &entry: projection->m_entries) {
                const auto key = recordKey(entry, categories()[static_cast<std::size_t>(category)]);
                if (std::ranges::find(keys, key) != keys.end() and
                    std::ranges::find(document.m_entries, entry.m_key, &config::Entry::m_key) ==
                        document.m_entries.end()) {
                    auto projected = entry;
                    projected.m_location = "production decoder default (omitted in source)";
                    document.m_entries.push_back(std::move(projected));
                }
            }
            std::ranges::sort(document.m_entries, {}, &config::Entry::m_key);
        }
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
        void actionButton(
            const char *label, const std::string_view reason, const EWorkshopIntent kind,
            Array<WorkshopIntent> &intents) {
            ImGui::BeginDisabled(not reason.empty());
            if (ImGui::Button(label)) {
                emit(intents, kind);
            }
            ImGui::EndDisabled();
            disabledReason(reason);
        }
        void selectDefinition(WorldgenPanelState &state, const std::string_view category, const std::string_view key) {
            state.m_selected_category = category;
            state.m_selected_key = key;
            state.m_edit_key.clear();
        }
        bool navigateDiagnostic(WorldgenPanelState &state, const config::Diagnostic &diagnostic) {
            const auto mapped = contentSourcePath(diagnostic.m_property);
            const std::string_view property = mapped ? std::string_view{mapped->second} : diagnostic.m_property;
            const auto first = property.find('.');

            if (first == std::string_view::npos or
                std::ranges::find(categories(), property.substr(0u, first)) == categories().end()) {
                return false;
            }

            const auto key = recordKey(config::Entry{std::string{property}}, property.substr(0u, first));
            if (key.empty()) {
                return false;
            }
            selectDefinition(state, property.substr(0u, first), key);
            state.m_focus_property = property;
            state.m_property_search[0u] = '\0';
            state.m_show_properties = true;
            return true;
        }
        void refreshDocuments(WorldgenPanelState &state, const WorldgenPanelView &view) {
            if (view.m_source == state.m_document_source and
                view.m_revision == state.m_document_revision and
                view.m_validated_revision == state.m_document_validated_revision and
                view.m_catalog == state.m_document_catalog) {
                return;
            }

            state.m_document_source = view.m_source;
            state.m_document_revision = view.m_revision;
            state.m_document_validated_revision = view.m_validated_revision;
            state.m_document_catalog = view.m_catalog;
            state.m_tree_selection.clear();

            for (std::size_t index = 0u; index < categories().size(); ++index) {
                state.m_documents[index] = {};
                state.m_record_keys[index].clear();
                if (view.m_source) {
                    const auto &source = view.m_source->m_documents[index];
                    config::Diagnostics diagnostics{8u};
                    auto decoded = config::decode(
                        source.m_bytes, source.m_path.string(), diagnostics, config::EDecodeMode::tree);
                    if (decoded) {
                        state.m_documents[index] = std::move(*decoded);
                    }
                }
            }

            if (view.m_catalog and view.m_validated_revision == view.m_revision) {
                for (const auto &binding: contentDocumentBindings()) {
                    appendResolvedDefaults(
                        state.m_documents[static_cast<std::size_t>(binding.m_document)], binding.m_document,
                        *view.m_catalog);
                }
            }

            for (const auto &binding: contentDocumentBindings()) {
                const auto index = static_cast<std::size_t>(binding.m_document);
                state.m_record_keys[index] = contentDefinitionKeys(binding.m_document, state.m_documents[index]);
            }
        }
        void refreshPropertyTree(WorldgenPanelState &state) {
            const auto selection = state.m_selected_category + "." + state.m_selected_key;
            if (selection == state.m_tree_selection) {
                return;
            }

            state.m_tree_selection = selection;
            state.m_property_tree.clear();

            const auto category = std::ranges::find(categories(), state.m_selected_category);
            if (category == categories().end()) {
                return;
            }

            const auto index = static_cast<std::size_t>(category - categories().begin());
            const auto &document = state.m_documents[index];
            const auto prefix = selection + ".";

            for (std::size_t entry_index = 0u; entry_index < document.m_entries.size(); ++entry_index) {
                const auto &entry = document.m_entries[entry_index];
                if (not entry.m_key.starts_with(prefix)) {
                    continue;
                }
                auto remaining = std::string_view{entry.m_key}.substr(prefix.size());
                const auto property = remaining;
                auto *children = &state.m_property_tree;
                std::string path = prefix;
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
                            static_cast<EContentDocument>(index), property, document, state.m_selected_key);
                        if (node->m_descriptor) {
                            for (const auto choice: node->m_descriptor->m_choices) {
                                node->m_choices.emplace_back(choice);
                            }
                            if (node->m_descriptor->m_reference_target) {
                                node->m_choices = contentPropertyReferenceChoices(
                                    static_cast<EContentDocument>(index), property, state.m_documents,
                                    state.m_selected_key);
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
        bool textControl(WorldgenPanelState &state, const config::Entry &entry, config::FileValue &value) {
            bool changed = false;
            if (state.m_edit_key != entry.m_key) {
                std::array<char, 1024u> temporary{};
                std::snprintf(temporary.data(), temporary.size(), "%s", value.m_text.c_str());
                if (ImGui::InputText(
                        "##value", temporary.data(), temporary.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    value.m_text = temporary.data();
                    changed = true;
                }
                if (ImGui::IsItemActive()) {
                    state.m_edit_key = entry.m_key;
                    state.m_text_edit = temporary;
                }
            } else {
                if (ImGui::InputText(
                        "##value", state.m_text_edit.data(), state.m_text_edit.size(),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    value.m_text = state.m_text_edit.data();
                    changed = true;
                }
                if (not ImGui::IsItemActive()) {
                    state.m_edit_key.clear();
                }
            }
            return changed;
        }
        bool numericControl(const WorldgenPanelState::PropertyNode &node, config::FileValue &value) {
            if (not node.m_descriptor) {
                return value.m_kind == config::EFileKind::integer
                           ? ImGui::InputScalar("##value", ImGuiDataType_S64, &value.m_integer)
                           : ImGui::InputDouble("##value", &value.m_floating, 0.0, 0.0, "%.8g");
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

            if (value.m_kind == config::EFileKind::integer) {
                const auto toInteger = [](const double number) {
                    if (number >= static_cast<double>((std::numeric_limits<i64>::max)())) {
                        return (std::numeric_limits<i64>::max)();
                    }
                    if (number <= static_cast<double>((std::numeric_limits<i64>::lowest)())) {
                        return (std::numeric_limits<i64>::lowest)();
                    }
                    return static_cast<i64>(number);
                };
                i64 minimum = (std::min) (toInteger(std::ceil(lower)), value.m_integer);
                i64 maximum = (std::max) (toInteger(std::floor(upper)), value.m_integer);
                return ImGui::SliderScalar("##value", ImGuiDataType_S64, &value.m_integer, &minimum, &maximum, "%lld");
            }
            if (std::isfinite(value.m_floating)) {
                lower = (std::min) (lower, value.m_floating);
                upper = (std::max) (upper, value.m_floating);
            }
            return ImGui::SliderScalar("##value", ImGuiDataType_Double, &value.m_floating, &lower, &upper, "%.6g");
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
            const auto category = std::ranges::find(categories(), state.m_selected_category);
            const auto canonical_key = category == categories().end()
                                           ? key
                                           : contentCanonicalPath(
                                                 static_cast<EContentDocument>(category - categories().begin()), key);
            const bool has_error = std::ranges::any_of(diagnostics, [&](const config::Diagnostic &diagnostic) {
                return diagnostic.m_property == key or diagnostic.m_property == canonical_key;
            });

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
                    if (diagnostic.m_property == key or diagnostic.m_property == canonical_key) {
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
            const auto &entry = state.m_documents[category].m_entries[*node.m_entry];
            const auto definition_key = recordKey(entry, categories()[category]);
            const auto prefix = std::string{categories()[category]} + "." + definition_key + ".";
            const auto property = std::string_view{entry.m_key}.substr(prefix.size());
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

            ImGui::BeginDisabled(not editable);
            switch (value.m_kind) {
                case config::EFileKind::boolean: changed = ImGui::Checkbox("##value", &value.m_boolean); break;
                case config::EFileKind::integer:
                case config::EFileKind::floating: changed = numericControl(node, value); break;
                case config::EFileKind::text:
                    changed = choice_control
                                  ? choiceControl(
                                        node, value,
                                        entry.m_location != "production decoder default (omitted in source)", cleared)
                                  : textControl(state, entry, value);
                    break;
            }
            ImGui::EndDisabled();

            if (numeric) {
                if (changed) {
                    state.m_numeric_edit_key = entry.m_key;
                    state.m_numeric_edit = value;
                }
                changed = ImGui::IsItemDeactivatedAfterEdit() and state.m_numeric_edit_key == entry.m_key;
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
                WorkshopIntent intent{cleared ? EWorkshopIntent::clear_property : EWorkshopIntent::edit_definition};
                intent.m_document = static_cast<EContentDocument>(category);
                intent.m_key = definition_key;
                intent.m_property = property;
                intent.m_value = std::move(value);
                intents.push_back(std::move(intent));
            }

            propertyErrors(state, entry.m_key, view);
            if (node.m_descriptor and
                node.m_descriptor->m_reference_category and
                entry.m_value.m_kind == config::EFileKind::text) {
                const auto referenced = static_cast<std::size_t>(*node.m_descriptor->m_reference_category);
                if (std::ranges::find(state.m_record_keys[referenced], entry.m_value.m_text) !=
                    state.m_record_keys[referenced].end()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("->##reference")) {
                        selectDefinition(state, categories()[referenced], entry.m_value.m_text);
                    }
                }
            }
            ImGui::PopID();
        }
        bool treeMatches(const WorldgenPanelState::PropertyNode &node, const std::string_view search) {
            return matches(node.m_path, search) or
                   std::ranges::any_of(node.m_children, [&](const auto &child) { return treeMatches(child, search); });
        }
        void propertyTree(
            WorldgenPanelState &state, const Array<WorldgenPanelState::PropertyNode> &nodes, const std::size_t category,
            const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            const std::string_view search = state.m_property_search.data();

            for (const auto &node: nodes) {
                if (not treeMatches(node, search)) {
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
                    propertyTree(state, node.m_children, category, view, intents);
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
        std::string_view previewReason(const WorldgenPanelState &state, const WorldgenPanelView &view) {
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
            if (state.m_preview_mode == EWorldgenPreviewMode::biome_sample and
                std::ranges::find(state.m_record_keys[1u], state.m_biome.data()) == state.m_record_keys[1u].end()) {
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
        void toolbar(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            ImGui::SetNextItemWidth(-240.0f);
            ImGui::InputText("##pack-root", state.m_root.data(), state.m_root.size());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Authored pack root. Build-staged assets may be overwritten by a build.");
            }

            ImGui::SameLine();
            const bool empty_root = std::string_view{state.m_root.data()}.find_first_not_of(" \t\r\n") ==
                                    std::string_view::npos;
            actionButton(
                "Open pack",
                view.m_closing ? "Workshop is closing."
                : view.m_busy  ? "Wait for the current operation or cancel it first."
                : empty_root   ? "Enter a content pack root before opening."
                               : "",
                EWorkshopIntent::open, intents);
            ImGui::SameLine();
            actionButton("Close", view.m_closing ? "Workshop is closing." : "", EWorkshopIntent::close, intents);
            if (empty_root and not view.m_source) {
                ImGui::TextDisabled("Enter an authored pack directory to enable Open pack.");
            }

            const auto unavailable = view.m_closing      ? "Workshop is closing."
                                     : not view.m_source ? "Open a content pack first."
                                     : view.m_busy       ? "Wait for the current operation or cancel it first."
                                                         : "";
            actionButton(
                "Save documents",
                unavailable[0u] != '\0' ? unavailable
                : not view.m_dirty      ? "No authored changes to save."
                : contentErrors(view)   ? "Fix highlighted definition errors before saving."
                                        : "",
                EWorkshopIntent::save, intents);
            ImGui::SameLine();
            actionButton("Reload", unavailable, EWorkshopIntent::reload, intents);
            ImGui::SameLine();
            actionButton(
                "Revert",
                unavailable[0u] != '\0' ? unavailable
                : not view.m_dirty      ? "The draft already matches the saved baseline."
                                        : "",
                EWorkshopIntent::revert, intents);
            ImGui::SameLine();
            actionButton(
                "Undo",
                view.m_closing        ? "Workshop is closing."
                : not view.m_can_undo ? "No command to undo."
                                      : "",
                EWorkshopIntent::undo, intents);
            ImGui::SameLine();
            actionButton(
                "Redo",
                view.m_closing        ? "Workshop is closing."
                : not view.m_can_redo ? "No command to redo."
                                      : "",
                EWorkshopIntent::redo, intents);
            ImGui::SameLine();
            actionButton(
                "Cancel work",
                view.m_closing or (not view.m_busy and not state.m_automatic_live)
                    ? "No active operation or automatic preview to cancel."
                    : "",
                EWorkshopIntent::cancel, intents);
            ImGui::SameLine();
            ImGui::BeginDisabled(not view.m_source or view.m_closing);
            if (ImGui::Checkbox("Live preview", &state.m_automatic_live)) {
                emit(intents, EWorkshopIntent::preview_settings_changed);
            }
            ImGui::EndDisabled();

            ImGui::Text(
                "%s | draft %llu / validated %llu%s", view.m_dirty ? "Unsaved changes" : "Saved", view.m_revision,
                view.m_validated_revision, view.m_busy ? " | worker busy" : "");
            ImGui::TextWrapped("%.*s", static_cast<int>(view.m_status.size()), view.m_status.data());

            if (contentErrors(view)) {
                ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.35f, 1.0f}, "This draft has property errors.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Show errors")) {
                    state.m_show_diagnostics = true;
                }
            }
            ImGui::Separator();
        }
        void browser(WorldgenPanelState &state, const WorldgenPanelView &view) {
            ImGui::TextUnformatted("CONTENT BROWSER");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##content-search", "Search definitions", state.m_search.data(), state.m_search.size());
            const std::string_view search = state.m_search.data();

            for (std::size_t category = 0u; category < categories().size(); ++category) {
                if (not search.empty()) {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                }
                const auto label = std::string{categories()[category]} +
                                   " (" +
                                   std::to_string(state.m_record_keys[category].size()) +
                                   ")";
                if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (category == 0u and
                        matches("vacuum", search) and
                        ImGui::Selectable("0 vacuum", state.m_selected_key == "vacuum")) {
                        selectDefinition(state, "materials", "vacuum");
                    }
                    for (const auto &key: state.m_record_keys[category]) {
                        if (not matches(key, search) and not matches(categories()[category], search)) {
                            continue;
                        }
                        if (ImGui::Selectable(
                                key.c_str(),
                                state.m_selected_category == categories()[category] and state.m_selected_key == key)) {
                            selectDefinition(state, categories()[category], key);
                        }
                    }
                    ImGui::TreePop();
                }
            }

            if (view.m_catalog and ImGui::TreeNode("Referenced textures")) {
                for (const auto &texture: view.m_catalog->textures()) {
                    if (matches(texture.m_path, search) and ImGui::Selectable(texture.m_path.c_str())) {
                        selectDefinition(state, "textures", texture.m_path);
                    }
                }
                ImGui::TreePop();
            }

            if (view.m_source and ImGui::TreeNode("Pack source")) {
                ImGui::TextWrapped("%s", view.m_source->m_manifest.m_pack_id.c_str());
                ImGui::TextWrapped("%s", view.m_source->m_manifest_source.m_path.string().c_str());
                ImGui::TreePop();
            }
        }
        void definitionActions(
            WorldgenPanelState &state, const std::size_t category, const WorldgenPanelView &view,
            Array<WorkshopIntent> &intents) {
            if (not ImGui::CollapsingHeader("Create / delete definition")) {
                return;
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint(
                "##new-key", "New unique definition key", state.m_new_key.data(), state.m_new_key.size());

            const std::string_view key = state.m_new_key.data();
            const bool valid_key = not key.empty() and
                                   key.find('.') == std::string_view::npos and
                                   config::isValidCanonicalName(key);
            const auto owner_end = state.m_selected_key.rfind('.');
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
                WorkshopIntent intent{EWorkshopIntent::clone_definition};
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
                WorkshopIntent intent{EWorkshopIntent::delete_definition};
                intent.m_document = static_cast<EContentDocument>(category);
                intent.m_key = state.m_selected_key;
                intents.push_back(std::move(intent));
            }
            ImGui::EndDisabled();
            ImGui::TextWrapped(
                "Cloning keeps the selected settings and references. Deletion is undoable; existing references may need repair.");
        }
        void inspector(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            const auto category = std::ranges::find(categories(), state.m_selected_category);

            if (state.m_selected_key.empty()) {
                ImGui::TextWrapped("Choose a definition in the browser to inspect its properties.");
                return;
            }
            if (state.m_selected_key == "vacuum" and state.m_selected_category == "materials") {
                ImGui::TextWrapped("Reserved element ID 0 represents empty terrain. Its identity is fixed.");
                return;
            }

            if (category == categories().end()) {
                if (view.m_catalog and state.m_selected_category == "textures") {
                    for (const auto &texture: view.m_catalog->textures()) {
                        if (texture.m_path == state.m_selected_key) {
                            ImGui::Text(
                                "%u x %u | %llu source bytes", texture.m_image.m_width, texture.m_image.m_height,
                                texture.m_source_size);
                            ImGui::Text("Identity %016llx", texture.m_source_fingerprint);
                        }
                    }
                }
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
                ImGui::TextWrapped(
                    "Linked constraints: lining thickness needs a lining material; enabled pore passes need a pore field and ordered radii. Soil, weathering and formations need their materials. Relaxation + pore + finish + erosion passes must total at most 100.");
            }
            ImGui::TextDisabled(
                "Hover labels for source, units and schema bounds. Ctrl-click sliders for exact values.");

            if (ImGui::BeginTable(
                    "Property tree", 2,
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 230.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                propertyTree(state, state.m_property_tree, index, view, intents);
                ImGui::EndTable();
            }

            definitionActions(state, index, view, intents);

            if (ImGui::CollapsingHeader("Used by")) {
                for (std::size_t other = 0u; other < categories().size(); ++other) {
                    for (const auto &entry: state.m_documents[other].m_entries) {
                        if (entry.m_value.m_kind == config::EFileKind::text and
                            entry.m_value.m_text == state.m_selected_key and
                            ImGui::Selectable(entry.m_key.c_str())) {
                            selectDefinition(state, categories()[other], recordKey(entry, categories()[other]));
                            state.m_focus_property = entry.m_key;
                        }
                    }
                }
            }
        }
        void previewControls(WorldgenPanelState &state, const WorldgenPanelView &view, Array<WorkshopIntent> &intents) {
            ImGui::TextUnformatted("256 x 256 fine cells | 85.33 m square");
            ImGui::TextWrapped(
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
            changed = keyCombo("Sample biome", state.m_biome.data(), state.m_biome.size(), state.m_record_keys[1u]) or
                      changed;
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
            ImGui::TextDisabled("Global +Y is height; depth follows the selected profile surface.");
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
            actionButton("Prepare preview", reason, EWorkshopIntent::preview, intents);
            if (not reason.empty()) {
                ImGui::TextWrapped("%.*s", static_cast<int>(reason.size()), reason.data());
            }
            if (view.m_publishing) {
                ImGui::TextWrapped("Preparing the render preview; the previous picture remains visible.");
            }

            if (view.m_preview) {
                const auto &preview = *view.m_preview;
                ImGui::Separator();
                ImGui::Text(
                    "Preview draft %llu: %s", preview.m_spec.m_revision,
                    view.m_preview_current ? "current" : "OUT OF DATE");
                ImGui::Text(
                    "Solid %.2f%% | vacuum %.2f%% | checksum %016llx",
                    100.0 * preview.m_solid_cells / (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                    100.0 *
                        (kWorldgenPreviewEdge * kWorldgenPreviewEdge - preview.m_solid_cells) /
                        (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                    preview.m_checksum);
                ImGui::Text(
                    "Visible [%u,%u) x [%u,%u)", preview.m_visible.m_left, preview.m_visible.m_right,
                    preview.m_visible.m_bottom, preview.m_visible.m_top);

                if (ImGui::TreeNode("Material counts")) {
                    for (const auto &count: preview.m_material_counts) {
                        const auto *material = preview.m_catalog.findMaterial(count.m_material);
                        ImGui::Text(
                            "%u %s: %llu", count.m_material, material ? material->m_key.c_str() : "vacuum",
                            count.m_cells);
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
                ImGui::TextWrapped(
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

        ImGui::SetNextWindowPos(ImVec2{12.0f, 34.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{1100.0f, 820.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2{900.0f, 520.0f}, ImVec2{(std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)()});
        if (not ImGui::Begin("Worldgen Workshop")) {
            ImGui::End();
            return intents;
        }

        toolbar(state, view, intents);

        if (ImGui::BeginTable("Workshop layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Browser", ImGuiTableColumnFlags_WidthFixed, 235.0f);
            ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("Content browser", ImVec2{0.0f, 0.0f});
            browser(state, view);
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("Definition inspector", ImVec2{0.0f, 0.0f});
            ImGui::Text(
                "%s / %s", state.m_selected_category.c_str(),
                state.m_selected_key.empty() ? "Choose a definition" : state.m_selected_key.c_str());
            refreshPropertyTree(state);
            if (ImGui::BeginTabBar("Inspector tabs")) {
                const auto property_flags = state.m_show_properties ? ImGuiTabItemFlags_SetSelected
                                                                    : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Properties", nullptr, property_flags)) {
                    state.m_show_properties = false;
                    inspector(state, view, intents);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Preview")) {
                    previewControls(state, view, intents);
                    ImGui::EndTabItem();
                }
                const auto diagnostic_flags = state.m_show_diagnostics ? ImGuiTabItemFlags_SetSelected
                                                                       : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Diagnostics", nullptr, diagnostic_flags)) {
                    state.m_show_diagnostics = false;
                    diagnosticsPanel(state, view);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::End();
        return intents;
    }
}
