module;
#include "pP/UnitTest.h"

module game.tests;

import engine.core;
import engine.asset;
import game.colony.asset_browser_panel;
import imgui;
import std;

namespace pP::game::tests::detail::asset_browser {
    // ------------------------------------------------------------------
    // fixtures
    // ------------------------------------------------------------------

    asset::IndexedAsset makeAsset(std::string pack, std::string local, std::string kind, std::string descriptor) {
        const asset::QualifiedName name{std::move(pack), std::move(local)};
        const auto key = asset::identityKey(name, kind);
        return {name, std::move(kind), std::filesystem::path{std::move(descriptor)}, key, 0u};
    }

    /// Catalog order every leaf relies on: three `ppr.base` assets then one
    /// `ppr.shared`; two `sim.element` assets tie on kind and pack.
    Array<asset::IndexedAsset> sampleAssets() {
        return {
            makeAsset("ppr.base", "solid", "sim.element", "elements/solid.toml"),
            makeAsset("ppr.base", "stone", "sim.element", "elements/stone.toml"),
            makeAsset("ppr.base", "hearth", "sim.biome", "biomes/hearth.toml"),
            makeAsset("ppr.shared", "granite", "sim.material", "materials/granite.toml"),
        };
    }

    [[nodiscard]] bool approximatelyEqual(const float lhs, const float rhs, const float epsilon = 1e-3f) noexcept {
        return std::abs(lhs - rhs) <= epsilon;
    }

    // ------------------------------------------------------------------
    // filter and order
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (filter_matching_is_case_insensitive_and_empty_filter_matches_all) {
        PPR_TEST_ASSERT(colony::assetBrowserMatches("ppr.base:stone", ""));
        PPR_TEST_ASSERT(colony::assetBrowserMatches("PPR.Base:Stone", "base:st"));
        PPR_TEST_ASSERT(colony::assetBrowserMatches("sim.element", "ELEMENT"));
        PPR_TEST_ASSERT(colony::assetBrowserMatches("assets/elements/stone.toml", "elements/STONE"));
        PPR_TEST_ASSERT(not colony::assetBrowserMatches("sim.element", "biome"));
        PPR_TEST_ASSERT(not colony::assetBrowserMatches("abc", "abcd"));
    };

    PPR_UNIT_TEST (filter_selects_by_kind_qualified_name_or_descriptor_path) {
        const auto assets = sampleAssets();

        PPR_TEST_ASSERT(colony::assetBrowserFiltered(assets, "sim.element") == Array<std::size_t>{0u, 1u});
        PPR_TEST_ASSERT(colony::assetBrowserFiltered(assets, "HEARTH") == Array<std::size_t>{2u});
        PPR_TEST_ASSERT(colony::assetBrowserFiltered(assets, "materials/granite.toml") == Array<std::size_t>{3u});

        PPR_TEST_ASSERT(colony::assetBrowserFiltered(assets, "no-such-asset").empty());
    };

    PPR_UNIT_TEST (filter_keeps_catalog_order_and_matches_pack_names) {
        const auto assets = sampleAssets();

        PPR_TEST_ASSERT(colony::assetBrowserFiltered(assets, "ppr.base") == Array<std::size_t>{0u, 1u, 2u});

        const std::span<const asset::IndexedAsset> no_index{};
        PPR_TEST_ASSERT(colony::assetBrowserFiltered(no_index, "").empty());
        PPR_TEST_ASSERT(colony::assetBrowserFiltered(no_index, "anything").empty());
    };

    PPR_UNIT_TEST (sort_by_local_name_ascending_and_descending) {
        const auto assets = sampleAssets();

        auto ascending = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(
            ascending, assets, std::array{colony::AssetSortSpec{colony::EAssetSortField::local_name}});
        PPR_TEST_ASSERT(ascending == Array<std::size_t>{3u, 2u, 0u, 1u});

        auto descending = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(
            descending, assets, std::array{colony::AssetSortSpec{colony::EAssetSortField::local_name, true}});
        PPR_TEST_ASSERT(descending == Array<std::size_t>{1u, 0u, 2u, 3u});
    };

    PPR_UNIT_TEST (sort_by_kind_groups_families_and_keeps_catalog_ties) {
        const auto assets = sampleAssets();

        auto ascending = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(ascending, assets, std::array{colony::AssetSortSpec{colony::EAssetSortField::kind}});

        // The two `sim.element` assets tie on kind and stay in catalog order.
        PPR_TEST_ASSERT(ascending == Array<std::size_t>{2u, 0u, 1u, 3u});

        auto descending = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(
            descending, assets, std::array{colony::AssetSortSpec{colony::EAssetSortField::kind, true}});
        PPR_TEST_ASSERT(descending == Array<std::size_t>{3u, 0u, 1u, 2u});
    };

