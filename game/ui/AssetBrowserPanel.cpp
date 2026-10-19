module;
#include "pP/Macros.h"

module game.colony.asset_browser_panel;

import imgui;
import engine.app;
import engine.asset;
import engine.core;
import std;
import game.ui;

namespace pP::colony {
    // ------------------------------------------------------------------
    // panel-local constants
    // ------------------------------------------------------------------

    namespace {
        /// Part of every tile gap stays outside the tile hit boxes, so a
        /// box-select or a void click can start from a gap.
        inline constexpr float kAssetIconHitSpacing{4.0f};
        inline constexpr float kAssetMarkerSize{4.0f};
        inline constexpr int kAssetIconSpacingMax{32};

        /// The reference browser fills a tile over the selectable's own
        /// selection fill at 220/255 alpha, letting the selection tint show
        /// through the tile; the same proportion keeps selection readable here.
        inline constexpr float kAssetTileBackgroundAlpha{220.0f / 255.0f};

        /// Tile labels appear once a tile is at least this wide. The reference
        /// measures the same threshold against a short reference string and
        /// starts its icon size exactly at it; real local names vary, and the
        /// label is clipped to its tile.
        inline constexpr std::string_view kAssetTileLabelReference{"granite"};

        /// Header order of the sort table; `browserSortRow` sets up its columns
        /// in this order.
        inline constexpr std::array kAssetSortColumns{
            EAssetSortField::local_name, EAssetSortField::kind, EAssetSortField::pack, EAssetSortField::descriptor};
    }

    // ------------------------------------------------------------------
    // filter matching and ordering
    // ------------------------------------------------------------------

    bool assetBrowserMatches(const std::string_view text, const std::string_view filter) noexcept {
        if (filter.empty()) {
            return true;
        }

        const auto lower = [](const char value) { return value >= 'A' and value <= 'Z' ? value + ('a' - 'A') : value; };

        return std::search(
                   text.begin(), text.end(), filter.begin(), filter.end(),
                   [&](const char left, const char right) { return lower(left) == lower(right); }) != text.end();
    }

    Array<std::size_t> assetBrowserFiltered(
        const std::span<const asset::IndexedAsset> assets, const std::string_view filter) {
        Array<std::size_t> order;
        order.reserve(assets.size());
        for (std::size_t index = 0u; index < assets.size(); ++index) {
            const auto &entry = assets[index];
            if (assetBrowserMatches(entry.m_kind, filter) or
                assetBrowserMatches(entry.m_name.string(), filter) or
                assetBrowserMatches(entry.m_descriptor.string(), filter)) {
                order.push_back(index);
            }
        }

        return order;
    }

    namespace {
        [[nodiscard]] constexpr int compareText(const std::string_view lhs, const std::string_view rhs) noexcept {
            return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
        }

        /// One sort spec's comparison. The descriptor compares as a path, so no
        /// per-comparison string copy is made.
        [[nodiscard]] int compareAssetField(
            const asset::IndexedAsset &lhs, const asset::IndexedAsset &rhs, const EAssetSortField field) noexcept {
            switch (field) {
                case EAssetSortField::local_name: return compareText(lhs.m_name.m_local, rhs.m_name.m_local);
                case EAssetSortField::kind: return compareText(lhs.m_kind, rhs.m_kind);
                case EAssetSortField::pack: return compareText(lhs.m_name.m_pack, rhs.m_name.m_pack);
                case EAssetSortField::descriptor: return lhs.m_descriptor.compare(rhs.m_descriptor);
            }

            return 0;
        }
    }

    void assetBrowserSort(
        Array<std::size_t> &order, const std::span<const asset::IndexedAsset> assets,
        const std::span<const AssetSortSpec> specs) {
        std::ranges::sort(order, [&](const std::size_t lhs, const std::size_t rhs) {
            for (const auto &spec: specs) {
                const int delta = compareAssetField(assets[lhs], assets[rhs], spec.m_field);
                if (delta != 0) {
                    return spec.m_descending ? delta > 0 : delta < 0;
                }
            }

            return lhs < rhs; // Catalog position keeps the order total, not merely stable.
        });
    }

