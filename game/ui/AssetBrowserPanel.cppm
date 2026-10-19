module;

export module game.colony.asset_browser_panel;

import engine.core;
import engine.asset;
import imgui;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // filtering and ordering
    // ------------------------------------------------------------------

    /// The sortable dimensions of an indexed asset, in header order.
    enum class EAssetSortField : u8 {
        local_name,
        kind,
        pack,
        descriptor
    };

    struct AssetSortSpec {
        EAssetSortField m_field{EAssetSortField::local_name};
        bool m_descending{};
    };

    /// Case-insensitive substring match used by the browser filter.
    [[nodiscard]] bool assetBrowserMatches(std::string_view text, std::string_view filter) noexcept;

    /// Assets that match the filter, in pack index order; entries are indices
    /// into `assets`. A filter matches the kind, the qualified name or the
    /// descriptor path.
    [[nodiscard]] Array<std::size_t> assetBrowserFiltered(
        std::span<const asset::IndexedAsset> assets, std::string_view filter);

    /// Orders `order` by `specs`, most significant first. Ties break on the
    /// asset's catalog position, so the result is a total order: the same
    /// index, filter and specs always produce the same grid.
    void assetBrowserSort(
        Array<std::size_t> &order, std::span<const asset::IndexedAsset> assets, std::span<const AssetSortSpec> specs);

    /// `assetBrowserFiltered` followed by `assetBrowserSort`.
    [[nodiscard]] Array<std::size_t> assetBrowserVisibleOrder(
        std::span<const asset::IndexedAsset> assets, std::string_view filter, std::span<const AssetSortSpec> specs);

    /// Empty-grid copy: an index without assets and a filter that keeps none
    /// of them call for different next steps.
    [[nodiscard]] std::string_view assetBrowserEmptyText(bool index_has_assets) noexcept;

    // ------------------------------------------------------------------
    // tile grid layout, zoom and markers
    // ------------------------------------------------------------------

    inline constexpr float kAssetIconSizeMin{16.0f};
    inline constexpr float kAssetIconSizeMax{128.0f};

    /// One frame's square-tile grid geometry, derived from the grid child's
    /// available width. Line count is what the clipper iterates per line, so
    /// the previous frame's value also sizes the child's scrollable content.
    struct AssetGridLayout {
        float m_item_size{};
        float m_item_step{};
        float m_item_spacing{};
        float m_selectable_spacing{};
        float m_outer_padding{};
        int m_column_count{};
        int m_line_count{};
    };

    /// Positions square icon tiles in `avail_width`. When `stretch`, the width
    /// left over by whole columns is spread into the gaps; hit spacing leaves
    /// part of every gap outside the tile hit boxes so box-select can start
    /// from a gap. Degenerate inputs clamp instead of dividing by zero.
    [[nodiscard]] AssetGridLayout assetGridLayout(
        float avail_width, float icon_size, int icon_spacing, bool stretch, int item_count) noexcept;

    /// Ctrl+wheel zoom: one 1.1 factor per wheel detent, clamped to the icon
    /// size policy so the grid stays usable.
    [[nodiscard]] float assetBrowserZoomedIconSize(float icon_size, int wheel_steps) noexcept;

    /// Reanchors the asset beneath the pointer after a grid zoom, using its
    /// row in the replacement layout rather than its previous row.
    [[nodiscard]] float assetBrowserZoomScrollY(
        const AssetGridLayout &before, const AssetGridLayout &after, float pointer_x, float pointer_y,
        float pointer_in_window_y, float padding_y) noexcept;

    /// Tile marker for an asset's kind family (the kind up to its first dot).
    /// Only families this browser knows are marked, mirroring the reference
    /// browser's unmarked default type.
    enum class EAssetKindMarker : u8 {
        none,
        simulation,
        image,
        colony
    };

    [[nodiscard]] EAssetKindMarker assetBrowserKindMarker(std::string_view kind) noexcept;

    // ------------------------------------------------------------------
    // selection identity
    // ------------------------------------------------------------------

    /// Selection storage identity of a catalog asset: its position offset so
    /// the first asset has a nonzero id (the selection storage reads id 0 as
    /// "not selected"). Identity survives filtering and sorting; a changed
    /// pack index invalidates it.
    [[nodiscard]] constexpr ImGuiID assetBrowserSelectionId(const std::size_t catalog_index) noexcept {
        return static_cast<ImGuiID>(catalog_index) + 1u;
    }

    /// `ImGuiSelectionBasicStorage::AdapterIndexToStorageId` for this grid:
    /// `self->UserData` is the browser's ordered view (`Array<std::size_t>`).
    [[nodiscard]] ImGuiID assetBrowserSelectionIdFromViewIndex(
        ImGuiSelectionBasicStorage *self, int view_index) noexcept;

    // ------------------------------------------------------------------
    // panel state
    // ------------------------------------------------------------------

    struct AssetBrowserPanelState {
        // catalog order cache:
        const asset::PackSet *m_order_index{};
        u64 m_order_revision{};
        std::string m_order_filter{};
        Array<AssetSortSpec> m_order_specs{};
        Array<std::size_t> m_visible{};

        // selection and stable inspector focus:
        ImGuiSelectionBasicStorage m_selection{};
        std::optional<asset::AssetKey> m_active_key{};

        // grid layout and options:
        AssetGridLayout m_layout{};
        float m_icon_size{};
        float m_zoom_wheel{};
        int m_icon_spacing{10};
        bool m_stretch_spacing{true};

        // filter:
        std::array<char, 129u> m_search{};

        // presentation toggles:
        bool m_show_type_overlay{true};
        bool m_show_sort_headers{true};
    };

    struct AssetBrowserPanelView {
        const asset::PackSet *m_index{};
        u64 m_index_revision{};
    };

    // ------------------------------------------------------------------
    // panel
    // ------------------------------------------------------------------

    /// Embedded navigation. An activated stable key is consumed after the old inspector commits.
    [[nodiscard]] std::optional<asset::AssetKey> drawAssetBrowserPanel(
        AssetBrowserPanelState &state, const AssetBrowserPanelView &view);
}