    PPR_UNIT_TEST (multi_spec_sort_applies_specs_in_significance_order) {
        const auto assets = sampleAssets();

        auto order = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(
            order, assets,
            std::array{
                colony::AssetSortSpec{colony::EAssetSortField::kind},
                colony::AssetSortSpec{colony::EAssetSortField::local_name, true}});

        // The kind spec decides the groups; only the `sim.element` tie is
        // reached by the second spec, which orders those two by local name
        // descending.
        PPR_TEST_ASSERT(order == Array<std::size_t>{2u, 1u, 0u, 3u});
    };

    PPR_UNIT_TEST (sort_ties_fall_back_to_catalog_position_in_both_directions) {
        // Three assets tied on the sorted field: the total order is catalog
        // position, whichever way the spec points, so a re-sort cannot shuffle
        // equal tiles between frames.
        Array<asset::IndexedAsset> tied{
            makeAsset("ppr.b", "stone", "sim.element", "elements/stone.toml"),
            makeAsset("ppr.a", "stone", "sim.element", "elements/stone.toml"),
            makeAsset("ppr.c", "stone", "sim.element", "elements/stone.toml"),
        };

        auto ascending = colony::assetBrowserFiltered(tied, "");
        colony::assetBrowserSort(
            ascending, tied, std::array{colony::AssetSortSpec{colony::EAssetSortField::local_name}});
        PPR_TEST_ASSERT(ascending == Array<std::size_t>{0u, 1u, 2u});

        auto descending = colony::assetBrowserFiltered(tied, "");
        colony::assetBrowserSort(
            descending, tied, std::array{colony::AssetSortSpec{colony::EAssetSortField::local_name, true}});
        PPR_TEST_ASSERT(descending == Array<std::size_t>{0u, 1u, 2u});
    };

    PPR_UNIT_TEST (sort_without_specs_keeps_catalog_order_and_repeats) {
        const auto assets = sampleAssets();
        const std::array<colony::AssetSortSpec, 0> no_specs{};

        auto order = colony::assetBrowserFiltered(assets, "");
        colony::assetBrowserSort(order, assets, no_specs);
        PPR_TEST_ASSERT(order == Array<std::size_t>{0u, 1u, 2u, 3u});

        const auto before = order;
        colony::assetBrowserSort(order, assets, no_specs);
        PPR_TEST_ASSERT(order == before);
    };

    PPR_UNIT_TEST (visible_order_combines_filter_and_sort) {
        const auto assets = sampleAssets();

        const auto ordered = colony::assetBrowserVisibleOrder(
            assets, "ppr.base", std::array{colony::AssetSortSpec{colony::EAssetSortField::local_name, true}});

        // The filter drops `ppr.shared`, the sort reorders what is left.
        PPR_TEST_ASSERT(ordered == Array<std::size_t>{1u, 0u, 2u});
    };

    PPR_UNIT_TEST (empty_text_distinguishes_an_empty_index_from_an_empty_filter_result) {
        const std::string_view filtered_empty = colony::assetBrowserEmptyText(true);
        const std::string_view index_empty = colony::assetBrowserEmptyText(false);

        PPR_TEST_ASSERT(not filtered_empty.empty());
        PPR_TEST_ASSERT(not index_empty.empty());
        PPR_TEST_ASSERT(filtered_empty != index_empty);
    };

    // ------------------------------------------------------------------
    // tile grid layout, zoom and markers
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (grid_layout_stretches_spacing_into_whole_tiles) {
        const auto layout = colony::assetGridLayout(1000.0f, 64.0f, 10, true, 100);

        // 13 whole tiles of 64 leave 168 pixels, spread into the 13 gaps.
        PPR_TEST_ASSERT(layout.m_column_count == 13);
        PPR_TEST_ASSERT(layout.m_line_count == 8);
        PPR_TEST_ASSERT(approximatelyEqual(layout.m_item_size, 64.0f));
        PPR_TEST_ASSERT(approximatelyEqual(layout.m_item_spacing, 168.0f / 13.0f));
        PPR_TEST_ASSERT(approximatelyEqual(layout.m_item_step, 64.0f + 168.0f / 13.0f));

        // Hit spacing stays outside the selectable, giving box-select its gaps;
        // the outer padding is half a gap.
        PPR_TEST_ASSERT(approximatelyEqual(layout.m_selectable_spacing, 8.0f));
        PPR_TEST_ASSERT(approximatelyEqual(layout.m_outer_padding, 6.0f));
    };