    Array<std::size_t> assetBrowserVisibleOrder(
        const std::span<const asset::IndexedAsset> assets, const std::string_view filter,
        const std::span<const AssetSortSpec> specs) {
        auto order = assetBrowserFiltered(assets, filter);
        assetBrowserSort(order, assets, specs);
        return order;
    }

    std::string_view assetBrowserEmptyText(const bool index_has_assets) noexcept {
        return index_has_assets ? std::string_view{"No asset matches the filter."}
                                : std::string_view{"No assets in this pack index."};
    }

    // ------------------------------------------------------------------
    // tile grid layout, zoom and markers
    // ------------------------------------------------------------------

    AssetGridLayout assetGridLayout(
        const float avail_width, const float icon_size, const int icon_spacing, const bool stretch,
        const int item_count) noexcept {
        AssetGridLayout layout{};
        layout.m_item_size = std::max(std::floor(icon_size), 1.0f);
        layout.m_item_spacing = static_cast<float>(std::max(icon_spacing, 0));

        // Without stretch the tiles may extend into the right-most gap, so the
        // width the column count divides is the child's width plus half a gap.
        float width = avail_width;
        if (not stretch) {
            width += std::floor(layout.m_item_spacing * 0.5f);
        }

        layout.m_column_count = std::max(static_cast<int>(width / (layout.m_item_size + layout.m_item_spacing)), 1);
        if (stretch and layout.m_column_count > 1) {
            layout.m_item_spacing = std::floor(width - layout.m_item_size * static_cast<float>(layout.m_column_count)) /
                                    static_cast<float>(layout.m_column_count);
        }

        layout.m_line_count = (std::max(item_count, 0) + layout.m_column_count - 1) / layout.m_column_count;
        layout.m_item_step = layout.m_item_size + layout.m_item_spacing;
        layout.m_selectable_spacing = std::max(std::floor(layout.m_item_spacing) - kAssetIconHitSpacing, 0.0f);
        layout.m_outer_padding = std::floor(layout.m_item_spacing * 0.5f);

        return layout;
    }

    float assetBrowserZoomedIconSize(const float icon_size, const int wheel_steps) noexcept {
        const float scaled = icon_size * std::pow(1.1f, static_cast<float>(wheel_steps));
        return std::clamp(scaled, kAssetIconSizeMin, kAssetIconSizeMax);
    }

    float assetBrowserZoomScrollY(
        const AssetGridLayout &before, const AssetGridLayout &after, const float pointer_x, const float pointer_y,
        const float pointer_in_window_y, const float padding_y) noexcept {
        const float x = (pointer_x + before.m_item_spacing * 0.5f) / before.m_item_step;
        const float y = (pointer_y + before.m_item_spacing * 0.5f) / before.m_item_step;
        const int item = static_cast<int>(y) * before.m_column_count + static_cast<int>(x);
        const float replacement_line = static_cast<float>(item / after.m_column_count) + std::fmod(y, 1.0f);

        return replacement_line * after.m_item_step + padding_y - pointer_in_window_y;
    }

    EAssetKindMarker assetBrowserKindMarker(const std::string_view kind) noexcept {
        const auto dot = kind.find('.');
        const std::string_view family = kind.substr(0u, dot == std::string_view::npos ? kind.size() : dot);
        if (family == "sim") {
            return EAssetKindMarker::simulation;
        }

        if (family == "image") {
            return EAssetKindMarker::image;
        }

        if (family == "colony") {
            return EAssetKindMarker::colony;
        }

        return EAssetKindMarker::none;
    }

    // ------------------------------------------------------------------
    // selection identity
    // ------------------------------------------------------------------

    ImGuiID assetBrowserSelectionIdFromViewIndex(
        ImGuiSelectionBasicStorage *const self, const int view_index) noexcept {
        const auto *const visible = static_cast<const Array<std::size_t> *>(self->UserData);
        return assetBrowserSelectionId((*visible)[static_cast<std::size_t>(view_index)]);
    }

    namespace {
        // ------------------------------------------------------------------
        // panel-local data
        // ------------------------------------------------------------------

