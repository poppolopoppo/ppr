module;
module game.colony.worldgen_panel;
import imgui;
import std;

namespace pP::colony {
    namespace {
        constexpr std::array<std::string_view, kContentDocumentCount> kCategories{
            "materials", "biomes", "fields", "features", "worlds", "backgrounds"};
        void emit(Array<WorkshopIntent> &intents, const EWorkshopIntent kind) {
            intents.push_back({kind});
        }
        std::string recordKey(const config::Entry &entry, const std::string_view category) {
            const auto prefix = std::string{category} + ".";
            if (not entry.m_key.starts_with(prefix)) {
                return {};
            }
            const auto end = entry.m_key.find('.', prefix.size());
            return entry.m_key.substr(prefix.size(), end - prefix.size());
        }
        void appendResolvedBiomeDefaults(config::Document &document, const ContentCatalog &catalog) {
            auto append = [&document](
                              const std::string &prefix, const std::string_view property, config::FileValue value) {
                const auto key = prefix + std::string{property};
                if (std::ranges::find(document.m_entries, key, &config::Entry::m_key) == document.m_entries.end()) {
                    document.m_entries.push_back(
                        {key, std::move(value), "production decoder default (omitted in source)"});
                }
            };
            for (const auto &biome: catalog.biomes()) {
                const auto prefix = "biomes." + biome.m_key + ".";
                append(prefix, "lining_thickness_fine", config::FileValue::integer(biome.m_lining_thickness_fine));
                append(prefix, "relaxation_passes", config::FileValue::integer(biome.m_relaxation_passes));
                append(prefix, "relaxation_adoption", config::FileValue::floating(biome.m_relaxation_adoption));
                append(prefix, "sprinkle_probability", config::FileValue::floating(biome.m_sprinkle_probability));
                append(prefix, "pore_probability", config::FileValue::floating(biome.m_pore_probability));
                append(prefix, "pore_radius_min_m", config::FileValue::floating(biome.m_pore_radius_min_m));
                append(prefix, "pore_radius_max_m", config::FileValue::floating(biome.m_pore_radius_max_m));
                append(prefix, "pore_affinity_min", config::FileValue::floating(biome.m_pore_affinity_min));
                append(prefix, "pore_passes", config::FileValue::integer(biome.m_pore_passes));
                append(prefix, "finish_passes", config::FileValue::integer(biome.m_finish_passes));
                append(prefix, "finish_adoption", config::FileValue::floating(biome.m_finish_adoption));
                append(prefix, "erosion_passes", config::FileValue::integer(biome.m_erosion_passes));
                append(prefix, "soil_depth_m", config::FileValue::floating(biome.m_soil_depth_m));
                append(prefix, "weathered_depth_m", config::FileValue::floating(biome.m_weathered_depth_m));
                append(prefix, "formation_probability", config::FileValue::floating(biome.m_formation_probability));
                append(prefix, "formation_length_m", config::FileValue::floating(biome.m_formation_length_m));
                for (const auto &band: biome.m_composition) {
                    const auto nested = prefix + "composition." + band.m_key + ".";
                    append(nested, "bed_period_m", config::FileValue::floating(band.m_bed_period_m));
                    append(nested, "bed_warp_m", config::FileValue::floating(band.m_bed_warp_m));
                    append(nested, "bed_dip", config::FileValue::floating(band.m_bed_dip));
                }
                for (const auto &deposit: biome.m_deposits) {
                    const auto nested = prefix + "deposits." + deposit.m_key + ".";
                    append(nested, "threshold_min", config::FileValue::floating(deposit.m_threshold_min));
                    append(nested, "threshold_max", config::FileValue::floating(deposit.m_threshold_max));
                    append(
                        nested, "orientation_min_degrees",
                        config::FileValue::floating(deposit.m_orientation_min_degrees));
                    append(
                        nested, "orientation_max_degrees",
                        config::FileValue::floating(deposit.m_orientation_max_degrees));
                }
            }
            std::ranges::sort(document.m_entries, {}, &config::Entry::m_key);
        }
        void navigateReference(WorldgenPanelState &state, const std::string_view key) {
            for (std::size_t category = 0u; category < kCategories.size(); ++category) {
                for (const auto &entry: state.m_documents[category].m_entries) {
                    if (recordKey(entry, kCategories[category]) == key) {
                        state.m_selected_category = kCategories[category];
                        state.m_selected_key = key;
                        return;
                    }
                }
            }
        }
        void propertyControl(
            WorldgenPanelState &state, const config::Entry &entry, const std::string_view property, const bool editable,
            Array<WorkshopIntent> &intents) {
            ImGui::PushID(entry.m_key.c_str());
            auto value = entry.m_value;
            bool changed = false;
            ImGui::BeginDisabled(not editable);
            switch (value.m_kind) {
                case config::EFileKind::boolean:
                    changed = ImGui::Checkbox(std::string{property}.c_str(), &value.m_boolean);
                    break;
                case config::EFileKind::integer:
                    changed = ImGui::InputScalar(std::string{property}.c_str(), ImGuiDataType_S64, &value.m_integer);
                    break;
                case config::EFileKind::floating:
                    changed = ImGui::InputDouble(std::string{property}.c_str(), &value.m_floating, 0.0, 0.0, "%.8g");
                    break;
                case config::EFileKind::text: {
                    if (state.m_edit_key != entry.m_key) {
                        std::array<char, 1024u> temporary{};
                        std::snprintf(temporary.data(), temporary.size(), "%s", value.m_text.c_str());
                        if (ImGui::InputText(
                                std::string{property}.c_str(), temporary.data(), temporary.size(),
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                            value.m_text = temporary.data();
                            changed = true;
                        }
                        if (ImGui::IsItemActive()) {
                            state.m_edit_key = entry.m_key;
                            state.m_text_edit = temporary;
                        }
                    } else {
                        if (ImGui::InputText(
                                std::string{property}.c_str(), state.m_text_edit.data(), state.m_text_edit.size(),
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                            value.m_text = state.m_text_edit.data();
                            changed = true;
                        }
                        if (not ImGui::IsItemActive()) {
                            state.m_edit_key.clear();
                        }
                    }
                    break;
                }
            }
            ImGui::EndDisabled();
            if (changed) {
                intents.push_back(
                    {EWorkshopIntent::edit_biome, state.m_selected_key, std::string{property}, std::move(value)});
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s\n%s\nAuthored value; omitted properties retain the production decoder defaults.",
                    entry.m_key.c_str(), entry.m_location.c_str());
            }
            if (entry.m_value.m_kind == config::EFileKind::text) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Go to reference")) {
                    navigateReference(state, entry.m_value.m_text);
                }
            }
            ImGui::PopID();
        }
    }
    Array<WorkshopIntent> drawWorldgenPanel(WorldgenPanelState &state, const WorldgenPanelView &view) {
        Array<WorkshopIntent> intents{};
        if (view.m_source != state.m_document_source or
            view.m_revision != state.m_document_revision or
            view.m_validated_revision != state.m_document_validated_revision or
            view.m_catalog != state.m_document_catalog) {
            state.m_document_source = view.m_source;
            state.m_document_revision = view.m_revision;
            state.m_document_validated_revision = view.m_validated_revision;
            state.m_document_catalog = view.m_catalog;
            for (std::size_t i = 0u; i < kCategories.size(); ++i) {
                state.m_documents[i] = {};
                if (view.m_source) {
                    const auto &source = view.m_source->m_documents[i];
                    config::Diagnostics diagnostics{8u};
                    auto decoded = config::decode(
                        source.m_bytes, source.m_path.string(), diagnostics, config::EDecodeMode::records);
                    if (decoded) {
                        state.m_documents[i] = std::move(*decoded);
                    }
                }
            }
            if (view.m_catalog and view.m_validated_revision == view.m_revision) {
                appendResolvedBiomeDefaults(state.m_documents[1u], *view.m_catalog);
            }
        }
        ImGui::SetNextWindowPos(ImVec2{12.0f, 34.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{500.0f, 800.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Worldgen Workshop");
        ImGui::TextUnformatted("Biome sample - 256 x 256 fine cells (85.33 m square)");
        ImGui::TextWrapped(
            "The cyan viewport outline marks the counted/exported interior. Extra rendered cells are support context for contours and formations.");
        ImGui::TextWrapped(
            "Open an explicit authored pack root. Staged executable assets may be overwritten by builds.");
        ImGui::InputText("Pack root", state.m_root.data(), state.m_root.size());
        ImGui::BeginDisabled(view.m_busy or view.m_closing);
        if (ImGui::Button("Open root")) {
            emit(intents, EWorkshopIntent::open);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(view.m_closing);
        if (ImGui::Button("Close Workshop")) {
            emit(intents, EWorkshopIntent::close);
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped("%.*s", static_cast<int>(view.m_status.size()), view.m_status.data());
        ImGui::Text(
            "Revision %llu / validated %llu - %s%s", view.m_revision, view.m_validated_revision,
            view.m_dirty ? "DIRTY" : "saved", view.m_busy ? " - worker busy" : "");
        if (view.m_publishing) {
            ImGui::TextUnformatted("Preparing render-owned preview; previous picture retained.");
        }
        ImGui::InputText("Profile", state.m_profile.data(), state.m_profile.size());
        ImGui::InputText("Sample biome", state.m_biome.data(), state.m_biome.size());
        ImGui::InputScalar("Full 64-bit seed", ImGuiDataType_U64, &state.m_seed);
        ImGui::InputInt2("Origin fine cells (X, Y)", state.m_origin);
        ImGui::TextUnformatted("Global Y is height; depth follows the selected profile surface.");
        constexpr const char *modes[]{"Textured", "Material", "Biome", "Temperature (200-1000 K)"};
        int mode = static_cast<int>(state.m_mode);
        if (ImGui::Combo("View", &mode, modes, 4)) {
            state.m_mode = static_cast<EGridDiagnosticMode>(mode);
        }
        ImGui::BeginDisabled(not view.m_source or view.m_closing);
        if (ImGui::Button("Preview")) {
            emit(intents, EWorkshopIntent::preview);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            emit(intents, EWorkshopIntent::cancel);
        }
        ImGui::BeginDisabled(view.m_busy or view.m_closing or not view.m_source);
        if (ImGui::Button("Save Biomes")) {
            emit(intents, EWorkshopIntent::save);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            emit(intents, EWorkshopIntent::reload);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            emit(intents, EWorkshopIntent::revert);
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(not view.m_can_undo or view.m_closing);
        if (ImGui::Button("Undo")) {
            emit(intents, EWorkshopIntent::undo);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(not view.m_can_redo or view.m_closing);
        if (ImGui::Button("Redo")) {
            emit(intents, EWorkshopIntent::redo);
        }
        ImGui::EndDisabled();
        ImGui::InputText("Fresh export directory", state.m_export_path.data(), state.m_export_path.size());
        ImGui::BeginDisabled(not view.m_can_export or view.m_busy or view.m_closing);
        if (ImGui::Button("Export Preview")) {
            emit(intents, EWorkshopIntent::export_preview);
        }
        ImGui::EndDisabled();
        if (view.m_preview) {
            const auto &preview = *view.m_preview;
            ImGui::Text(
                "Preview draft %llu: %s", preview.m_spec.m_revision,
                view.m_preview_current ? "current" : "OUT OF DATE");
            ImGui::Text(
                "Solid %.2f%%; vacuum %.2f%%; checksum %016llx",
                100.0 * preview.m_solid_cells / (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                100.0 *
                    (kWorldgenPreviewEdge * kWorldgenPreviewEdge - preview.m_solid_cells) /
                    (kWorldgenPreviewEdge * kWorldgenPreviewEdge),
                preview.m_checksum);
            ImGui::Text(
                "Visible [%u,%u) x [%u,%u); support [%u,%u) x [%u,%u)", preview.m_visible.m_left,
                preview.m_visible.m_right, preview.m_visible.m_bottom, preview.m_visible.m_top,
                preview.m_support.m_left, preview.m_support.m_right, preview.m_support.m_bottom,
                preview.m_support.m_top);
            if (ImGui::TreeNode("Material counts")) {
                for (const auto &count: preview.m_material_counts) {
                    const auto *material = preview.m_catalog.findMaterial(count.m_material);
                    ImGui::Text(
                        "%u %s: %llu", count.m_material, material ? material->m_key.c_str() : "vacuum", count.m_cells);
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Profile-owned participating cave / worm rules")) {
                if (ImGui::SmallButton("Inspect owning world profile")) {
                    state.m_selected_category = "worlds";
                    state.m_selected_key = preview.m_spec.m_profile;
                }
                for (const auto &rule: preview.m_participating_rules) {
                    ImGui::TextUnformatted(rule.c_str());
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
        if (not view.m_diagnostics.empty()) {
            ImGui::TextWrapped("%.*s", static_cast<int>(view.m_diagnostics.size()), view.m_diagnostics.data());
        }
        ImGui::Separator();
        ImGui::InputText("Search content", state.m_search.data(), state.m_search.size());
        if (ImGui::TreeNode("Pack")) {
            if (view.m_source) {
                ImGui::Text("%s", view.m_source->m_manifest.m_pack_id.c_str());
                ImGui::TextWrapped("Opened manifest: %s", view.m_source->m_manifest_source.m_path.string().c_str());
                for (const auto &document: view.m_source->m_documents) {
                    ImGui::TextWrapped("%s", document.m_path.string().c_str());
                }
            }
            ImGui::TreePop();
        }
        for (std::size_t i = 0u; i < kCategories.size(); ++i) {
            if (ImGui::TreeNode(std::string{kCategories[i]}.c_str())) {
                if (i == 0u and ImGui::Selectable("0 vacuum")) {
                    state.m_selected_category = "materials";
                    state.m_selected_key = "vacuum";
                }
                std::string previous;
                for (const auto &entry: state.m_documents[i].m_entries) {
                    const auto key = recordKey(entry, kCategories[i]);
                    if (key.empty() or key == previous) {
                        continue;
                    }
                    previous = key;
                    if (not std::string_view{state.m_search.data()}.empty() and
                        key.find(state.m_search.data()) == std::string::npos) {
                        continue;
                    }
                    const auto label = std::string{kCategories[i]} + "/" + key;
                    if (ImGui::Selectable(
                            label.c_str(),
                            state.m_selected_category == kCategories[i] and state.m_selected_key == key)) {
                        state.m_selected_category = kCategories[i];
                        state.m_selected_key = key;
                        if (i == 1u) {
                            std::snprintf(state.m_biome.data(), state.m_biome.size(), "%s", key.c_str());
                        }
                    }
                }
                ImGui::TreePop();
            }
        }
        if (view.m_catalog and ImGui::TreeNode("Referenced textures")) {
            for (const auto &texture: view.m_catalog->textures()) {
                if (ImGui::Selectable(texture.m_path.c_str())) {
                    state.m_selected_category = "textures";
                    state.m_selected_key = texture.m_path;
                }
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::Text("%s / %s", state.m_selected_category.c_str(), state.m_selected_key.c_str());
        ImGui::TextWrapped(
            "Units: _m = metres, _k = Kelvin, _fine = fine cells, normalized/probability = unitless. Hover a property for its source or omitted default.");
        const auto prefix = state.m_selected_category + "." + state.m_selected_key + ".";
        if (state.m_selected_key == "vacuum") {
            ImGui::TextUnformatted("Reserved element ID 0; empty terrain. Identity is fixed.");
        }
        if (view.m_catalog and state.m_selected_category == "textures") {
            for (const auto &texture: view.m_catalog->textures()) {
                if (texture.m_path != state.m_selected_key) {
                    continue;
                }
                ImGui::Text(
                    "%s: %u x %u, %llu source bytes, identity %016llx",
                    texture.m_normal_map ? "Normal map" : "Albedo / background", texture.m_image.m_width,
                    texture.m_image.m_height, texture.m_source_size, texture.m_source_fingerprint);
            }
        }
        for (std::size_t i = 0u; i < kCategories.size(); ++i) {
            if (state.m_selected_category != kCategories[i]) {
                continue;
            }
            for (const auto &entry: state.m_documents[i].m_entries) {
                if (not entry.m_key.starts_with(prefix)) {
                    continue;
                }
                const auto property = std::string_view{entry.m_key}.substr(prefix.size());
                const bool editable = i == 1u and property != "id" and property != "key" and not view.m_closing;
                propertyControl(state, entry, property, editable, intents);
            }
        }
        if (ImGui::TreeNode("Used by")) {
            for (std::size_t i = 0u; i < kCategories.size(); ++i) {
                for (const auto &entry: state.m_documents[i].m_entries) {
                    if (entry.m_value.m_kind != config::EFileKind::text or
                        entry.m_value.m_text != state.m_selected_key) {
                        continue;
                    }
                    if (ImGui::Selectable(entry.m_key.c_str())) {
                        state.m_selected_category = kCategories[i];
                        state.m_selected_key = recordKey(entry, kCategories[i]);
                    }
                }
            }
            ImGui::TreePop();
        }
        ImGui::End();
        return intents;
    }
}