    PPR_UNIT_TEST (grid_layout_clamps_degenerate_inputs) {
        // No width still fits one column instead of dividing by zero.
        const auto tight = colony::assetGridLayout(0.0f, 64.0f, 10, true, 5);
        PPR_TEST_ASSERT(tight.m_column_count == 1 and tight.m_line_count == 5);

        // Icon size and spacing floor at one pixel, keeping every value finite.
        const auto collapsed = colony::assetGridLayout(300.0f, 0.0f, 0, true, 8);
        PPR_TEST_ASSERT(collapsed.m_item_size == 1.0f and collapsed.m_item_step == 1.0f);
        PPR_TEST_ASSERT(collapsed.m_column_count == 300 and collapsed.m_line_count == 1);
        PPR_TEST_ASSERT(collapsed.m_item_spacing == 0.0f and collapsed.m_outer_padding == 0.0f);

        // Negative spacing clamps to zero, stretch off keeps the nominal gaps.
        const auto unspaced = colony::assetGridLayout(200.0f, 64.0f, -5, false, 4);
        PPR_TEST_ASSERT(unspaced.m_item_spacing == 0.0f and unspaced.m_column_count == 3);
        PPR_TEST_ASSERT(unspaced.m_line_count == 2 and unspaced.m_selectable_spacing == 0.0f);

        // Without stretch the column count may use half a gap of extra width.
        const auto unstretched = colony::assetGridLayout(100.0f, 64.0f, 10, false, 3);
        PPR_TEST_ASSERT(unstretched.m_column_count == 1 and unstretched.m_line_count == 3);
        PPR_TEST_ASSERT(approximatelyEqual(unstretched.m_item_spacing, 10.0f));
        PPR_TEST_ASSERT(approximatelyEqual(unstretched.m_outer_padding, 5.0f));
    };

    PPR_UNIT_TEST (zoom_steps_clamp_to_the_icon_size_policy) {
        PPR_TEST_ASSERT(approximatelyEqual(colony::assetBrowserZoomedIconSize(64.0f, 0), 64.0f));
        PPR_TEST_ASSERT(approximatelyEqual(colony::assetBrowserZoomedIconSize(64.0f, 1), 70.4f));

        PPR_TEST_ASSERT(colony::assetBrowserZoomedIconSize(16.0f, -1) == colony::kAssetIconSizeMin);
        PPR_TEST_ASSERT(colony::assetBrowserZoomedIconSize(128.0f, 1) == colony::kAssetIconSizeMax);
        PPR_TEST_ASSERT(colony::assetBrowserZoomedIconSize(100.0f, -1000) == colony::kAssetIconSizeMin);
        PPR_TEST_ASSERT(colony::assetBrowserZoomedIconSize(100.0f, 1000) == colony::kAssetIconSizeMax);

        // Zooming out undoes zooming in.
        PPR_TEST_ASSERT(approximatelyEqual(
            colony::assetBrowserZoomedIconSize(colony::assetBrowserZoomedIconSize(64.0f, 2), -2), 64.0f));
    };

    PPR_UNIT_TEST (zoom_anchors_the_same_asset_when_eight_columns_become_six) {
        const auto before = colony::assetGridLayout(640.0f, 64.0f, 16, true, 100);
        const auto after = colony::assetGridLayout(640.0f, 96.0f, 10, true, 100);
        PPR_TEST_ASSERT(before.m_column_count == 8 and after.m_column_count == 6);

        // Asset 30 was in row 3, column 6. It moves to row 5, column 0;
        // preserve the pointer's half-row offset in that replacement row.
        const float pointer_x = 6.25f * before.m_item_step - before.m_item_spacing * 0.5f;
        const float pointer_y = 3.5f * before.m_item_step - before.m_item_spacing * 0.5f;
        const float scroll = colony::assetBrowserZoomScrollY(before, after, pointer_x, pointer_y, 120.0f, 8.0f);

        PPR_TEST_ASSERT(approximatelyEqual(scroll + 120.0f - 8.0f, 5.5f * after.m_item_step));
    };