        /// Tiles mark the kind family: the three families the content kinds use
        /// read as an accent, a positive and a caution tone, and anything else
        /// stays unmarked rather than inventing a colour.
        [[nodiscard]] ui::ThemeColor assetMarkerToken(const EAssetKindMarker marker) noexcept {
            switch (marker) {
                case EAssetKindMarker::simulation: return ui::theme::kAccent;
                case EAssetKindMarker::image: return ui::theme::kSuccess;
                case EAssetKindMarker::colony: return ui::theme::kWarning;
                case EAssetKindMarker::none: break;
            }

            return ui::theme::kTextSecondary; // Unmarked tiles draw no marker.
        }

        [[nodiscard]] ImU32 assetMarkerColor(const EAssetKindMarker marker) {
            return ImGui::GetColorU32(assetMarkerToken(marker).vec4());
        }

        std::string_view assetKindIcon(const std::string_view kind) noexcept {
            if (kind == "sim.material" or kind == "sim.element") {
                return ui::icons::kCubesLabel;
            }

            if (kind == "sim.biome") {
                return ui::icons::kLeafLabel;
            }

            if (kind == "sim.field" or kind == "sim.rule") {
                return ui::icons::kSlidersLabel;
            }

            if (kind == "sim.feature") {
                return ui::icons::kMountainLabel;
            }

            if (kind == "sim.world") {
                return ui::icons::kGlobeLabel;
            }

            if (kind == "image.texture") {
                return ui::icons::kImageLabel;
            }

            if (kind.starts_with("colony.background")) {
                return ui::icons::kLayersLabel;
            }

            return ui::icons::kFileLabel;
        }

        /// 16-digit hex identity, as the tooltip, the facts table and the window
        /// title all show it.
        [[nodiscard]] std::string assetKeyText(const asset::AssetKey key) {
            char text[17]{};
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(*key));
            return text;
        }

        // ------------------------------------------------------------------
        // order cache
        // ------------------------------------------------------------------

        /// Rebuilds the visible order when the index, its revision or the filter
        /// changed. A changed index also drops the selection: stored ids are
        /// catalog positions, and stale ones would name a different asset.
        void refreshOrder(AssetBrowserPanelState &state, const AssetBrowserPanelView &view) {
            const std::string_view filter = state.m_search.data();
            const bool same_index = state.m_order_index == view.m_index and
                                    state.m_order_revision == view.m_index_revision;
            if (same_index and state.m_order_filter == filter) {
                return;
            }

            if (not same_index) {
                state.m_selection.Clear();
                if (state.m_active_key) {
                    const auto assets = view.m_index->assets();
                    const auto active = std::ranges::find(assets, *state.m_active_key, &asset::IndexedAsset::m_key);
                    if (active != assets.end()) {
                        state.m_selection.SetItemSelected(
                            assetBrowserSelectionId(static_cast<std::size_t>(active - assets.begin())), true);
                    }
                }
            }

            state.m_order_index = view.m_index;
            state.m_order_revision = view.m_index_revision;
            state.m_order_filter = filter;
            state.m_visible = assetBrowserVisibleOrder(view.m_index->assets(), filter, state.m_order_specs);
        }

        /// Adopts the header table's specs and re-orders the grid in place: the
        /// filter result is unchanged, so only the ordering work repeats.
        void applySortSpecs(
            AssetBrowserPanelState &state, const asset::PackSet &index, const ImGuiTableSortSpecs &specs) {
            state.m_order_specs.clear();
            for (const ImGuiTableColumnSortSpecs &spec:
                std::span{specs.Specs, static_cast<std::size_t>(specs.SpecsCount)}) {
                const auto column = static_cast<std::size_t>(spec.ColumnIndex);
                if (column >= kAssetSortColumns.size()) {
                    continue;
                }

                state.m_order_specs.push_back(
                    {kAssetSortColumns[column], spec.SortDirection == ImGuiSortDirection_Descending});
            }

            assetBrowserSort(state.m_visible, index.assets(), state.m_order_specs);
        }

        // ------------------------------------------------------------------
        // navigation options
        // ------------------------------------------------------------------