    PPR_UNIT_TEST (kind_markers_select_the_known_families) {
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("sim.element") == colony::EAssetKindMarker::simulation);
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("sim") == colony::EAssetKindMarker::simulation);
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("image.texture") == colony::EAssetKindMarker::image);
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("colony.background") == colony::EAssetKindMarker::colony);

        // Only the exact family matches; anything else stays unmarked.
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("simulation.element") == colony::EAssetKindMarker::none);
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("material") == colony::EAssetKindMarker::none);
        PPR_TEST_ASSERT(colony::assetBrowserKindMarker("") == colony::EAssetKindMarker::none);
    };

    // ------------------------------------------------------------------
    // selection identity
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (selection_ids_are_nonzero_catalog_positions) {
        // The storage reads id 0 as "not selected", so the first asset is 1.
        PPR_TEST_ASSERT(colony::assetBrowserSelectionId(0u) == 1u);
        PPR_TEST_ASSERT(colony::assetBrowserSelectionId(7u) == 8u);
    };

    PPR_UNIT_TEST (selection_identity_follows_the_asset_not_its_view_position) {
        Array<std::size_t> view{3u, 2u, 0u, 1u};
        ImGuiSelectionBasicStorage selection{};
        selection.UserData = std::addressof(view);
        selection.AdapterIndexToStorageId = colony::assetBrowserSelectionIdFromViewIndex;

        // Selecting view positions 1 through 2 selects catalog assets 2 and 0.
        for (int index = 1; index <= 2; ++index) {
            selection.SetItemSelected(colony::assetBrowserSelectionIdFromViewIndex(&selection, index), true);
        }

        PPR_TEST_ASSERT(selection.Size == 2);
        PPR_TEST_ASSERT(selection.Contains(colony::assetBrowserSelectionId(2u)));
        PPR_TEST_ASSERT(selection.Contains(colony::assetBrowserSelectionId(0u)));
        PPR_TEST_ASSERT(not selection.Contains(colony::assetBrowserSelectionId(1u)));

        // A re-sort moves the assets; the same ones stay selected.
        view = Array<std::size_t>{0u, 1u, 2u, 3u};
        PPR_TEST_ASSERT(
            colony::assetBrowserSelectionIdFromViewIndex(&selection, 2) == colony::assetBrowserSelectionId(2u));
        PPR_TEST_ASSERT(selection.Size == 2);
        PPR_TEST_ASSERT(selection.Contains(colony::assetBrowserSelectionId(2u)));
        PPR_TEST_ASSERT(selection.Contains(colony::assetBrowserSelectionId(0u)));
    };

    PPR_UNIT_TEST (selection_storage_counts_and_clears_items) {
        ImGuiSelectionBasicStorage selection{};
        PPR_TEST_ASSERT(selection.Size == 0);

        selection.SetItemSelected(1u, true);
        selection.SetItemSelected(2u, true);
        selection.SetItemSelected(1u, true); // A repeated select does not double-count.
        PPR_TEST_ASSERT(selection.Size == 2);
        PPR_TEST_ASSERT(selection.Contains(1u) and selection.Contains(2u));
        PPR_TEST_ASSERT(not selection.Contains(0u));

        selection.SetItemSelected(1u, false);
        PPR_TEST_ASSERT(selection.Size == 1 and not selection.Contains(1u));

        // An index change clears through this path.
        selection.Clear();
        PPR_TEST_ASSERT(selection.Size == 0 and not selection.Contains(2u));
    };


}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // asset browser suite
    // ------------------------------------------------------------------

    const UnitTest asset_browser_suite = UnitTest::Named("asset_browser") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::asset_browser::filter_matching_is_case_insensitive_and_empty_filter_matches_all,
            detail::asset_browser::filter_selects_by_kind_qualified_name_or_descriptor_path,
            detail::asset_browser::filter_keeps_catalog_order_and_matches_pack_names,
            detail::asset_browser::sort_by_local_name_ascending_and_descending,
            detail::asset_browser::sort_by_kind_groups_families_and_keeps_catalog_ties,
            detail::asset_browser::multi_spec_sort_applies_specs_in_significance_order,
            detail::asset_browser::sort_ties_fall_back_to_catalog_position_in_both_directions,
            detail::asset_browser::sort_without_specs_keeps_catalog_order_and_repeats,
            detail::asset_browser::visible_order_combines_filter_and_sort,
            detail::asset_browser::empty_text_distinguishes_an_empty_index_from_an_empty_filter_result,
            detail::asset_browser::grid_layout_stretches_spacing_into_whole_tiles,
            detail::asset_browser::grid_layout_clamps_degenerate_inputs,
            detail::asset_browser::zoom_steps_clamp_to_the_icon_size_policy,
            detail::asset_browser::zoom_anchors_the_same_asset_when_eight_columns_become_six,
            detail::asset_browser::kind_markers_select_the_known_families,
            detail::asset_browser::selection_ids_are_nonzero_catalog_positions,
            detail::asset_browser::selection_identity_follows_the_asset_not_its_view_position,
            detail::asset_browser::selection_storage_counts_and_clears_items,
        });
    };

    const UnitTest &gameAssetBrowserTests() noexcept {
        return asset_browser_suite;
    }
}