        void markerLegendEntry(const EAssetKindMarker marker, const char *const family) {
            const float line_height = ImGui::GetTextLineHeight();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2{kAssetMarkerSize + 4.0f, line_height});
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2{origin.x, origin.y + (line_height - kAssetMarkerSize) * 0.5f},
                ImVec2{origin.x + kAssetMarkerSize, origin.y + (line_height + kAssetMarkerSize) * 0.5f},
                assetMarkerColor(marker));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", family);
        }

        void browserMenuBar(AssetBrowserPanelState &state) {
            if (ImGui::Button("View##asset_options")) {
                ImGui::OpenPopup("##asset_options");
            }

            if (ImGui::BeginPopup("##asset_options")) {
                ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SeparatorText("Contents");
                ImGui::Checkbox("Show type overlay", &state.m_show_type_overlay);
                ImGui::Checkbox("Show sort headers", &state.m_show_sort_headers);
                ImGui::SeparatorText("Layout");
                ImGui::SliderFloat("Icon size", &state.m_icon_size, kAssetIconSizeMin, kAssetIconSizeMax, "%.0f");
                ImGui::SameLine();
                ui::helpMarker("Use Ctrl+Wheel to zoom");

                ImGui::SliderInt("Icon spacing", &state.m_icon_spacing, 0, kAssetIconSpacingMax);
                ImGui::Checkbox("Stretch spacing", &state.m_stretch_spacing);
                ImGui::PopItemWidth();
                ImGui::SeparatorText("Kind markers");
                markerLegendEntry(EAssetKindMarker::simulation, "sim");
                ImGui::SameLine();
                markerLegendEntry(EAssetKindMarker::image, "image");
                ImGui::SameLine();
                markerLegendEntry(EAssetKindMarker::colony, "colony");
                ImGui::EndPopup();
            }
        }

        // ------------------------------------------------------------------
        // browser sort row
        // ------------------------------------------------------------------

        /// Header-only sortable table: the grid has no list view to hang headers
        /// on, so the specs live in a row of their own above it.
        void browserSortRow(AssetBrowserPanelState &state, const asset::PackSet &index) {
            const auto flags = ImGuiTableFlags_Sortable |
                               ImGuiTableFlags_SortMulti |
                               ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_Borders;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0f, 0.0f});
            if (ImGui::BeginTable(
                    "Asset sort", static_cast<int>(kAssetSortColumns.size()), flags,
                    ImVec2{0.0f, ImGui::GetFrameHeight()})) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Kind");
                ImGui::TableSetupColumn("Pack");
                ImGui::TableSetupColumn("Descriptor");
                ImGui::TableHeadersRow();
                if (ImGuiTableSortSpecs *const specs = ImGui::TableGetSortSpecs()) {
                    if (specs->SpecsDirty) {
                        applySortSpecs(state, index, *specs);
                        specs->SpecsDirty = false;
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopStyleVar();
        }

        // ------------------------------------------------------------------
        // browser tile grid
        // ------------------------------------------------------------------

        /// Native selectable hit box, symbolic kind glyph, clipped label and tooltip.
        /// No runtime image or texture is loaded for a thumbnail.
        void assetTile(
            AssetBrowserPanelState &state, const AssetBrowserPanelView &view, const std::size_t position,
            const int column_count, const int line_index, const ImVec2 start_pos, const float label_threshold,
            std::optional<asset::AssetKey> &activation) {
            const asset::IndexedAsset &entry = view.m_index->assets()[state.m_visible[position]];
            const AssetGridLayout &layout = state.m_layout;
            const ImGuiID id = assetBrowserSelectionId(state.m_visible[position]);

            const ImVec2 pos{
                start_pos.x +
                    static_cast<float>(position % static_cast<std::size_t>(column_count)) * layout.m_item_step,
                start_pos.y + static_cast<float>(line_index) * layout.m_item_step};
            // Widget focus follows the stable asset identity; multi-select storage still adapts catalog positions.
            ImGui::PushID(static_cast<int>(*entry.m_key >> 32u));
            ImGui::PushID(static_cast<int>(*entry.m_key & 0xffffffffu));
            ImGui::SetCursorScreenPos(pos);

            ImGui::SetNextItemSelectionUserData(static_cast<ImGuiSelectionUserData>(position));
            bool selected = state.m_selection.Contains(id);
            const bool visible = ImGui::IsRectVisible(ImVec2{layout.m_item_size, layout.m_item_size});
            ImGui::Selectable("", selected, ImGuiSelectableFlags_None, ImVec2{layout.m_item_size, layout.m_item_size});

            if (ImGui::IsItemClicked() or (ImGui::IsItemFocused() and state.m_active_key != entry.m_key)) {
                activation = entry.m_key;
            } else if (not activation and
                       visible and
                       ImGui::IsWindowHovered() and
                       ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // A value field under edit owns the active item, and ImGui withholds hover
                // from every other item while one is active. The press that deactivates that
                // field still targets the tile it landed on, so route it by position.
                const ImVec2 extent{layout.m_item_size, layout.m_item_size};
                const ImVec2 mouse = ImGui::GetMousePos();
                const bool within_x = mouse.x >= pos.x and mouse.x < pos.x + extent.x;
                const bool within_y = mouse.y >= pos.y and mouse.y < pos.y + extent.y;
                if (within_x and within_y) {
                    activation = entry.m_key;
                }
            }

            // The label colour follows the selection immediately, without
            // waiting for the end-of-frame selection requests.
            if (ImGui::IsItemToggledSelection()) {
                selected = not selected;
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s\nkind: %s\n%s\nkey: %s", entry.m_name.string().c_str(), entry.m_kind.c_str(),
                    entry.m_descriptor.string().c_str(), assetKeyText(entry.m_key).c_str());
            }

            // Box-select widens the clip rect, so a tile clips its own drawing.
            if (visible) {
                ImDrawList *const draw_list = ImGui::GetWindowDrawList();
                const ImVec2 box_min{pos.x - 1.0f, pos.y - 1.0f};
                const ImVec2 box_max{box_min.x + layout.m_item_size + 2.0f, box_min.y + layout.m_item_size + 2.0f};
                const ImVec4 tile_fill = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
                draw_list->AddRectFilled(
                    box_min, box_max,
                    ImGui::GetColorU32(ImVec4{tile_fill.x, tile_fill.y, tile_fill.z, kAssetTileBackgroundAlpha}));

                if (state.m_show_type_overlay) {
                    const auto marker = assetBrowserKindMarker(entry.m_kind);
                    if (marker != EAssetKindMarker::none) {
                        draw_list->AddRectFilled(
                            ImVec2{box_max.x - 2.0f - kAssetMarkerSize, box_min.y + 2.0f},
                            ImVec2{box_max.x - 2.0f, box_min.y + 2.0f + kAssetMarkerSize}, assetMarkerColor(marker));
                    }
                }

                const auto icon = assetKindIcon(entry.m_kind);
                const char *const icon_end = icon.data() + icon.find(' ');
                const float font_size = ImGui::GetFontSize();
                const float icon_size = std::clamp(layout.m_item_size * 0.45f, font_size, font_size * 2.5f);
                const float icon_width = ImGui::CalcTextSize(icon.data(), icon_end).x * icon_size / font_size;
                const ImVec2 parent_min = draw_list->GetClipRectMin();
                const ImVec2 parent_max = draw_list->GetClipRectMax();
                const ImVec4 icon_clip{
                    std::max(box_min.x + 2.0f, parent_min.x), std::max(box_min.y + 2.0f, parent_min.y),
                    std::min(box_max.x - 2.0f, parent_max.x), std::min(box_max.y - 2.0f, parent_max.y)};

                if (icon_clip.z > icon_clip.x and icon_clip.w > icon_clip.y) {
                    draw_list->AddText(
                        nullptr, icon_size,
                        ImVec2{
                            pos.x + (layout.m_item_size - icon_width) * 0.5f,
                            pos.y + (layout.m_item_size - icon_size) * 0.3f},
                        ImGui::GetColorU32(ImGuiCol_Text), icon.data(), icon_end, 0.0f, &icon_clip);
                }

                if (layout.m_item_size >= label_threshold) {
                    const ImU32 label_color = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                    const ImVec2 label_parent_min = draw_list->GetClipRectMin();
                    const ImVec2 label_parent_max = draw_list->GetClipRectMax();
                    const ImVec4 label_clip{
                        std::max(box_min.x + 2.0f, label_parent_min.x), std::max(pos.y, label_parent_min.y),
                        std::min(box_max.x - 2.0f, label_parent_max.x), std::min(box_max.y - 2.0f, label_parent_max.y)};

                    // Native glyph clipping preserves tile bounds without splitting the parent draw command.
                    if (label_clip.z > label_clip.x and label_clip.w > label_clip.y) {
                        draw_list->AddText(
                            nullptr, 0.0f, ImVec2{box_min.x + 4.0f, box_max.y - ImGui::GetFontSize() - 2.0f},
                            label_color, entry.m_name.m_local.c_str(), nullptr, 0.0f, &label_clip);
                    }
                }
            }

            ImGui::PopID();
            ImGui::PopID();
        }

        /// Exact height of the two count lines `browserFooter` draws, the
        /// spacing between them included. The grid child reserves precisely
        /// this, so a short host strip hands every pixel above the footer to
        /// the scrollable grid instead of an over-estimated two-line plus
        /// extra-spacing block, and the footer keeps its place however short
        /// the region gets.
        [[nodiscard]] float browserFooterHeight() noexcept {
            return 2.0f * ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        }

        /// The bordered child holding the virtualized grid: multi-select with
        /// box-select, a clipper over lines, ctrl+wheel zoom and the context
        /// menus. Assets are only read; nothing here mutates the pack index.
        void assetGrid(
            AssetBrowserPanelState &state, const AssetBrowserPanelView &view,
            std::optional<asset::AssetKey> &activation) {
            const std::size_t item_count = state.m_visible.size();

            // Content size comes from the previous frame's layout: the column
            // count depends on the child's width, which is known only inside it.
            const float content_height = state.m_layout.m_outer_padding +
                                         static_cast<float>(state.m_layout.m_line_count) * state.m_layout.m_item_step;
            ImGui::SetNextWindowContentSize(ImVec2{0.0f, content_height});
            if (not ImGui::BeginChild(
                    "Assets", ImVec2{0.0f, -browserFooterHeight()}, ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar)) {
                ImGui::EndChild();
                return;
            }

            const float avail_width = ImGui::GetContentRegionAvail().x;
            state.m_layout = assetGridLayout(
                avail_width, state.m_icon_size, state.m_icon_spacing, state.m_stretch_spacing,
                static_cast<int>(item_count));
            const ImVec2 frame_origin = ImGui::GetCursorScreenPos();
            const ImVec2 start_pos{
                frame_origin.x + state.m_layout.m_outer_padding, frame_origin.y + state.m_layout.m_outer_padding};
            ImGui::SetCursorScreenPos(start_pos);

            if (item_count == 0u) {
                const std::string_view text = assetBrowserEmptyText(not view.m_index->assets().empty());
                ImGui::TextDisabled("%.*s", static_cast<int>(text.size()), text.data());
                ImGui::EndChild();
                return;
            }

            const auto ms_flags = ImGuiMultiSelectFlags_ClearOnEscape |
                                  ImGuiMultiSelectFlags_ClearOnClickVoid |
                                  ImGuiMultiSelectFlags_BoxSelect2d |
                                  ImGuiMultiSelectFlags_NavWrapX;
            state.m_selection.UserData = std::addressof(state.m_visible);
            state.m_selection.AdapterIndexToStorageId = assetBrowserSelectionIdFromViewIndex;
            ImGuiMultiSelectIO *ms_io = ImGui::BeginMultiSelect(
                ms_flags, state.m_selection.Size, static_cast<int>(item_count));
            state.m_selection.ApplyRequests(ms_io);

            // Selectables fill their gap by default and the clipper measures a
            // line through this spacing, so both take the hit-box-spacing value.
            ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing,
                ImVec2{state.m_layout.m_selectable_spacing, state.m_layout.m_selectable_spacing});

            const int column_count = state.m_layout.m_column_count;
            const float label_threshold = ImGui::CalcTextSize(kAssetTileLabelReference.data()).x;
            ImGuiListClipper clipper;
            clipper.Begin(state.m_layout.m_line_count, state.m_layout.m_item_step);
            if (ms_io->RangeSrcItem != -1) {
                clipper.IncludeItemByIndex(static_cast<int>(ms_io->RangeSrcItem) / column_count);
            }

            while (clipper.Step()) {
                for (int line_index = clipper.DisplayStart; line_index < clipper.DisplayEnd; ++line_index) {
                    const int first = line_index * column_count;
                    const int last = std::min((line_index + 1) * column_count, static_cast<int>(item_count));
                    for (int position = first; position < last; ++position) {
                        assetTile(
                            state, view, static_cast<std::size_t>(position), column_count, line_index, start_pos,
                            label_threshold, activation);
                    }
                }
            }

            clipper.End();
            ImGui::PopStyleVar();

            if (ImGui::BeginPopupContextWindow()) {
                ImGui::Text("Selection: %d assets", state.m_selection.Size);
                ImGui::Separator();
                if (ImGui::MenuItem("Clear selection")) {
                    state.m_selection.Clear();
                }

                ImGui::EndPopup();
            }

            ms_io = ImGui::EndMultiSelect();
            state.m_selection.ApplyRequests(ms_io);

            // Ctrl+wheel zoom keeps the tile under the cursor in place, so the
            // scale change does not move the user's target out from under it.
            const ImGuiIO &io = ImGui::GetIO();
            if (ImGui::IsWindowAppearing()) {
                state.m_zoom_wheel = 0.0f;
            }

            if (ImGui::IsWindowHovered() and
                io.MouseWheel != 0.0f and
                ImGui::IsKeyDown(ImGuiMod_Ctrl) and
                not ImGui::IsAnyItemActive()) {
                state.m_zoom_wheel += io.MouseWheel;
                const int steps = static_cast<int>(state.m_zoom_wheel);
                if (steps != 0) {
                    const AssetGridLayout previous_layout = state.m_layout;

                    state.m_icon_size = assetBrowserZoomedIconSize(state.m_icon_size, steps);
                    state.m_zoom_wheel -= static_cast<float>(steps);
                    state.m_layout = assetGridLayout(
                        avail_width, state.m_icon_size, state.m_icon_spacing, state.m_stretch_spacing,
                        static_cast<int>(item_count));

                    const float mouse_local_y = io.MousePos.y - ImGui::GetWindowPos().y;
                    ImGui::SetScrollY(assetBrowserZoomScrollY(
                        previous_layout, state.m_layout, io.MousePos.x - start_pos.x, io.MousePos.y - start_pos.y,
                        mouse_local_y, ImGui::GetStyle().WindowPadding.y));
                }
            }

            ImGui::EndChild();
        }

        void browserFooter(const AssetBrowserPanelState &state, const AssetBrowserPanelView &view) {
            ImGui::Text(
                "Selected: %llu/%llu assets", static_cast<unsigned long long>(state.m_selection.Size),
                static_cast<unsigned long long>(state.m_visible.size()));
            ImGui::TextDisabled(
                "| %llu of %llu in %llu packs", static_cast<unsigned long long>(state.m_visible.size()),
                static_cast<unsigned long long>(view.m_index->assets().size()),
                static_cast<unsigned long long>(view.m_index->packs().size()));
        }
    }

    // ------------------------------------------------------------------
    // panel
    // ------------------------------------------------------------------

    std::optional<asset::AssetKey> drawAssetBrowserPanel(
        AssetBrowserPanelState &state, const AssetBrowserPanelView &view) {
        std::optional<asset::AssetKey> activation{};
        if (state.m_icon_size <= 0.0f) {
            state.m_icon_size = ImGui::CalcTextSize(kAssetTileLabelReference.data()).x;
        }

        if (not view.m_index) {
            ImGui::TextWrapped("Open a pack to browse its assets.");
            return activation;
        }

        refreshOrder(state, view);
        browserMenuBar(state);
        if (state.m_show_sort_headers) {
            browserSortRow(state, *view.m_index);
        }

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##asset-search", "Filter kind, name, pack or path", state.m_search.data(), state.m_search.size());
        assetGrid(state, view, activation);
        browserFooter(state, view);
        return activation;
    }
}
