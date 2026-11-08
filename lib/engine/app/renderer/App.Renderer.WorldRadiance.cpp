module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.world_radiance;
import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP::lighting {
    PPR_DEFINE_LOG_CATEGORY(WorldRadiance, info, none)

    // ------------------------------------------------------------------
    // bounded GPU protocol and checked resource planning
    // ------------------------------------------------------------------

    namespace {
        struct GpuMaterial {
            float m_source[3]{};
            u32 m_kind{3u};
            float m_sigma[3]{}, m_emission[3]{}, m_albedo[3]{};
        };
        using GpuOverlay = WorldRadianceOverlayCell;
        static_assert(sizeof(GpuOverlay) == 56u and alignof(GpuOverlay) == 4u);
        static_assert(std::is_standard_layout_v<GpuOverlay> and std::is_trivially_copyable_v<GpuOverlay>);
        static_assert(PPR_OFFSETOF(GpuOverlay, m_coverage) == 4u and PPR_OFFSETOF(GpuOverlay, m_source) == 8u);
        static_assert(PPR_OFFSETOF(GpuOverlay, m_albedo) == 20u and PPR_OFFSETOF(GpuOverlay, m_sigma) == 32u);
        static_assert(PPR_OFFSETOF(GpuOverlay, m_emission) == 44u);

        struct GpuOriginalMaterial {
            u32 m_id{};
            GpuMaterial m_value{};
        };

        struct GpuMatterRecord {
            u32 m_cell{}, m_material{};
            float m_fill{};
            u32 m_tag{};
        };

        struct GpuMatterFrame {
            u32 m_count{}, m_moving_count{}, m_width{}, m_height{};
            float m_cell_metres{}, m_origin{}, m_minimum_x{}, m_minimum_y{};
            u32 m_shift{}, m_blocks{}, m_lookup_capacity{}, m_ancestor_words{};
            u32 m_palette_count{}, m_terrain_palette_count{}, m_old_count{}, m_old_lookup_capacity{1u};
            u32 m_hierarchy_levels{}, m_phase{}, m_reserved[2]{};
        };

        static_assert(sizeof(GpuOriginalMaterial) == 56u and alignof(GpuOriginalMaterial) == 4u);
        static_assert(PPR_OFFSETOF(GpuOriginalMaterial, m_value) == 4u);
        static_assert(sizeof(GpuMatterRecord) == 16u and alignof(GpuMatterRecord) == 4u);
        static_assert(PPR_OFFSETOF(GpuMatterRecord, m_fill) == 8u and PPR_OFFSETOF(GpuMatterRecord, m_tag) == 12u);
        static_assert(sizeof(GpuMatterFrame) == 80u and alignof(GpuMatterFrame) == 4u);
        static_assert(PPR_OFFSETOF(GpuMatterFrame, m_shift) == 32u and PPR_OFFSETOF(GpuMatterFrame, m_phase) == 68u);

        struct GpuHierarchy {
            u32 m_offset{}, m_width{}, m_height{}, m_block_size{};
        };
        struct GpuLevel {
            i32 m_min_x{}, m_min_y{};
            u32 m_count_x{}, m_count_y{};
            u32 m_spacing{}, m_directions{}, m_direction_offset{}, m_records{};
            float m_near{}, m_far{};
            u32 m_reserved[2]{};
        };
        struct GpuDirection {
            float m_x{}, m_y{};
        };
        struct GpuParameters {
            u32 m_width{}, m_height{}, m_palette_count{}, m_hierarchy_levels{};
            float m_boundary[3]{}; // RGB for a uniform source, or solar axis XY and daylight intensity.
            u32 m_max_segments{};
            i32 m_refined_min_x{-1}, m_refined_min_y{-1};
            u32 m_refined_count_x{}, m_refined_count_y{};
            float m_max_source[3]{};
            float m_maximum_response{};
            float m_cell_metres{1.0f}, m_far_bound[3]{};
            u32 m_overlay_count{}, m_overlay_capacity{}, m_overlay_ancestor_words{}, m_sky_hour_plus_one{};
        };
        struct GpuDispatch {
            u32 m_level{}, m_parent_level{}, m_count{}, m_refined_spacing{};
            i32 m_min_x{}, m_min_y{};
            u32 m_count_x{}, m_count_y{};
        };
        struct GpuTemporalDispatch {
            u32 m_phase{}, m_first_record{}, m_phase_width{}, m_refined_spacing{};
            float m_history_weight{};
            u32 m_feedback_available{}, m_skip_table{}, m_region_offset{};
            u32 m_work_queue{}, m_work_group_size{};
            std::array<u32, 2> m_sample_time_ns{}, m_source_version{};
            u32 m_reserved[2]{}; // Complete four 16-byte Slang constant registers.
        };
        static_assert(sizeof(GpuTemporalDispatch) == 64u and alignof(GpuTemporalDispatch) == 4u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_history_weight) == 16u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_region_offset) == 28u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_work_queue) == 32u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_work_group_size) == 36u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_sample_time_ns) == 40u);
        static_assert(PPR_OFFSETOF(GpuTemporalDispatch, m_source_version) == 48u);
        static_assert(sizeof(WorldRadianceProbeState) == 24u and alignof(WorldRadianceProbeState) == 4u);
        static_assert(PPR_OFFSETOF(WorldRadianceProbeState, m_variation) == 16u);
        static_assert(PPR_OFFSETOF(WorldRadianceProbeState, m_samples) == 20u);

        struct TemporalWork {
            GpuDispatch m_dispatch{};
            GpuTemporalDispatch m_temporal{};
            u64 m_queries{};
            bool m_refresh{}, m_prioritize{};
        };

        struct GpuInterval {
            float m_source[3]{};
            u32 m_status{};
            float m_transmission[3]{};
            u32 m_segments{};
        };

        struct GpuBounceDispatch {
            u32 m_face_count{}, m_lookup_capacity{}, m_first_record{}, m_reserved{};
        };
        static_assert(sizeof(GpuBounceDispatch) == 16u and alignof(GpuBounceDispatch) == 4u);
        static_assert(PPR_OFFSETOF(GpuBounceDispatch, m_first_record) == 8u);

        struct GpuDiscoveryDispatch {
            u32 m_level{}, m_parent_level{}, m_first_record{}, m_count{};
            u32 m_refined_spacing{1u}, m_key_capacity{}, m_bitset_words{}, m_reserved{};
        };

        static_assert(
            sizeof(float) == 4u and std::numeric_limits<float>::is_iec559 and std::numeric_limits<float>::digits == 24);
        static_assert(sizeof(GpuMaterial) == 52u and alignof(GpuMaterial) == 4u);
        static_assert(PPR_OFFSETOF(GpuMaterial, m_kind) == 12u);
        static_assert(PPR_OFFSETOF(GpuMaterial, m_sigma) == 16u and PPR_OFFSETOF(GpuMaterial, m_emission) == 28u);
        static_assert(PPR_OFFSETOF(GpuMaterial, m_albedo) == 40u);
        static_assert(sizeof(GpuHierarchy) == 16u and sizeof(GpuDirection) == 8u);
        static_assert(sizeof(GpuLevel) == 48u and alignof(GpuLevel) == 4u);
        static_assert(PPR_OFFSETOF(GpuLevel, m_spacing) == 16u and PPR_OFFSETOF(GpuLevel, m_near) == 32u);
        static_assert(sizeof(GpuParameters) == 96u and sizeof(GpuDispatch) == 32u);
        static_assert(sizeof(GpuDiscoveryDispatch) == 32u and alignof(GpuDiscoveryDispatch) == 4u);
        static_assert(PPR_OFFSETOF(GpuDiscoveryDispatch, m_refined_spacing) == 16u);
        static_assert(PPR_OFFSETOF(GpuDiscoveryDispatch, m_bitset_words) == 24u);
        static_assert(PPR_OFFSETOF(GpuDispatch, m_refined_spacing) == 12u);
        static_assert(PPR_OFFSETOF(GpuParameters, m_refined_min_x) == 32u);
        static_assert(PPR_OFFSETOF(GpuParameters, m_max_source) == 48u);
        static_assert(PPR_OFFSETOF(GpuParameters, m_maximum_response) == 60u);
        static_assert(PPR_OFFSETOF(GpuParameters, m_cell_metres) == 64u);
        static_assert(
            PPR_OFFSETOF(GpuParameters, m_overlay_count) == 80u and
            PPR_OFFSETOF(GpuParameters, m_overlay_capacity) == 84u);
        static_assert(PPR_OFFSETOF(GpuDispatch, m_min_x) == 16u);
        static_assert(sizeof(GpuInterval) == 32u and PPR_OFFSETOF(GpuInterval, m_transmission) == 16u);
        static_assert(sizeof(WorldRadianceCoefficients) == 112u and alignof(WorldRadianceCoefficients) == 4u);
        static_assert(PPR_OFFSETOF(WorldRadianceCoefficients, m_status) == 108u);

        std::error_code require_(const bool valid, const std::errc error = std::errc::invalid_argument) noexcept {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        std::error_code add_(u64 &sum, const u64 value) noexcept {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(value <= std::numeric_limits<u64>::max() - sum, std::errc::value_too_large));
            sum += value;
            return {};
        }

        std::error_code chargeOwner_(
            std::vector<const void *> &owners, const void *const owner, const u64 bytes, const u64 resource, u64 &peak,
            u64 &largest) {
            if (not owner or std::ranges::find(owners, owner) != owners.end())
                return {};

            owners.push_back(owner);
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, add_(peak, bytes));
            largest = std::max(largest, resource);
            return {};
        }

        Expected<u64> multiply_(const u64 a, const u64 b) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(b == 0u or a <= std::numeric_limits<u64>::max() / b, std::errc::value_too_large));
            return a * b;
        }

        Expected<float> sourceFloat_(const double value) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(value) and
                                       value >= 0.0 and
                                       value <= static_cast<double>(std::numeric_limits<float>::max()),
                                   std::errc::argument_out_of_domain));
            const float result = static_cast<float>(value);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(result) and (value == 0.0 or std::isnormal(result)),
                                   std::errc::argument_out_of_domain));
            return result;
        }

        struct ReceiverPage {
            i32 m_min_x{}, m_min_y{};
            u32 m_count_x{}, m_count_y{};
            std::array<u32, 8> m_mask{};
            u64 m_queries{};
        };

        // A strip borrows its unchanged 32x32 publication-page direction mask.
        struct ReceiverStrip {
            u64 m_queries{};
            i32 m_min_x{}, m_min_y{};
            u32 m_count_x{}, m_count_y{}, m_page{};
        };

        struct EmitterBounds {
            u32 m_id{}, m_min_x{}, m_min_y{}, m_max_x{}, m_max_y{};
        };

        // One concrete same-device static input owner, shared only by sources
        // that actually retain THESE immutable allocations. No world-sized clone.
        struct StaticWorldInput {
            std::shared_ptr<const WorldRadianceSource> m_source;
            rhi::ComPtr<rhi::IBuffer> m_ids, m_materials, m_vacuum, m_hierarchy, m_particle_materials;
            rhi::IDevice *m_device{};
            u64 m_bytes{};
        };

        struct ProjectedWorldInput {
            std::shared_ptr<const StaticWorldInput> m_base;
            rhi::ComPtr<rhi::IBuffer> m_rows, m_lookup, m_ancestors;
            WorldRadianceMatterIdentity m_identity{};
            std::optional<WorldRadianceMatterIdentity> m_compared_identity;
            WorldRadianceMatterStatistics m_statistics{};
            FenceCompletionValue m_fence{kFenceValueNone};
            u64 m_bytes{}, m_work_quanta{};
            double m_freeze_cost_ms{}, m_sort_cost_ms{}, m_lookup_cost_ms{}, m_compare_cost_ms{};
            u32 m_count{}, m_capacity{}, m_ancestor_words{};
        };

        bool pageMayReach_(const ReceiverPage &page, const EmitterBounds &emitter, const GpuDirection direction) {
            // Necessary support tests only: false positives trace normally.
            // Native4096 integer/half bounds times binary32 direction components
            // fit exactly in these double two-term dot products. Anything outside
            // the finite supported calculation includes the direction.
            const double min_x = double(emitter.m_min_x) - (double(page.m_min_x) + page.m_count_x - 0.5);
            const double min_y = double(emitter.m_min_y) - (double(page.m_min_y) + page.m_count_y - 0.5);
            const double max_x = double(emitter.m_max_x) - (double(page.m_min_x) + 0.5);
            const double max_y = double(emitter.m_max_y) - (double(page.m_min_y) + 0.5);
            const double dx = double{direction.m_x}, dy = double{direction.m_y}, nx = -dy, ny = dx;
            const double perpendicular_min = nx * (nx >= 0.0 ? min_x : max_x) + ny * (ny >= 0.0 ? min_y : max_y);
            const double perpendicular_max = nx * (nx >= 0.0 ? max_x : min_x) + ny * (ny >= 0.0 ? max_y : min_y);
            const double forward_max = dx * (dx >= 0.0 ? max_x : min_x) + dy * (dy >= 0.0 ? max_y : min_y);
            if (not std::isfinite(perpendicular_min) or
                not std::isfinite(perpendicular_max) or
                not std::isfinite(forward_max))
                return true;

            return perpendicular_min <= 0.0 and perpendicular_max >= 0.0 and forward_max >= 0.0;
        }

        struct TemporalCost {
            std::array<double, 8u> m_recent{};
            u32 m_next{}, m_samples{};

            void observe(const double milliseconds) noexcept {
                m_recent[m_next] = milliseconds;
                m_next = (m_next + 1u) % static_cast<u32>(m_recent.size());
                m_samples = std::min(m_samples + 1u, static_cast<u32>(m_recent.size()));
            }

            [[nodiscard]] double maximum() const noexcept {
                return *std::max_element(m_recent.begin(), m_recent.end());
            }
        };

        struct Plan {
            GpuParameters m_parameters{};
            std::vector<GpuLevel> m_levels;
            std::vector<GpuDirection> m_directions;
            std::vector<ReceiverPage> m_pages;
            std::vector<ReceiverStrip> m_strips;
            std::vector<u32> m_masks;
            std::vector<TemporalWork> m_temporal_work;
            std::vector<u32> m_batch_begin;
            std::array<WorldRadianceReceiverWindow, 8u> m_temporal_regions{};
            u64 m_retained_rows{}, m_queue_bytes{sizeof(u32)};
            WorldRadianceReceiverWindow m_copy_window{}, m_coverage_window{}, m_coverage_probes{}, m_gameplay_probes{};
            u64 m_gameplay_bytes{};
            bool m_refresh_displaced{};
            WorldRadianceDemand m_demand{};
            u32 m_stage_query_stride{2u}, m_primary_work_end{};

            [[nodiscard]] u32 stageQueryOffset(const u32 index) const noexcept {
                return 2u * static_cast<u32>(m_temporal_work.size()) + m_stage_query_stride * index;
            }

            [[nodiscard]] u64 gameplayOffset() const noexcept {
                return m_temporal_work.size() * 8u * sizeof(u32);
            }

            [[nodiscard]] u64 selectedCountOffset(const u32 index) const noexcept {
                return m_demand.m_readback_bytes - (m_temporal_work.size() - index + 1u) * sizeof(u32);
            }

            [[nodiscard]] u32 setupQueryOffset() const noexcept {
                // A copy-only publication still owns a batch timestamp pair.
                return (2u + m_stage_query_stride) * std::max(1u, static_cast<u32>(m_temporal_work.size()));
            }

            [[nodiscard]] u32 temporalBatches() const noexcept {
                return m_batch_begin.empty() ? 0u : static_cast<u32>(m_batch_begin.size() - 1u);
            }
        };

        std::array<GpuLevel, 4u> indirectLevels_(
            const u32 width, const u32 height, const WorldRadianceReceiverWindow window) {
            std::array<GpuLevel, 4u> levels{};
            auto minimum = window.m_minimum;
            std::array<i32, 2u> maximum{
                minimum[0] + static_cast<i32>(window.m_counts[0]) - 1,
                minimum[1] + static_cast<i32>(window.m_counts[1]) - 1};
            u32 lower_spacing = 1u, direction_offset{};
            for (u32 index = 1u; index <= 3u; ++index) {
                auto &level = levels[index];
                level.m_spacing = 2u << index;
                level.m_directions = 4u << (2u * index);
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    const double low = (double(minimum[axis]) + .5) * lower_spacing / level.m_spacing - .5;
                    const double high = (double(maximum[axis]) + .5) * lower_spacing / level.m_spacing - .5;
                    minimum[axis] = static_cast<i32>(std::floor(low));
                    maximum[axis] = static_cast<i32>(std::ceil(high));
                }

                level.m_min_x = minimum[0];
                level.m_min_y = minimum[1];
                level.m_count_x = static_cast<u32>(maximum[0] - minimum[0] + 1);
                level.m_count_y = static_cast<u32>(maximum[1] - minimum[1] + 1);
                level.m_records = level.m_count_x * level.m_count_y * level.m_directions;
                level.m_direction_offset = direction_offset;
                level.m_near = float(u32{1u} << (2u * index));
                level.m_far = float(u32{1u} << (2u * (index + 1u)));

                direction_offset += level.m_directions;
                lower_spacing = level.m_spacing;
            }

            auto &top = levels[3u];
            double maximum_l1{};
            for (const i32 x: {top.m_min_x, top.m_min_x + static_cast<i32>(top.m_count_x) - 1}) {
                for (const i32 y: {top.m_min_y, top.m_min_y + static_cast<i32>(top.m_count_y) - 1}) {
                    const double px = (double(x) + .5) * top.m_spacing;
                    const double py = (double(y) + .5) * top.m_spacing;
                    for (const u32 wx: {0u, width}) {
                        for (const u32 wy: {0u, height}) {
                            maximum_l1 = std::max(maximum_l1, std::abs(px - wx) + std::abs(py - wy));
                        }
                    }
                }
            }

            top.m_far = static_cast<float>(std::max(double{top.m_far}, 2.0 * std::ceil(maximum_l1)));
            return levels;
        }

        Expected<WorldRadianceReceiverWindow> temporalProbeWindow_(
            const u32 width, const u32 height, const u32 spacing, const std::array<u32, 2> full_counts,
            const WorldRadianceReceiverWindow requested) {
            WorldRadianceReceiverWindow region{{-1, -1}, full_counts};
            const bool full = requested.m_counts == std::array<u32, 2>{};
            for (u32 axis = 0u; axis < 2u; ++axis) {
                const u32 extent = axis == 0u ? width : height;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(
                        full ? requested.m_minimum[axis] == 0
                             : requested.m_minimum[axis] >= 0 and
                                   requested.m_counts[axis] > 0u and
                                   u64{static_cast<u32>(requested.m_minimum[axis])} + requested.m_counts[axis] <=
                                       extent));
                if (full)
                    continue;

                const i32 low = static_cast<i32>(
                    std::floor(requested.m_minimum[axis] / static_cast<double>(spacing) - 0.5));
                const i32 high = static_cast<i32>(std::ceil(
                    (requested.m_minimum[axis] + static_cast<double>(requested.m_counts[axis])) / spacing - 0.5));
                region.m_minimum[axis] = low;
                region.m_counts[axis] = static_cast<u32>(high - low + 1);
            }
            return region;
        }

        // Keep both dependency closures sparse. At coarse levels they often
        // overlap; emit the refresh-only strips so each record is written once.
        std::array<WorldRadianceReceiverWindow, 5u> temporalWorkAreas_(
            const WorldRadianceReceiverWindow primary, const WorldRadianceReceiverWindow refresh) noexcept {
            std::array<WorldRadianceReceiverWindow, 5u> areas{primary};
            if (refresh.m_counts[0] == 0u or refresh.m_counts[1] == 0u)
                return areas;

            const auto &low = refresh.m_minimum;
            std::array<i32, 2u> high{}, begin{}, end{};
            for (u32 axis = 0u; axis < 2u; ++axis) {
                high[axis] = low[axis] + static_cast<i32>(refresh.m_counts[axis]);
                begin[axis] = std::max(low[axis], primary.m_minimum[axis]);
                end[axis] = std::min(high[axis], primary.m_minimum[axis] + static_cast<i32>(primary.m_counts[axis]));
                if (begin[axis] >= end[axis]) {
                    areas[1] = refresh;
                    return areas;
                }
            }

            const auto strip = [](i32 x, i32 y, i32 right, i32 bottom) -> WorldRadianceReceiverWindow {
                return {{x, y}, {static_cast<u32>(right - x), static_cast<u32>(bottom - y)}};
            };

            areas[1] = strip(low[0], low[1], high[0], begin[1]);
            areas[2] = strip(low[0], end[1], high[0], high[1]);
            areas[3] = strip(low[0], begin[1], begin[0], end[1]);
            areas[4] = strip(end[0], begin[1], high[0], end[1]);
            return areas;
        }

        Expected<Plan> temporalPlan_(
            const u32 width, const u32 height, const WorldRadianceSettings settings, const u32 groups_limit) {
            const auto profile = planWorldRadianceTemporalProfile(width, height, settings.m_temporal);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, profile);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    groups_limit > 0u and
                    (not settings.m_prioritize_convergence or (settings.m_temporal_gpu_queue and
                                                                  not settings.m_temporal_serial_reference and
                                                                  not settings.m_temporal_refresh_window)) and
                    settings.m_temporal_query_budget >= 256u and
                    settings.m_temporal_query_budget <= 16777216u and
                    std::isfinite(settings.m_temporal_frame_allowance_ms) and
                    settings.m_temporal_frame_allowance_ms >= 0.0f and
                    (settings.m_temporal_frame_allowance_ms == 0.0f or settings.m_measure_gpu) and
                    settings.m_phase == 0.5f and
                    std::isfinite(settings.m_temporal_history_weight) and
                    settings.m_temporal_history_weight >= 0.0f and
                    settings.m_temporal_history_weight < 1.0f));

            Plan result;
            if (settings.m_prioritize_convergence)
                result.m_stage_query_stride = 3u;

            auto &parameters = result.m_parameters;
            parameters.m_width = width;
            parameters.m_height = height;
            parameters.m_max_segments = width + height + 4u;
            parameters.m_refined_count_x = profile->m_refined_counts[0];
            parameters.m_refined_count_y = profile->m_refined_counts[1];
            result.m_levels.resize(settings.m_temporal.m_cascade_count);
            for (u32 index = 1u; index < result.m_levels.size(); ++index) {
                const auto &input = profile->m_levels[index];
                auto &level = result.m_levels[index];
                level.m_min_x = input.m_minimum[0];
                level.m_min_y = input.m_minimum[1];
                level.m_count_x = input.m_counts[0];
                level.m_count_y = input.m_counts[1];
                level.m_spacing = input.m_spacing;
                level.m_directions = input.m_directions;
                level.m_records = static_cast<u32>(input.m_records);
                level.m_near = input.m_near_cells;
                level.m_far = input.m_far_cells;
                level.m_direction_offset = static_cast<u32>(result.m_directions.size());

                for (u32 direction = 0u; direction < level.m_directions; ++direction) {
                    const double angle = 2.0 * std::numbers::pi * (double(direction) + 0.5) / level.m_directions;
                    result.m_directions.push_back(
                        {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))});
                }
            }

            const auto selected_region = temporalProbeWindow_(
                width, height, settings.m_temporal.m_refined_spacing, profile->m_refined_counts,
                settings.m_receiver_window);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, selected_region);
            auto &region = result.m_temporal_regions[0];
            region = *selected_region;

            result.m_coverage_window = settings.m_temporal_coverage_window;
            if (result.m_coverage_window.m_counts == std::array<u32, 2>{}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(result.m_coverage_window.m_minimum == std::array<i32, 2>{}));
                result.m_coverage_window.m_counts = {width, height};
            }
            const auto coverage = temporalProbeWindow_(
                width, height, settings.m_temporal.m_refined_spacing, profile->m_refined_counts,
                result.m_coverage_window);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, coverage);
            result.m_coverage_probes = *coverage;

            if (settings.m_gameplay_demand) {
                const auto &request = *settings.m_gameplay_demand;
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(request.m_sequence != 0u));
                for (const u32 count: request.m_window.m_counts) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, require_(count > 0u and count <= WorldRadianceGameplayQuery::kMaxWindowEdge));
                }

                const auto support = temporalProbeWindow_(
                    width, height, settings.m_temporal.m_refined_spacing, profile->m_refined_counts, request.m_window);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, support);
                result.m_gameplay_probes = *support;
                result.m_gameplay_bytes = u64{support->m_counts[0]} *
                                          support->m_counts[1] *
                                          sizeof(WorldRadianceCoefficients);
            }

            const auto expand_parents = [&](auto &regions) {
                for (u32 index = 1u; index < result.m_levels.size(); ++index) {
                    const auto &child = regions[index - 1u];
                    auto &parent = regions[index];
                    const double child_spacing = index == 1u ? settings.m_temporal.m_refined_spacing
                                                             : result.m_levels[index - 1u].m_spacing;
                    for (u32 axis = 0u; axis < 2u; ++axis) {
                        const double low = (child.m_minimum[axis] + 0.5) *
                                               child_spacing /
                                               result.m_levels[index].m_spacing -
                                           0.5;
                        const double high = (child.m_minimum[axis] + static_cast<double>(child.m_counts[axis]) - 0.5) *
                                                child_spacing /
                                                result.m_levels[index].m_spacing -
                                            0.5;
                        parent.m_minimum[axis] = static_cast<i32>(std::floor(low));
                        parent.m_counts[axis] = static_cast<u32>(std::ceil(high) - std::floor(low) + 1.0);
                    }
                }
            };

            expand_parents(result.m_temporal_regions);

            decltype(result.m_temporal_regions) refresh_regions{};
            const bool additional_gameplay = settings.m_gameplay_demand and
                                             not region.contains(result.m_gameplay_probes);
            result.m_refresh_displaced = additional_gameplay and settings.m_temporal_refresh_window.has_value();
            if (additional_gameplay) {
                refresh_regions[0] = result.m_gameplay_probes;
                expand_parents(refresh_regions);
            } else if (settings.m_temporal_refresh_window) {
                const auto &tile = *settings.m_temporal_refresh_window;
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(tile.m_counts[0] > 0u and tile.m_counts[1] > 0u));
                const auto probes = temporalProbeWindow_(
                    width, height, settings.m_temporal.m_refined_spacing, profile->m_refined_counts, tile);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, probes);
                refresh_regions[0] = *probes;
                expand_parents(refresh_regions);
            }

            result.m_retained_rows = u64{parameters.m_refined_count_x} * parameters.m_refined_count_y;

            // Complete every required parent parity before any finer level.
            // The spatial phase selects rows, not an indivisible frame workload.
            result.m_batch_begin.push_back(0u);
            const bool adaptive = settings.m_temporal_frame_allowance_ms > 0.0f;
            // Small measured work units allow later batches to regroup without changing shader roots.
            const u32 quantum_queries = adaptive ? std::min(settings.m_temporal_query_budget, 8192u)
                                                 : settings.m_temporal_query_budget;
            u64 batch_queries{};
            // Each closure is coarsest first. Complete the primary closure before
            // emitting supplemental strips so the unsubmitted suffix can yield.
            const u32 closures = additional_gameplay or settings.m_temporal_refresh_window ? 2u : 1u;
            for (u32 closure = 0u; closure < closures; ++closure) {
                for (u32 index = static_cast<u32>(result.m_levels.size()) - 1u;; --index) {
                    const auto areas = temporalWorkAreas_(result.m_temporal_regions[index], refresh_regions[index]);
                    const std::size_t area_end = closure == 0u ? 1u : areas.size();
                    for (std::size_t area = closure; area < area_end; ++area) {
                        const auto &selected = areas[area];
                        if (selected.m_counts[0] == 0u or selected.m_counts[1] == 0u)
                            continue;

                        if (index == 0u)
                            result.m_retained_rows -= u64{selected.m_counts[0]} * selected.m_counts[1];

                        // Indirect lists carry absolute record indices. Pack adjacent
                        // probes into a single row sequence; the direct control
                        // keeps the reference four-parity enumeration.
                        const bool queued = settings.m_temporal_gpu_queue;
                        const u32 phases = queued ? 1u : 4u;
                        for (u32 phase = 0u; phase < phases; ++phase) {
                            const bool refine = index == 0u;
                            const auto &level = result.m_levels[index];
                            const u32 count_x = selected.m_counts[0];
                            const u32 count_y = selected.m_counts[1];
                            const i32 min_x = refine ? parameters.m_refined_min_x : level.m_min_x;
                            const i32 min_y = refine ? parameters.m_refined_min_y : level.m_min_y;
                            const u32 full_width = refine ? parameters.m_refined_count_x : level.m_count_x;
                            const u32 region_offset = static_cast<u32>(selected.m_minimum[1] - min_y) * full_width +
                                                      static_cast<u32>(selected.m_minimum[0] - min_x);
                            const u32 phase_width = queued ? count_x : (count_x + 1u - (phase & 1u)) / 2u;
                            const u32 phase_height = queued ? count_y : (count_y + 1u - (phase >> 1u)) / 2u;
                            const u32 records = phase_width * phase_height * (refine ? 1u : level.m_directions);
                            const bool terminal = index + 1u == result.m_levels.size();
                            const u32 work_group_size = settings.m_temporal_serial_reference ? 64u
                                                        : refine                             ? 1u
                                                        : terminal                           ? 64u
                                                                                             : 16u;
                            const u32 queries_per_record = refine     ? 4u * result.m_levels[1].m_directions
                                                           : terminal ? 1u
                                                                      : 4u;
                            const u64 counter_limit = std::numeric_limits<u32>::max() /
                                                      (u64{parameters.m_max_segments} * queries_per_record);
                            const bool prioritize = refine and settings.m_prioritize_convergence and closure == 0u;
                            const u32 max_records = prioritize ? 256u : std::numeric_limits<u32>::max();
                            const u32 limit = static_cast<u32>(std::min(
                                {counter_limit, u64{max_records}, u64{quantum_queries / queries_per_record},
                                    u64{groups_limit} * work_group_size}));
                            PPR_RETURN_UNEXPECTED_ON_FAIL(
                                WorldRadiance, require_(limit > 0u, std::errc::value_too_large));

                            for (u32 first = 0u; first < records;) {
                                const u32 count = std::min(limit, records - first);
                                TemporalWork work;
                                work.m_refresh = area != 0u;
                                work.m_prioritize = prioritize;
                                work.m_dispatch = {
                                    index, terminal ? index : index + 1u, count, settings.m_temporal.m_refined_spacing};
                                work.m_temporal = {
                                    phase, first, phase_width, settings.m_temporal.m_refined_spacing,
                                    settings.m_temporal_history_weight};
                                work.m_temporal.m_region_offset = region_offset;
                                work.m_temporal.m_work_queue = settings.m_temporal_gpu_queue ? 1u : 0u;
                                work.m_temporal.m_work_group_size = work_group_size;
                                if (settings.m_temporal_gpu_queue)
                                    result.m_queue_bytes = std::max(
                                        result.m_queue_bytes, (u64{count} + 1u) * sizeof(u32));
                                work.m_queries = u64{count} * queries_per_record;
                                if (batch_queries + work.m_queries > settings.m_temporal_query_budget) {
                                    result.m_batch_begin.push_back(static_cast<u32>(result.m_temporal_work.size()));
                                    batch_queries = 0u;
                                }

                                batch_queries += work.m_queries;
                                result.m_temporal_work.push_back(work);
                                PPR_RETURN_UNEXPECTED_ON_FAIL(
                                    WorldRadiance, add_(result.m_demand.m_query_upper_bound, work.m_queries));
                                PPR_RETURN_UNEXPECTED_ON_FAIL(
                                    WorldRadiance, add_(result.m_demand.m_expected_rows, count));
                                first += count;
                            }
                        }
                    }

                    if (index == 0u)
                        break;
                }

                if (closure == 0u or additional_gameplay)
                    result.m_primary_work_end = static_cast<u32>(result.m_temporal_work.size());
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(result.m_demand.m_expected_rows, result.m_retained_rows));
            result.m_batch_begin.push_back(static_cast<u32>(result.m_temporal_work.size()));
            if (adaptive) {
                // Only the first calibration batch is chosen now. Subsequent boundaries
                // are appended after accepted completion, using per-class cost samples.
                result.m_batch_begin.reserve(result.m_temporal_work.size() + 1u);
                result.m_batch_begin.resize(2u);
                result.m_batch_begin[1] = 1u;
            }

            result.m_demand.m_refined_bytes = profile->m_refined_bytes;
            result.m_demand.m_probe_state_bytes = profile->m_probe_state_bytes;
            // Conservatively charge the complete persistent pool on admission.
            result.m_demand.m_parity_bytes = {
                profile->m_cascade_bytes + sizeof(GpuInterval),
                2u * profile->m_refined_bytes + 3u * profile->m_probe_state_bytes};
            result.m_demand.m_readback_bytes =
                result.m_temporal_work.size() * (settings.m_prioritize_convergence ? 9u : 8u) * sizeof(u32) +
                sizeof(u32) +
                result.m_gameplay_bytes +
                (settings.m_diagnostic_coefficients ? profile->m_refined_bytes + profile->m_probe_state_bytes : 0u);
            return result;
        }

        Expected<Plan> plan_(
            const u32 width, const u32 height, const WorldRadianceSettings settings, const u32 groups_limit) {
            if (settings.m_transport == WorldRadianceTransport::temporal_cascades)
                return temporalPlan_(width, height, settings, groups_limit);

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(not settings.m_gameplay_demand, std::errc::invalid_argument));
            const bool windowed = settings.m_transport == WorldRadianceTransport::direct_window;
            const bool straight = windowed or settings.m_transport == WorldRadianceTransport::straight256_reference;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   (straight or (settings.m_cascade_count >= 2u and settings.m_cascade_count <= 8u)) and
                                   std::isfinite(settings.m_phase) and
                                   settings.m_phase >= 0.0f and
                                   settings.m_phase < 1.0f and
                                   std::isfinite(settings.m_maximum_response) and
                                   settings.m_maximum_response > 0.0f and
                                   (straight or settings.m_maximum_response == (std::numeric_limits<float>::max)()) and
                                   groups_limit > 0u and
                                   (settings.m_refined_spacing == WorldRadianceRefinedSpacing::native_cell_centres or
                                       settings.m_refined_spacing == WorldRadianceRefinedSpacing::two_cell_centres) and
                                   (settings.m_transport == WorldRadianceTransport::cascade_diagnostic or
                                       settings.m_transport == WorldRadianceTransport::straight256_reference or
                                       windowed)));

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    not straight or (settings.m_refined_spacing == WorldRadianceRefinedSpacing::native_cell_centres and
                                        settings.m_phase == 0.5f)));

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   not windowed or (settings.m_receiver_rows_per_quantum >= 32u and
                                                       settings.m_receiver_rows_per_quantum <= 1024u and
                                                       settings.m_receiver_rows_per_quantum % 32u == 0u)));

            Plan result;
            auto &p = result.m_parameters;
            p.m_width = width;
            p.m_height = height;
            p.m_max_segments = width + height + 4u;
            const u32 refined_spacing = static_cast<u32>(settings.m_refined_spacing);

            // Reference rows are exactly the native cell centres. Cascade mode
            // retains its real padded floor/ceil support, unchanged.
            p.m_refined_min_x = straight ? 0 : -1;
            p.m_refined_min_y = straight ? 0 : -1;
            p.m_refined_count_x = straight ? width : width / refined_spacing + 2u;
            p.m_refined_count_y = straight ? height : height / refined_spacing + 2u;

            if (windowed) {
                const auto &window = settings.m_receiver_window;
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    const u32 extent = axis == 0u ? width : height;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        require_(
                            window.m_minimum[axis] >= 0 and
                            window.m_counts[axis] > 0u and
                            u64{static_cast<u32>(window.m_minimum[axis])} + window.m_counts[axis] <= extent and
                            window.m_minimum[axis] % 32 == 0 and
                            (window.m_counts[axis] % 32u == 0u or
                                u64{static_cast<u32>(window.m_minimum[axis])} + window.m_counts[axis] == extent)));
                }

                p.m_refined_min_x = window.m_minimum[0];
                p.m_refined_min_y = window.m_minimum[1];
                p.m_refined_count_x = window.m_counts[0];
                p.m_refined_count_y = window.m_counts[1];

                for (u32 y = 0u; y < window.m_counts[1]; y += 32u) {
                    for (u32 x = 0u; x < window.m_counts[0]; x += 32u) {
                        result.m_pages.push_back(
                            {window.m_minimum[0] + static_cast<i32>(x), window.m_minimum[1] + static_cast<i32>(y),
                                std::min(32u, window.m_counts[0] - x), std::min(32u, window.m_counts[1] - y)});
                    }
                }
            }

            u64 expected = u64{p.m_refined_count_x} * p.m_refined_count_y;
            u64 queries = expected * (straight ? 256u : 64u);

            if (straight) {
                result.m_levels.resize(1u); // One terminal descriptor, no coarse field.
                auto &terminal = result.m_levels.front();
                terminal.m_directions = 256u;
                terminal.m_spacing = 1u;
                // Every q is inside the native world. max(|d|)>=1/2 makes
                // 2*(width+height) a conservative actual-world escape bound.
                terminal.m_far = static_cast<float>(2u * (width + height));

                for (u32 direction = 0u; direction < 256u; ++direction) {
                    const double angle = 2.0 * std::numbers::pi * (static_cast<double>(direction) + 0.5) / 256.0;
                    const GpuDirection value{static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, require_(
                                           std::isfinite(value.m_x) and
                                               std::isfinite(value.m_y) and
                                               std::max(std::abs(value.m_x), std::abs(value.m_y)) >= 0.5f,
                                           std::errc::argument_out_of_domain));
                    result.m_directions.push_back(value);
                }
            } else {
                result.m_levels.resize(settings.m_cascade_count);
                std::array<i32, 2> minimum{-1, -1};
                std::array<i32, 2> maximum{
                    static_cast<i32>(p.m_refined_count_x) - 2, static_cast<i32>(p.m_refined_count_y) - 2};
                u32 lower_spacing = refined_spacing;

                for (u32 index = 1u; index < settings.m_cascade_count; ++index) {
                    GpuLevel &level = result.m_levels[index];
                    level.m_spacing = 2u << index;
                    level.m_directions = 4u << (2u * index);
                    for (u32 axis = 0u; axis < 2u; ++axis) {
                        // Include the complete support closure of every padded lower
                        // probe. Integer extrema are exact in this <=4096-cell domain.
                        const double low = (static_cast<double>(minimum[axis]) + 0.5) *
                                               lower_spacing /
                                               level.m_spacing -
                                           0.5;
                        const double high = (static_cast<double>(maximum[axis]) + 0.5) *
                                                lower_spacing /
                                                level.m_spacing -
                                            0.5;
                        minimum[axis] = static_cast<i32>(std::floor(low));
                        maximum[axis] = static_cast<i32>(std::ceil(high));
                    }

                    level.m_min_x = minimum[0];
                    level.m_min_y = minimum[1];
                    level.m_count_x = static_cast<u32>(maximum[0] - minimum[0] + 1);
                    level.m_count_y = static_cast<u32>(maximum[1] - minimum[1] + 1);
                    const auto records = multiply_(u64{level.m_count_x} * level.m_count_y, level.m_directions);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, records);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        require_(
                            *records <= std::numeric_limits<u32>::max() and (*records + 63u) / 64u <= groups_limit,
                            std::errc::value_too_large));

                    level.m_records = static_cast<u32>(*records);
                    level.m_direction_offset = static_cast<u32>(result.m_directions.size());
                    level.m_near = static_cast<float>(u32{1u} << (2u * index));
                    level.m_far = static_cast<float>(u32{1u} << (2u * (index + 1u)));
                    lower_spacing = level.m_spacing;

                    for (u32 direction = 0u; direction < level.m_directions; ++direction) {
                        const double angle = 2.0 *
                                             std::numbers::pi *
                                             (static_cast<double>(direction) + static_cast<double>(settings.m_phase)) /
                                             level.m_directions;
                        const GpuDirection value{
                            static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance, require_(
                                               std::isfinite(value.m_x) and
                                                   std::isfinite(value.m_y) and
                                                   std::max(std::abs(value.m_x), std::abs(value.m_y)) >= 0.5f,
                                               std::errc::argument_out_of_domain));
                        result.m_directions.push_back(value);
                    }

                    auto &parity = result.m_demand.m_parity_bytes[index % 2u];
                    parity = std::max(parity, *records * sizeof(GpuInterval));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(expected, *records));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, add_(queries, *records * (index + 1u == settings.m_cascade_count ? 1u : 4u)));
                }

                auto &top = result.m_levels.back();
                double maximum_l1 = 0.0;
                for (const i32 x: {top.m_min_x, top.m_min_x + static_cast<i32>(top.m_count_x) - 1}) {
                    for (const i32 y: {top.m_min_y, top.m_min_y + static_cast<i32>(top.m_count_y) - 1}) {
                        const double px = (static_cast<double>(x) + 0.5) * top.m_spacing;
                        const double py = (static_cast<double>(y) + 0.5) * top.m_spacing;
                        for (const u32 wx: {0u, width}) {
                            for (const u32 wy: {0u, height}) {
                                maximum_l1 = std::max(maximum_l1, std::abs(px - wx) + std::abs(py - wy));
                            }
                        }
                    }
                }
                // The largest uploaded direction component is >=1/2. Twice the
                // maximum probe/world-corner L1 distance therefore bounds every
                // positive world exit, including the complete padded top domain.
                const double terminal = std::max(static_cast<double>(top.m_far), 2.0 * std::ceil(maximum_l1));
                top.m_far = static_cast<float>(terminal);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       std::isfinite(top.m_far) and static_cast<double>(top.m_far) >= terminal,
                                       std::errc::value_too_large));
            }

            const u64 refined_count = u64{p.m_refined_count_x} * p.m_refined_count_y;
            const u64 page_rows = u64{std::min(32u, p.m_refined_count_x)} * std::min(32u, p.m_refined_count_y);
            const u64 dispatch_rows = windowed ? std::min<u64>(page_rows, settings.m_receiver_rows_per_quantum)
                                               : refined_count;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   (windowed ? dispatch_rows : (dispatch_rows + 63u) / 64u) <= groups_limit and
                                       expected <= std::numeric_limits<u32>::max() and
                                       (windowed or queries <= std::numeric_limits<u32>::max()),
                                   std::errc::value_too_large));
            const auto segment_bound = multiply_(
                windowed ? std::min<u64>(queries, dispatch_rows * 256u) : queries, p.m_max_segments);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, segment_bound);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(*segment_bound <= std::numeric_limits<u32>::max(), std::errc::value_too_large));
            result.m_demand.m_query_upper_bound = queries;
            result.m_demand.m_expected_rows = expected;
            result.m_demand.m_refined_bytes = refined_count * sizeof(WorldRadianceCoefficients);
            result.m_demand.m_readback_bytes = 8u * sizeof(u32) + (settings.m_diagnostic_coefficients
                                                                          ? result.m_demand.m_refined_bytes
                                                                          : 0u);
            return result;
        }

        rhi::BufferRange range_(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code buffer_(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial, const bool readback,
            rhi::ComPtr<rhi::IBuffer> &result, const bool indirect = false) {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = readback ? 0u : stride;
            description.memoryType = readback ? rhi::MemoryType::ReadBack : rhi::MemoryType::DeviceLocal;
            description.usage = readback ? rhi::BufferUsage::CopyDestination
                                         : static_cast<rhi::BufferUsage>(
                                               static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                                               static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                               static_cast<u32>(rhi::BufferUsage::CopySource) |
                                               static_cast<u32>(rhi::BufferUsage::CopyDestination) |
                                               (indirect ? static_cast<u32>(rhi::BufferUsage::IndirectArgument) : 0u));
            description.defaultState = readback ? rhi::ResourceState::CopyDestination
                                                : rhi::ResourceState::UnorderedAccess;
            description.label = "WorldRadiancePass bounded native-cell candidate";
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, device.createBuffer(description, initial, result.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   result != nullptr and
                                   result->getDesc().size == bytes and
                                   result->getDesc().elementSize == description.elementSize));
            return {};
        }
    }

    // ------------------------------------------------------------------
    // immutable selected-ID projection and vacuum hierarchy capture
    // ------------------------------------------------------------------

    struct WorldRadianceSource::Data {
        std::vector<u16> m_ids;
        std::vector<u32> m_packed_ids, m_vacuum;
        std::vector<GpuMaterial> m_materials;
        std::vector<GpuOriginalMaterial> m_particle_materials;
        std::vector<u32> m_particle_ids;
        std::vector<GpuOverlay> m_overlay;
        std::vector<u32> m_overlay_lookup;
        std::vector<GpuHierarchy> m_hierarchy;
        std::vector<EmitterBounds> m_emitters;
        std::shared_ptr<const WorldRadianceSource> m_terrain;
        std::shared_ptr<const ProjectedWorldInput> m_projected;
        GridWorldIdentity m_world_identity{};
        std::array<double, 2> m_minimum{};
        std::array<double, 3> m_far_bound{}, m_underflow_loss{}, m_transport_max{}, m_zero_emission{};
        double m_cell_metres{};
        u64 m_cpu_bytes{}, m_surface_cells{}, m_partial_cells{}, m_terrain_revision{};
        std::array<float, 3> m_sky{};
        u32 m_width{}, m_height{};

        [[nodiscard]] const Data &terrain() const noexcept {
            return m_terrain ? *m_terrain->m_data : *this;
        }

        std::error_code charge(
            std::vector<const void *> &owners, const WorldRadianceSource *const source, u64 &peak, u64 &largest) const {
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, chargeOwner_(owners, source, m_cpu_bytes, 0u, peak, largest));
            if (m_terrain)
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    chargeOwner_(owners, m_terrain.get(), m_terrain->m_data->m_cpu_bytes, 0u, peak, largest));

            if (m_projected) {
                const auto &projection = *m_projected;
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, chargeOwner_(
                                       owners, m_projected.get(), projection.m_bytes,
                                       std::max(
                                           {projection.m_rows->getDesc().size, projection.m_lookup->getDesc().size,
                                               projection.m_ancestors->getDesc().size}),
                                       peak, largest));
                const auto &base = *projection.m_base;
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, chargeOwner_(
                                       owners, projection.m_base.get(), base.m_bytes + sizeof(StaticWorldInput),
                                       std::max(
                                           {base.m_ids->getDesc().size, base.m_materials->getDesc().size,
                                               base.m_vacuum->getDesc().size, base.m_hierarchy->getDesc().size,
                                               base.m_particle_materials->getDesc().size}),
                                       peak, largest));
            }
            return {};
        }

        void markInputs(std::vector<const void *> &owners, const WorldRadianceSource *const source) const {
            owners.push_back(source);
            if (m_terrain)
                owners.push_back(m_terrain.get());
            if (m_projected) {
                owners.push_back(m_projected.get());
                owners.push_back(m_projected->m_base.get());
            }
        }

        [[nodiscard]] const GpuMaterial *material(const u32 id) const noexcept {
            const auto &base = terrain();
            if (id < base.m_materials.size() and base.m_materials[id].m_kind != 3u)
                return &base.m_materials[id];

            const auto found = std::ranges::lower_bound(base.m_particle_materials, id, {}, &GpuOriginalMaterial::m_id);
            return found != base.m_particle_materials.end() and found->m_id == id ? &found->m_value : nullptr;
        }
    };

    WorldRadianceSource::WorldRadianceSource(std::unique_ptr<const Data> data) : m_data{std::move(data)} {
    }

    Expected<WorldRadianceEmissionAggregate> admitWorldRadianceEmission(
        const u64 cell_count, const std::array<double, 3> per_cell_maximum) {
        for (const double maximum: per_cell_maximum) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(std::isfinite(maximum) and maximum >= 0.0, std::errc::argument_out_of_domain));
        }

        const double cells = static_cast<double>(cell_count);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(cell_count <= (u64{1} << 53u), std::errc::result_out_of_range));

        WorldRadianceEmissionAggregate aggregate{};
        aggregate.m_cells = cell_count;
        aggregate.m_maximum = per_cell_maximum;
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            const double bound = cells * per_cell_maximum[channel];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(bound) and
                                       bound >= 0.0 and
                                       bound <= static_cast<double>(std::numeric_limits<float>::max()),
                                   std::errc::result_out_of_range));
            aggregate.m_bound[channel] = bound;
        }

        return aggregate;
    }

    Expected<std::array<double, 3>> admitWorldRadianceExposure(
        const WorldRadianceEmissionAggregate &aggregate, const double exposure_multiplier) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                std::isfinite(exposure_multiplier) and exposure_multiplier > 0.0, std::errc::argument_out_of_domain));

        std::array<double, 3> exposed{};
        for (std::size_t channel = 0u; channel < 3u; ++channel) {
            const double bound = aggregate.m_bound[channel];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(bound) and
                                       bound >= 0.0 and
                                       bound <= static_cast<double>(std::numeric_limits<float>::max()),
                                   std::errc::argument_out_of_domain));

            const double value = bound * exposure_multiplier;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(value) and
                                       value >= 0.0 and
                                       value <= static_cast<double>(std::numeric_limits<float>::max()),
                                   std::errc::result_out_of_range));

            // Zero keeps zero; a nonzero bound that loses all FP32 magnitude
            // (subnormal or zero after narrowing) is rejected rather than clamped.
            const float narrowed = static_cast<float>(value);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(narrowed) and (bound == 0.0 or std::isnormal(narrowed)),
                                   std::errc::result_out_of_range));

            exposed[channel] = value;
        }

        return exposed;
    }

    // ------------------------------------------------------------------
    // allocation-free temporal cascade profile
    // ------------------------------------------------------------------

    Expected<WorldRadianceTemporalProfile> planWorldRadianceTemporalProfile(
        const u32 width, const u32 height, const WorldRadianceTemporalSettings settings) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               width > 0u and
                               width <= 4096u and
                               height > 0u and
                               height <= 4096u and
                               std::has_single_bit(settings.m_refined_spacing) and
                               settings.m_refined_spacing >= 2u and
                               settings.m_refined_spacing <= 64u and
                               settings.m_cascade_count >= 2u and
                               settings.m_cascade_count <= 8u and
                               std::has_single_bit(settings.m_base_directions) and
                               settings.m_base_directions >= 4u and
                               settings.m_base_directions <= 16u and
                               std::isfinite(settings.m_interval_scale) and
                               settings.m_interval_scale > 0.0f and
                               settings.m_interval_scale <= 16.0f and
                               settings.m_sky_hour >= -1 and
                               settings.m_sky_hour < 24));

        WorldRadianceTemporalProfile result;
        const u32 spacing = settings.m_refined_spacing;
        result.m_refined_counts = {(width + spacing - 1u) / spacing + 2u, (height + spacing - 1u) / spacing + 2u};
        result.m_refined_bytes = u64{result.m_refined_counts[0]} *
                                 result.m_refined_counts[1] *
                                 sizeof(WorldRadianceCoefficients);
        result.m_probe_state_bytes = u64{result.m_refined_counts[0]} *
                                     result.m_refined_counts[1] *
                                     sizeof(WorldRadianceProbeState);
        result.m_field_pool_bytes = 3u * (result.m_refined_bytes + result.m_probe_state_bytes);

        std::array<i32, 2> minimum{-1, -1};
        std::array<i32, 2> maximum{
            static_cast<i32>(result.m_refined_counts[0]) - 2, static_cast<i32>(result.m_refined_counts[1]) - 2};
        u32 lower_spacing = spacing;
        for (u32 index = 1u; index < settings.m_cascade_count; ++index) {
            auto &level = result.m_levels[index];
            level.m_spacing = spacing << index;
            level.m_directions = settings.m_base_directions << (2u * index);
            for (u32 axis = 0u; axis < 2u; ++axis) {
                const double low = (double(minimum[axis]) + 0.5) * lower_spacing / level.m_spacing - 0.5;
                const double high = (double(maximum[axis]) + 0.5) * lower_spacing / level.m_spacing - 0.5;
                minimum[axis] = static_cast<i32>(std::floor(low));
                maximum[axis] = static_cast<i32>(std::ceil(high));
                level.m_minimum[axis] = minimum[axis];
                level.m_counts[axis] = static_cast<u32>(maximum[axis] - minimum[axis] + 1);
            }

            level.m_records = u64{level.m_counts[0]} * level.m_counts[1] * level.m_directions;
            level.m_first_record = result.m_cascade_records;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(result.m_cascade_records, level.m_records));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(result.m_cascade_records <= std::numeric_limits<u32>::max(), std::errc::value_too_large));

            level.m_near_cells = float(spacing * (1u << (2u * index))) * settings.m_interval_scale;
            level.m_far_cells = float(spacing * (1u << (2u * (index + 1u)))) * settings.m_interval_scale;
            lower_spacing = level.m_spacing;
        }

        auto &terminal = result.m_levels[settings.m_cascade_count - 1u];
        double maximum_l1{};
        for (const i32 x: {terminal.m_minimum[0], terminal.m_minimum[0] + static_cast<i32>(terminal.m_counts[0]) - 1}) {
            for (const i32 y:
                {terminal.m_minimum[1], terminal.m_minimum[1] + static_cast<i32>(terminal.m_counts[1]) - 1}) {
                const double px = (double(x) + 0.5) * terminal.m_spacing;
                const double py = (double(y) + 0.5) * terminal.m_spacing;
                for (const u32 wx: {0u, width}) {
                    for (const u32 wy: {0u, height})
                        maximum_l1 = std::max(maximum_l1, std::abs(px - wx) + std::abs(py - wy));
                }
            }
        }

        terminal.m_far_cells = std::max(terminal.m_far_cells, static_cast<float>(2.0 * std::ceil(maximum_l1)));
        result.m_cascade_bytes = result.m_cascade_records * sizeof(GpuInterval);
        return result;
    }

    // ------------------------------------------------------------------
    // camera demand rectangles for retained diagnostic transport
    // ------------------------------------------------------------------

    bool WorldRadianceReceiverWindow::contains(const WorldRadianceReceiverWindow other) const noexcept {
        if (other.m_counts[0] == 0u or other.m_counts[1] == 0u)
            return true;

        for (u32 axis = 0u; axis < 2u; ++axis) {
            if (other.m_minimum[axis] < m_minimum[axis] or
                i64{other.m_minimum[axis]} + other.m_counts[axis] > i64{m_minimum[axis]} + m_counts[axis])
                return false;
        }
        return true;
    }

    Expected<WorldRadianceReceiverWindow> planWorldRadianceReceiverWindow(
        const WorldRadianceSource &source, const CameraSnapshot &camera,
        const std::span<const WorldRadianceReceiverWindow> additional) {
        const auto &inverse = camera.m_invert_view_projection;
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(std::isfinite(inverse[row][column])));
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               camera.m_camera_mode == ECameraProjection::orthographic and
                                   inverse[0][3] == 0.0f and
                                   inverse[1][3] == 0.0f and
                                   inverse[2][3] == 0.0f and
                                   inverse[3][3] == 1.0f and
                                   inverse[2][0] == 0.0f and
                                   inverse[2][1] == 0.0f and
                                   inverse[0][2] == 0.0f and
                                   inverse[1][2] == 0.0f and
                                   inverse[2][2] > 0.0f and
                                   inverse[3][2] <= 0.0f and
                                   double(inverse[3][2]) + double(inverse[2][2]) >= 0.0,
                               std::errc::not_supported));

        const auto origin = source.minimumMetres();
        const std::array<u32, 2> extent{source.width(), source.height()};
        std::array<double, 2> minimum{}, maximum{};
        for (u32 axis = 0u; axis < 2u; ++axis) {
            const double centre = (double(inverse[3][axis]) - origin[axis]) / source.cellMetres();
            const double radius = (std::abs(double(inverse[0][axis])) + std::abs(double(inverse[1][axis]))) /
                                  source.cellMetres();
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(std::isfinite(centre) and std::isfinite(radius)));

            minimum[axis] = std::clamp(std::floor(centre - radius) - 2.0, 0.0, double(extent[axis]));
            maximum[axis] = std::clamp(std::ceil(centre + radius) + 2.0, 0.0, double(extent[axis]));
        }

        const bool visible = maximum[0] > minimum[0] and maximum[1] > minimum[1];
        if (not visible) {
            // Empty visible intersection is explicit no-demand, not a fatal
            // camera error. Independent gameplay demands still establish work.
            minimum = {double(extent[0]), double(extent[1])};
            maximum = {};
        }

        for (const auto &demand: additional) {
            for (u32 axis = 0u; axis < 2u; ++axis) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(
                        demand.m_minimum[axis] >= 0 and
                        demand.m_counts[axis] > 0u and
                        u64{static_cast<u32>(demand.m_minimum[axis])} + demand.m_counts[axis] <= extent[axis]));

                minimum[axis] = std::min(minimum[axis], std::max(0.0, double(demand.m_minimum[axis]) - 2.0));
                maximum[axis] = std::max(
                    maximum[axis],
                    std::min(double(extent[axis]), double(demand.m_minimum[axis]) + demand.m_counts[axis] + 2.0));
            }
        }

        if (not visible and additional.empty())
            return WorldRadianceReceiverWindow{};

        WorldRadianceReceiverWindow window{};
        for (u32 axis = 0u; axis < 2u; ++axis) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(maximum[axis] > minimum[axis], std::errc::result_out_of_range));

            window.m_minimum[axis] = static_cast<i32>(minimum[axis] / 32.0) * 32;
            const u32 end = std::min(extent[axis], static_cast<u32>(std::ceil(maximum[axis] / 32.0)) * 32u);
            window.m_counts[axis] = end - static_cast<u32>(window.m_minimum[axis]);
        }

        return window;
    }

    WorldRadianceSource::~WorldRadianceSource() = default;

    Expected<std::shared_ptr<const WorldRadianceSource>> WorldRadianceSource::capture(const WorldRadianceInput input) {
        const hal::ProfileScope scope{"WorldRadiance.CaptureNativeInput"};
        const u64 cells = u64{input.m_width} * input.m_height;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               input.m_width != 0u and
                               input.m_height != 0u and
                               input.m_width <= kMaxNativeWorldEdge and
                               input.m_height <= kMaxNativeWorldEdge and
                               input.m_material_ids.size() == cells and
                               input.m_exterior == OpticalBoundary::vacuum and
                               std::isfinite(input.m_cell_metres) and
                               input.m_cell_metres > 0.0 and
                               std::isfinite(input.m_min_m[0]) and
                               std::isfinite(input.m_min_m[1])));

        for (u32 axis = 0u; axis < 2u; ++axis) {
            const double size = axis == 0u ? input.m_width : input.m_height;
            const double end = input.m_min_m[axis] + size * input.m_cell_metres;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(std::isfinite(end) and end > input.m_min_m[axis], std::errc::argument_out_of_domain));

            double previous = input.m_min_m[axis];
            for (u32 cell = 1u; cell <= static_cast<u32>(size); ++cell) {
                const double plane = input.m_min_m[axis] + cell * input.m_cell_metres;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(std::isfinite(plane) and plane > previous, std::errc::argument_out_of_domain));
                previous = plane;
            }
        }

        auto data = std::make_unique<Data>();
        data->m_ids.assign(input.m_material_ids.begin(), input.m_material_ids.end());
        data->m_width = input.m_width;
        data->m_height = input.m_height;
        data->m_cell_metres = input.m_cell_metres;
        data->m_minimum = input.m_min_m;
        data->m_world_identity = input.m_world_identity;
        data->m_terrain_revision = input.m_terrain_revision;

        u32 maximum_id = 0u;
        for (const u16 id: data->m_ids)
            maximum_id = std::max(maximum_id, u32{id});

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(maximum_id < 65535u));

        data->m_materials.resize(maximum_id + 1u);
        std::vector<bool> used(maximum_id + 1u), present(maximum_id + 1u);
        used[0u] = true;
        for (const u16 id: data->m_ids)
            used[id] = true;

        for (const auto &row: input.m_materials) {
            if (row.m_material_id > maximum_id)
                continue;

            const u32 id = row.m_material_id;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(not present[id]));

            present[id] = true;
            if (not used[id])
                continue;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   row.m_optics.m_kind == OpticalKind::vacuum or
                                       row.m_optics.m_kind == OpticalKind::opaque or
                                       row.m_optics.m_kind == OpticalKind::absorbing,
                                   std::errc::not_supported));
            const OpticalGridView validation{
                .m_cells = std::span{&row.m_optics, 1u},
                .m_cell_size_m = 1.0,
                .m_width = 1u,
                .m_height = 1u,
                .m_boundary = OpticalBoundary::vacuum};
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, validateOpticalGrid(validation));

            auto &destination = data->m_materials[id];
            destination.m_kind = row.m_optics.m_kind == OpticalKind::vacuum   ? 0u
                                 : row.m_optics.m_kind == OpticalKind::opaque ? 1u
                                                                              : 2u;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(id != 0u or destination.m_kind == 0u));
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const auto source = sourceFloat_(row.m_optics.m_surface_emission[channel]);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, source);
                destination.m_source[channel] = *source;

                const auto sigma = sourceFloat_(row.m_optics.m_extinction[channel]);
                const auto emission = sourceFloat_(row.m_optics.m_volume_emission[channel]);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sigma);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, emission);
                destination.m_sigma[channel] = *sigma;
                destination.m_emission[channel] = *emission;
                const float albedo = row.m_diffuse_albedo[channel];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       std::isfinite(albedo) and albedo >= 0.0f and albedo <= 1.0f,
                                       std::errc::argument_out_of_domain));
                destination.m_albedo[channel] = albedo;
            }
        }

        for (u32 id = 0u; id <= maximum_id; ++id) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(not used[id] or present[id], std::errc::resource_unavailable_try_again));
        }

        std::vector<WorldRadianceMaterial> particle_materials(
            input.m_particle_materials.begin(), input.m_particle_materials.end());
        std::ranges::sort(particle_materials, {}, &WorldRadianceMaterial::m_material_id);
        for (const auto &row: particle_materials) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   row.m_material_id > 0u and
                                   row.m_material_id < 65535u and
                                   (data->m_particle_materials.empty() or
                                       data->m_particle_materials.back().m_id < row.m_material_id)));

            GpuOriginalMaterial projected{};
            projected.m_id = row.m_material_id;
            if (row.m_optics.m_kind != OpticalKind::unavailable) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       row.m_optics.m_kind == OpticalKind::vacuum or
                                           row.m_optics.m_kind == OpticalKind::opaque or
                                           row.m_optics.m_kind == OpticalKind::absorbing,
                                       std::errc::not_supported));
                const OpticalGridView validation{
                    .m_cells = std::span{&row.m_optics, 1u},
                    .m_cell_size_m = 1.0,
                    .m_width = 1u,
                    .m_height = 1u,
                    .m_boundary = OpticalBoundary::vacuum};
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, validateOpticalGrid(validation));

                auto &value = projected.m_value;
                value.m_kind = row.m_optics.m_kind == OpticalKind::vacuum   ? 0u
                               : row.m_optics.m_kind == OpticalKind::opaque ? 1u
                                                                            : 2u;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const auto surface = sourceFloat_(row.m_optics.m_surface_emission[channel]);
                    const auto sigma = sourceFloat_(row.m_optics.m_extinction[channel]);
                    const auto emission = sourceFloat_(row.m_optics.m_volume_emission[channel]);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, surface);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sigma);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, emission);
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, require_(
                                           std::isfinite(row.m_diffuse_albedo[channel]) and
                                           row.m_diffuse_albedo[channel] >= 0.0f and
                                           row.m_diffuse_albedo[channel] <= 1.0f));
                    value.m_source[channel] = *surface;
                    value.m_sigma[channel] = *sigma;
                    value.m_emission[channel] = *emission;
                    value.m_albedo[channel] = row.m_diffuse_albedo[channel];
                }
            }

            if (projected.m_id < data->m_materials.size() and data->m_materials[projected.m_id].m_kind != 3u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(
                        std::memcmp(&projected.m_value, &data->m_materials[projected.m_id], sizeof(GpuMaterial)) == 0));
            }

            data->m_particle_ids.push_back(projected.m_id);
            data->m_particle_materials.push_back(projected);
        }

        for (u32 channel = 0u; channel < 3u; ++channel) {
            const auto sky = sourceFloat_(input.m_sky[channel]);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sky);
            data->m_sky[channel] = *sky;
        }

        // Validate and own the sparse physical projection before building any
        // hierarchy, emitter summary or source-domain bound from this source.
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(input.m_overlay.size() <= cells));
        data->m_overlay.assign(input.m_overlay.begin(), input.m_overlay.end());
        data->m_overlay_lookup.assign(1u, 0u);
        while (data->m_overlay_lookup.size() < 2u * u64{data->m_overlay.size()}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    data->m_overlay_lookup.size() <= std::numeric_limits<u32>::max() / 2u, std::errc::value_too_large));
            data->m_overlay_lookup.resize(2u * data->m_overlay_lookup.size());
        }

        for (u32 ordinal = 0u; ordinal < data->m_overlay.size(); ++ordinal) {
            const auto &row = data->m_overlay[ordinal];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   row.m_cell < cells and
                                   (ordinal == 0u or data->m_overlay[ordinal - 1u].m_cell < row.m_cell) and
                                   std::isfinite(row.m_coverage) and
                                   row.m_coverage >= 0.0f and
                                   row.m_coverage <= 1.0f));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sourceFloat_(double(row.m_coverage)));

            for (u32 channel = 0u; channel < 3u; ++channel) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sourceFloat_(double(row.m_source[channel])));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sourceFloat_(double(row.m_sigma[channel])));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sourceFloat_(double(row.m_emission[channel])));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       std::isfinite(row.m_albedo[channel]) and
                                       row.m_albedo[channel] >= 0.0f and
                                       row.m_albedo[channel] <= 1.0f));
            }

            u32 slot = (row.m_cell * 2654435761u) & static_cast<u32>(data->m_overlay_lookup.size() - 1u);
            while (data->m_overlay_lookup[slot] != 0u)
                slot = (slot + 1u) & static_cast<u32>(data->m_overlay_lookup.size() - 1u);

            data->m_overlay_lookup[slot] = ordinal + 1u;

            if (data->m_materials[data->m_ids[row.m_cell]].m_kind != 1u and row.m_coverage > 0.0f) {
                ++data->m_surface_cells;
                if (row.m_coverage < 1.0f)
                    ++data->m_partial_cells;
            }
        }

        const auto composed_medium = [&](const GpuOverlay &row) -> Expected<GpuMaterial> {
            GpuMaterial material = data->m_materials[data->m_ids[row.m_cell]];
            if (material.m_kind == 1u)
                return material;

            for (u32 channel = 0u; channel < 3u; ++channel) {
                const auto sigma = sourceFloat_(double(material.m_sigma[channel]) + double(row.m_sigma[channel]));
                const auto emission = sourceFloat_(
                    double(material.m_emission[channel]) + double(row.m_emission[channel]));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sigma);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, emission);
                material.m_sigma[channel] = *sigma;
                material.m_emission[channel] = *emission;
                material.m_source[channel] = row.m_coverage > 0.0f ? row.m_source[channel] : 0.0f;
                material.m_albedo[channel] = row.m_albedo[channel];
            }

            return material;
        };

        // One source-domain bound, not a per-ray certificate. Heterogeneous
        // positive extinction is bounded by its largest equilibrium j/sigma;
        // zero-extinction emission is bounded by the complete world path.
        const double path_metres = 2.0 * (double(input.m_width) + double(input.m_height)) * input.m_cell_metres;
        const auto cell_metres = sourceFloat_(input.m_cell_metres);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cell_metres);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(std::isfinite(path_metres)));

        constexpr double absolute_transport_tolerance = 1e-5;
        for (u32 channel = 0u; channel < 3u; ++channel) {
            double maximum = double{data->m_sky[channel]}, zero_sigma_emission = 0.0;
            for (u32 id = 0u; id < data->m_materials.size(); ++id) {
                if (not used[id])
                    continue;

                const auto &material = data->m_materials[id];
                maximum = std::max(maximum, double(material.m_source[channel]));
                const double sigma = double{material.m_sigma[channel]}, emission = double{material.m_emission[channel]};
                if (sigma > 0.0)
                    maximum = std::max(maximum, emission / sigma);
                else
                    zero_sigma_emission = std::max(zero_sigma_emission, emission);
            }

            for (const auto &row: data->m_overlay) {
                const auto composed = composed_medium(row);
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, composed);
                maximum = std::max(maximum, double(composed->m_source[channel]));
                const double sigma = double(composed->m_sigma[channel]);
                const double emission = double(composed->m_emission[channel]);
                if (sigma > 0.0)
                    maximum = std::max(maximum, emission / sigma);
                else
                    zero_sigma_emission = std::max(zero_sigma_emission, emission);
            }

            data->m_transport_max[channel] = maximum;
            data->m_zero_emission[channel] = zero_sigma_emission;
            data->m_far_bound[channel] = maximum + zero_sigma_emission * path_metres;
            // Global T can first flush once/channel. Each cell can also lose a
            // subnormal optical depth or attenuation product under FTZ; charge
            // the full far bound for every possible crossing, conservatively.
            // Local radiance underflow contributes at most one FLT_MIN/cell.
            // These are absolute FTZ losses, not a certification of all FP32
            // roundoff or geometric traversal error.
            const double crossings = double(input.m_width) + input.m_height + 4.0;
            data->m_underflow_loss[channel] = double(std::numeric_limits<float>::min()) *
                                              ((2.0 + 2.0 * crossings) * data->m_far_bound[channel] +
                                                  2.0 * crossings +
                                                  512.0);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(data->m_far_bound[channel]) and
                                       std::isfinite(data->m_underflow_loss[channel]) and
                                       data->m_underflow_loss[channel] <= absolute_transport_tolerance,
                                   std::errc::argument_out_of_domain));
        }

        std::vector<u64> occurrences(maximum_id + 1u);
        std::vector<EmitterBounds> emitting_bounds(maximum_id + 1u);
        std::vector<bool> emitting(maximum_id + 1u);
        for (u32 id = 0u; id <= maximum_id; ++id) {
            const auto &material = data->m_materials[id];
            emitting[id] = std::ranges::any_of(material.m_source, [](const float value) { return value > 0.0f; }) or
                           std::ranges::any_of(material.m_emission, [](const float value) { return value > 0.0f; });
            emitting_bounds[id] = {id, input.m_width, input.m_height, 0u, 0u};
        }

        for (u32 index = 0u; index < cells; ++index) {
            const u16 id = data->m_ids[index];
            ++occurrences[id];
            if (data->m_materials[id].m_kind == 1u)
                ++data->m_surface_cells;
            if (not emitting[id])
                continue;

            auto &bounds = emitting_bounds[id];
            const u32 x = index % input.m_width, y = index / input.m_width;
            bounds.m_min_x = std::min(bounds.m_min_x, x);
            bounds.m_min_y = std::min(bounds.m_min_y, y);
            bounds.m_max_x = std::max(bounds.m_max_x, x + 1u);
            bounds.m_max_y = std::max(bounds.m_max_y, y + 1u);
        }

        for (u32 id = 0u; id <= maximum_id; ++id) {
            if (occurrences[id] == 0u)
                continue;

            if (emitting[id])
                data->m_emitters.push_back(emitting_bounds[id]);

            PPR_LOG(
                WorldRadiance, info, "Native source used material",
                {{"id", id}, {"cells", occurrences[id]}, {"model", data->m_materials[id].m_kind},
                    {"emitting", bool(emitting[id])}});
        }

        EmitterBounds overlay_emitters{std::numeric_limits<u32>::max(), input.m_width, input.m_height, 0u, 0u};
        for (const auto &row: data->m_overlay) {
            const auto composed = composed_medium(row);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, composed);
            if (data->m_materials[data->m_ids[row.m_cell]].m_kind == 1u or
                (not std::ranges::any_of(composed->m_source, [](float v) { return v > 0.0f; }) and
                    not std::ranges::any_of(composed->m_emission, [](float v) { return v > 0.0f; })))
                continue;

            const u32 x = row.m_cell % input.m_width, y = row.m_cell / input.m_width;
            overlay_emitters.m_min_x = std::min(overlay_emitters.m_min_x, x);
            overlay_emitters.m_min_y = std::min(overlay_emitters.m_min_y, y);
            overlay_emitters.m_max_x = std::max(overlay_emitters.m_max_x, x + 1u);
            overlay_emitters.m_max_y = std::max(overlay_emitters.m_max_y, y + 1u);
        }

        if (overlay_emitters.m_min_x < overlay_emitters.m_max_x)
            data->m_emitters.push_back(overlay_emitters);

        PPR_LOG(
            WorldRadiance, info, "Native source practical FP32 domain",
            {{"width", input.m_width}, {"height", input.m_height}, {"cell_metres", input.m_cell_metres},
                {"sky_r", data->m_sky[0]}, {"sky_g", data->m_sky[1]}, {"sky_b", data->m_sky[2]},
                {"far_bound_r", data->m_far_bound[0]}, {"far_bound_g", data->m_far_bound[1]},
                {"far_bound_b", data->m_far_bound[2]}, {"underflow_loss_r", data->m_underflow_loss[0]},
                {"underflow_loss_g", data->m_underflow_loss[1]}, {"underflow_loss_b", data->m_underflow_loss[2]},
                {"absolute_transport_tolerance", absolute_transport_tolerance}});

        data->m_packed_ids.resize((cells + 1u) / 2u);
        for (u32 index = 0u; index < cells; ++index) {
            data->m_packed_ids[index / 2u] |= u32{data->m_ids[index]} << (16u * (index % 2u));
        }

        data->m_hierarchy.push_back({0u, input.m_width, input.m_height, 1u});
        data->m_vacuum.resize(cells);
        for (u32 index = 0u; index < cells; ++index) {
            data->m_vacuum[index] = data->m_materials[data->m_ids[index]].m_kind == 0u ? 1u : 0u;
        }

        for (const auto &row: data->m_overlay) {
            if (row.m_coverage > 0.0f or
                std::ranges::any_of(row.m_sigma, [](float v) { return v > 0.0f; }) or
                std::ranges::any_of(row.m_emission, [](float v) { return v > 0.0f; }))
                data->m_vacuum[row.m_cell] = 0u;
        }

        while (data->m_hierarchy.back().m_width > 1u or data->m_hierarchy.back().m_height > 1u) {
            const auto child = data->m_hierarchy.back();
            const GpuHierarchy parent{
                static_cast<u32>(data->m_vacuum.size()), (child.m_width + 1u) / 2u, (child.m_height + 1u) / 2u,
                child.m_block_size * 2u};
            for (u32 y = 0u; y < parent.m_height; ++y) {
                for (u32 x = 0u; x < parent.m_width; ++x) {
                    bool vacuum = true;
                    for (u32 dy = 0u; dy < 2u; ++dy) {
                        for (u32 dx = 0u; dx < 2u; ++dx) {
                            const u32 cx = 2u * x + dx, cy = 2u * y + dy;
                            vacuum = vacuum and
                                     cx < child.m_width and
                                     cy < child.m_height and
                                     data->m_vacuum[child.m_offset + cy * child.m_width + cx] != 0u;
                        }
                    }

                    data->m_vacuum.push_back(vacuum ? 1u : 0u);
                }
            }

            data->m_hierarchy.push_back(parent);
        }

        data->m_cpu_bytes = sizeof(WorldRadianceSource) +
                            sizeof(Data) +
                            data->m_ids.capacity() * sizeof(u16) +
                            data->m_packed_ids.capacity() * sizeof(u32) +
                            data->m_vacuum.capacity() * sizeof(u32) +
                            data->m_materials.capacity() * sizeof(GpuMaterial) +
                            data->m_particle_materials.capacity() * sizeof(GpuOriginalMaterial) +
                            data->m_particle_ids.capacity() * sizeof(u32) +
                            data->m_overlay.capacity() * sizeof(GpuOverlay) +
                            data->m_overlay_lookup.capacity() * sizeof(u32) +
                            data->m_hierarchy.capacity() * sizeof(GpuHierarchy) +
                            data->m_emitters.capacity() * sizeof(EmitterBounds);
        return std::shared_ptr<const WorldRadianceSource>{new WorldRadianceSource{std::move(data)}};
    }

    Expected<std::shared_ptr<const WorldRadianceSource>> WorldRadianceSource::bindMatterIdentity(
        std::shared_ptr<const WorldRadianceSource> terrain, const GridWorldIdentity world, const u64 terrain_revision) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               terrain and
                               not terrain->m_data->m_projected and
                               world.m_job_id != 0u and
                               world.m_world_generation != 0u and
                               terrain_revision != 0u));
        const auto &input = *terrain->m_data;
        const auto &base = input.terrain();
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                base.m_overlay.empty() and
                (input.m_world_identity.m_job_id == 0u or input.m_world_identity.m_job_id == world.m_job_id) and
                (input.m_world_identity.m_world_generation == 0u or
                    input.m_world_identity.m_world_generation == world.m_world_generation) and
                (input.m_world_identity.m_content_fingerprint == 0u or
                    input.m_world_identity.m_content_fingerprint == world.m_content_fingerprint) and
                (input.m_terrain_revision == 0u or input.m_terrain_revision == terrain_revision)));

        auto data = std::make_unique<Data>();
        data->m_terrain = input.m_terrain ? input.m_terrain : std::move(terrain);
        data->m_world_identity = world;
        data->m_terrain_revision = terrain_revision;
        data->m_width = base.m_width;
        data->m_height = base.m_height;
        data->m_cell_metres = base.m_cell_metres;
        data->m_minimum = base.m_minimum;
        data->m_sky = base.m_sky;
        data->m_far_bound = base.m_far_bound;
        data->m_transport_max = base.m_transport_max;
        data->m_zero_emission = base.m_zero_emission;
        data->m_underflow_loss = base.m_underflow_loss;
        data->m_surface_cells = base.m_surface_cells;
        data->m_emitters = base.m_emitters;
        data->m_overlay_lookup.assign(1u, 0u);
        data->m_cpu_bytes = sizeof(WorldRadianceSource) +
                            sizeof(Data) +
                            data->m_emitters.capacity() * sizeof(EmitterBounds) +
                            data->m_overlay_lookup.capacity() * sizeof(u32);
        return std::shared_ptr<const WorldRadianceSource>{new WorldRadianceSource{std::move(data)}};
    }

    std::error_code WorldRadianceSource::validateMaterialProjection(const u32 id, const OpticalCell &optics) const {
        if (optics.m_kind == OpticalKind::unavailable) {
            // Unavailable is an explicit catalog contract, never a substitute
            // for known uploaded transport in a particle-only visual row.
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(m_data->material(id) != nullptr and m_data->material(id)->m_kind == 3u));
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_data->material(id) != nullptr and (optics.m_kind == OpticalKind::vacuum or
                                                                       optics.m_kind == OpticalKind::opaque or
                                                                       optics.m_kind == OpticalKind::absorbing)));

        const OpticalGridView validation{
            .m_cells = std::span{&optics, 1u},
            .m_cell_size_m = 1.0,
            .m_width = 1u,
            .m_height = 1u,
            .m_boundary = OpticalBoundary::vacuum};
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, validateOpticalGrid(validation));

        const auto &row = *m_data->material(id);
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               row.m_kind == (optics.m_kind == OpticalKind::vacuum      ? 0u
                                                 : optics.m_kind == OpticalKind::opaque ? 1u
                                                                                        : 2u)));
        for (u32 channel = 0u; channel < 3u; ++channel) {
            for (const auto &[actual, expected]: std::array<std::pair<float, double>, 3>{
                     std::pair{row.m_source[channel], optics.m_surface_emission[channel]},
                     {row.m_sigma[channel], optics.m_extinction[channel]},
                     {row.m_emission[channel], optics.m_volume_emission[channel]}}) {
                const auto value = sourceFloat_(expected);
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, value ? std::error_code{} : value.error());
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(actual == *value));
            }
        }
        return {};
    }

    bool WorldRadianceSource::matchesMaterialDiffuseProjection(
        const u32 id, const std::array<float, 3u> albedo) const noexcept {
        const auto *const material = m_data->material(id);
        if (not material or material->m_kind == 3u)
            return false;

        for (u32 channel = 0u; channel < 3u; ++channel) {
            if (not std::isfinite(albedo[channel]) or
                albedo[channel] < 0.0f or
                albedo[channel] > 1.0f or
                albedo[channel] != material->m_albedo[channel])
                return false;
        }

        return true;
    }

    u32 WorldRadianceSource::width() const noexcept {
        return m_data->m_width;
    }

    u32 WorldRadianceSource::height() const noexcept {
        return m_data->m_height;
    }

    double WorldRadianceSource::cellMetres() const noexcept {
        return m_data->m_cell_metres;
    }

    std::array<double, 2> WorldRadianceSource::minimumMetres() const noexcept {
        return m_data->m_minimum;
    }

    std::span<const u16> WorldRadianceSource::materialIds() const noexcept {
        return m_data->terrain().m_ids;
    }

    std::span<const u32> WorldRadianceSource::particleMaterialIds() const noexcept {
        return m_data->terrain().m_particle_ids;
    }

    bool WorldRadianceSource::matchesTerrainSource(const WorldRadianceSource &other) const noexcept {
        const auto *const a = m_data->m_terrain ? m_data->m_terrain.get() : this;
        const auto *const b = other.m_data->m_terrain ? other.m_data->m_terrain.get() : &other;
        return a == b;
    }

    bool WorldRadianceSource::matchesProjectedOptics(const WorldRadianceSource &previous) const noexcept {
        const auto compared = matterComparedIdentity();
        const auto statistics = matterStatistics();
        return compared and
               compared == previous.matterIdentity() and
               matchesTerrainSource(previous) and
               statistics and
               statistics->m_changed_cells == 0u;
    }

    u64 WorldRadianceSource::retainedCpuBytes() const noexcept {
        return m_data->m_cpu_bytes + (m_data->m_terrain ? m_data->m_terrain->retainedCpuBytes() : 0u);
    }

    std::array<double, 3> WorldRadianceSource::underflowLossBound() const noexcept {
        return m_data->m_underflow_loss;
    }

    std::optional<WorldRadianceMatterIdentity> WorldRadianceSource::matterIdentity() const noexcept {
        return m_data->m_projected ? std::optional{m_data->m_projected->m_identity} : std::nullopt;
    }

    std::optional<WorldRadianceMatterStatistics> WorldRadianceSource::matterStatistics() const noexcept {
        return m_data->m_projected ? std::optional{m_data->m_projected->m_statistics} : std::nullopt;
    }

    std::optional<WorldRadianceMatterIdentity> WorldRadianceSource::matterComparedIdentity() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_compared_identity : std::nullopt;
    }

    FenceCompletionValue WorldRadianceSource::matterCompletionFence() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_fence : kFenceValueNone;
    }

    rhi::IBuffer *WorldRadianceSource::overlayBuffer() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_rows.get() : nullptr;
    }

    rhi::IBuffer *WorldRadianceSource::overlayLookupBuffer() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_lookup.get() : nullptr;
    }

    rhi::IBuffer *WorldRadianceSource::overlayAncestorBuffer() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_ancestors.get() : nullptr;
    }

    u32 WorldRadianceSource::overlayCount() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_count : safe_narrowing(m_data->m_overlay.size());
    }

    u32 WorldRadianceSource::overlayLookupCapacity() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_capacity : safe_narrowing(m_data->m_overlay_lookup.size());
    }

    u32 WorldRadianceSource::overlayAncestorWords() const noexcept {
        return m_data->m_projected ? m_data->m_projected->m_ancestor_words : 0u;
    }

    // ------------------------------------------------------------------
    // renderer-independent local gameplay illumination query
    // ------------------------------------------------------------------

    Expected<WorldRadianceGameplayQuerySettings> WorldRadianceGameplayQuery::windowForWorldPoint(
        const WorldRadianceSource &source, const std::array<double, 2> center_m, const u32 edge) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(edge > 0u, std::errc::argument_out_of_domain));

        const double cell_metres = source.cellMetres();
        const auto minimum = source.minimumMetres();
        const std::array<u32, 2> extent{source.width(), source.height()};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               std::isfinite(cell_metres) and
                                   cell_metres > 0.0 and
                                   std::isfinite(center_m[0]) and
                                   std::isfinite(center_m[1]) and
                                   std::isfinite(minimum[0]) and
                                   std::isfinite(minimum[1]) and
                                   extent[0] > 0u and
                                   extent[1] > 0u,
                               std::errc::argument_out_of_domain));

        const u32 clamped_edge = std::min({kMaxWindowEdge, extent[0], extent[1], edge});
        WorldRadianceGameplayQuerySettings settings{};
        settings.m_counts = {clamped_edge, clamped_edge};
        for (std::size_t axis = 0u; axis < 2u; ++axis) {
            const double local = (center_m[axis] - minimum[axis]) / cell_metres;
            const double center_cell = std::clamp(std::floor(local), 0.0, double(extent[axis] - 1u));
            const auto half_edge = static_cast<i32>(clamped_edge / 2u);
            settings.m_minimum[axis] = std::clamp<i32>(
                static_cast<i32>(center_cell) - half_edge, 0, static_cast<i32>(extent[axis] - clamped_edge));
        }

        return settings;
    }

    Expected<WorldRadianceGameplayQuery> WorldRadianceGameplayQuery::capture(
        const WorldRadianceSource &source, const WorldRadianceGameplayQuerySettings settings) {
        const hal::ProfileScope scope{"WorldRadiance.GameplayQueryCapture"};

        const auto &data = source.m_data->terrain();
        for (std::size_t axis = 0u; axis < 2u; ++axis) {
            const u32 extent = axis == 0u ? data.m_width : data.m_height;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   settings.m_counts[axis] > 0u and
                                       settings.m_counts[axis] <= kMaxWindowEdge and
                                       settings.m_minimum[axis] >= 0,
                                   std::errc::argument_out_of_domain));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   u64{static_cast<u32>(settings.m_minimum[axis])} + settings.m_counts[axis] <= extent,
                                   std::errc::result_out_of_range));
        }

        const auto kind_of = [](const u32 kind) noexcept {
            return kind == 0u ? OpticalKind::vacuum : kind == 1u ? OpticalKind::opaque : OpticalKind::absorbing;
        };

        const u32 width = settings.m_counts[0u], height = settings.m_counts[1u];
        std::vector<OpticalCell> cells(static_cast<std::size_t>(width) * height);
        for (u32 y = 0u; y < height; ++y) {
            for (u32 x = 0u; x < width; ++x) {
                const u32 global_x = static_cast<u32>(settings.m_minimum[0]) + x;
                const u32 global_y = static_cast<u32>(settings.m_minimum[1]) + y;
                const u32 global = global_y * data.m_width + global_x;
                const GpuMaterial &base = data.m_materials[data.m_ids[global]];
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(base.m_kind <= 2u, std::errc::not_supported));

                OpticalCell cell{};
                cell.m_kind = kind_of(base.m_kind);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    cell.m_extinction[channel] = double(base.m_sigma[channel]);
                    cell.m_volume_emission[channel] = double(base.m_emission[channel]);
                    cell.m_surface_emission[channel] = double(base.m_source[channel]);
                }

                if (base.m_kind != 1u and not data.m_overlay.empty()) {
                    const auto found = std::ranges::lower_bound(
                        data.m_overlay, global, {}, &WorldRadianceOverlayCell::m_cell);
                    if (found != data.m_overlay.end() and found->m_cell == global) {
                        const auto &row = *found;
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance,
                            require_(row.m_coverage <= 0.0f or row.m_coverage >= 1.0f, std::errc::not_supported));

                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            cell.m_extinction[channel] = double(base.m_sigma[channel]) + double(row.m_sigma[channel]);
                            cell.m_volume_emission[channel] = double(base.m_emission[channel]) +
                                                              double(row.m_emission[channel]);
                            cell.m_surface_emission[channel] = double(row.m_source[channel]);
                        }

                        const bool any_medium = std::ranges::any_of(cell.m_extinction, [](const double value) {
                            return value > 0.0;
                        }) or std::ranges::any_of(cell.m_volume_emission, [](const double value) {
                            return value > 0.0;
                        });
                        cell.m_kind = row.m_coverage == 1.0f ? OpticalKind::opaque
                                      : any_medium           ? OpticalKind::absorbing
                                                             : kind_of(base.m_kind);
                    }
                }

                // OpticalCell invariants: opaque keeps only surface emission;
                // absorbing keeps only extinction and volume emission; vacuum
                // zeroes every optical term.
                if (cell.m_kind == OpticalKind::opaque) {
                    cell.m_extinction = {};
                    cell.m_volume_emission = {};
                } else if (cell.m_kind == OpticalKind::absorbing) {
                    cell.m_surface_emission = {};
                } else {
                    cell.m_extinction = {};
                    cell.m_volume_emission = {};
                    cell.m_surface_emission = {};
                }

                cells[static_cast<std::size_t>(y) * width + x] = cell;
            }
        }

        const std::array<double, 2> minimum{
            data.m_minimum[0] + double(settings.m_minimum[0]) * data.m_cell_metres,
            data.m_minimum[1] + double(settings.m_minimum[1]) * data.m_cell_metres};
        const bool sky_zero = std::ranges::all_of(data.m_sky, [](const float value) { return value == 0.0f; });
        const OpticalGridView grid{
            .m_cells = cells,
            .m_min_m = minimum,
            .m_cell_size_m = data.m_cell_metres,
            .m_width = width,
            .m_height = height,
            .m_boundary = sky_zero ? OpticalBoundary::opaque : OpticalBoundary::vacuum};
        auto hierarchy = OpticalHierarchy::capture(grid);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, hierarchy);

        u64 retained = sizeof(WorldRadianceGameplayQuery) + cells.size() * sizeof(OpticalCell);
        for (u64 level_width = width, level_height = height;;) {
            retained += level_width * level_height * sizeof(u8) + sizeof(std::vector<u8>);
            if (level_width == 1u and level_height == 1u) {
                break;
            }

            level_width = (level_width + 1u) / 2u;
            level_height = (level_height + 1u) / 2u;
        }

        const OpticalRgb sky{double(data.m_sky[0]), double(data.m_sky[1]), double(data.m_sky[2])};
        return WorldRadianceGameplayQuery{
            std::move(*hierarchy), sky, minimum, data.m_cell_metres, width, height, not sky_zero, retained};
    }

    Expected<WorldRadianceGameplayQueryResult> WorldRadianceGameplayQuery::trace(const OpticalRay &ray) const {
        auto normalized = normalizeOpticalRay(ray);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, normalized);

        const std::array<double, 2> maximum{
            m_minimum[0] + double(m_width) * m_cell_metres, m_minimum[1] + double(m_height) * m_cell_metres};
        for (std::size_t axis = 0u; axis < 2u; ++axis) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    normalized->m_origin_m[axis] >= m_minimum[axis] and normalized->m_origin_m[axis] <= maximum[axis],
                    std::errc::result_out_of_range));
        }

        auto traced = traceOpticalAccelerated(m_hierarchy, *normalized);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, traced);

        WorldRadianceGameplayQueryResult result{
            traced->m_interval.m_radiance, traced->m_interval.m_transmittance, traced->m_segments};
        double exit = std::numeric_limits<double>::infinity();
        for (std::size_t axis = 0u; axis < 2u; ++axis) {
            if (normalized->m_direction[axis] == 0.0) {
                continue;
            }

            const double plane = normalized->m_direction[axis] > 0.0 ? maximum[axis] : m_minimum[axis];
            exit = std::min(exit, (plane - normalized->m_origin_m[axis]) / normalized->m_direction[axis]);
        }

        if (normalized->m_far_m >= exit) {
            if (m_vacuum_boundary) {
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    result.m_radiance[channel] += result.m_transmittance[channel] * m_sky[channel];
                }
            } else {
                result.m_transmittance = {};
            }
        }

        return result;
    }

    WorldRadianceGameplayQuery::WorldRadianceGameplayQuery(
        OpticalHierarchy hierarchy, const OpticalRgb sky, const std::array<double, 2> minimum, const double cell_metres,
        const u32 width, const u32 height, const bool vacuum_boundary, const u64 retained_bytes)
        : m_hierarchy{std::move(hierarchy)}, m_sky{sky}, m_minimum{minimum}, m_cell_metres{cell_metres},
          m_retained_bytes{retained_bytes}, m_width{width}, m_height{height}, m_vacuum_boundary{vacuum_boundary} {
    }

    u32 WorldRadianceGameplayQuery::width() const noexcept {
        return m_width;
    }

    u32 WorldRadianceGameplayQuery::height() const noexcept {
        return m_height;
    }

    double WorldRadianceGameplayQuery::cellMetres() const noexcept {
        return m_cell_metres;
    }

    std::array<double, 2> WorldRadianceGameplayQuery::minimumMetres() const noexcept {
        return m_minimum;
    }

    u64 WorldRadianceGameplayQuery::retainedCpuBytes() const noexcept {
        return m_retained_bytes;
    }

    // ------------------------------------------------------------------
    // owned GPU generations and owner-confined candidate state
    // ------------------------------------------------------------------

    const std::shared_ptr<const WorldRadianceSource> &WorldRadianceGeneration::source() const noexcept {
        return m_source;
    }

    TerrainReceiverCohort WorldRadianceGeneration::cohort() const noexcept {
        return m_cohort;
    }

    rhi::IBuffer *WorldRadianceGeneration::coefficientsBuffer() const noexcept {
        return m_coefficients.get();
    }

    rhi::IBuffer *WorldRadianceGeneration::probeStateBuffer() const noexcept {
        return m_probe_states.get();
    }

    u64 WorldRadianceGeneration::probeStateBytes() const noexcept {
        return m_probe_states ? m_probe_states->getDesc().size : 0u;
    }

    i32 WorldRadianceGeneration::skyHour() const noexcept {
        return m_sky_hour;
    }

    u64 WorldRadianceGeneration::transportVersion() const noexcept {
        return m_transport_version;
    }

    const WorldRadianceStatistics &WorldRadianceGeneration::statistics() const noexcept {
        return m_statistics;
    }

    std::array<i32, 2> WorldRadianceGeneration::probeMinimum() const noexcept {
        return m_probe_min;
    }

    std::array<u32, 2> WorldRadianceGeneration::probeCounts() const noexcept {
        return m_probe_counts;
    }

    const std::shared_ptr<const WorldRadianceGameplayField> &WorldRadianceGeneration::gameplayField() const noexcept {
        return m_gameplay_field;
    }

    Expected<WorldRadianceCoefficients> WorldRadianceGameplayField::sample(
        const std::array<double, 2> world_metres) const {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_spacing > 0u and
                               std::isfinite(m_cell_metres) and
                               m_cell_metres > 0.0 and
                               not m_rows.empty() and
                               m_rows.size() == u64{m_probes.m_counts[0]} * m_probes.m_counts[1]));
        std::array<u32, 2> low{}, high{};
        std::array<float, 2> fraction{};
        for (u32 axis = 0u; axis < 2u; ++axis) {
            const double local = (world_metres[axis] - m_origin_metres[axis]) / m_cell_metres;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    std::isfinite(local) and
                        local >= m_demand.m_window.m_minimum[axis] and
                        local <= double(m_demand.m_window.m_minimum[axis]) + m_demand.m_window.m_counts[axis],
                    std::errc::result_out_of_range));
            const double coord = local / m_spacing - 0.5 - m_probes.m_minimum[axis];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(coord >= 0.0 and std::ceil(coord) < m_probes.m_counts[axis]));
            low[axis] = static_cast<u32>(std::floor(coord));
            high[axis] = static_cast<u32>(std::ceil(coord));
            fraction[axis] = static_cast<float>(coord - low[axis]);
        }

        WorldRadianceCoefficients result{};
        for (u32 corner = 0u; corner < 4u; ++corner) {
            const bool right = (corner & 1u) != 0u, upper = (corner & 2u) != 0u;
            const float weight = (right ? fraction[0] : 1.0f - fraction[0]) *
                                 (upper ? fraction[1] : 1.0f - fraction[1]);
            if (weight == 0.0f)
                continue;

            const auto &row = m_rows[(upper ? high[1] : low[1]) * m_probes.m_counts[0] + (right ? high[0] : low[0])];
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(row.m_status == 0u, std::errc::result_out_of_range));
            for (u32 index = 0u; index < 27u; ++index)
                result.m_coefficients[index] += weight * row.m_coefficients[index];
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                std::ranges::all_of(result.m_coefficients, [](float value) { return std::isfinite(value); }),
                std::errc::result_out_of_range));
        return result;
    }

    WorldRadianceReceiverWindow WorldRadianceGeneration::updatedProbeWindow() const noexcept {
        return m_updated_probe_window;
    }

    bool WorldRadianceGeneration::supplementalRefreshYielded() const noexcept {
        return m_refresh_yielded;
    }

    WorldRadianceReceiverWindow WorldRadianceGeneration::currentReceiverWindow() const noexcept {
        return m_current_receiver_window;
    }

    WorldRadianceRefinedSpacing WorldRadianceGeneration::refinedSpacing() const noexcept {
        return m_refined_spacing;
    }

    WorldRadianceTransport WorldRadianceGeneration::transport() const noexcept {
        return m_transport;
    }

    float WorldRadianceGeneration::maximumResponse() const noexcept {
        return m_maximum_response;
    }

    u64 WorldRadianceGeneration::bufferBytes() const noexcept {
        return m_bytes;
    }

    bool WorldRadianceGeneration::matchesDevice(const rhi::IDevice &device) const noexcept {
        return m_device == &device;
    }

    // ------------------------------------------------------------------
    // immutable complete discovery identity and resources
    // ------------------------------------------------------------------

    const std::shared_ptr<const WorldRadianceSource> &WorldRadianceFaceDiscovery::source() const noexcept {
        return m_source;
    }

    TerrainReceiverCohort WorldRadianceFaceDiscovery::cohort() const noexcept {
        return m_cohort;
    }

    WorldRadianceReceiverWindow WorldRadianceFaceDiscovery::window() const noexcept {
        return m_window;
    }

    u64 WorldRadianceGeneration::bounceEpoch() const noexcept {
        return m_bounce_epoch;
    }

    FenceCompletionValue WorldRadianceGeneration::completionFence() const noexcept {
        return m_completion;
    }

    std::optional<double> WorldRadianceGeneration::maximumGpuMilliseconds() const noexcept {
        return m_max_gpu_milliseconds;
    }

    u64 WorldRadianceFaceDiscovery::requestEpoch() const noexcept {
        return m_request_epoch;
    }

    u32 WorldRadianceFaceDiscovery::faceCount() const noexcept {
        return m_count;
    }

    u64 WorldRadianceFaceDiscovery::faceHitOccurrences() const noexcept {
        return m_face_hit_occurrences;
    }

    u32 WorldRadianceFaceDiscovery::conservativeOffscreenFaceCount() const noexcept {
        return m_offscreen_count;
    }

    rhi::IBuffer *WorldRadianceFaceDiscovery::keysBuffer() const noexcept {
        return m_keys.get();
    }

    u64 WorldRadianceFaceDiscovery::bufferBytes() const noexcept {
        return m_bytes;
    }

    FenceCompletionValue WorldRadianceFaceDiscovery::completionFence() const noexcept {
        return m_completion;
    }

    const WorldRadianceStatistics &WorldRadianceFaceDiscovery::statistics() const noexcept {
        return m_statistics;
    }

    std::optional<double> WorldRadianceFaceDiscovery::maximumGpuMilliseconds() const noexcept {
        return m_max_gpu_milliseconds;
    }

    // ------------------------------------------------------------------
    // pass-owned lighting and discovery batches
    // ------------------------------------------------------------------

    struct WorldRadiancePass::State {
        enum class BounceOperation : u32 {
            lookup,
            moments,
            coarse,
            refine,
            combine
        };
        struct BounceWork {
            u64 m_queries{};
            GpuDispatch m_dispatch{};
            BounceOperation m_operation{};
            u32 m_first{};
        };

        struct BounceBatch {
            std::shared_ptr<const WorldRadianceFaceDiscovery> m_discovery;
            std::shared_ptr<const WorldRadianceGeneration> m_direct;
            std::shared_ptr<WorldRadianceGeneration> m_generation;
            std::array<rhi::ComPtr<rhi::IBuffer>, 2u> m_parity;
            std::shared_ptr<const StaticWorldInput> m_static_input;
            rhi::ComPtr<rhi::IBuffer> m_overlay, m_overlay_lookup, m_overlay_ancestors;
            rhi::ComPtr<rhi::IBuffer> m_ids, m_materials, m_vacuum, m_hierarchy, m_masks;
            rhi::ComPtr<rhi::IBuffer> m_levels, m_directions, m_counts, m_readback, m_lookup, m_moments, m_indirect;
            std::vector<BounceWork> m_work;
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_roots;
            rhi::ComPtr<rhi::IQueryPool> m_queries;
            WorldRadianceBounceSettings m_settings{};
            WorldRadianceBounceDemand m_demand{};
            GpuParameters m_parameters{};
            FenceCompletionValue m_fence{kFenceValueNone};
            u64 m_actual_frame{};
            u32 m_next{};
            bool m_encoded{}, m_submitted{}, m_completed{}, m_published{};
        };

        struct TemporalPool {
            std::array<rhi::ComPtr<rhi::IBuffer>, 8u> m_levels;
            std::array<rhi::ComPtr<rhi::IBuffer>, 3u> m_fields, m_probe_states;
            std::array<std::weak_ptr<const WorldRadianceGeneration>, 3u> m_leases;
            rhi::ComPtr<rhi::IBuffer> m_dummy;
            // Separate power-of-two work sizes: dispatch overhead dominates short tails.
            // The last size class also covers larger fixed-control batches.
            // Traversal through the rotating offscreen tile can cost substantially
            // more than the same number of nearby visible records.
            std::array<std::array<std::array<TemporalCost, 14u>, 8u>, 2u> m_costs;
            std::array<TemporalCost, 4u> m_setup_costs;
            TemporalCost m_overhead;
            WorldRadianceTemporalSettings m_settings{};
            TerrainReceiverCohort m_identity{};
            u32 m_width{}, m_height{}, m_cost_mode{}, m_last_budgeted_queries{};
            bool m_initialized{};

            [[nodiscard]] TemporalCost &costFor(const TemporalWork &work) noexcept {
                const auto size_class = std::bit_width(std::min<u64>(8192u, work.m_queries) - 1u);
                return m_costs[work.m_refresh ? 1u : 0u][work.m_dispatch.m_level][static_cast<std::size_t>(size_class)];
            }

            [[nodiscard]] std::optional<double> estimateMilliseconds(const TemporalWork &work) const noexcept {
                const auto size = static_cast<std::size_t>(std::bit_width(std::min<u64>(8192u, work.m_queries) - 1u));
                const auto &costs = m_costs[work.m_refresh ? 1u : 0u][work.m_dispatch.m_level];
                if (costs[size].m_samples > 0u)
                    return costs[size].maximum() * static_cast<double>(work.m_queries);

                // A new offscreen strip often ends in a previously unseen short
                // tail. Charge a whole measured larger dispatch, or scale up the
                // nearest smaller dispatch. The first sample at each level still
                // runs alone and cannot borrow a cheaper visible-ray estimate.
                if (work.m_refresh) {
                    for (std::size_t other = size + 1u; other < costs.size(); ++other) {
                        if (costs[other].m_samples > 0u)
                            return costs[other].maximum() * static_cast<double>(u64{1u} << other);
                    }
                    for (auto other = size; other > 0u; --other) {
                        if (costs[other - 1u].m_samples > 0u)
                            return costs[other - 1u].maximum() * static_cast<double>(work.m_queries);
                    }
                }
                return std::nullopt;
            }
        };

        struct Batch {
            Plan m_plan;
            std::shared_ptr<TemporalPool> m_temporal_pool;
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_temporal_roots, m_temporal_queue_roots;
            rhi::ComPtr<rhi::IBuffer> m_temporal_queue, m_temporal_arguments;
            rhi::ComPtr<rhi::ITexture> m_skip_table;
            rhi::ComPtr<rhi::ITextureView> m_skip_view;
            rhi::ComPtr<rhi::IShaderObject> m_skip_root;
            std::array<rhi::ComPtr<rhi::IShaderObject>, 2u> m_coverage_roots;
            std::shared_ptr<WorldRadianceGeneration> m_generation;
            // Own the immutable donor through the accepted copy-containing fence.
            std::shared_ptr<const WorldRadianceGeneration> m_copy_donor;
            std::array<rhi::ComPtr<rhi::IBuffer>, 2> m_parity;
            std::shared_ptr<const StaticWorldInput> m_static_input;
            rhi::ComPtr<rhi::IBuffer> m_overlay, m_overlay_lookup, m_overlay_ancestors;
            rhi::ComPtr<rhi::IBuffer> m_ids, m_materials, m_vacuum, m_hierarchy, m_levels, m_directions, m_counts,
                m_readback, m_masks;
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_roots, m_page_roots;
            rhi::ComPtr<rhi::IShaderObject> m_refine_root;
            rhi::ComPtr<rhi::IQueryPool> m_queries;
            WorldRadianceTiming m_timing{};
            WorldRadianceSettings m_settings{};
            FenceCompletionValue m_fence{kFenceValueNone};
            u64 m_own_largest_resource_bytes{}, m_actual_frame{};
            double m_predicted_milliseconds{-1.0};
            u32 m_page{}, m_temporal_slot{}, m_setup_step{2u}, m_work_end{};
            u32 m_unupdated_probes{};
            bool m_encoded{}, m_submitted{}, m_completed{}, m_published{};

            [[nodiscard]] TemporalCost &setupCost() noexcept {
                if (m_setup_step == 0u)
                    return m_temporal_pool->m_setup_costs[m_copy_donor ? 1u : 0u];

                return m_temporal_pool->m_setup_costs[m_skip_root ? 2u : 3u];
            }

            void selectTemporalBatch();
        };

        struct DiscoveryBatch {
            std::shared_ptr<WorldRadianceFaceDiscovery> m_result;
            std::shared_ptr<const StaticWorldInput> m_static_input;
            rhi::ComPtr<rhi::IBuffer> m_overlay, m_overlay_lookup, m_overlay_ancestors;
            rhi::ComPtr<rhi::IBuffer> m_ids, m_materials, m_vacuum, m_hierarchy;
            rhi::ComPtr<rhi::IBuffer> m_levels, m_directions, m_counts, m_append_state, m_readback;
            std::vector<GpuDiscoveryDispatch> m_dispatches;
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_roots;
            rhi::ComPtr<rhi::IQueryPool> m_queries;
            WorldRadianceDiscoverySettings m_settings{};
            WorldRadianceDiscoveryDemand m_demand{};
            std::optional<u64> m_last_frame{};
            FenceCompletionValue m_fence{kFenceValueNone};
            u32 m_next{};
            bool m_encoded{}, m_submitted{}, m_completed{}, m_published{};
        };

        enum class MatterOperation : u32 {
            freeze,
            sort,
            reduce,
            lookup,
            compare
        };

        struct MatterWork {
            MatterOperation m_operation{};
            u32 m_shift{};
        };

        struct MatterBatch {
            std::shared_ptr<const WorldRadianceSource> m_terrain, m_previous;
            std::shared_ptr<const StaticWorldInput> m_base;
            std::shared_ptr<ProjectedWorldInput> m_output;
            std::shared_ptr<const WorldRadianceSource> m_result;
            GridParticleGpuView m_particles;
            std::array<rhi::ComPtr<rhi::IBuffer>, 2u> m_sort;
            rhi::ComPtr<rhi::IBuffer> m_ranks, m_counts, m_prefix, m_summary, m_readback, m_unused_overlay,
                m_unused_lookup;
            std::vector<MatterWork> m_work;
            std::vector<std::array<rhi::ComPtr<rhi::IShaderObject>, 3u>> m_roots;
            rhi::ComPtr<rhi::IQueryPool> m_queries;
            WorldRadianceMatterSettings m_settings{};
            WorldRadianceMatterDemand m_demand{};
            GpuMatterFrame m_parameters{};
            FenceCompletionValue m_fence{kFenceValueNone};
            u64 m_actual_frame{};
            double m_sort_cost_ms{};
            u32 m_next{}, m_encoded_count{};
            bool m_encoded{}, m_submitted{}, m_completed{}, m_acquired{};
        };

        rhi::IDevice *m_device{};
        IShaderService *m_shaders{}; // Borrowed, like m_device; initialize owner outlives this pass.
        std::filesystem::path m_content_root;
        shader::SharedModule m_module, m_matter_module;
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 8> m_programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 8> m_pipelines;

        rhi::ComPtr<rhi::IShaderProgram> m_parallel_program;
        rhi::ComPtr<rhi::IComputePipeline> m_parallel_pipeline;

        std::shared_ptr<TemporalPool> m_temporal_pool;
        u64 m_next_temporal_version{1u};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 8u> m_temporal_programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 8u> m_temporal_pipelines;

        std::unique_ptr<Batch> m_batch;
        std::unique_ptr<DiscoveryBatch> m_discovery;
        std::unique_ptr<BounceBatch> m_bounce;
        std::unique_ptr<MatterBatch> m_matter;
        std::shared_ptr<const WorldRadianceSource> m_matter_baseline;
        std::vector<std::weak_ptr<const WorldRadianceSource>> m_projected_sources;
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 6u> m_matter_programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 6u> m_matter_pipelines;
        std::optional<u64> m_last_matter_frame;
        std::vector<std::weak_ptr<const WorldRadianceGeneration>> m_generations;
        std::vector<std::weak_ptr<const WorldRadianceGameplayField>> m_gameplay_fields;

        std::vector<std::weak_ptr<const WorldRadianceFaceDiscovery>> m_discoveries;
        std::optional<WorldRadianceTiming> m_retired_timing;
        std::optional<u64> m_last_discovery_frame{};
        std::thread::id m_owner;
        FenceCompletionValue m_last_submit{kFenceValueNone};
        std::shared_ptr<const StaticWorldInput> m_static_input;
        bool m_unknown{}, m_support_screen_done{};

        [[nodiscard]] std::error_code chargeGameplayFields(u64 &peak) {
            std::erase_if(m_gameplay_fields, [](const auto &field) { return field.expired(); });
            for (const auto &weak: m_gameplay_fields) {
                const auto field = weak.lock();
                if (field and (not m_batch or field != m_batch->m_generation->gameplayField()))
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldRadiance, add_(
                                           peak, sizeof(WorldRadianceGameplayField) +
                                                     field->m_rows.capacity() * sizeof(WorldRadianceCoefficients)));
            }
            return {};
        }
    };

    void WorldRadiancePass::State::Batch::selectTemporalBatch() {
        const u32 first = m_plan.m_batch_begin[m_page];
        // A new optical epoch can reuse measured throughput of its compatible pool.
        // Unknown dispatch classes still run alone, regardless of the growth limit.
        u64 previous_queries = m_temporal_pool->m_last_budgeted_queries;
        if (m_page > 0u) {
            previous_queries = 0u;
            for (u32 index = m_plan.m_batch_begin[m_page - 1u]; index < first; ++index)
                previous_queries += m_plan.m_temporal_work[index].m_queries;
        }
        const u64 growth_limit = std::min<u64>(
            m_settings.m_temporal_query_budget, std::max<u64>(8192u, 2u * previous_queries));

        // Per-class maxima already include observed cost variation. Keep a
        // larger reserve for optional refresh than for the edit closure.
        const double allowance = double{m_settings.m_temporal_frame_allowance_ms};
        double predicted = m_temporal_pool->m_overhead.maximum();
        u64 queries{};
        u32 next = first;
        while (next < m_work_end) {
            const auto &work = m_plan.m_temporal_work[next];
            const auto estimate = m_temporal_pool->estimateMilliseconds(work);
            // A disjoint gameplay closure uses offscreen cost history, but it
            // shares the edit deadline. Reserve additional slack only for the
            // optional rotating tile beyond the mandatory work boundary.
            const double target = (next >= m_plan.m_primary_work_end ? 0.6 : 0.92) * allowance;
            if (next > first and
                (not estimate or predicted + *estimate > target or queries + work.m_queries > growth_limit))
                break;

            queries += work.m_queries;
            predicted += estimate.value_or(0.0);
            ++next;
            if (not estimate) {
                predicted = -1.0;
                break;
            }
        }

        // An indivisible unit can overshoot: this predictive controller is not preemption.
        if (predicted > allowance) {
            PPR_LOG(
                WorldRadiance, warning, "WORLD_TEMPORAL_QUANTUM_OVER_ALLOWANCE",
                {{"predicted_ms", predicted}, {"allowance_ms", allowance}, {"queries", queries}});
        }
        m_predicted_milliseconds = predicted;
        m_plan.m_batch_begin[m_page + 1u] = next;
    }

    WorldRadiancePass::WorldRadiancePass() = default;
    WorldRadiancePass::~WorldRadiancePass() {
        if (m_state and
            (m_state->m_unknown or
                (m_state->m_matter and (m_state->m_matter->m_encoded or
                                           (m_state->m_matter->m_submitted and not m_state->m_matter->m_completed))) or
                (m_state->m_bounce and (m_state->m_bounce->m_encoded or
                                           (m_state->m_bounce->m_submitted and not m_state->m_bounce->m_completed))) or
                (m_state->m_batch and (m_state->m_batch->m_encoded or
                                          (m_state->m_batch->m_submitted and not m_state->m_batch->m_completed))) or
                (m_state->m_discovery and
                    (m_state->m_discovery->m_encoded or
                        (m_state->m_discovery->m_submitted and not m_state->m_discovery->m_completed))))) {
            // Only explicit completion/removal proof may release accepted work.
            hal::fatalExit("WorldRadiancePass destroyed before GPU quiescence");
        }
    }

    std::error_code WorldRadiancePass::initialize(
        rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &content_root) {
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(not m_state, std::errc::already_connected));
        const auto &limits = device.getInfo().limits;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               limits.maxComputeThreadsPerGroup >= 64u and limits.maxComputeThreadGroupSize[0] >= 64u,
                               std::errc::not_supported));

        const hal::ProfileScope scope{"WorldRadiance.Initialize"};
        auto state = std::make_unique<State>();
        state->m_device = &device;
        state->m_shaders = &shaders;
        state->m_content_root = content_root;
        state->m_owner = std::this_thread::get_id();
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance,
            shaders.loadModuleFromFile(
                content_root / "shaders" / "world_radiance.slang", "world_radiance", state->m_module.writeRef()));
        auto *const layout = state->m_module->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(layout != nullptr));
        for (const auto &[name, stride]: std::array<std::pair<const char *, std::size_t>, 27>{
                 std::pair{"g_overlay", sizeof(GpuOverlay)}, {"g_overlay_lookup", sizeof(u32)},
                 {"g_overlay_ancestors", sizeof(u32)}, {"g_world", sizeof(GpuParameters)},
                 {"g_dispatch", sizeof(GpuDispatch)}, {"g_cell_ids", sizeof(u32)}, {"g_counts", sizeof(u32)},
                 {"g_materials", sizeof(GpuMaterial)}, {"g_hierarchy", sizeof(GpuHierarchy)},
                 {"g_levels", sizeof(GpuLevel)}, {"g_directions", sizeof(GpuDirection)},
                 {"g_parent", sizeof(GpuInterval)}, {"g_current", sizeof(GpuInterval)},
                 {"g_refined", sizeof(WorldRadianceCoefficients)}, {"g_page_masks", sizeof(u32)},
                 {"g_discovery", sizeof(GpuDiscoveryDispatch)}, {"g_discovery_bits", sizeof(u32)},
                 {"g_discovery_keys", sizeof(u32)}, {"g_discovery_state", sizeof(u32)},
                 {"g_bounce", sizeof(GpuBounceDispatch)}, {"g_face_keys", sizeof(u32)}, {"g_face_lookup", sizeof(u32)},
                 {"g_face_moments", 3u * sizeof(float)}, {"g_bounce_direct", sizeof(WorldRadianceCoefficients)},
                 {"g_bounce_indirect", sizeof(WorldRadianceCoefficients)}, {"g_temporal", sizeof(GpuTemporalDispatch)},
                 {"g_previous_field", sizeof(WorldRadianceCoefficients)}}) {
            slang::TypeLayoutReflection *element{};
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == name) {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                require_(element != nullptr and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride));

            if (std::string_view{name} == "g_dispatch") {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    require_(
                        element->getFieldCount() == 8u and
                        std::string_view{element->getFieldByIndex(3u)->getName()} == "refinedSpacing" and
                        element->getFieldByIndex(3u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 12u));
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    require_(
                        std::string_view{element->getFieldByIndex(6u)->getName()} == "receiverCountX" and
                        element->getFieldByIndex(6u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 24u));
            }

            if (std::string_view{name} == "g_discovery") {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    require_(
                        element->getFieldCount() == 8u and
                        element->getFieldByIndex(4u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 16u and
                        element->getFieldByIndex(6u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 24u));
            }

            if (std::string_view{name} == "g_world") {
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(element->getFieldCount() == 24u));
                auto *const response = element->getFieldByIndex(15u);
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, require_(
                                       std::string_view{response->getName()} == "maximumResponse" and
                                       response->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 60u and
                                       response->getTypeLayout()->getScalarType() == slang::TypeReflection::Float32));

                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    require_(
                        element->getFieldByIndex(20u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 80u and
                        element->getFieldByIndex(21u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 84u));

                auto *const metric = element->getFieldByIndex(16u);
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, require_(
                                       std::string_view{metric->getName()} == "cellMetres" and
                                       metric->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 64u and
                                       metric->getTypeLayout()->getScalarType() == slang::TypeReflection::Float32));
            }

            if (std::string_view{name} == "g_bounce") {
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    require_(
                        element->getFieldCount() == 4u and
                        std::string_view{element->getFieldByIndex(2u)->getName()} == "firstRecord" and
                        element->getFieldByIndex(2u)->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == 8u));
            }

            if (std::string_view{name} == "g_overlay") {
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(element->getFieldCount() == 14u));
                for (u32 field = 0u; field < 14u; ++field) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldRadiance, require_(
                                           element->getFieldByIndex(field)->getOffset(
                                               SLANG_PARAMETER_CATEGORY_UNIFORM) == u64{field} * 4u));
                }
            }

            if (std::string_view{name} == "g_materials") {
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(element->getFieldCount() == 13u));
                for (u32 field = 4u; field < 13u; ++field) {
                    auto *const value = element->getFieldByIndex(field);
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldRadiance,
                        require_(
                            value->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == u64{field} * sizeof(float) and
                            value->getTypeLayout()->getScalarType() == slang::TypeReflection::Float32));
                }
            }
        }

        m_state = std::move(state);
        return {};
    }

    // ------------------------------------------------------------------
    // admission before candidate allocation and immutable root construction
    // ------------------------------------------------------------------

    Expected<WorldRadianceDemand> WorldRadiancePass::prepare(
        std::shared_ptr<const WorldRadianceSource> source, const TerrainReceiverCohort cohort,
        const WorldRadianceSettings settings, const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(m_state != nullptr, std::errc::operation_not_permitted));
        auto &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               state.m_owner == std::this_thread::get_id() and
                               not state.m_unknown and
                               not state.m_matter and
                               not state.m_discovery and
                               not state.m_bounce and
                               source != nullptr and
                               cohort.m_job_id != 0u and
                               cohort.m_world_generation != 0u and
                               cohort.m_content_fingerprint != 0u and
                               cohort.m_geometry_revision != 0u));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               not state.m_batch or (not state.m_batch->m_encoded and
                                                        slotFenceComplete(completed, state.m_batch->m_fence)),
                               std::errc::device_or_resource_busy));

        const hal::ProfileScope scope{"WorldRadiance.PrepareCandidate"};
        const auto observe_phase = [&](const char *const phase) {
            if (not settings.m_measure_gpu) {
                return;
            }

            PPR_LOG(
                WorldRadiance, info, "World radiance CPU preparation phase",
                {{"phase", phase}, {"job", cohort.m_job_id}, {"world", cohort.m_world_generation},
                    {"content", cohort.m_content_fingerprint}, {"revision", cohort.m_geometry_revision}});
        };

        observe_phase("prepare.begin");

        const auto &data = *source->m_data;
        const auto &base = data.terrain();

        // GPU overlays are immutable allocations on their original device.
        // CPU-only sources remain portable; no foreign overlay is rebound after
        // replacing only its static terrain buffers on this pass's device.
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(not data.m_projected or data.m_projected->m_base->m_device == state.m_device));

        const auto &limits = state.m_device->getInfo().limits;
        auto planned = plan_(data.m_width, data.m_height, settings, limits.maxComputeDispatchThreadGroups[0]);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, planned);
        Plan plan = std::move(*planned);

        // The first committed empty Matter projection of an already published
        // terrain has identical optics. Preserve the complete field and verify
        // its demanded probe versions at a new GPU fence. Later feedback
        // iterations and any changed demand keep their ordinary ray work.
        const auto &prior = state.m_batch;
        const auto projected_identity = source->matterIdentity();
        const GridWorldIdentity world{cohort.m_job_id, cohort.m_world_generation, cohort.m_content_fingerprint};
        if (settings.m_transport == WorldRadianceTransport::temporal_cascades and
            not settings.m_temporal_refresh_window and
            not settings.m_prioritize_convergence and
            prior and
            prior->m_temporal_pool and
            prior->m_published and
            projected_identity and
            projected_identity->m_particles.m_world == world and
            slotFenceComplete(completed, source->matterCompletionFence()) and
            source->overlayCount() == 0u and
            prior->m_generation->cohort() == cohort and
            not prior->m_generation->source()->matterIdentity() and
            prior->m_generation->source()->overlayCount() == 0u and
            source->matchesTerrainSource(*prior->m_generation->source()) and
            prior->m_settings.m_temporal == settings.m_temporal and
            prior->m_settings.m_temporal_history_weight == settings.m_temporal_history_weight and
            prior->m_settings.m_temporal_serial_reference == settings.m_temporal_serial_reference and
            prior->m_settings.m_temporal_gpu_queue == settings.m_temporal_gpu_queue and
            prior->m_settings.m_temporal_skip_table == settings.m_temporal_skip_table and
            prior->m_settings.m_maximum_response == settings.m_maximum_response and
            settings.m_temporal_sample_time_ns >= prior->m_settings.m_temporal_sample_time_ns and
            prior->m_generation->currentReceiverWindow() == plan.m_coverage_window and
            plan.m_coverage_probes.contains(plan.m_gameplay_probes) and
            (plan.m_temporal_regions[0] == plan.m_coverage_probes or
                plan.m_temporal_regions[0] == prior->m_generation->updatedProbeWindow())) {
            plan.m_demand.m_readback_bytes -= plan.m_temporal_work.size() * 8u * sizeof(u32);
            plan.m_temporal_work.clear();
            plan.m_batch_begin = {0u, 0u};
            plan.m_temporal_regions[0] = {};
            plan.m_retained_rows = u64{plan.m_parameters.m_refined_count_x} * plan.m_parameters.m_refined_count_y;
            plan.m_demand.m_expected_rows = plan.m_retained_rows;
            plan.m_demand.m_query_upper_bound = 0u;
        }

        // Invalid settings must leave the previously usable batch untouched.
        if (state.m_batch and state.m_batch->m_submitted and slotFenceComplete(completed, state.m_batch->m_fence)) {
            state.m_batch->m_completed = true;
        }

        auto &parameters = plan.m_parameters;
        parameters.m_palette_count = static_cast<u32>(base.m_materials.size());
        parameters.m_hierarchy_levels = static_cast<u32>(base.m_hierarchy.size());
        parameters.m_overlay_count = source->overlayCount();
        parameters.m_overlay_capacity = source->overlayLookupCapacity();
        parameters.m_overlay_ancestor_words = source->overlayAncestorWords();
        parameters.m_maximum_response = settings.m_maximum_response;
        parameters.m_cell_metres = static_cast<float>(data.m_cell_metres);
        const bool daylight = settings.m_transport == WorldRadianceTransport::temporal_cascades and
                              settings.m_temporal.m_sky_hour >= 0;
        if (daylight) {
            const auto hour = settings.m_temporal.m_sky_hour;
            const double angle = double(hour - 6) * (2.0 * std::numbers::pi / 24.0);
            parameters.m_sky_hour_plus_one = static_cast<u32>(hour + 1);
            parameters.m_boundary[0] = static_cast<float>(std::cos(angle));
            parameters.m_boundary[1] = static_cast<float>(std::sin(angle));
            parameters.m_boundary[2] = hour > 6 and hour < 18 ? parameters.m_boundary[1] : 0.0f;
        } else {
            std::ranges::copy(data.m_sky, parameters.m_boundary);
        }


        for (const auto &material: base.m_materials) {
            for (u32 channel = 0u; channel < 3u; ++channel)
                parameters.m_max_source[channel] = std::max(
                    parameters.m_max_source[channel], material.m_source[channel]);
        }

        for (u32 channel = 0u; channel < 3u; ++channel) {
            // The analytic environment is a convex RGB mixture in [0,1].
            // Include its full envelope even when this hour is dark.
            const double far_bound = data.m_far_bound[channel] + (daylight ? 1.0 : 0.0);
            if (daylight) {
                const double crossings = double(data.m_width) + data.m_height + 4.0;
                const double extra_loss = double(std::numeric_limits<float>::min()) * (2.0 + 2.0 * crossings);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(data.m_underflow_loss[channel] + extra_loss <= 1e-5, std::errc::argument_out_of_domain));
            }

            parameters.m_far_bound[channel] = static_cast<float>(far_bound);
            parameters.m_max_source[channel] = std::max(
                parameters.m_max_source[channel], parameters.m_far_bound[channel]);
        }

        auto &demand = plan.m_demand;
        const bool temporal = settings.m_transport == WorldRadianceTransport::temporal_cascades;
        const bool windowed = settings.m_transport == WorldRadianceTransport::direct_window;
        const bool straight = windowed or settings.m_transport == WorldRadianceTransport::straight256_reference;

        std::shared_ptr<const WorldRadianceGeneration> copy_donor;
        if (windowed and state.m_batch and state.m_batch->m_published and state.m_batch->m_completed) {
            const auto &old = *state.m_batch;
            const auto &previous = old.m_settings;
            if (old.m_generation->source().get() == source.get() and
                old.m_generation->cohort() == cohort and
                old.m_generation->matchesDevice(*state.m_device) and
                previous.m_transport == settings.m_transport and
                previous.m_refined_spacing == settings.m_refined_spacing and
                previous.m_phase == settings.m_phase and
                previous.m_cascade_count == settings.m_cascade_count and
                previous.m_maximum_response == settings.m_maximum_response and
                previous.m_diagnostic_coefficients == settings.m_diagnostic_coefficients and
                previous.m_measure_gpu == settings.m_measure_gpu) {
                const auto minimum = old.m_generation->probeMinimum();
                const auto counts = old.m_generation->probeCounts();
                auto &intersection = plan.m_copy_window;
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    intersection.m_minimum[axis] = std::max(minimum[axis], settings.m_receiver_window.m_minimum[axis]);
                    const i32 end = std::min(
                        minimum[axis] + static_cast<i32>(counts[axis]),
                        settings.m_receiver_window.m_minimum[axis] +
                            static_cast<i32>(settings.m_receiver_window.m_counts[axis]));
                    intersection.m_counts[axis] = static_cast<u32>(std::max(0, end - intersection.m_minimum[axis]));
                }

                if (intersection.m_counts[0] != 0u and intersection.m_counts[1] != 0u)
                    copy_donor = old.m_generation;
            }
        }

        plan.m_masks.assign(std::max<std::size_t>(1u, plan.m_pages.size()) * 8u, ~u32{0u});
        if (windowed) {
            const bool sky = std::ranges::any_of(data.m_sky, [](const float value) { return value > 0.0f; });
            demand.m_query_upper_bound = 0u;
            for (u32 index = 0u; index < plan.m_pages.size(); ++index) {
                auto &page = plan.m_pages[index];
                page.m_mask.fill(0u);
                for (u32 bin = 0u; bin < 256u; ++bin) {
                    const bool included = sky or std::ranges::any_of(data.m_emitters, [&](const EmitterBounds &bounds) {
                        return pageMayReach_(page, bounds, plan.m_directions[bin]);
                    });
                    if (not included)
                        continue;

                    page.m_mask[bin / 32u] |= u32{1u} << (bin % 32u);
                    page.m_queries += u64{page.m_count_x} * page.m_count_y;
                }

                const std::size_t mask_offset = std::size_t{index} * 8u;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(mask_offset <= plan.m_masks.size() and 8u <= plan.m_masks.size() - mask_offset));

                std::ranges::copy(page.m_mask, std::span{plan.m_masks}.subspan(mask_offset, 8u).begin());
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_query_upper_bound, page.m_queries));
            }
        }

        if (windowed) {
            demand.m_query_upper_bound = 0u;
            for (u32 index = 0u; index < plan.m_pages.size(); ++index) {
                const auto &page = plan.m_pages[index];
                const auto &overlap = plan.m_copy_window;
                const bool copied = copy_donor and
                                    page.m_min_x >= overlap.m_minimum[0] and
                                    page.m_min_y >= overlap.m_minimum[1] and
                                    page.m_min_x + static_cast<i32>(page.m_count_x) <=
                                        overlap.m_minimum[0] + static_cast<i32>(overlap.m_counts[0]) and
                                    page.m_min_y + static_cast<i32>(page.m_count_y) <=
                                        overlap.m_minimum[1] + static_cast<i32>(overlap.m_counts[1]);
                if (copied)
                    continue;

                // Keep the page mask unchanged. Only dispatch geometry shrinks;
                // each receiver executes the same ordered 256-bin accumulation.
                const u32 directions = static_cast<u32>(page.m_queries / (u64{page.m_count_x} * page.m_count_y));
                const u32 strip_height = std::min(32u, settings.m_receiver_rows_per_quantum / page.m_count_x);
                for (u32 y = 0u; y < page.m_count_y; y += strip_height) {
                    const u32 height = std::min(strip_height, page.m_count_y - y);
                    const u64 queries = u64{page.m_count_x} * height * directions;
                    plan.m_strips.push_back(
                        {queries, page.m_min_x, page.m_min_y + static_cast<i32>(y), page.m_count_x, height, index});
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_query_upper_bound, queries));
                }
            }

            // A wholly covered replacement still needs a real fenced copy and
            // normal statistics/readback/publication settlement, but no tracing.
            if (plan.m_strips.empty())
                plan.m_strips.push_back({});
        }

        // Direct refinement never reads this parent, but reflection requires one
        // valid disjoint binding. Count exactly this 32B dummy, no coarse atlas.
        if (straight) {
            demand.m_parity_bytes[1] = sizeof(GpuInterval);
        } else {
            for (auto &bytes: demand.m_parity_bytes)
                bytes = std::max(bytes, u64{sizeof(GpuInterval)});
        }

        const std::array<u64, 12> source_buffers{
            base.m_packed_ids.size() * sizeof(u32), base.m_materials.size() * sizeof(GpuMaterial),
            base.m_vacuum.size() * sizeof(u32), base.m_hierarchy.size() * sizeof(GpuHierarchy),
            plan.m_levels.size() * sizeof(GpuLevel), plan.m_directions.size() * sizeof(GpuDirection), 8u * sizeof(u32),
            plan.m_masks.size() * sizeof(u32),
            data.m_projected ? source->overlayBuffer()->getDesc().size
                             : std::max<u64>(1u, data.m_overlay.size()) * sizeof(GpuOverlay),
            data.m_projected ? source->overlayLookupBuffer()->getDesc().size
                             : data.m_overlay_lookup.size() * sizeof(u32),
            std::max<u64>(1u, base.m_particle_materials.size()) * sizeof(GpuOriginalMaterial),
            data.m_projected ? source->overlayAncestorBuffer()->getDesc().size : sizeof(u32)};
        for (const u64 bytes: source_buffers) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_source_gpu_bytes, bytes));
            demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, bytes);
        }

        if (temporal) {
            const u64 skip_bytes = settings.m_temporal_skip_table ? u64{data.m_width} * data.m_height : 1u;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_source_gpu_bytes, skip_bytes));
            demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, skip_bytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, add_(demand.m_source_gpu_bytes, plan.m_queue_bytes + 3u * sizeof(u32)));
            demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, plan.m_queue_bytes);
        }

        // Temporal parity charges represent the whole pool; each cascade level
        // and refined slot is a separate resource with its own device limit.
        u64 transport_resource = std::max(demand.m_parity_bytes[0], demand.m_parity_bytes[1]);
        if (temporal) {
            transport_resource = sizeof(GpuInterval);
            for (const auto &level: plan.m_levels)
                transport_resource = std::max(transport_resource, u64{level.m_records} * sizeof(GpuInterval));
        }

        demand.m_largest_resource_bytes = std::max(
            {demand.m_largest_resource_bytes, transport_resource, demand.m_refined_bytes, demand.m_probe_state_bytes,
                demand.m_readback_bytes});
        const u64 own_largest_resource_bytes = demand.m_largest_resource_bytes;

        // Scope: application-owned data, typed protocol buffers, diagnostic host
        // storage and root handles. Opaque native driver/program allocations are
        // caller-reserved, not claimed to be measured or individually bounded.
        demand.m_root_cpu_bytes = sizeof(State::Batch) +
                                  plan.m_gameplay_bytes +
                                  (settings.m_gameplay_demand ? sizeof(WorldRadianceGameplayField) : 0u) +
                                  (temporal ? sizeof(State::TemporalPool) : 0u) +
                                  sizeof(StaticWorldInput) +
                                  sizeof(WorldRadianceGeneration) +
                                  plan.m_levels.capacity() * sizeof(GpuLevel) +
                                  plan.m_directions.capacity() * sizeof(GpuDirection) +
                                  plan.m_levels.size() * sizeof(rhi::ComPtr<rhi::IShaderObject>) +
                                  plan.m_pages.capacity() * sizeof(ReceiverPage) +
                                  plan.m_strips.capacity() * sizeof(ReceiverStrip) +
                                  plan.m_masks.capacity() * sizeof(u32) +
                                  plan.m_temporal_work.capacity() * sizeof(TemporalWork) +
                                  plan.m_batch_begin.capacity() * sizeof(u32) +
                                  plan.m_temporal_work.size() *
                                      (settings.m_temporal_gpu_queue ? 2u : 1u) *
                                      sizeof(rhi::ComPtr<rhi::IShaderObject>) +
                                  plan.m_strips.size() * sizeof(rhi::ComPtr<rhi::IShaderObject>);
        demand.m_candidate_bytes = source->retainedCpuBytes();
        for (const u64 bytes:
            {demand.m_source_gpu_bytes, demand.m_parity_bytes[0], demand.m_parity_bytes[1], demand.m_refined_bytes,
                demand.m_readback_bytes, demand.m_root_cpu_bytes,
                settings.m_diagnostic_coefficients ? demand.m_refined_bytes + demand.m_probe_state_bytes : 0u}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_candidate_bytes, bytes));
        }

        demand.m_peak_bytes = demand.m_candidate_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, state.chargeGameplayFields(demand.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, settings.m_reserve_bytes));
        std::vector<const void *> counted_sources;
        data.markInputs(counted_sources, source.get());
        const auto current_base_source = data.m_terrain ? data.m_terrain : source;
        auto current_static = data.m_projected ? data.m_projected->m_base : state.m_static_input;
        if (current_static and current_static->m_source != current_base_source)
            current_static.reset();

        if (current_static)
            counted_sources.push_back(current_static.get());

        const auto count_source = [&](const WorldRadianceSource &input) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                input.m_data->charge(counted_sources, &input, demand.m_peak_bytes, demand.m_largest_resource_bytes));
            return {};
        };

        if (state.m_batch) {
            // Candidate construction is transactional: every previous buffer/root
            // remains owned until replacement is complete. Multiple field/window
            // leases share one immutable CPU input, counted once by owner identity.
            const auto old_source = state.m_batch->m_generation->source();
            u64 shared_bytes{};
            if (current_static and current_static == state.m_batch->m_static_input)
                shared_bytes = current_static->m_bytes + sizeof(StaticWorldInput);
            if (data.m_projected and data.m_projected == old_source->m_data->m_projected)
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(shared_bytes, data.m_projected->m_bytes));

            const u64 old_bytes = state.m_batch->m_plan.m_demand.m_candidate_bytes;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(old_bytes >= old_source->retainedCpuBytes() + shared_bytes));
            if (old_source->m_data->m_projected) {
                counted_sources.push_back(old_source->m_data->m_projected.get());
                counted_sources.push_back(old_source->m_data->m_projected->m_base.get());
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, add_(demand.m_peak_bytes, old_bytes - old_source->retainedCpuBytes() - shared_bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*old_source));
            demand.m_largest_resource_bytes = std::max(
                demand.m_largest_resource_bytes, state.m_batch->m_own_largest_resource_bytes);
        }
        for (auto iterator = state.m_generations.begin(); iterator != state.m_generations.end();) {
            if (const auto generation = iterator->lock()) {
                if (not state.m_batch or generation != state.m_batch->m_generation) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        add_(demand.m_peak_bytes, generation->bufferBytes() + generation->probeStateBytes()));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*generation->source()));
                    demand.m_largest_resource_bytes = std::max(
                        demand.m_largest_resource_bytes, generation->bufferBytes());
                }
                ++iterator;
            } else {
                iterator = state.m_generations.erase(iterator);
            }
        }

        for (auto iterator = state.m_discoveries.begin(); iterator != state.m_discoveries.end();) {
            if (const auto discovery = iterator->lock()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(
                                       demand.m_peak_bytes,
                                       discovery->bufferBytes() + discovery->m_support_keys.capacity() * sizeof(u32)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(demand.m_peak_bytes, sizeof(WorldRadianceFaceDiscovery)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*discovery->source()));
                demand.m_largest_resource_bytes = std::max(
                    {demand.m_largest_resource_bytes, discovery->m_bits->getDesc().size,
                        discovery->m_keys->getDesc().size});
                ++iterator;
            } else {
                iterator = state.m_discoveries.erase(iterator);
            }
        }

        for (const auto &weak: state.m_projected_sources) {
            if (const auto retained = weak.lock())
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*retained));
        }

        if (state.m_static_input) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                chargeOwner_(
                    counted_sources, state.m_static_input.get(),
                    state.m_static_input->m_bytes + sizeof(StaticWorldInput),
                    std::max(
                        state.m_static_input->m_ids->getDesc().size, state.m_static_input->m_vacuum->getDesc().size),
                    demand.m_peak_bytes, demand.m_largest_resource_bytes));
        }

        demand.m_admission_error = require_(
            demand.m_peak_bytes <= settings.m_budget_bytes and
                demand.m_largest_resource_bytes <= settings.m_max_resource_bytes and
                demand.m_largest_resource_bytes <= limits.maxBufferSize,
            std::errc::no_buffer_space);
        if (demand.m_admission_error)
            return demand;

        if (temporal and not state.m_temporal_programs[0]) {
            std::array<rhi::ComPtr<rhi::IShaderProgram>, 8u> programs;
            std::array<rhi::ComPtr<rhi::IComputePipeline>, 8u> pipelines;
            constexpr std::array names{
                "solveWorldTemporalCascade", "refineWorldTemporal", "refineWorldTemporalSerial", "buildWorldSkipTable",
                "buildWorldTemporalWork", "checkWorldTemporalCoverage", "solveWorldTemporalCascadeParallel",
                "buildWorldConvergenceWork"};
            for (u32 index = 0u; index < names.size(); ++index) {
                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_module->findEntryPointByName(names[index], entry.writeRef()));
                slang::IComponentType *entries[]{entry.get()};
                rhi::ShaderProgramDesc description{};
                description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                description.slangGlobalScope = state.m_module.get();
                description.slangEntryPoints = entries;
                description.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createShaderProgram(
                                       description, programs[index].writeRef(), diagnostics.writeRef()));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = programs[index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createComputePipeline(pipeline, pipelines[index].writeRef()));
            }

            state.m_temporal_programs = std::move(programs);
            state.m_temporal_pipelines = std::move(pipelines);
        }

        if (not temporal and not state.m_programs[0]) {
            // Both original direct/cascade pairs are one immutable family. A
            // partial compile never changes this pass or its previous candidate.
            std::array<rhi::ComPtr<rhi::IShaderProgram>, 2u> programs;
            std::array<rhi::ComPtr<rhi::IComputePipeline>, 2u> pipelines;
            constexpr std::array names{"solveWorldCascade", "refineWorldRadiance"};
            for (u32 index = 0u; index < names.size(); ++index) {
                shader::ComPtr<slang::IEntryPoint> entry;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_module->findEntryPointByName(names[index], entry.writeRef()));
                slang::IComponentType *entries[]{entry.get()};
                rhi::ShaderProgramDesc description{};
                description.linkingStyle = rhi::LinkingStyle::SingleProgram;
                description.slangGlobalScope = state.m_module.get();
                description.slangEntryPoints = entries;
                description.slangEntryPointCount = 1u;
                shader::Diagnose diagnostics;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createShaderProgram(
                                       description, programs[index].writeRef(), diagnostics.writeRef()));

                rhi::ComputePipelineDesc pipeline{};
                pipeline.program = programs[index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createComputePipeline(pipeline, pipelines[index].writeRef()));
            }

            for (u32 index = 0u; index < names.size(); ++index) {
                state.m_programs[index] = std::move(programs[index]);
                state.m_pipelines[index] = std::move(pipelines[index]);
            }
        }

        if (windowed and not state.m_parallel_pipeline) {
            // Keep the new family local until both acquisitions succeed. A
            // failed compile cannot replace a live candidate or legacy pair.
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_module->findEntryPointByName("refineWorldRadianceParallel", entry.writeRef()));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = state.m_module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            rhi::ComPtr<rhi::IShaderProgram> program;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                state.m_device->createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program;
            rhi::ComPtr<rhi::IComputePipeline> compiled;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createComputePipeline(pipeline, compiled.writeRef()));

            state.m_parallel_program = std::move(program);
            state.m_parallel_pipeline = std::move(compiled);
        }

        if (windowed and
            settings.m_measure_gpu and
            (data.m_projected or not data.m_overlay.empty()) and
            (not state.m_batch or state.m_batch->m_generation->source().get() != source.get())) {
            PPR_LOG(
                WorldRadiance, info,
                "Terrain-only CPU face preflight unavailable for overlay source; no future GO or complete face-count claim",
                {{"overlay_rows", u64{source->overlayCount()}}, {"source_width", data.m_width},
                    {"source_height", data.m_height}});
        }

        if (windowed and
            settings.m_measure_gpu and
            not data.m_projected and
            data.m_overlay.empty() and
            (not state.m_batch or state.m_batch->m_generation->source().get() != source.get())) {
            const hal::ProfileScope preflight_scope{"WorldRadiance.PreflightExposedFaces"};
            observe_phase("face-preflight.begin");

            // Census only: no face/value/lookup GPU allocation and no CPU rays.
            // The already validated source admits only vacuum/opaque/absorbing.
            const u32 pages_x = (data.m_width + 31u) / 32u, pages_y = (data.m_height + 31u) / 32u;
            std::vector<std::array<u64, 4u>> page_faces(std::size_t{pages_x} * pages_y);
            std::array<u64, 4u> faces{};
            constexpr std::array<std::array<i32, 2u>, 4u> offsets{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (u32 y = 0u; y < data.m_height; ++y) {
                for (u32 x = 0u; x < data.m_width; ++x) {
                    if (data.m_materials[data.m_ids[std::size_t{y} * data.m_width + x]].m_kind != 1u) {
                        continue;
                    }

                    auto &page = page_faces[std::size_t{y / 32u} * pages_x + x / 32u];
                    for (u32 side = 0u; side < offsets.size(); ++side) {
                        const i32 nx = static_cast<i32>(x) + offsets[side][0];
                        const i32 ny = static_cast<i32>(y) + offsets[side][1];
                        if (nx >= 0 and
                            ny >= 0 and
                            nx < static_cast<i32>(data.m_width) and
                            ny < static_cast<i32>(data.m_height)) {
                            const auto neighbor =
                                data.m_ids[std::size_t{static_cast<u32>(ny)} * data.m_width + static_cast<u32>(nx)];
                            if (data.m_materials[neighbor].m_kind == 1u) {
                                continue;
                            }
                        }

                        ++page[side];
                        ++faces[side];
                    }
                }
            }

            const bool sky = std::ranges::any_of(data.m_sky, [](const float value) { return value > 0.0f; });
            u64 face_count{}, active_pages{}, mask_tests{}, admitted_rays{}, included_page_directions{};
            for (const u64 count: faces) {
                face_count += count;
            }

            for (u32 py = 0u; py < pages_y; ++py) {
                for (u32 px = 0u; px < pages_x; ++px) {
                    const auto &counts = page_faces[std::size_t{py} * pages_x + px];
                    if (std::ranges::all_of(counts, [](const u64 count) { return count == 0u; })) {
                        continue;
                    }

                    ++active_pages;

                    // Existing pageMayReach_ bounds cell CENTRES. This halo
                    // also encloses boundary face centres and empty-side centres.
                    ReceiverPage support{};
                    support.m_min_x = static_cast<i32>(px * 32u) - 1;
                    support.m_min_y = static_cast<i32>(py * 32u) - 1;
                    support.m_count_x = std::min(32u, data.m_width - px * 32u) + 2u;
                    support.m_count_y = std::min(32u, data.m_height - py * 32u) + 2u;

                    std::array<u64, 4u> outward{};
                    for (const auto direction: plan.m_directions) {
                        bool included = sky;
                        for (const auto &emitter: data.m_emitters) {
                            if (included) {
                                break;
                            }

                            ++mask_tests;
                            included = pageMayReach_(support, emitter, direction);
                        }

                        if (not included) {
                            continue;
                        }

                        ++included_page_directions;
                        outward[0] += direction.m_x < 0.0f ? 1u : 0u;
                        outward[1] += direction.m_x > 0.0f ? 1u : 0u;
                        outward[2] += direction.m_y < 0.0f ? 1u : 0u;
                        outward[3] += direction.m_y > 0.0f ? 1u : 0u;
                    }

                    for (u32 side = 0u; side < counts.size(); ++side) {
                        admitted_rays += counts[side] * outward[side];
                    }
                }
            }

            // The same fixed support closure drives discovery preparation;
            // no independent diagnostic sampling geometry is maintained here.
            const auto levels = indirectLevels_(data.m_width, data.m_height, settings.m_receiver_window);
            std::array<u64, 2u> indirect_parity{};
            u64 indirect_records{}, indirect_traces{}, largest_dispatch_records{};
            for (u32 index = 1u; index <= 3u; ++index) {
                const auto &level = levels[index];
                const u64 records = level.m_records;
                const u64 bytes = records * sizeof(GpuInterval);
                const u64 traces = records * (index == 3u ? 1u : 16u);

                indirect_records += records;
                indirect_traces += traces;
                indirect_parity[index % 2u] = std::max(indirect_parity[index % 2u], bytes);
                largest_dispatch_records = std::max(largest_dispatch_records, records);

                const auto message = std::format(
                    "First-bounce CPU preflight level={} minimum=({},{}) counts=({},{}) spacing={} directions={} "
                    "records={} interval_bytes={} connector_trace_upper={} near_cells={} far_cells={} "
                    "job={} world={} revision={} content={} allocation=false measured_GPU_cost=false",
                    index, level.m_min_x, level.m_min_y, level.m_count_x, level.m_count_y, level.m_spacing,
                    level.m_directions, records, bytes, traces, level.m_near, level.m_far, cohort.m_job_id,
                    cohort.m_world_generation, cohort.m_geometry_revision, cohort.m_content_fingerprint);
                PPR_LOG_RAW(WorldRadiance, info, message);
            }

            const u64 native_cells = u64{data.m_width} * data.m_height;
            const u64 values_bytes = 12u * face_count;
            const u64 lookup_bytes = 4u * sizeof(u32) * native_cells;
            const u64 cpu_staging_bytes = values_bytes + lookup_bytes;
            const u64 cpu_census_bytes = page_faces.capacity() * sizeof(std::array<u64, 4u>);

            const u64 window_rows = u64{parameters.m_refined_count_x} * parameters.m_refined_count_y;
            const u64 indirect_window_bytes = window_rows * sizeof(WorldRadianceCoefficients);
            const u64 refined_connector_traces = window_rows * 64u;

            indirect_traces += refined_connector_traces;

            // These are explicit future charges, not live allocations. Charge
            // two complete future bundles plus one full upload staging copy;
            // the existing ledger already counts current/retained producer owners.
            const u64 future_bundle_bytes =
                values_bytes + lookup_bytes + indirect_window_bytes + indirect_parity[0] + indirect_parity[1];
            const u64 future_overlap_bytes = 2u * future_bundle_bytes;

            u64 future_peak_bytes = demand.m_peak_bytes;
            bool future_peak_overflow{};
            for (const u64 bytes: {future_overlap_bytes, cpu_staging_bytes, cpu_census_bytes}) {
                if (const std::error_code error = add_(future_peak_bytes, bytes)) {
                    // A prospective forecast is not current direct admission.
                    // Preserve it as explicit NO_GO, never a wrapped small peak.
                    future_peak_overflow = true;
                    future_peak_bytes = std::numeric_limits<u64>::max();
                    break;
                }
            }

            const u64 future_largest = std::max(
                {values_bytes, lookup_bytes, indirect_window_bytes, indirect_parity[0], indirect_parity[1],
                    demand.m_largest_resource_bytes});
            const bool no_go = future_peak_overflow or
                               future_peak_bytes > settings.m_budget_bytes or
                               future_largest > settings.m_max_resource_bytes or
                               future_largest > limits.maxBufferSize;

            const u64 safe_batch_queries = std::numeric_limits<u32>::max() / u64{parameters.m_max_segments};

            const auto census = std::format(
                "First-bounce CPU preflight source_extent=({},{}) cell_metres={} faces={} "
                "sides_negX_posX_negY_posY=({},{},{},{}) "
                "pages={} active_pages={} emitters={} mask_tests={} included_page_directions={} sky_nonzero={} "
                "outward_unmasked_rays={} conservative_page_masked_outward_rays={} "
                "face_values_bytes={} lookup_bytes={} CPU_upload_staging_bytes={} CPU_census_bytes={} "
                "window_rows={} coarse_interval_records={} indirect_connector_trace_upper={} refined_connector_traces={} "
                "indirect_parity_bytes=({},{}) indirect_window_bytes={} safe_u32_queries_per_batch={} "
                "largest_dispatch_records={} max_dispatch_records={} "
                "OWNED_producer_ledger_bytes={} external_reserve_ESTIMATE_bytes={} "
                "future_two_bundle_overlap_bytes={} future_peak_including_CPU_staging_bytes={} future_peak_overflow={} "
                "future_largest_resource_bytes={} resource_ceiling={} budget_bytes={} future_admission={} "
                "job={} world={} revision={} content={} "
                "mask_is_conservative=true exact_visibility=false allocated_face_GPU_bytes=0 "
                "GPU_time_UNMEASURED=true full_live_VRAM_UNMEASURED=true product_budget_qualified=false",
                data.m_width, data.m_height, data.m_cell_metres, face_count, faces[0], faces[1], faces[2], faces[3],
                u64{pages_x} * pages_y, active_pages, static_cast<u64>(data.m_emitters.size()), mask_tests,
                included_page_directions, sky, face_count * 128u, admitted_rays, values_bytes, lookup_bytes,
                cpu_staging_bytes, cpu_census_bytes, window_rows, indirect_records, indirect_traces,
                refined_connector_traces, indirect_parity[0], indirect_parity[1], indirect_window_bytes,
                safe_batch_queries, largest_dispatch_records, u64{limits.maxComputeDispatchThreadGroups[0]} * 64u,
                demand.m_peak_bytes - settings.m_reserve_bytes, settings.m_reserve_bytes, future_overlap_bytes,
                future_peak_bytes, future_peak_overflow, future_largest, settings.m_max_resource_bytes,
                settings.m_budget_bytes, no_go ? "NO_GO" : "WITHIN_MODELED_LIMITS_UNQUALIFIED", cohort.m_job_id,
                cohort.m_world_generation, cohort.m_geometry_revision, cohort.m_content_fingerprint);
            PPR_LOG_RAW(WorldRadiance, info, census);

            observe_phase("face-preflight.end");
        }

        const auto selected_parameters = parameters;
        const auto selected_demand = demand;
        auto candidate = std::make_unique<State::Batch>();
        candidate->m_plan = std::move(plan);
        candidate->m_work_end = static_cast<u32>(candidate->m_plan.m_temporal_work.size());
        candidate->m_settings = settings;
        if (temporal and settings.m_temporal_frame_allowance_ms > 0.0f)
            candidate->m_setup_step = 0u;
        candidate->m_copy_donor = std::move(copy_donor);
        candidate->m_own_largest_resource_bytes = own_largest_resource_bytes;
        candidate->m_generation = std::shared_ptr<WorldRadianceGeneration>{new WorldRadianceGeneration{}};
        auto &generation = *candidate->m_generation;
        generation.m_refresh_yielded = candidate->m_plan.m_refresh_displaced;
        generation.m_source = std::move(source);
        generation.m_device = state.m_device;
        generation.m_cohort = cohort;
        generation.m_probe_min = {selected_parameters.m_refined_min_x, selected_parameters.m_refined_min_y};
        generation.m_probe_counts = {selected_parameters.m_refined_count_x, selected_parameters.m_refined_count_y};
        generation.m_updated_probe_window = temporal ? candidate->m_plan.m_temporal_regions[0]
                                                     : WorldRadianceReceiverWindow{
                                                           generation.m_probe_min, generation.m_probe_counts};
        if (settings.m_prioritize_convergence)
            generation.m_updated_probe_window = {}; // The GPU selects individual rows inside the certified region.
        generation.m_refined_spacing = temporal ? static_cast<WorldRadianceRefinedSpacing>(
                                                      settings.m_temporal.m_refined_spacing)
                                                : settings.m_refined_spacing;
        generation.m_transport = settings.m_transport;
        generation.m_sky_hour = temporal ? settings.m_temporal.m_sky_hour : -1;
        generation.m_maximum_response = settings.m_maximum_response;
        generation.m_bytes = selected_demand.m_refined_bytes;
        generation.m_statistics.m_query_upper_bound = selected_demand.m_query_upper_bound;
        generation.m_statistics.m_expected_rows = selected_demand.m_expected_rows;

        // Includes device allocation and initial source uploads; it is outside
        // the existing encoded-producer GPU timestamp pair.
        observe_phase("buffers-and-uploads.begin");

        const auto static_source = data.m_terrain ? data.m_terrain : generation.m_source;
        auto shared_static = data.m_projected ? data.m_projected->m_base : state.m_static_input;
        if (shared_static and (shared_static->m_source != static_source or shared_static->m_device != state.m_device))
            shared_static.reset();

        if (shared_static) {
            candidate->m_static_input = shared_static;
            candidate->m_ids = shared_static->m_ids;
            candidate->m_materials = shared_static->m_materials;
            candidate->m_vacuum = shared_static->m_vacuum;
            candidate->m_hierarchy = shared_static->m_hierarchy;
        } else {
            auto owned = std::make_shared<StaticWorldInput>();
            owned->m_source = static_source;
            owned->m_device = state.m_device;
            const GpuOriginalMaterial unused{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                buffer_(
                    *state.m_device, source_buffers[0], sizeof(u32), base.m_packed_ids.data(), false, owned->m_ids));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, source_buffers[1], sizeof(GpuMaterial), base.m_materials.data(),
                                   false, owned->m_materials));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                buffer_(*state.m_device, source_buffers[2], sizeof(u32), base.m_vacuum.data(), false, owned->m_vacuum));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, source_buffers[3], sizeof(GpuHierarchy), base.m_hierarchy.data(),
                                   false, owned->m_hierarchy));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, source_buffers[10], sizeof(GpuOriginalMaterial),
                                   base.m_particle_materials.empty() ? &unused : base.m_particle_materials.data(),
                                   false, owned->m_particle_materials));
            owned->m_bytes =
                source_buffers[0] + source_buffers[1] + source_buffers[2] + source_buffers[3] + source_buffers[10];
            candidate->m_static_input = owned;
            candidate->m_ids = owned->m_ids;
            candidate->m_materials = owned->m_materials;
            candidate->m_vacuum = owned->m_vacuum;
            candidate->m_hierarchy = owned->m_hierarchy;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, source_buffers[4], sizeof(GpuLevel), candidate->m_plan.m_levels.data(),
                               false, candidate->m_levels));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, source_buffers[5], sizeof(GpuDirection),
                               candidate->m_plan.m_directions.data(), false, candidate->m_directions));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, source_buffers[6], sizeof(u32), nullptr, false, candidate->m_counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, source_buffers[7], sizeof(u32), candidate->m_plan.m_masks.data(), false,
                               candidate->m_masks));
        if (data.m_projected) {
            candidate->m_overlay = generation.m_source->overlayBuffer();
            candidate->m_overlay_lookup = generation.m_source->overlayLookupBuffer();
            candidate->m_overlay_ancestors = generation.m_source->overlayAncestorBuffer();
        } else {
            const GpuOverlay empty_overlay{};
            const u32 zero{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                buffer_(
                    *state.m_device, source_buffers[8], sizeof(GpuOverlay),
                    data.m_overlay.empty() ? &empty_overlay : data.m_overlay.data(), false, candidate->m_overlay));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, source_buffers[9], sizeof(u32), data.m_overlay_lookup.data(), false,
                                   candidate->m_overlay_lookup));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                buffer_(*state.m_device, sizeof(zero), sizeof(u32), &zero, false, candidate->m_overlay_ancestors));
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, selected_demand.m_readback_bytes, 0u, nullptr, true, candidate->m_readback));
        if (temporal) {
            auto pool = state.m_temporal_pool;
            if (pool and (pool->m_width != data.m_width or
                             pool->m_height != data.m_height or
                             pool->m_identity.m_world_generation != cohort.m_world_generation or
                             pool->m_identity.m_content_fingerprint != cohort.m_content_fingerprint or
                             pool->m_settings.m_refined_spacing != settings.m_temporal.m_refined_spacing or
                             pool->m_settings.m_cascade_count != settings.m_temporal.m_cascade_count or
                             pool->m_settings.m_base_directions != settings.m_temporal.m_base_directions or
                             pool->m_settings.m_interval_scale != settings.m_temporal.m_interval_scale)) {
                pool.reset();
            }

            if (not pool) {
                pool = std::make_shared<State::TemporalPool>();
                pool->m_width = data.m_width;
                pool->m_height = data.m_height;
                pool->m_identity = cohort;
                pool->m_settings = settings.m_temporal;
                for (u32 index = 1u; index < candidate->m_plan.m_levels.size(); ++index) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        buffer_(
                            *state.m_device, u64{candidate->m_plan.m_levels[index].m_records} * sizeof(GpuInterval),
                            sizeof(GpuInterval), nullptr, false, pool->m_levels[index]));
                }

                for (auto &field: pool->m_fields) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, buffer_(
                                           *state.m_device, selected_demand.m_refined_bytes,
                                           sizeof(WorldRadianceCoefficients), nullptr, false, field));
                }

                for (auto &probe_states: pool->m_probe_states) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, buffer_(
                                           *state.m_device, selected_demand.m_probe_state_bytes,
                                           sizeof(WorldRadianceProbeState), nullptr, false, probe_states));
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    buffer_(*state.m_device, sizeof(GpuInterval), sizeof(GpuInterval), nullptr, false, pool->m_dummy));
            } else if (state.m_batch and state.m_batch->m_temporal_pool == pool) {
                // A terrain edit may supersede an unfinished window. Only its
                // already published input is eligible to seed the new revision.
                const auto &previous = state.m_batch->m_published ? state.m_batch->m_generation
                                                                  : state.m_batch->m_copy_donor;
                const bool same_lattice = previous and
                                          previous->cohort().m_job_id == cohort.m_job_id and
                                          previous->source()->minimumMetres() ==
                                              generation.m_source->minimumMetres() and
                                          previous->source()->cellMetres() == generation.m_source->cellMetres();
                if (same_lattice) {
                    candidate->m_copy_donor = previous;
                }
            }

            u32 slot = 0u;
            if (state.m_batch and state.m_batch->m_temporal_pool == pool and not state.m_batch->m_published) {
                // prepare() already established completion of the old batch's last
                // fence. Its unpublished output has never escaped through acquire().
                slot = state.m_batch->m_temporal_slot;
            } else {
                while (slot < pool->m_fields.size() and not pool->m_leases[slot].expired())
                    ++slot;
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(slot < pool->m_fields.size(), std::errc::device_or_resource_busy));
            candidate->m_temporal_slot = slot;
            candidate->m_temporal_pool = pool;
            generation.m_coefficients = pool->m_fields[slot];
            generation.m_probe_states = pool->m_probe_states[slot];
            const bool same_source = candidate->m_copy_donor and
                                     candidate->m_copy_donor->skyHour() == generation.skyHour() and
                                     (candidate->m_copy_donor->source() == generation.m_source or
                                         candidate->m_plan.m_temporal_work.empty() or
                                         (candidate->m_copy_donor->cohort() == cohort and
                                             generation.m_source->matchesProjectedOptics(
                                                 *candidate->m_copy_donor->source())));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   same_source or state.m_next_temporal_version < std::numeric_limits<u64>::max(),
                                   std::errc::value_too_large));
            if (same_source) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(
                        settings.m_temporal_sample_time_ns >= state.m_batch->m_settings.m_temporal_sample_time_ns));
            }
            generation.m_transport_version = same_source ? candidate->m_copy_donor->transportVersion()
                                                         : state.m_next_temporal_version;
        } else {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, selected_demand.m_refined_bytes, sizeof(WorldRadianceCoefficients),
                                   nullptr, false, generation.m_coefficients));
        }

        for (u32 parity = 0u; not temporal and parity < 2u; ++parity) {
            if (selected_demand.m_parity_bytes[parity] == 0u)
                continue;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, selected_demand.m_parity_bytes[parity], sizeof(GpuInterval),
                                   nullptr, false, candidate->m_parity[parity]));
        }

        observe_phase("buffers-and-uploads.end");

        observe_phase("roots-and-timing-query.begin");

        const auto bind = [&](rhi::IShaderObject *const root, const GpuDispatch dispatch,
                              const bool refine) -> std::error_code {
            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, cursor["g_world"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, constants.setData(&candidate->m_plan.m_parameters, sizeof(GpuParameters)));
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, cursor["g_dispatch"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, constants.setData(&dispatch, sizeof(dispatch)));
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 11>{
                     std::pair{"g_overlay", candidate->m_overlay.get()},
                     {"g_overlay_lookup", candidate->m_overlay_lookup.get()},
                     {"g_overlay_ancestors", candidate->m_overlay_ancestors.get()},
                     {"g_cell_ids", candidate->m_ids.get()}, {"g_materials", candidate->m_materials.get()},
                     {"g_vacuum", candidate->m_vacuum.get()}, {"g_hierarchy", candidate->m_hierarchy.get()},
                     {"g_levels", candidate->m_levels.get()}, {"g_directions", candidate->m_directions.get()},
                     {"g_counts", candidate->m_counts.get()}, {"g_page_masks", candidate->m_masks.get()}}) {
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, cursor[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            }

            // The inactive top branch still has a valid, DISJOINT parent SRV.
            // No active SRV/UAV alias is bound to the same parity resource.
            auto *const parent = temporal
                                     ? (refine ? candidate->m_temporal_pool->m_levels[1].get()
                                           : dispatch.m_parent_level == dispatch.m_level
                                               ? candidate->m_temporal_pool->m_dummy.get()
                                               : candidate->m_temporal_pool->m_levels[dispatch.m_parent_level].get())
                                     : candidate->m_parity[refine ? 1u : ((dispatch.m_level + 1u) % 2u)].get();
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, cursor["g_parent"].setBinding(rhi::Binding(parent, range_(parent))));
            if (refine) {
                auto *const output = generation.m_coefficients.get();
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, cursor["g_refined"].setBinding(rhi::Binding(output, range_(output))));
            } else {
                auto *const output = temporal ? candidate->m_temporal_pool->m_levels[dispatch.m_level].get()
                                              : candidate->m_parity[dispatch.m_level % 2u].get();
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance, cursor["g_current"].setBinding(rhi::Binding(output, range_(output))));
            }
            return {};
        };

        if (temporal) {
            rhi::FormatSupport support{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->getFormatSupport(rhi::Format::R8Uint, &support));
            const u32 required = static_cast<u32>(rhi::FormatSupport::Texture) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderLoad) |
                                 static_cast<u32>(rhi::FormatSupport::ShaderUavStore);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_((static_cast<u32>(support) & required) == required, std::errc::not_supported));

            const bool build_skip = settings.m_temporal_skip_table and not candidate->m_plan.m_temporal_work.empty();
            rhi::TextureDesc texture{};
            texture.type = rhi::TextureType::Texture2D;
            texture.size = {
                .width = build_skip ? data.m_width : 1u, .height = build_skip ? data.m_height : 1u, .depth = 1u};
            texture.format = rhi::Format::R8Uint;
            texture.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::ShaderResource) |
                static_cast<u32>(rhi::TextureUsage::UnorderedAccess));
            texture.defaultState = rhi::ResourceState::ShaderResource;
            texture.label = "World temporal source-local R8UI skip table";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createTexture(texture, nullptr, candidate->m_skip_table.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, candidate->m_skip_table->getDefaultView(candidate->m_skip_view.writeRef()));

            if (build_skip) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createRootShaderObject(
                                       state.m_temporal_programs[3], candidate->m_skip_root.writeRef()));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    bind(candidate->m_skip_root, candidate->m_plan.m_temporal_work.front().m_dispatch, false));
                rhi::ShaderCursor cursor{candidate->m_skip_root};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_skip_table_output"].setBinding(rhi::Binding(candidate->m_skip_view)));
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, candidate->m_plan.m_queue_bytes, sizeof(u32), nullptr, false,
                                   candidate->m_temporal_queue));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, 3u * sizeof(u32), sizeof(u32), nullptr, false,
                                   candidate->m_temporal_arguments, true));
            if (settings.m_temporal_gpu_queue)
                candidate->m_temporal_queue_roots.resize(candidate->m_plan.m_temporal_work.size());

            candidate->m_temporal_roots.resize(candidate->m_plan.m_temporal_work.size());
            for (u32 index = 0u; index < candidate->m_temporal_roots.size(); ++index) {
                auto &root = candidate->m_temporal_roots[index];
                auto work = candidate->m_plan.m_temporal_work[index];
                const bool refine = work.m_dispatch.m_level == 0u;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createRootShaderObject(
                                       state.m_temporal_programs
                                           [refine ? (settings.m_temporal_serial_reference ? 2u : 1u)
                                               : work.m_temporal.m_work_group_size == 16u ? 6u
                                                                                          : 0u],
                                       root.writeRef()));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind(root, work.m_dispatch, refine));

                rhi::ShaderCursor cursor{root}, constants{};
                work.m_temporal.m_feedback_available = candidate->m_copy_donor ? 1u : 0u;
                work.m_temporal.m_skip_table = settings.m_temporal_skip_table ? 1u : 0u;
                work.m_temporal.m_source_version = {
                    static_cast<u32>(generation.m_transport_version),
                    static_cast<u32>(generation.m_transport_version >> 32u)};
                work.m_temporal.m_sample_time_ns = {
                    static_cast<u32>(settings.m_temporal_sample_time_ns),
                    static_cast<u32>(settings.m_temporal_sample_time_ns >> 32u)};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_skip_table"].setBinding(rhi::Binding(candidate->m_skip_view)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_temporal"].getDereferenced(constants));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, constants.setData(&work.m_temporal, sizeof(GpuTemporalDispatch)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_temporal_work"].setBinding(
                                       rhi::Binding(candidate->m_temporal_queue, range_(candidate->m_temporal_queue))));
                if (settings.m_temporal_gpu_queue) {
                    auto &queue_root = candidate->m_temporal_queue_roots[index];
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        state.m_device->createRootShaderObject(
                            state.m_temporal_programs[work.m_prioritize ? 7u : 4u], queue_root.writeRef()));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind(queue_root, work.m_dispatch, refine));

                    rhi::ShaderCursor queue_cursor{queue_root}, queue_constants{};
                    if (work.m_prioritize) {
                        auto *const history = candidate->m_copy_donor
                                                  ? candidate->m_copy_donor->probeStateBuffer()
                                                  : candidate->m_temporal_pool
                                                        ->m_probe_states[(candidate->m_temporal_slot + 1u) % 3u]
                                                        .get();
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance,
                            queue_cursor["g_previous_probe_state"].setBinding(rhi::Binding(history, range_(history))));
                    }

                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, queue_cursor["g_temporal"].getDereferenced(queue_constants));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, queue_constants.setData(&work.m_temporal, sizeof(GpuTemporalDispatch)));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        queue_cursor["g_temporal_work_output"].setBinding(
                            rhi::Binding(candidate->m_temporal_queue, range_(candidate->m_temporal_queue))));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        queue_cursor["g_temporal_arguments"].setBinding(
                            rhi::Binding(candidate->m_temporal_arguments, range_(candidate->m_temporal_arguments))));
                }

                auto *const previous =
                    candidate->m_copy_donor
                        ? candidate->m_copy_donor->coefficientsBuffer()
                        : candidate->m_temporal_pool->m_fields[(candidate->m_temporal_slot + 1u) % 3u].get();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_previous_field"].setBinding(rhi::Binding(previous, range_(previous))));
                auto *const previous_states =
                    candidate->m_copy_donor
                        ? candidate->m_copy_donor->probeStateBuffer()
                        : candidate->m_temporal_pool->m_probe_states[(candidate->m_temporal_slot + 1u) % 3u].get();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_previous_probe_state"].setBinding(
                                       rhi::Binding(previous_states, range_(previous_states))));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_probe_state"].setBinding(
                                       rhi::Binding(generation.m_probe_states, range_(generation.m_probe_states))));
            }
            const std::array coverage_windows{candidate->m_plan.m_coverage_probes, candidate->m_plan.m_gameplay_probes};
            for (std::size_t index = 0u; index < coverage_windows.size(); ++index) {
                const auto &coverage = coverage_windows[index];
                if (index > 0u and coverage_windows[0].contains(coverage))
                    break;
                auto &root = candidate->m_coverage_roots[index];
                const GpuDispatch coverage_dispatch{
                    0u, 1u, coverage.m_counts[0] * coverage.m_counts[1], settings.m_temporal.m_refined_spacing,
                    coverage.m_minimum[0], coverage.m_minimum[1], coverage.m_counts[0], coverage.m_counts[1]};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    state.m_device->createRootShaderObject(state.m_temporal_programs[5], root.writeRef()));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind(root, coverage_dispatch, true));

                GpuTemporalDispatch coverage_parameters{};
                coverage_parameters.m_source_version = {
                    static_cast<u32>(generation.m_transport_version),
                    static_cast<u32>(generation.m_transport_version >> 32u)};
                rhi::ShaderCursor coverage_cursor{root}, coverage_constants{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, coverage_cursor["g_temporal"].getDereferenced(coverage_constants));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, coverage_constants.setData(&coverage_parameters, sizeof(coverage_parameters)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, coverage_cursor["g_previous_probe_state"].setBinding(
                                       rhi::Binding(generation.m_probe_states, range_(generation.m_probe_states))));
            }
        } else {
            candidate->m_roots.resize(candidate->m_plan.m_levels.size());
            for (u32 level = 1u; level < candidate->m_plan.m_levels.size(); ++level) {
                auto &root = candidate->m_roots[level];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, state.m_device->createRootShaderObject(state.m_programs[0], root.writeRef()));
                const bool top = level + 1u == candidate->m_plan.m_levels.size();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    bind(
                        root, {level, top ? level : level + 1u, candidate->m_plan.m_levels[level].m_records, 0u},
                        false));
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                state.m_device->createRootShaderObject(state.m_programs[1], candidate->m_refine_root.writeRef()));
            const u32 refined_count = selected_parameters.m_refined_count_x * selected_parameters.m_refined_count_y;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                bind(
                    candidate->m_refine_root,
                    {straight ? 1u : 0u, 1u, refined_count, static_cast<u32>(settings.m_refined_spacing)}, true));

            if (windowed) {
                candidate->m_page_roots.resize(candidate->m_plan.m_strips.size());
                for (u32 index = 0u; index < candidate->m_plan.m_strips.size(); ++index) {
                    const auto &page = candidate->m_plan.m_strips[index];
                    if (page.m_count_x == 0u)
                        continue;

                    auto &root = candidate->m_page_roots[index];
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        state.m_device->createRootShaderObject(state.m_parallel_program, root.writeRef()));

                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind(
                                           root,
                                           {2u, page.m_page, page.m_count_x * page.m_count_y, 1u, page.m_min_x,
                                               page.m_min_y, page.m_count_x, page.m_count_y},
                                           true));
                }
            }
        }

        candidate->m_timing.m_cohort = cohort;
        candidate->m_timing.m_status = WorldRadianceTimingStatus::disabled;
        if (settings.m_measure_gpu) {
            const auto frequency = state.m_device->getInfo().timestampFrequency;
            if (frequency == 0u) {
                candidate->m_timing.m_status = WorldRadianceTimingStatus::unsupported;
                candidate->m_timing.m_error = std::make_error_code(std::errc::not_supported);
            } else {
                rhi::QueryPoolDesc queries{};
                queries.type = rhi::QueryType::Timestamp;
                queries.count = temporal   ? candidate->m_plan.setupQueryOffset() + 4u
                                : windowed ? static_cast<u32>(candidate->m_page_roots.size()) * 2u
                                           : 2u;
                queries.label = "World radiance producer batch timing";
                candidate->m_timing.m_error = make_error_code(
                    state.m_device->createQueryPool(queries, candidate->m_queries.writeRef()));
                if (not candidate->m_timing.m_error and not candidate->m_queries) {
                    candidate->m_timing.m_error = std::make_error_code(std::errc::io_error);
                }

                candidate->m_timing.m_status = candidate->m_timing.m_error ? WorldRadianceTimingStatus::failed
                                                                           : WorldRadianceTimingStatus::pending;
                if (candidate->m_queries and not candidate->m_timing.m_error) {
                    candidate->m_timing.m_error = make_error_code(candidate->m_queries->reset(0u, queries.count));
                    if (candidate->m_timing.m_error) {
                        candidate->m_timing.m_status = WorldRadianceTimingStatus::failed;
                    }
                }

                if (candidate->m_timing.m_error) {
                    candidate->m_queries.setNull(); // No command has referenced this optional pool.
                }
            }
        }

        if (temporal and settings.m_temporal_frame_allowance_ms > 0.0f) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   candidate->m_queries != nullptr and
                                       candidate->m_timing.m_status == WorldRadianceTimingStatus::pending,
                                   std::errc::not_supported));
        }

        observe_phase("roots-and-timing-query.end");

        if (state.m_batch) {
            auto previous = pollTiming(state.m_batch->m_generation->cohort(), completed);
            if (previous.m_status == WorldRadianceTimingStatus::pending) {
                previous.m_status = WorldRadianceTimingStatus::retired_unresolved;
                previous.m_error = std::make_error_code(std::errc::operation_canceled);
            }

            state.m_retired_timing = previous;
        }

        // No submitted commands reference the candidate yet. All fallible buffer
        // and root work succeeded before replacement of the previous prepared batch.
        const WorldRadianceDemand accepted = candidate->m_plan.m_demand;
        if (temporal) {
            auto &pool = *candidate->m_temporal_pool;
            const u32 mode = (settings.m_temporal_skip_table ? 1u : 0u) |
                             (settings.m_temporal_serial_reference ? 2u : 0u) |
                             (settings.m_temporal_gpu_queue ? 4u : 0u);
            if (pool.m_cost_mode != mode) {
                pool.m_costs = {};
                pool.m_overhead = {};
                pool.m_last_budgeted_queries = 0u;
                pool.m_cost_mode = mode;
            }
            if (generation.m_transport_version == state.m_next_temporal_version)
                ++state.m_next_temporal_version;
            candidate->m_temporal_pool->m_leases[candidate->m_temporal_slot] = candidate->m_generation;
            state.m_temporal_pool = candidate->m_temporal_pool;
        }

        state.m_static_input = candidate->m_static_input;
        state.m_batch = std::move(candidate);

        observe_phase("prepare.end");
        return accepted;
    }

    // ------------------------------------------------------------------
    // same-queue encoding, explicit submit fate and immutable publication
    // ------------------------------------------------------------------

    std::error_code WorldRadiancePass::encode(const PreRenderContext &context, const u64 actual_frame) {
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(m_state != nullptr, std::errc::operation_not_permitted));
        auto &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               state.m_owner == std::this_thread::get_id() and
                                   not state.m_unknown and
                                   not state.m_discovery and
                                   not state.m_bounce and
                                   state.m_batch and
                                   state.m_device == &context.m_device,
                               std::errc::operation_not_permitted));
        auto &batch = *state.m_batch;
        const bool windowed = batch.m_settings.m_transport == WorldRadianceTransport::direct_window;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               not batch.m_encoded and
                                   not batch.m_submitted and
                                   (not windowed or batch.m_page < batch.m_page_roots.size()),
                               std::errc::operation_in_progress));

        if (batch.m_temporal_pool) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                require_(batch.m_page < batch.m_plan.temporalBatches(), std::errc::operation_in_progress));
            auto &encoder = context.m_encoder;
            auto &pool = *batch.m_temporal_pool;
            batch.m_encoded = true;
            batch.m_actual_frame = actual_frame;
            if (batch.m_queries and batch.m_setup_step == 2u)
                encoder.writeTimestamp(batch.m_queries, 2u * batch.m_page);

            const u32 setup_query = batch.m_plan.setupQueryOffset();
            const auto preserve_field = [&]() -> std::error_code {
                // Required cascade rows are overwritten in parent-before-child order.
                // Full refinement overwrites every output row; unavailable feedback is
                // never read. Only the dummy binding needs pool-wide initialization.
                if (not pool.m_initialized) {
                    encoder.setBufferState(pool.m_dummy, rhi::ResourceState::CopyDestination);
                    encoder.clearBuffer(pool.m_dummy, 0u, pool.m_dummy->getDesc().size);
                }

                if (batch.m_plan.m_retained_rows > 0u or
                    batch.m_settings.m_prioritize_convergence or
                    batch.m_settings.m_temporal_refresh_window) {
                    auto *const output = batch.m_generation->coefficientsBuffer();
                    encoder.setBufferState(output, rhi::ResourceState::CopyDestination);
                    if (batch.m_copy_donor) {
                        auto *const donor = batch.m_copy_donor->coefficientsBuffer();
                        encoder.setBufferState(donor, rhi::ResourceState::CopySource);
                        encoder.copyBuffer(output, 0u, donor, 0u, batch.m_generation->bufferBytes());

                        auto *const state_output = batch.m_generation->probeStateBuffer();
                        auto *const state_donor = batch.m_copy_donor->probeStateBuffer();
                        encoder.setBufferState(state_output, rhi::ResourceState::CopyDestination);
                        encoder.setBufferState(state_donor, rhi::ResourceState::CopySource);
                        encoder.copyBuffer(state_output, 0u, state_donor, 0u, batch.m_generation->probeStateBytes());

                        // Presentation may consume the donor between setup submissions.
                        encoder.setBufferState(donor, rhi::ResourceState::ShaderResource);
                        encoder.setBufferState(state_donor, rhi::ResourceState::ShaderResource);
                    } else {
                        // Cold and recycled sparse slots must publish zero for unknown rows.
                        encoder.clearBuffer(output, 0u, batch.m_generation->bufferBytes());
                        auto *const state_output = batch.m_generation->probeStateBuffer();
                        encoder.setBufferState(state_output, rhi::ResourceState::CopyDestination);
                        encoder.clearBuffer(state_output, 0u, batch.m_generation->probeStateBytes());
                    }
                }
                return {};
            };

            const auto prepare_source = [&](const bool build_skip) -> std::error_code {
                for (auto *const buffer:
                    {batch.m_overlay.get(), batch.m_overlay_lookup.get(), batch.m_overlay_ancestors.get(),
                        batch.m_ids.get(), batch.m_materials.get(), batch.m_vacuum.get(), batch.m_hierarchy.get(),
                        batch.m_levels.get(), batch.m_directions.get(), batch.m_masks.get(), pool.m_dummy.get()}) {
                    encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);
                }

                auto *const previous = batch.m_copy_donor ? batch.m_copy_donor->coefficientsBuffer()
                                                          : pool.m_fields[(batch.m_temporal_slot + 1u) % 3u].get();
                encoder.setBufferState(previous, rhi::ResourceState::ShaderResource);
                auto *const previous_states = batch.m_copy_donor
                                                  ? batch.m_copy_donor->probeStateBuffer()
                                                  : pool.m_probe_states[(batch.m_temporal_slot + 1u) % 3u].get();
                encoder.setBufferState(previous_states, rhi::ResourceState::ShaderResource);
                encoder.setBufferState(batch.m_readback, rhi::ResourceState::CopyDestination);
                const rhi::SubresourceRange skip_range{.layer = 0u, .layerCount = 1u, .mip = 0u, .mipCount = 1u};
                if (build_skip and batch.m_skip_root) {
                    const hal::ProfileScope build_scope{"WorldRadiance.BuildSkipTable"};
                    encoder.setTextureState(batch.m_skip_table, skip_range, rhi::ResourceState::UnorderedAccess);
                    auto *const pass = encoder.beginComputePass();
                    PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
                    pass->bindPipeline(state.m_temporal_pipelines[3], batch.m_skip_root);
                    pass->dispatchCompute(
                        (batch.m_plan.m_parameters.m_width + 7u) / 8u, (batch.m_plan.m_parameters.m_height + 7u) / 8u,
                        1u);
                    pass->end();

                    encoder.globalBarrier();
                }

                encoder.setTextureState(batch.m_skip_table, skip_range, rhi::ResourceState::ShaderResource);
                return {};
            };

            if (batch.m_setup_step < 2u) {
                // Setup completes through the same accepted-submit fence as ray work.
                // No field is publishable until both steps and all required rays finish.
                encoder.writeTimestamp(batch.m_queries, setup_query + 2u * batch.m_setup_step);
                if (batch.m_setup_step == 0u) {
                    PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, preserve_field());
                } else {
                    PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, prepare_source(true));
                }
                encoder.writeTimestamp(batch.m_queries, setup_query + 2u * batch.m_setup_step + 1u);
                return {};
            }

            const bool inline_setup = batch.m_page == 0u and batch.m_settings.m_temporal_frame_allowance_ms == 0.0f;
            if (inline_setup) {
                if (batch.m_queries)
                    encoder.writeTimestamp(batch.m_queries, setup_query);
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, preserve_field());
                if (batch.m_queries) {
                    encoder.writeTimestamp(batch.m_queries, setup_query + 1u);
                    encoder.writeTimestamp(batch.m_queries, setup_query + 2u);
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, prepare_source(inline_setup));
            if (inline_setup and batch.m_queries)
                encoder.writeTimestamp(batch.m_queries, setup_query + 3u);

            const hal::ProfileScope scope{"WorldRadiance.EncodeTemporalPhase"};
            for (u32 index = batch.m_plan.m_batch_begin[batch.m_page];
                index < batch.m_plan.m_batch_begin[batch.m_page + 1u]; ++index) {
                const auto &work = batch.m_plan.m_temporal_work[index];
                const bool refine = work.m_dispatch.m_level == 0u;
                if (batch.m_queries)
                    encoder.writeTimestamp(batch.m_queries, batch.m_plan.stageQueryOffset(index));
                if (batch.m_settings.m_temporal_gpu_queue) {
                    const hal::ProfileScope queue_scope{"WorldRadiance.BuildTemporalWork"};
                    encoder.setBufferState(batch.m_temporal_queue, rhi::ResourceState::UnorderedAccess);
                    encoder.setBufferState(batch.m_temporal_arguments, rhi::ResourceState::UnorderedAccess);
                    auto *const queue_pass = encoder.beginComputePass();
                    PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(queue_pass != nullptr, std::errc::io_error));
                    queue_pass->bindPipeline(
                        state.m_temporal_pipelines[work.m_prioritize ? 7u : 4u], batch.m_temporal_queue_roots[index]);
                    queue_pass->dispatchCompute(work.m_prioritize ? 1u : (work.m_dispatch.m_count + 63u) / 64u, 1u, 1u);
                    queue_pass->end();

                    encoder.globalBarrier();
                    encoder.setBufferState(batch.m_temporal_arguments, rhi::ResourceState::IndirectArgument);
                }
                encoder.setBufferState(batch.m_temporal_queue, rhi::ResourceState::ShaderResource);
                if (batch.m_queries and batch.m_settings.m_prioritize_convergence)
                    encoder.writeTimestamp(batch.m_queries, batch.m_plan.stageQueryOffset(index) + 1u);


                encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopyDestination);
                encoder.clearBuffer(batch.m_counts, 0u, 8u * sizeof(u32));
                encoder.setBufferState(batch.m_counts, rhi::ResourceState::UnorderedAccess);
                auto *const parent = refine ? pool.m_levels[1u].get()
                                     : work.m_dispatch.m_parent_level == work.m_dispatch.m_level
                                         ? pool.m_dummy.get()
                                         : pool.m_levels[work.m_dispatch.m_parent_level].get();
                auto *const output = refine ? batch.m_generation->coefficientsBuffer()
                                            : pool.m_levels[work.m_dispatch.m_level].get();
                encoder.setBufferState(parent, rhi::ResourceState::ShaderResource);
                encoder.setBufferState(output, rhi::ResourceState::UnorderedAccess);
                if (refine)
                    encoder.setBufferState(batch.m_generation->probeStateBuffer(), rhi::ResourceState::UnorderedAccess);
                auto *const pass = encoder.beginComputePass();
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
                pass->bindPipeline(
                    state.m_temporal_pipelines
                        [refine ? (batch.m_settings.m_temporal_serial_reference ? 2u : 1u)
                            : work.m_temporal.m_work_group_size == 16u ? 6u
                                                                       : 0u],
                    batch.m_temporal_roots[index]);
                if (batch.m_settings.m_temporal_gpu_queue)
                    pass->dispatchComputeIndirect({batch.m_temporal_arguments, 0u});
                else
                    pass->dispatchCompute(
                        (work.m_dispatch.m_count + work.m_temporal.m_work_group_size - 1u) /
                            work.m_temporal.m_work_group_size,
                        1u, 1u);
                pass->end();

                encoder.globalBarrier();
                encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
                encoder.copyBuffer(
                    batch.m_readback, u64{index} * 8u * sizeof(u32), batch.m_counts, 0u, 8u * sizeof(u32));
                if (work.m_prioritize) {
                    encoder.setBufferState(batch.m_temporal_queue, rhi::ResourceState::CopySource);
                    encoder.copyBuffer(
                        batch.m_readback, batch.m_plan.selectedCountOffset(index), batch.m_temporal_queue, 0u,
                        sizeof(u32));
                }

                if (batch.m_queries)
                    encoder.writeTimestamp(
                        batch.m_queries, batch.m_plan.stageQueryOffset(index) + batch.m_plan.m_stage_query_stride - 1u);
            }

            if (batch.m_plan.m_batch_begin[batch.m_page + 1u] == batch.m_work_end) {
                const hal::ProfileScope coverage_scope{"WorldRadiance.CheckTemporalCoverage"};
                encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopyDestination);
                encoder.clearBuffer(batch.m_counts, 0u, 8u * sizeof(u32));
                encoder.setBufferState(batch.m_counts, rhi::ResourceState::UnorderedAccess);
                encoder.setBufferState(batch.m_generation->probeStateBuffer(), rhi::ResourceState::ShaderResource);
                const std::array coverage_windows{batch.m_plan.m_coverage_probes, batch.m_plan.m_gameplay_probes};
                for (std::size_t index = 0u; index < coverage_windows.size(); ++index) {
                    if (not batch.m_coverage_roots[index])
                        break;
                    const auto &coverage = coverage_windows[index];
                    auto *const coverage_pass = encoder.beginComputePass();
                    PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(coverage_pass != nullptr, std::errc::io_error));
                    coverage_pass->bindPipeline(state.m_temporal_pipelines[5], batch.m_coverage_roots[index]);
                    coverage_pass->dispatchCompute(
                        (coverage.m_counts[0] + 7u) / 8u, (coverage.m_counts[1] + 7u) / 8u, 1u);
                    coverage_pass->end();

                    encoder.globalBarrier();
                }
                encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
                encoder.copyBuffer(
                    batch.m_readback, batch.m_plan.m_demand.m_readback_bytes - sizeof(u32), batch.m_counts, 0u,
                    sizeof(u32));
            }

            if (batch.m_plan.m_batch_begin[batch.m_page + 1u] == batch.m_work_end and
                batch.m_settings.m_gameplay_demand) {
                const auto &support = batch.m_plan.m_gameplay_probes;
                const auto &field = *batch.m_generation;
                const u32 x = static_cast<u32>(support.m_minimum[0] - field.probeMinimum()[0]);
                const u32 y = static_cast<u32>(support.m_minimum[1] - field.probeMinimum()[1]);
                const u64 row_bytes = u64{support.m_counts[0]} * sizeof(WorldRadianceCoefficients);

                encoder.setBufferState(field.coefficientsBuffer(), rhi::ResourceState::CopySource);
                for (u32 row = 0; row < support.m_counts[1]; ++row) {
                    const u64 first = u64{y + row} * field.probeCounts()[0] + x;
                    encoder.copyBuffer(
                        batch.m_readback, batch.m_plan.gameplayOffset() + row * row_bytes, field.coefficientsBuffer(),
                        first * sizeof(WorldRadianceCoefficients), row_bytes);
                }
            }

            if (batch.m_plan.m_batch_begin[batch.m_page + 1u] == batch.m_work_end and
                batch.m_settings.m_diagnostic_coefficients) {
                encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::CopySource);
                encoder.copyBuffer(
                    batch.m_readback, batch.m_plan.gameplayOffset() + batch.m_plan.m_gameplay_bytes,
                    batch.m_generation->m_coefficients, 0u, batch.m_plan.m_demand.m_refined_bytes);
                encoder.setBufferState(batch.m_generation->probeStateBuffer(), rhi::ResourceState::CopySource);
                encoder.copyBuffer(
                    batch.m_readback,
                    batch.m_plan.gameplayOffset() +
                        batch.m_plan.m_gameplay_bytes +
                        batch.m_plan.m_demand.m_refined_bytes,
                    batch.m_generation->probeStateBuffer(), 0u, batch.m_plan.m_demand.m_probe_state_bytes);
            }

            encoder.setBufferState(batch.m_generation->probeStateBuffer(), rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::ShaderResource);
            if (batch.m_queries)
                encoder.writeTimestamp(batch.m_queries, 2u * batch.m_page + 1u);
            return {};
        }

        auto &encoder = context.m_encoder;
        batch.m_encoded = true;
        batch.m_actual_frame = actual_frame;
        if (batch.m_queries) {
            encoder.writeTimestamp(batch.m_queries, windowed ? 2u * batch.m_page : 0u);
        }

        if (windowed and batch.m_page == 0u and batch.m_copy_donor) {
            const auto &overlap = batch.m_plan.m_copy_window;
            const auto donor_minimum = batch.m_copy_donor->probeMinimum();
            const auto donor_counts = batch.m_copy_donor->probeCounts();
            auto *const donor = batch.m_copy_donor->coefficientsBuffer();
            auto *const output = batch.m_generation->coefficientsBuffer();

            encoder.setBufferState(donor, rhi::ResourceState::CopySource);
            encoder.setBufferState(output, rhi::ResourceState::CopyDestination);
            for (u32 y = 0u; y < overlap.m_counts[1]; ++y) {
                const u64 source_row = u64{static_cast<u32>(overlap.m_minimum[1] - donor_minimum[1]) + y} *
                                           donor_counts[0] +
                                       static_cast<u32>(overlap.m_minimum[0] - donor_minimum[0]);
                const auto &parameters = batch.m_plan.m_parameters;
                const u64 destination_row = u64{static_cast<u32>(overlap.m_minimum[1] - parameters.m_refined_min_y) +
                                                y} *
                                                parameters.m_refined_count_x +
                                            static_cast<u32>(overlap.m_minimum[0] - parameters.m_refined_min_x);

                encoder.copyBuffer(
                    output, destination_row * sizeof(WorldRadianceCoefficients), donor,
                    source_row * sizeof(WorldRadianceCoefficients),
                    u64{overlap.m_counts[0]} * sizeof(WorldRadianceCoefficients));
            }

            encoder.setBufferState(donor, rhi::ResourceState::ShaderResource);
        }

        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopyDestination);
        encoder.clearBuffer(batch.m_counts, 0u, 8u * sizeof(u32));
        encoder.setBufferState(batch.m_counts, rhi::ResourceState::UnorderedAccess);
        for (auto *const buffer:
            {batch.m_overlay.get(), batch.m_overlay_lookup.get(), batch.m_overlay_ancestors.get(), batch.m_ids.get(),
                batch.m_materials.get(), batch.m_vacuum.get(), batch.m_hierarchy.get(), batch.m_levels.get(),
                batch.m_directions.get()}) {
            encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);
        }
        encoder.setBufferState(batch.m_masks, rhi::ResourceState::ShaderResource);

        const hal::ProfileScope scope{"WorldRadiance.EncodeGeneration"};
        for (u32 level = static_cast<u32>(batch.m_plan.m_levels.size()) - 1u; level >= 1u; --level) {
            auto *const output = batch.m_parity[level % 2u].get();
            auto *const parent = batch.m_parity[(level + 1u) % 2u].get();
            encoder.setBufferState(parent, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(output, rhi::ResourceState::UnorderedAccess);
            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_pipelines[0], batch.m_roots[level]);
            pass->dispatchCompute((batch.m_plan.m_levels[level].m_records + 63u) / 64u, 1u, 1u);
            pass->end();

            encoder.globalBarrier();
        }

        const auto &parameters = batch.m_plan.m_parameters;
        const u32 rows = windowed ? batch.m_plan.m_strips[batch.m_page].m_count_x *
                                        batch.m_plan.m_strips[batch.m_page].m_count_y
                                  : parameters.m_refined_count_x * parameters.m_refined_count_y;
        if (rows != 0u) {
            encoder.setBufferState(batch.m_parity[1], rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::UnorderedAccess);
            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(
                windowed ? state.m_parallel_pipeline : state.m_pipelines[1],
                windowed ? batch.m_page_roots[batch.m_page] : batch.m_refine_root);
            pass->dispatchCompute(windowed ? rows : (rows + 63u) / 64u, 1u, 1u);
            pass->end();
        }

        encoder.globalBarrier();
        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
        encoder.setBufferState(batch.m_readback, rhi::ResourceState::CopyDestination);
        encoder.copyBuffer(batch.m_readback, 0u, batch.m_counts, 0u, 8u * sizeof(u32));
        if (batch.m_settings.m_diagnostic_coefficients and
            (not windowed or batch.m_page + 1u == batch.m_plan.m_strips.size())) {
            encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::CopySource);
            encoder.copyBuffer(
                batch.m_readback, 8u * sizeof(u32), batch.m_generation->m_coefficients, 0u,
                batch.m_plan.m_demand.m_refined_bytes);
        }

        encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::ShaderResource);
        if (batch.m_queries) {
            encoder.writeTimestamp(batch.m_queries, windowed ? 2u * batch.m_page + 1u : 1u);
        }

        return {};
    }

    std::error_code WorldRadiancePass::notifySubmitted(const FenceCompletionValue submitted) {
        if (m_state and m_state->m_matter and m_state->m_matter->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and
                                       not m_state->m_unknown and
                                       submitted > m_state->m_last_submit,
                                   std::errc::operation_not_permitted));
            auto &batch = *m_state->m_matter;
            batch.m_fence = submitted;
            batch.m_submitted = true;
            batch.m_encoded = false;
            batch.m_completed = false;
            m_state->m_last_submit = submitted;
            return {};
        }

        if (m_state and m_state->m_bounce and m_state->m_bounce->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and
                                       not m_state->m_unknown and
                                       submitted > m_state->m_last_submit,
                                   std::errc::operation_not_permitted));
            auto &batch = *m_state->m_bounce;
            batch.m_fence = submitted;
            batch.m_submitted = true;
            batch.m_encoded = false;
            batch.m_completed = false;
            m_state->m_last_submit = submitted;
            return {};
        }

        if (m_state and m_state->m_discovery and m_state->m_discovery->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and
                                       not m_state->m_unknown and
                                       submitted > m_state->m_last_submit,
                                   std::errc::operation_not_permitted));
            auto &batch = *m_state->m_discovery;
            batch.m_fence = submitted;
            batch.m_submitted = true;
            batch.m_encoded = false;
            batch.m_completed = false;
            m_state->m_last_submit = submitted;
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_batch and
                                   m_state->m_batch->m_encoded and
                                   not m_state->m_unknown and
                                   submitted > m_state->m_last_submit,
                               std::errc::operation_not_permitted));
        auto &batch = *m_state->m_batch;
        batch.m_fence = submitted;
        batch.m_timing.m_submission = submitted;
        batch.m_submitted = true;
        if (batch.m_temporal_pool)
            batch.m_temporal_pool->m_initialized = true;
        batch.m_encoded = false;
        batch.m_completed = false;
        m_state->m_last_submit = submitted;
        return {};
    }

    std::error_code WorldRadiancePass::discardUnsubmitted() {
        if (m_state and m_state->m_matter and m_state->m_matter->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                                   std::errc::operation_not_permitted));
            m_state->m_matter->m_encoded = false;
            return {};
        }

        if (m_state and m_state->m_bounce and m_state->m_bounce->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and
                                       not m_state->m_unknown and
                                       not m_state->m_bounce->m_submitted,
                                   std::errc::operation_not_permitted));
            m_state->m_bounce->m_encoded = false;
            return {};
        }

        if (m_state and m_state->m_discovery and m_state->m_discovery->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(
                                   m_state->m_owner == std::this_thread::get_id() and
                                       not m_state->m_unknown and
                                       not m_state->m_discovery->m_submitted,
                                   std::errc::operation_not_permitted));
            m_state->m_discovery->m_encoded = false;
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_batch and
                                   not m_state->m_batch->m_submitted and
                                   not m_state->m_unknown,
                               std::errc::operation_not_permitted));
        m_state->m_batch->m_encoded = false;
        return {};
    }

    std::error_code WorldRadiancePass::notifySubmitFailed() {
        if (m_state and m_state->m_matter and m_state->m_matter->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                require_(m_state->m_owner == std::this_thread::get_id(), std::errc::operation_not_permitted));
            m_state->m_unknown = true;
            return {};
        }

        if (m_state and m_state->m_bounce and m_state->m_bounce->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                require_(m_state->m_owner == std::this_thread::get_id(), std::errc::operation_not_permitted));
            m_state->m_unknown = true;
            return {};
        }

        if (m_state and m_state->m_discovery and m_state->m_discovery->m_encoded) {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                require_(m_state->m_owner == std::this_thread::get_id(), std::errc::operation_not_permitted));
            m_state->m_unknown = true;
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_batch and
                                   m_state->m_batch->m_encoded,
                               std::errc::operation_not_permitted));
        m_state->m_unknown = true;
        return {};
    }

    Expected<bool> WorldRadiancePass::budgetTemporalFrame(
        const float available_milliseconds, const bool allow_refresh) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(std::isfinite(available_milliseconds) and available_milliseconds >= 0.0f));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_batch and
                                   m_state->m_batch->m_temporal_pool and
                                   m_state->m_batch->m_settings.m_temporal_frame_allowance_ms > 0.0f and
                                   not m_state->m_batch->m_encoded and
                                   not m_state->m_batch->m_submitted and
                                   m_state->m_batch->m_page < m_state->m_batch->m_plan.temporalBatches(),
                               std::errc::operation_not_permitted));
        if (available_milliseconds == 0.0f)
            return false;

        auto &batch = *m_state->m_batch;
        if (not allow_refresh and
            batch.m_settings.m_temporal_refresh_window and
            not batch.m_generation->m_refresh_yielded) {
            const u32 first = batch.m_plan.m_batch_begin[batch.m_page];
            const auto &work = batch.m_plan.m_temporal_work;
            const u32 end = std::max(first, batch.m_plan.m_primary_work_end);
            if (end < batch.m_work_end) {
                batch.m_work_end = end;
                auto &statistics = batch.m_generation->m_statistics;
                for (u32 index = end; index < work.size(); ++index) {
                    const auto &dispatch = work[index].m_dispatch;
                    if (dispatch.m_level == 0u)
                        batch.m_unupdated_probes += dispatch.m_count;
                    else
                        statistics.m_expected_rows -= dispatch.m_count;
                }
                batch.m_generation->m_refresh_yielded = true;
            }
        }

        batch.m_settings.m_temporal_frame_allowance_ms = available_milliseconds;
        if (batch.m_setup_step < 2u) {
            const auto &cost = batch.setupCost();
            batch.m_predicted_milliseconds = cost.m_samples > 0u ? cost.maximum() : -1.0;
        } else {
            batch.selectTemporalBatch();
        }
        return true;
    }

    bool WorldRadiancePass::canEncodeNextBatch() const noexcept {
        return m_state and
               not m_state->m_unknown and
               not m_state->m_matter and
               m_state->m_batch and
               not m_state->m_batch->m_encoded and
               not m_state->m_batch->m_submitted and
               (not m_state->m_batch->m_temporal_pool or
                   m_state->m_batch->m_page < m_state->m_batch->m_plan.temporalBatches()) and
               (m_state->m_batch->m_settings.m_transport != WorldRadianceTransport::direct_window or
                   m_state->m_batch->m_page < m_state->m_batch->m_plan.m_strips.size());
    }

    u32 WorldRadiancePass::pendingBatches() const noexcept {
        if (not m_state or not m_state->m_batch)
            return 0u;

        const auto &batch = *m_state->m_batch;
        if (batch.m_temporal_pool) {
            const u32 remaining_pages = batch.m_plan.temporalBatches() - batch.m_page;
            if (batch.m_settings.m_temporal_frame_allowance_ms == 0.0f)
                return remaining_pages;

            const u32 remaining_work = batch.m_plan.m_temporal_work.empty()
                                           ? remaining_pages
                                           : std::max(
                                                 remaining_pages,
                                                 batch.m_work_end - batch.m_plan.m_batch_begin[batch.m_page]);
            return 2u - batch.m_setup_step + remaining_work;
        }

        return batch.m_settings.m_transport == WorldRadianceTransport::direct_window
                   ? static_cast<u32>(batch.m_plan.m_strips.size()) - batch.m_page
               : batch.m_published ? 0u
                                   : 1u;
    }

    Expected<bool> WorldRadiancePass::advance(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_batch and
                                   (m_state->m_batch->m_settings.m_transport == WorldRadianceTransport::direct_window or
                                       m_state->m_batch->m_temporal_pool),
                               std::errc::operation_not_permitted));
        auto &batch = *m_state->m_batch;
        if (batch.m_temporal_pool) {
            if (batch.m_page == batch.m_plan.temporalBatches())
                return true;
            if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence))
                return false;

            batch.m_completed = true;
            const auto timing = pollTiming(batch.m_generation->cohort(), completed);
            if (timing.m_status == WorldRadianceTimingStatus::pending)
                return false;

            if (batch.m_setup_step < 2u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, timing.m_error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(timing.m_milliseconds.has_value(), std::errc::state_not_recoverable));
                const double milliseconds = *timing.m_milliseconds;
                batch.setupCost().observe(milliseconds);
                auto &maximum = batch.m_generation->m_max_gpu_milliseconds;
                maximum = std::max(maximum.value_or(0.0), milliseconds);
                PPR_LOG(
                    WorldRadiance, info, "WORLD_TEMPORAL_SETUP_BATCH",
                    {{"frame", batch.m_actual_frame}, {"step", batch.m_setup_step}, {"gpu_ms", milliseconds},
                        {"allowance_ms", batch.m_settings.m_temporal_frame_allowance_ms},
                        {"predicted_ms", batch.m_predicted_milliseconds},
                        {"over_allowance", milliseconds > double{batch.m_settings.m_temporal_frame_allowance_ms}}});

                ++batch.m_setup_step;
                batch.m_submitted = false;
                batch.m_timing.m_submission = kFenceValueNone;
                batch.m_timing.m_milliseconds.reset();
                batch.m_timing.m_status = WorldRadianceTimingStatus::pending;
                return false;
            }

            const u32 first = batch.m_plan.m_batch_begin[batch.m_page];
            const u32 end = batch.m_plan.m_batch_begin[batch.m_page + 1u];
            std::vector<double> stage_milliseconds;
            if (batch.m_queries and batch.m_page == 0u) {
                const u32 offset = batch.m_plan.setupQueryOffset();
                rhi::QueryResultState setup_readiness{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, batch.m_queries->getResultState(offset, 4u, &setup_readiness));
                if (setup_readiness == rhi::QueryResultState::Pending)
                    return false;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(setup_readiness == rhi::QueryResultState::Resolved, std::errc::state_not_recoverable));

                std::array<u64, 4u> setup_ticks{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, batch.m_queries->getResult(offset, 4u, setup_ticks.data()));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       setup_ticks[1] >= setup_ticks[0] and setup_ticks[3] >= setup_ticks[2],
                                       std::errc::result_out_of_range));
                const double scale = 1000.0 / double(m_state->m_device->getInfo().timestampFrequency);
                PPR_LOG(
                    WorldRadiance, info, "WORLD_TEMPORAL_SETUP",
                    {{"frame", batch.m_actual_frame},
                        {"preservation_ms", double(setup_ticks[1] - setup_ticks[0]) * scale},
                        {"source_skip_ms", double(setup_ticks[3] - setup_ticks[2]) * scale},
                        {"copy_donor", bool(batch.m_copy_donor)}, {"retained_rows", batch.m_plan.m_retained_rows}});
            }

            if (batch.m_queries and end > first) {
                const u32 stride = batch.m_plan.m_stage_query_stride;
                const double scale = 1000.0 / double(m_state->m_device->getInfo().timestampFrequency);
                double queue_ms{}, priority_queue_ms{};
                u32 priority_groups{};
                stage_milliseconds.resize(end - first);
                rhi::QueryResultState readiness{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, batch.m_queries->getResultState(
                                       batch.m_plan.stageQueryOffset(first), stride * (end - first), &readiness));
                if (readiness == rhi::QueryResultState::Pending)
                    return false;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(readiness == rhi::QueryResultState::Resolved, std::errc::state_not_recoverable));
                std::vector<u64> ticks(stride * (end - first));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    batch.m_queries->getResult(
                        batch.m_plan.stageQueryOffset(first), static_cast<u32>(ticks.size()), ticks.data()));
                for (u32 index = first; index < end; ++index) {
                    const auto offset = stride * (index - first);
                    const auto stop = ticks[offset + stride - 1u];
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, require_(stop >= ticks[offset], std::errc::result_out_of_range));
                    const double milliseconds = double(stop - ticks[offset]) * scale;
                    if (stride == 3u) {
                        const auto middle = ticks[offset + 1u];
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance,
                            require_(middle >= ticks[offset] and middle <= stop, std::errc::result_out_of_range));
                        const double measured = double(middle - ticks[offset]) * scale;
                        queue_ms += measured;
                        if (batch.m_plan.m_temporal_work[index].m_prioritize) {
                            priority_queue_ms += measured;
                            ++priority_groups;
                        }
                    }

                    stage_milliseconds[index - first] = milliseconds;
                    PPR_LOG(
                        WorldRadiance, info, "WORLD_TEMPORAL_STAGE",
                        {{"frame", batch.m_actual_frame}, {"phase", batch.m_page},
                            {"level", batch.m_plan.m_temporal_work[index].m_dispatch.m_level},
                            {"records", batch.m_plan.m_temporal_work[index].m_dispatch.m_count},
                            {"refresh", batch.m_plan.m_temporal_work[index].m_refresh}, {"gpu_ms", milliseconds}});
                }

                if (stride == 3u) {
                    PPR_LOG(
                        WorldRadiance, info, "WORLD_TEMPORAL_QUEUE_TIMING",
                        {{"frame", batch.m_actual_frame}, {"queue_ms", queue_ms},
                            {"priority_queue_ms", priority_queue_ms}, {"priority_groups", priority_groups},
                            {"total_groups", end - first}});
                }
            }

            std::vector<std::array<u32, 8u>> counters(end - first);
            bool selection_valid = true;
            u64 deferred_rows{};
            u32 missing_coverage{};
            std::shared_ptr<WorldRadianceGameplayField> gameplay;
            if (end == batch.m_work_end and batch.m_settings.m_gameplay_demand) {
                gameplay = std::make_shared<WorldRadianceGameplayField>();
                gameplay->m_rows.resize(batch.m_plan.m_gameplay_bytes / sizeof(WorldRadianceCoefficients));
            }

            void *mapped{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
            if (not counters.empty())
                std::memcpy(
                    counters.data(), static_cast<const std::byte *>(mapped) + u64{first} * 8u * sizeof(u32),
                    counters.size() * sizeof(counters[0]));
            if (batch.m_settings.m_prioritize_convergence) {
                for (u32 index = first; index < end; ++index) {
                    const auto &work = batch.m_plan.m_temporal_work[index];
                    if (not work.m_prioritize)
                        continue;

                    u32 selected{};
                    std::memcpy(
                        &selected, static_cast<const std::byte *>(mapped) + batch.m_plan.selectedCountOffset(index),
                        sizeof(selected));
                    if (selected != counters[index - first][3] or selected > work.m_dispatch.m_count) {
                        selection_valid = false;
                        break;
                    }
                    deferred_rows += work.m_dispatch.m_count - selected;
                }
            }

            if (end == batch.m_work_end)
                std::memcpy(
                    &missing_coverage,
                    static_cast<const std::byte *>(mapped) + batch.m_plan.m_demand.m_readback_bytes - sizeof(u32),
                    sizeof(missing_coverage));
            if (gameplay and missing_coverage == 0u)
                std::memcpy(
                    gameplay->m_rows.data(), static_cast<const std::byte *>(mapped) + batch.m_plan.gameplayOffset(),
                    batch.m_plan.m_gameplay_bytes);
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(selection_valid, std::errc::result_out_of_range));

            auto &statistics = batch.m_generation->m_statistics;
            for (u32 index = first; index < end; ++index) {
                const auto &counts = counters[index - first];
                const auto &work = batch.m_plan.m_temporal_work[index];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       counts[0] == counts[1] and
                                           counts[0] <= work.m_queries and
                                           counts[2] == 0u and
                                           (work.m_prioritize or counts[3] == work.m_dispatch.m_count) and
                                           counts[4] == 0u and
                                           counts[7] == 0u,
                                       std::errc::result_out_of_range));
            }

            u64 batch_queries{};
            for (const auto &counts: counters)
                batch_queries += counts[0];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(batch_queries <= batch.m_settings.m_temporal_query_budget, std::errc::result_out_of_range));

            for (const auto &counts: counters) {
                statistics.m_queries_attempted += counts[0];
                statistics.m_queries_completed += counts[1];
                statistics.m_rows_written += counts[3];
                statistics.m_segments += counts[5];
                statistics.m_skipped_blocks += counts[6];
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(deferred_rows == 0u or bool(batch.m_copy_donor)));
            statistics.m_rows_written += deferred_rows;
            statistics.m_rows_copied += deferred_rows;

            if (batch.m_page == 0u) {
                statistics.m_rows_written += batch.m_plan.m_retained_rows;
                if (batch.m_copy_donor)
                    statistics.m_rows_copied += batch.m_plan.m_retained_rows;
            }

            // Commit cost observations only after every GPU status/counter was validated.
            auto &pool = *batch.m_temporal_pool;
            double stage_total{};
            for (u32 index = first; not stage_milliseconds.empty() and index < end; ++index) {
                const auto &work = batch.m_plan.m_temporal_work[index];
                const double elapsed = stage_milliseconds[index - first];
                if (not work.m_prioritize or counters[index - first][3] == work.m_dispatch.m_count)
                    pool.costFor(work).observe(elapsed / static_cast<double>(work.m_queries));
                stage_total += elapsed;
            }
            if (timing.m_milliseconds and batch.m_page > 0u)
                pool.m_overhead.observe(std::max(0.0, *timing.m_milliseconds - stage_total));

            if (timing.m_milliseconds and batch.m_settings.m_temporal_frame_allowance_ms > 0.0f) {
                pool.m_last_budgeted_queries = *timing.m_milliseconds <=
                                                       double{batch.m_settings.m_temporal_frame_allowance_ms}
                                                   ? static_cast<u32>(batch_queries)
                                                   : 0u;
            }

            if (timing.m_milliseconds) {
                auto &maximum = batch.m_generation->m_max_gpu_milliseconds;
                maximum = std::max(maximum.value_or(0.0), *timing.m_milliseconds);
            }
            PPR_LOG(
                WorldRadiance, info, "WORLD_TEMPORAL_BATCH",
                {{"frame", batch.m_actual_frame}, {"batch", batch.m_page}, {"queries", batch_queries},
                    {"deferred_rows", deferred_rows}, {"query_budget", batch.m_settings.m_temporal_query_budget},
                    {"allowance_ms", batch.m_settings.m_temporal_frame_allowance_ms},
                    {"predicted_ms", batch.m_predicted_milliseconds},
                    {"over_allowance", batch.m_settings.m_temporal_frame_allowance_ms > 0.0f and
                                           timing.m_milliseconds.value_or(0.0) >
                                               double{batch.m_settings.m_temporal_frame_allowance_ms}},
                    {"gpu_ms", timing.m_milliseconds.value_or(-1.0)}, {"rows", statistics.m_rows_written},
                    {"segments", statistics.m_segments}});

            if (batch.m_settings.m_temporal_frame_allowance_ms > 0.0f and end < batch.m_work_end)
                batch.m_plan.m_batch_begin.push_back(end + 1u);

            ++batch.m_page;
            if (batch.m_page < batch.m_plan.temporalBatches()) {
                if (batch.m_settings.m_temporal_frame_allowance_ms > 0.0f)
                    batch.selectTemporalBatch();

                batch.m_submitted = false;
                batch.m_timing.m_submission = kFenceValueNone;
                batch.m_timing.m_milliseconds.reset();
                if (batch.m_queries)
                    batch.m_timing.m_status = WorldRadianceTimingStatus::pending;
                return false;
            }

            statistics.m_rows_written += batch.m_unupdated_probes;
            if (batch.m_copy_donor)
                statistics.m_rows_copied += batch.m_unupdated_probes;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(statistics.m_rows_written == statistics.m_expected_rows, std::errc::result_out_of_range));
            const auto &primary = batch.m_plan.m_coverage_probes;
            const auto &requested = batch.m_plan.m_gameplay_probes;
            const u32 coverage_rows = primary.m_counts[0] * primary.m_counts[1] +
                                      (batch.m_coverage_roots[1] ? requested.m_counts[0] * requested.m_counts[1] : 0u);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(missing_coverage <= coverage_rows, std::errc::result_out_of_range));
            if (missing_coverage == 0u)
                batch.m_generation->m_current_receiver_window = batch.m_plan.m_coverage_window;
            if (gameplay and missing_coverage == 0u) {
                for (const auto &row: gameplay->m_rows) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        require_(
                            row.m_status == 0u and
                                std::ranges::all_of(
                                    row.m_coefficients, [](const float value) { return std::isfinite(value); }),
                            std::errc::result_out_of_range));
                }

                const auto &field = *batch.m_generation;
                gameplay->m_cohort = field.cohort();
                gameplay->m_matter = field.source()->matterIdentity();
                gameplay->m_origin_metres = field.source()->minimumMetres();
                gameplay->m_cell_metres = field.source()->cellMetres();
                gameplay->m_transport_version = field.transportVersion();
                gameplay->m_sky_hour = field.skyHour();
                gameplay->m_completion = batch.m_fence;
                gameplay->m_spacing = batch.m_settings.m_temporal.m_refined_spacing;
                gameplay->m_probes = batch.m_plan.m_gameplay_probes;
                gameplay->m_demand = *batch.m_settings.m_gameplay_demand;
                batch.m_generation->m_gameplay_field = std::move(gameplay);
            }

            PPR_LOG(
                WorldRadiance, info, "WORLD_TEMPORAL_COVERAGE",
                {{"version", batch.m_generation->transportVersion()}, {"required_probes", coverage_rows},
                    {"missing_probes", missing_coverage}, {"current", missing_coverage == 0u}});

            batch.m_copy_donor.reset(); // Its last reader fence is complete.
            batch.m_queries.setNull();
            return true;
        }

        if (batch.m_page == batch.m_plan.m_strips.size())
            return true;

        if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence))
            return false;

        batch.m_completed = true;
        const auto timing = pollTiming(batch.m_generation->cohort(), completed);
        if (timing.m_status == WorldRadianceTimingStatus::pending)
            return false; // Keep the exact fenced batch until its optional host query is ready.

        std::array<u32, 8> counts{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(counts.data(), mapped, sizeof(counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

        const auto &page = batch.m_plan.m_strips[batch.m_page];
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               counts[0] == page.m_queries and
                                   counts[0] == counts[1] and
                                   counts[2] == 0u and
                                   counts[3] == page.m_count_x * page.m_count_y and
                                   counts[4] == 0u and
                                   counts[7] == 0u,
                               std::errc::result_out_of_range));

        auto &statistics = batch.m_generation->m_statistics;
        statistics.m_queries_attempted += counts[0];
        statistics.m_queries_completed += counts[1];
        statistics.m_queries_failed += counts[2];
        const u64 copied_rows = batch.m_page == 0u and batch.m_copy_donor
                                    ? u64{batch.m_plan.m_copy_window.m_counts[0]} *
                                          batch.m_plan.m_copy_window.m_counts[1]
                                    : 0u;
        statistics.m_rows_copied += copied_rows;
        statistics.m_rows_written += counts[3] + copied_rows;
        statistics.m_invalid_rows += counts[4];
        statistics.m_segments += counts[5];
        statistics.m_skipped_blocks += counts[6];
        statistics.m_status |= counts[7];

        // The accepted copy fence is now proven. Later strips cannot read the
        // donor; readers retain their own independent immutable generation.
        batch.m_copy_donor.reset();

        const u32 completed_page = batch.m_page;
        const bool ready = batch.m_page + 1u == batch.m_plan.m_strips.size();
        if (ready) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   statistics.m_rows_written == batch.m_plan.m_demand.m_expected_rows and
                                       statistics.m_queries_attempted == batch.m_plan.m_demand.m_query_upper_bound,
                                   std::errc::result_out_of_range));
        }

        ++batch.m_page;
        if (not ready) {
            batch.m_submitted = false;
            batch.m_timing.m_submission = kFenceValueNone;
            batch.m_timing.m_milliseconds.reset();
            if (batch.m_queries)
                batch.m_timing.m_status = WorldRadianceTimingStatus::pending;
        }

        const auto message = std::format(
            "WORLD_DIRECT_QUANTUM frame={} quantum={} quanta={} publication_page={} "
            "minimum=({},{}) receiver_rows_per_quantum={} fresh_rows={} copied_rows={} copied_bytes={} queries={} segments={} skipped={} "
            "submission={} gpu_ms={} gpu_timing_status={} copy_barriers_counters_trace_included=true "
            "CPU_prepare_uploads_excluded=true job={} world={} content={} revision={}",
            batch.m_actual_frame, completed_page, batch.m_plan.m_strips.size(), page.m_page, page.m_min_x, page.m_min_y,
            batch.m_settings.m_receiver_rows_per_quantum, counts[3], copied_rows,
            copied_rows * sizeof(WorldRadianceCoefficients), counts[0], counts[5], counts[6],
            static_cast<u64>(batch.m_fence), timing.m_milliseconds.value_or(-1.0), static_cast<u32>(timing.m_status),
            batch.m_generation->cohort().m_job_id, batch.m_generation->cohort().m_world_generation,
            batch.m_generation->cohort().m_content_fingerprint, batch.m_generation->cohort().m_geometry_revision);
        PPR_LOG_RAW(WorldRadiance, info, message);

        return ready;
    }

    Expected<std::shared_ptr<const WorldRadianceGeneration>> WorldRadiancePass::acquire(
        const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_batch and
                                   not m_state->m_unknown and
                                   m_state->m_batch->m_submitted,
                               std::errc::operation_not_permitted));
        auto &batch = *m_state->m_batch;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(slotFenceComplete(completed, batch.m_fence), std::errc::device_or_resource_busy));
        batch.m_completed = true;
        if (batch.m_published)
            return std::shared_ptr<const WorldRadianceGeneration>{batch.m_generation};

        batch.m_generation->m_completion = batch.m_fence;

        if (batch.m_temporal_pool or batch.m_settings.m_transport == WorldRadianceTransport::direct_window) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   batch.m_page == (batch.m_temporal_pool ? batch.m_plan.temporalBatches()
                                                                          : batch.m_plan.m_strips.size()),
                                   std::errc::device_or_resource_busy));
            if (batch.m_generation->gameplayField())
                m_state->m_gameplay_fields.push_back(batch.m_generation->gameplayField());
            m_state->m_generations.push_back(batch.m_generation);
            batch.m_published = true;
            return std::shared_ptr<const WorldRadianceGeneration>{batch.m_generation};
        }

        std::array<u32, 8> counts{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(counts.data(), mapped, sizeof(counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));
        const auto &demand = batch.m_plan.m_demand;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               counts[0] == counts[1] and
                                   counts[0] <= demand.m_query_upper_bound and
                                   (batch.m_settings.m_transport != WorldRadianceTransport::straight256_reference or
                                       counts[0] == demand.m_query_upper_bound) and
                                   counts[2] == 0u and
                                   counts[3] == demand.m_expected_rows and
                                   counts[4] == 0u and
                                   counts[7] == 0u,
                               std::errc::result_out_of_range));

        auto &statistics = batch.m_generation->m_statistics;
        statistics = {
            demand.m_query_upper_bound, demand.m_expected_rows, counts[0], counts[1], counts[2], counts[3], counts[4],
            counts[5], counts[6], 0u, counts[7]};
        m_state->m_generations.push_back(batch.m_generation);
        batch.m_published = true;
        return std::shared_ptr<const WorldRadianceGeneration>{batch.m_generation};
    }

    WorldRadianceTiming WorldRadiancePass::pollTiming(
        const TerrainReceiverCohort cohort, const FenceCompletionValue completed) {
        WorldRadianceTiming missing{};
        missing.m_cohort = cohort;
        if (not m_state or m_state->m_owner != std::this_thread::get_id()) {
            missing.m_status = WorldRadianceTimingStatus::failed;
            missing.m_error = std::make_error_code(std::errc::operation_not_permitted);
            return missing;
        }

        auto &state = *m_state;
        if (not state.m_batch or state.m_batch->m_generation->cohort() != cohort) {
            return state.m_retired_timing and state.m_retired_timing->m_cohort == cohort ? *state.m_retired_timing
                                                                                         : missing;
        }

        auto &batch = *state.m_batch;
        auto &timing = batch.m_timing;
        if (timing.m_status != WorldRadianceTimingStatus::pending) {
            return timing;
        }

        if (state.m_unknown) {
            // The unknown-submit batch and pool stay owned until quiescence.
            auto unknown = timing;
            unknown.m_status = WorldRadianceTimingStatus::failed;
            unknown.m_error = std::make_error_code(std::errc::state_not_recoverable);
            return unknown;
        }

        if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence)) {
            return timing;
        }

        batch.m_completed = true;
        rhi::QueryResultState readiness{};
        const bool windowed = batch.m_settings.m_transport == WorldRadianceTransport::direct_window;
        const u32 first_query = batch.m_temporal_pool and batch.m_setup_step < 2u
                                    ? batch.m_plan.setupQueryOffset() + 2u * batch.m_setup_step
                                : (windowed or batch.m_temporal_pool) ? 2u * batch.m_page
                                                                      : 0u;
        timing.m_error = make_error_code(batch.m_queries->getResultState(first_query, 2u, &readiness));
        if (not timing.m_error and readiness == rhi::QueryResultState::Pending) {
            return timing;
        }

        if (not timing.m_error and readiness != rhi::QueryResultState::Resolved) {
            timing.m_error = std::make_error_code(std::errc::state_not_recoverable);
        }

        std::array<u64, 2> ticks{};
        if (not timing.m_error) {
            // Both the real app fence and backend host readiness are proven.
            // Pinned Vulkan/D3D12 getResult performs a host read, no submission.
            timing.m_error = make_error_code(batch.m_queries->getResult(first_query, 2u, ticks.data()));
        }

        if (not timing.m_error and ticks[1] < ticks[0]) {
            timing.m_error = std::make_error_code(std::errc::result_out_of_range);
        }

        if (timing.m_error) {
            timing.m_status = WorldRadianceTimingStatus::failed;
        } else {
            timing.m_milliseconds = static_cast<double>(ticks[1] - ticks[0]) *
                                    1000.0 /
                                    static_cast<double>(state.m_device->getInfo().timestampFrequency);
            timing.m_status = WorldRadianceTimingStatus::ready;
        }

        if (not batch.m_temporal_pool and (not windowed or batch.m_page + 1u == batch.m_plan.m_strips.size()))
            batch.m_queries.setNull(); // Every timestamp use completed, including a failed final host read.
        return timing;
    }

    Expected<std::vector<WorldRadianceCoefficients>> WorldRadiancePass::readDiagnosticCoefficients(
        const FenceCompletionValue completed) {
        const auto generation = acquire(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, generation);
        auto &batch = *m_state->m_batch;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(batch.m_settings.m_diagnostic_coefficients, std::errc::operation_not_supported));
        const auto &parameters = batch.m_plan.m_parameters;
        std::vector<WorldRadianceCoefficients> rows(u64{parameters.m_refined_count_x} * parameters.m_refined_count_y);
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(
            rows.data(),
            static_cast<const std::byte *>(mapped) +
                (batch.m_temporal_pool ? batch.m_plan.gameplayOffset() + batch.m_plan.m_gameplay_bytes
                                       : 8u * sizeof(u32)),
            rows.size() * sizeof(rows[0]));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

        for (const auto &row: rows) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(row.m_status == 0u, std::errc::result_out_of_range));
            for (const float value: row.m_coefficients) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(std::isfinite(value), std::errc::result_out_of_range));
            }
        }
        return rows;
    }

    Expected<std::vector<WorldRadianceProbeState>> WorldRadiancePass::readDiagnosticProbeStates(
        const FenceCompletionValue completed) {
        const auto generation = acquire(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, generation);
        auto &batch = *m_state->m_batch;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               batch.m_temporal_pool != nullptr and batch.m_settings.m_diagnostic_coefficients,
                               std::errc::operation_not_supported));

        std::vector<WorldRadianceProbeState> rows(
            batch.m_plan.m_demand.m_probe_state_bytes / sizeof(WorldRadianceProbeState));
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(
            rows.data(),
            static_cast<const std::byte *>(mapped) +
                batch.m_plan.gameplayOffset() +
                batch.m_plan.m_gameplay_bytes +
                batch.m_plan.m_demand.m_refined_bytes,
            rows.size() * sizeof(rows[0]));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

        for (const auto &row: rows) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   std::isfinite(row.m_variation) and
                                       row.m_variation >= 0.0f and
                                       row.m_variation <= 4.0f and
                                       ((row.m_source_version == std::array<u32, 2>{}) == (row.m_samples == 0u)),
                                   std::errc::result_out_of_range));
        }
        return rows;
    }

    // ------------------------------------------------------------------
    // discovery admission and immutable resource ownership
    // ------------------------------------------------------------------

    std::error_code WorldRadiancePass::prepareDiscoveryPipeline() {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_state and m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                               std::errc::operation_not_permitted));

        const hal::ProfileScope scope{"WorldRadiance.PrepareDiscoveryPipeline"};
        auto &state = *m_state;

        if (not state.m_pipelines[2]) {
            // Direct-only initialization never compiles discovery. Commit this
            // optional immutable pair only after both creations succeed.
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, state.m_module->findEntryPointByName("discoverWorldFaces", entry.writeRef()));

            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = state.m_module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            rhi::ComPtr<rhi::IShaderProgram> program;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                state.m_device->createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program;
            rhi::ComPtr<rhi::IComputePipeline> compiled;
            PPR_LOG(WorldRadiance, info, "World discovery pipeline creation begin");
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, state.m_device->createComputePipeline(pipeline, compiled.writeRef()));
            PPR_LOG(WorldRadiance, info, "World discovery pipeline creation end");

            state.m_programs[2] = std::move(program);
            state.m_pipelines[2] = std::move(compiled);
        }

        return {};
    }

    Expected<WorldRadianceDiscoveryDemand> WorldRadiancePass::prepareDiscovery(
        std::shared_ptr<const WorldRadianceSource> source, const TerrainReceiverCohort cohort,
        const WorldRadianceDiscoverySettings settings, const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   not m_state->m_matter and
                                   not m_state->m_discovery and
                                   not m_state->m_bounce and
                                   source and
                                   settings.m_request_epoch != 0u,
                               std::errc::operation_not_permitted));

        auto &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               state.m_batch and
                                   state.m_batch->m_published and
                                   state.m_batch->m_generation->source().get() == source.get() and
                                   state.m_batch->m_generation->cohort() == cohort and
                                   slotFenceComplete(completed, state.m_batch->m_fence),
                               std::errc::device_or_resource_busy));

        const hal::ProfileScope scope{"WorldRadiance.PrepareDiscovery"};
        const auto &data = *source->m_data;
        const auto &limits = state.m_device->getInfo().limits;

        WorldRadianceSettings validation{};
        validation.m_transport = WorldRadianceTransport::direct_window;
        validation.m_refined_spacing = WorldRadianceRefinedSpacing::native_cell_centres;
        validation.m_receiver_window = settings.m_window;
        const auto validated = plan_(data.m_width, data.m_height, validation, limits.maxComputeDispatchThreadGroups[0]);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, validated);

        const auto levels = indirectLevels_(data.m_width, data.m_height, settings.m_window);
        const u64 max_segments = u64{data.m_width} + data.m_height + 4u;
        const u64 trace_quantum = std::min<u64>(262144u, std::numeric_limits<u32>::max() / max_segments);

        auto candidate = std::make_unique<State::DiscoveryBatch>();
        candidate->m_settings = settings;
        const bool support_screen = settings.m_readback_keys or
                                    (settings.m_measure_gpu and data.m_projected and not state.m_support_screen_done);
        candidate->m_settings.m_readback_keys = settings.m_readback_keys or support_screen;
        auto &demand = candidate->m_demand;
        for (u32 level = 3u; level != 0u; --level) {
            const u32 calls_per_record = level == 3u ? 1u : 16u;
            const u32 quantum = static_cast<u32>(
                std::min(trace_quantum / calls_per_record, u64{limits.maxComputeDispatchThreadGroups[0]} * 64u));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(quantum != 0u, std::errc::not_supported));

            for (u32 first = 0u; first < levels[level].m_records;) {
                const u32 count = std::min(quantum, levels[level].m_records - first);
                candidate->m_dispatches.push_back({level, level == 3u ? level : level + 1u, first, count});
                first += count;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_record_count, levels[level].m_records));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, add_(demand.m_trace_upper_bound, u64{levels[level].m_records} * calls_per_record));
        }

        const u32 rows = settings.m_window.m_counts[0] * settings.m_window.m_counts[1];
        const u32 refine_quantum = static_cast<u32>(
            std::min(trace_quantum / 64u, u64{limits.maxComputeDispatchThreadGroups[0]} * 64u));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(refine_quantum != 0u, std::errc::not_supported));
        for (u32 first = 0u; first < rows;) {
            const u32 count = std::min(refine_quantum, rows - first);
            candidate->m_dispatches.push_back({0u, 1u, first, count});
            first += count;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_record_count, rows));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_trace_upper_bound, u64{rows} * 64u));
        const u64 face_keys = 4u * u64{data.m_width} * data.m_height;
        const auto structural = multiply_(4u, data.m_surface_cells);
        const auto partial_faces = multiply_(4u, data.m_partial_cells);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, structural);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, partial_faces);

        u64 dependency_bound = demand.m_trace_upper_bound;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(dependency_bound, *partial_faces));
        demand.m_key_capacity = std::max<u64>(1u, std::min({face_keys, *structural, dependency_bound}));
        if (settings.m_key_capacity_limit != 0u) {
            demand.m_key_capacity = std::min(demand.m_key_capacity, u64{settings.m_key_capacity_limit});
        }

        demand.m_batches = safe_narrowing(candidate->m_dispatches.size());
        demand.m_bitset_bytes = ((face_keys + 31u) / 32u) * sizeof(u32);
        demand.m_key_bytes = demand.m_key_capacity * sizeof(u32);
        const u64 readback_bytes = 12u * sizeof(u32) +
                                   (candidate->m_settings.m_readback_keys ? demand.m_key_bytes : 0u);
        const u64 levels_bytes = sizeof(levels);
        const u64 directions_bytes = 336u * sizeof(GpuDirection);
        demand.m_owned_bytes = sizeof(State::DiscoveryBatch) +
                               sizeof(WorldRadianceFaceDiscovery) +
                               candidate->m_dispatches.capacity() * sizeof(GpuDiscoveryDispatch) +
                               candidate->m_dispatches.size() * sizeof(rhi::ComPtr<rhi::IShaderObject>);
        for (const u64 bytes:
            {demand.m_largest_resource_bytes, demand.m_bitset_bytes, demand.m_key_bytes, readback_bytes, levels_bytes,
                directions_bytes, 12u * u64{sizeof(u32)}, 16u * u64{demand.m_batches}}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_owned_bytes, bytes));
        }

        if (support_screen) {
            // Retained result vector plus the existing optional public returned
            // copy are distinct CPU allocations; both are admitted up front.
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_owned_bytes, demand.m_key_bytes));
        }

        // The donor batch keeps the exact same-device immutable input alive;
        // discovery owns ComPtr leases, not a second source upload or cache.
        demand.m_peak_bytes = state.m_batch->m_plan.m_demand.m_candidate_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, state.chargeGameplayFields(demand.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, settings.m_reserve_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, demand.m_owned_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, directions_bytes));
        if (candidate->m_settings.m_readback_keys) {
            // Charge worst-case capacity of an additional returned CPU vector,
            // alongside the retained result snapshot and readback allocation.
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, demand.m_key_bytes));
        }

        std::vector<const void *> counted_sources;
        data.markInputs(counted_sources, source.get());
        counted_sources.push_back(state.m_batch->m_static_input.get());
        const auto count_source = [&](const WorldRadianceSource &input) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                input.m_data->charge(counted_sources, &input, demand.m_peak_bytes, demand.m_largest_resource_bytes));
            return {};
        };

        u64 retained_lighting_largest = state.m_batch->m_own_largest_resource_bytes;
        for (const auto &weak: state.m_generations) {
            if (const auto generation = weak.lock(); generation and generation != state.m_batch->m_generation) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    add_(demand.m_peak_bytes, generation->bufferBytes() + generation->probeStateBytes()));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*generation->source()));
                retained_lighting_largest = std::max(retained_lighting_largest, generation->bufferBytes());
            }
        }

        u64 retained_discovery_largest{};
        for (auto iterator = state.m_discoveries.begin(); iterator != state.m_discoveries.end();) {
            if (const auto discovery = iterator->lock()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(
                                       demand.m_peak_bytes,
                                       discovery->bufferBytes() + discovery->m_support_keys.capacity() * sizeof(u32)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(demand.m_peak_bytes, sizeof(WorldRadianceFaceDiscovery)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*discovery->source()));
                retained_discovery_largest = std::max(
                    {retained_discovery_largest, discovery->m_bits->getDesc().size, discovery->m_keys->getDesc().size});
                ++iterator;
            } else {
                iterator = state.m_discoveries.erase(iterator);
            }
        }

        demand.m_largest_resource_bytes = std::max(
            {demand.m_bitset_bytes, demand.m_key_bytes, readback_bytes, levels_bytes, directions_bytes,
                retained_lighting_largest, retained_discovery_largest});
        for (const auto &weak: state.m_projected_sources) {
            if (const auto retained = weak.lock())
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*retained));
        }

        demand.m_admission_error = require_(
            demand.m_peak_bytes <= settings.m_budget_bytes and
                demand.m_largest_resource_bytes <= settings.m_max_resource_bytes and
                demand.m_largest_resource_bytes <= limits.maxBufferSize,
            std::errc::no_buffer_space);
        if (demand.m_admission_error) {
            return demand;
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, prepareDiscoveryPipeline());

        candidate->m_result = std::shared_ptr<WorldRadianceFaceDiscovery>{new WorldRadianceFaceDiscovery{}};
        auto &result = *candidate->m_result;
        result.m_source = std::move(source);
        result.m_cohort = cohort;
        result.m_window = settings.m_window;
        result.m_request_epoch = settings.m_request_epoch;
        result.m_support_requested = support_screen;
        result.m_support_explicit = settings.m_readback_keys;
        result.m_work_quanta = candidate->m_dispatches.size();
        result.m_bytes = demand.m_bitset_bytes + demand.m_key_bytes;
        result.m_statistics.m_query_upper_bound = demand.m_trace_upper_bound;
        result.m_statistics.m_expected_rows = demand.m_record_count;

        candidate->m_ids = state.m_batch->m_ids;
        candidate->m_materials = state.m_batch->m_materials;
        candidate->m_overlay = state.m_batch->m_overlay;
        candidate->m_overlay_lookup = state.m_batch->m_overlay_lookup;
        candidate->m_overlay_ancestors = state.m_batch->m_overlay_ancestors;
        candidate->m_static_input = state.m_batch->m_static_input;
        candidate->m_vacuum = state.m_batch->m_vacuum;
        candidate->m_hierarchy = state.m_batch->m_hierarchy;

        std::vector<GpuDirection> directions;
        directions.reserve(336u);
        for (u32 level = 1u; level <= 3u; ++level) {
            for (u32 index = 0u; index < levels[level].m_directions; ++index) {
                const double angle = 2.0 * std::numbers::pi * (double(index) + .5) / levels[level].m_directions;
                directions.push_back({static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))});
            }
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(*state.m_device, demand.m_bitset_bytes, sizeof(u32), nullptr, false, result.m_bits));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(*state.m_device, demand.m_key_bytes, sizeof(u32), nullptr, false, result.m_keys));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, levels_bytes, sizeof(GpuLevel), levels.data(), false, candidate->m_levels));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, directions_bytes, sizeof(GpuDirection), directions.data(), false,
                               candidate->m_directions));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, 8u * sizeof(u32), sizeof(u32), nullptr, false, candidate->m_counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, 4u * sizeof(u32), sizeof(u32), nullptr, false, candidate->m_append_state));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(*state.m_device, readback_bytes, 0u, nullptr, true, candidate->m_readback));

        auto parameters = state.m_batch->m_plan.m_parameters;
        parameters.m_refined_min_x = settings.m_window.m_minimum[0];
        parameters.m_refined_min_y = settings.m_window.m_minimum[1];
        parameters.m_refined_count_x = settings.m_window.m_counts[0];
        parameters.m_refined_count_y = settings.m_window.m_counts[1];
        candidate->m_roots.resize(candidate->m_dispatches.size());
        for (u32 index = 0u; index < candidate->m_dispatches.size(); ++index) {
            auto &dispatch = candidate->m_dispatches[index];
            dispatch.m_key_capacity = static_cast<u32>(demand.m_key_capacity);
            dispatch.m_bitset_words = static_cast<u32>(demand.m_bitset_bytes / sizeof(u32));
            auto &root = candidate->m_roots[index];
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createRootShaderObject(state.m_programs[2], root.writeRef()));

            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_world"].getDereferenced(constants));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, constants.setData(&parameters, sizeof(parameters)));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_discovery"].getDereferenced(constants));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, constants.setData(&dispatch, sizeof(dispatch)));
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 13u>{
                     std::pair{"g_overlay", candidate->m_overlay.get()},
                     {"g_overlay_lookup", candidate->m_overlay_lookup.get()},
                     {"g_overlay_ancestors", candidate->m_overlay_ancestors.get()},
                     {"g_cell_ids", candidate->m_ids.get()}, {"g_materials", candidate->m_materials.get()},
                     {"g_vacuum", candidate->m_vacuum.get()}, {"g_hierarchy", candidate->m_hierarchy.get()},
                     {"g_levels", candidate->m_levels.get()}, {"g_directions", candidate->m_directions.get()},
                     {"g_counts", candidate->m_counts.get()}, {"g_discovery_bits", result.m_bits.get()},
                     {"g_discovery_keys", result.m_keys.get()},
                     {"g_discovery_state", candidate->m_append_state.get()}}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            }
        }

        if (settings.m_measure_gpu) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(state.m_device->getInfo().timestampFrequency != 0u, std::errc::not_supported));
            rhi::QueryPoolDesc queries{};
            queries.type = rhi::QueryType::Timestamp;
            queries.count = 2u * demand.m_batches;
            queries.label = "World contributor discovery quanta";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createQueryPool(queries, candidate->m_queries.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, candidate->m_queries->reset(0u, queries.count));
        }

        const auto accepted = demand;
        state.m_discovery = std::move(candidate);
        return accepted;
    }

    // ------------------------------------------------------------------
    // one discovery quantum per actual rendered frame, explicit settlement
    // ------------------------------------------------------------------

    bool WorldRadiancePass::canEncodeDiscovery() const noexcept {
        return m_state and
               not m_state->m_unknown and
               not m_state->m_matter and
               m_state->m_discovery and
               not m_state->m_discovery->m_encoded and
               not m_state->m_discovery->m_submitted and
               m_state->m_discovery->m_next < m_state->m_discovery->m_dispatches.size();
    }

    std::error_code WorldRadiancePass::encodeDiscovery(const PreRenderContext &context, const u64 actual_frame) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               canEncodeDiscovery() and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_device == &context.m_device,
                               std::errc::operation_not_permitted));

        auto &batch = *m_state->m_discovery;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               not m_state->m_last_discovery_frame or *m_state->m_last_discovery_frame < actual_frame,
                               std::errc::operation_not_permitted));

        const hal::ProfileScope scope{"WorldRadiance.EncodeDiscoveryQuantum"};
        m_state->m_last_discovery_frame = actual_frame;
        batch.m_last_frame = actual_frame;
        batch.m_encoded = true;
        auto &encoder = context.m_encoder;
        if (batch.m_queries) {
            encoder.writeTimestamp(batch.m_queries, 2u * batch.m_next);
        }

        if (batch.m_next == 0u) {
            for (auto *const buffer: {batch.m_result->m_bits.get(), batch.m_append_state.get()}) {
                encoder.setBufferState(buffer, rhi::ResourceState::CopyDestination);
                encoder.clearBuffer(buffer, 0u, buffer->getDesc().size);
            }
        } else {
            encoder.setBufferState(batch.m_append_state, rhi::ResourceState::CopyDestination);
            encoder.clearBuffer(batch.m_append_state, 2u * sizeof(u32), sizeof(u32));
        }

        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopyDestination);
        encoder.clearBuffer(batch.m_counts, 0u, 8u * sizeof(u32));
        for (auto *const buffer:
            {batch.m_overlay.get(), batch.m_overlay_lookup.get(), batch.m_overlay_ancestors.get(), batch.m_ids.get(),
                batch.m_materials.get(), batch.m_vacuum.get(), batch.m_hierarchy.get(), batch.m_levels.get(),
                batch.m_directions.get()}) {
            encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);
        }

        for (auto *const buffer:
            {batch.m_counts.get(), batch.m_append_state.get(), batch.m_result->m_bits.get(),
                batch.m_result->m_keys.get()}) {
            encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);
        }

        auto *const pass = encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));

        pass->bindPipeline(m_state->m_pipelines[2], batch.m_roots[batch.m_next]);
        pass->dispatchCompute((batch.m_dispatches[batch.m_next].m_count + 63u) / 64u, 1u, 1u);
        pass->end();

        encoder.globalBarrier();
        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
        encoder.setBufferState(batch.m_append_state, rhi::ResourceState::CopySource);
        encoder.setBufferState(batch.m_readback, rhi::ResourceState::CopyDestination);
        encoder.copyBuffer(batch.m_readback, 0u, batch.m_counts, 0u, 8u * sizeof(u32));
        encoder.copyBuffer(batch.m_readback, 8u * sizeof(u32), batch.m_append_state, 0u, 4u * sizeof(u32));
        if (batch.m_settings.m_readback_keys and batch.m_next + 1u == batch.m_dispatches.size()) {
            encoder.setBufferState(batch.m_result->m_keys, rhi::ResourceState::CopySource);
            encoder.copyBuffer(
                batch.m_readback, 12u * sizeof(u32), batch.m_result->m_keys, 0u, batch.m_demand.m_key_bytes);
        }

        if (batch.m_queries) {
            encoder.writeTimestamp(batch.m_queries, 2u * batch.m_next + 1u);
        }
        return {};
    }

    Expected<bool> WorldRadiancePass::advanceDiscovery(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_discovery,
                               std::errc::operation_not_permitted));

        auto &batch = *m_state->m_discovery;
        if (batch.m_next == batch.m_dispatches.size()) {
            return true;
        }

        if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence)) {
            return false;
        }

        batch.m_completed = true;
        double milliseconds = -1.0;
        if (batch.m_queries) {
            rhi::QueryResultState readiness{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, make_error_code(batch.m_queries->getResultState(2u * batch.m_next, 2u, &readiness)));
            if (readiness == rhi::QueryResultState::Pending) {
                return false;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(readiness == rhi::QueryResultState::Resolved, std::errc::state_not_recoverable));

            std::array<u64, 2u> ticks{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, make_error_code(batch.m_queries->getResult(2u * batch.m_next, 2u, ticks.data())));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(ticks[1] >= ticks[0], std::errc::result_out_of_range));

            milliseconds = static_cast<double>(ticks[1] - ticks[0]) *
                           1000.0 /
                           static_cast<double>(m_state->m_device->getInfo().timestampFrequency);
            auto &maximum = batch.m_result->m_max_gpu_milliseconds;
            maximum = std::max(maximum.value_or(0.0), milliseconds);
        }

        std::array<u32, 12u> counts{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(counts.data(), mapped, sizeof(counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

        const auto &dispatch = batch.m_dispatches[batch.m_next];
        const u64 query_bound = u64{dispatch.m_count} * (dispatch.m_level == 0u      ? 64u
                                                            : dispatch.m_level == 3u ? 1u
                                                                                     : 16u);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               counts[0] == counts[1] and
                                   counts[0] == query_bound and
                                   counts[2] == 0u and
                                   counts[3] == dispatch.m_count and
                                   counts[4] == 0u and
                                   counts[7] == 0u and
                                   counts[8] >= batch.m_result->m_count and
                                   counts[8] <= batch.m_demand.m_key_capacity and
                                   counts[9] == 0u and
                                   counts[10] <= query_bound * (u64{batch.m_result->source()->width()} +
                                                                   batch.m_result->source()->height() +
                                                                   4u) and
                                   counts[10] >= counts[8] - batch.m_result->m_count and
                                   counts[11] >= batch.m_result->m_offscreen_count and
                                   counts[11] <= counts[8],
                               std::errc::result_out_of_range));

        auto &statistics = batch.m_result->m_statistics;
        statistics.m_queries_attempted += counts[0];
        statistics.m_queries_completed += counts[1];
        statistics.m_queries_failed += counts[2];
        statistics.m_rows_written += counts[3];
        statistics.m_invalid_rows += counts[4];
        statistics.m_segments += counts[5];
        statistics.m_skipped_blocks += counts[6];
        statistics.m_status |= counts[7];
        batch.m_result->m_count = counts[8];
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(batch.m_result->m_face_hit_occurrences, counts[10]));
        batch.m_result->m_offscreen_count = counts[11];

        const auto message = std::format(
            "World contributor discovery quantum epoch={} batch={} batches={} frame={} fence={} "
            "level={} records={} trace_calls={} trace_upper={} unique_faces={} hit_occurrences={} conservative_offscreen_distinct={} segments={} skipped={} "
            "GPU_ms={} bitmap_clear_included={} OWNED_bytes={} peak_model_bytes={} external_reserve_ESTIMATE={} "
            "job={} world={} revision={} content={} full_live_VRAM_UNMEASURED=true product_budget_qualified=false",
            batch.m_settings.m_request_epoch, batch.m_next, batch.m_dispatches.size(), *batch.m_last_frame,
            u64{batch.m_fence}, dispatch.m_level, counts[3], counts[0], query_bound, counts[8], counts[10], counts[11],
            counts[5], counts[6], milliseconds, batch.m_next == 0u, batch.m_demand.m_owned_bytes,
            batch.m_demand.m_peak_bytes, batch.m_settings.m_reserve_bytes, batch.m_result->m_cohort.m_job_id,
            batch.m_result->m_cohort.m_world_generation, batch.m_result->m_cohort.m_geometry_revision,
            batch.m_result->m_cohort.m_content_fingerprint);
        PPR_LOG_RAW(WorldRadiance, info, message);

        ++batch.m_next;
        if (batch.m_next == batch.m_dispatches.size()) {
            batch.m_result->m_completion = batch.m_fence;
            return true;
        }

        batch.m_submitted = false;
        return false;
    }

    Expected<std::shared_ptr<const WorldRadianceFaceDiscovery>> WorldRadiancePass::acquireDiscovery(
        const FenceCompletionValue completed) {
        const auto ready = advanceDiscovery(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, ready);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(*ready, std::errc::device_or_resource_busy));

        auto &batch = *m_state->m_discovery;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               batch.m_result->m_statistics.m_rows_written == batch.m_demand.m_record_count,
                               std::errc::result_out_of_range));

        if (not batch.m_published) {
            if (batch.m_result->m_support_requested) {
                std::vector<u32> keys(batch.m_result->faceCount());
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(keys.capacity() <= batch.m_demand.m_key_capacity, std::errc::no_buffer_space));

                void *mapped{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
                if (not keys.empty())
                    std::memcpy(
                        keys.data(), static_cast<const std::byte *>(mapped) + 12u * sizeof(u32),
                        keys.size() * sizeof(u32));

                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

                batch.m_result->m_support_keys = std::move(keys);
                batch.m_result->m_support_keys_ready = true;
            }

            m_state->m_discoveries.push_back(batch.m_result);
            batch.m_published = true;
        }
        return std::shared_ptr<const WorldRadianceFaceDiscovery>{batch.m_result};
    }

    Expected<bool> WorldRadiancePass::cancelDiscovery(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                               std::errc::operation_not_permitted));

        if (not m_state->m_discovery) {
            return true;
        }

        const auto &batch = *m_state->m_discovery;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(not batch.m_encoded, std::errc::operation_in_progress));
        if (batch.m_submitted and not slotFenceComplete(completed, batch.m_fence)) {
            return false;
        }

        m_state->m_discovery.reset();
        return true;
    }

    Expected<std::vector<u32>> WorldRadiancePass::readDiscoveryKeys(const FenceCompletionValue completed) {
        const auto result = acquireDiscovery(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, result);
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(m_state->m_discovery->m_settings.m_readback_keys, std::errc::operation_not_supported));

        if ((*result)->m_support_keys_ready)
            return (*result)->m_support_keys;

        std::vector<u32> keys((*result)->faceCount());
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            m_state->m_device->mapBuffer(m_state->m_discovery->m_readback, rhi::CpuAccessMode::Read, &mapped));
        if (not keys.empty()) {
            std::memcpy(
                keys.data(), static_cast<const std::byte *>(mapped) + 12u * sizeof(u32), keys.size() * sizeof(u32));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(m_state->m_discovery->m_readback));
        return keys;
    }

    // ------------------------------------------------------------------
    // completed contributor keys to one immutable single-bounce window
    // ------------------------------------------------------------------

    Expected<WorldRadianceBounceDemand> WorldRadiancePass::prepareBounce(
        std::shared_ptr<const WorldRadianceFaceDiscovery> discovery, const WorldRadianceBounceSettings settings,
        const FenceCompletionValue completed, const ITimerClock *const diagnostic_clock) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   not m_state->m_matter and
                                   not m_state->m_bounce and
                                   not m_state->m_discovery and
                                   discovery,
                               std::errc::operation_not_permitted));
        auto &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               state.m_batch and
                                   state.m_batch->m_published and
                                   state.m_batch->m_generation->transport() == WorldRadianceTransport::direct_window and
                                   state.m_batch->m_generation->source().get() == discovery->source().get() and
                                   state.m_batch->m_generation->cohort() == discovery->cohort() and
                                   settings.m_request_epoch == discovery->requestEpoch() and
                                   settings.m_request_epoch != 0u and
                                   slotFenceComplete(completed, discovery->completionFence()) and
                                   slotFenceComplete(completed, state.m_batch->m_fence) and
                                   std::ranges::any_of(
                                       state.m_discoveries, [&](const auto &weak) { return weak.lock() == discovery; }),
                               std::errc::invalid_argument));

        const hal::ProfileScope scope{"WorldRadiance.PrepareSingleBounce"};
        const auto window = discovery->window();
        const auto direct = state.m_batch->m_generation;
        const auto direct_minimum = direct->probeMinimum();
        const auto direct_counts = direct->probeCounts();
        for (u32 axis = 0u; axis < 2u; ++axis) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    window.m_counts[axis] > 0u and
                        window.m_minimum[axis] >= direct_minimum[axis] and
                        u64{static_cast<u32>(window.m_minimum[axis] - direct_minimum[axis])} + window.m_counts[axis] <=
                            direct_counts[axis],
                    std::errc::invalid_argument));
        }

        auto candidate = std::make_unique<State::BounceBatch>();
        candidate->m_settings = settings;
        candidate->m_discovery = std::move(discovery);
        candidate->m_direct = direct;
        auto &demand = candidate->m_demand;
        demand.m_faces = candidate->m_discovery->faceCount();
        demand.m_lookup_capacity = 1u;
        while (demand.m_lookup_capacity < 2u * demand.m_faces) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(demand.m_lookup_capacity <= std::numeric_limits<u32>::max() / 2u, std::errc::value_too_large));
            demand.m_lookup_capacity *= 2u;
        }

        auto levels = indirectLevels_(direct->source()->width(), direct->source()->height(), window);
        std::vector<GpuDirection> directions = state.m_batch->m_plan.m_directions;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(directions.size() == 256u));
        std::array<u64, 2u> parity_bytes{sizeof(GpuInterval), sizeof(GpuInterval)};
        for (u32 level = 1u; level <= 3u; ++level) {
            auto &value = levels[level];
            value.m_direction_offset = safe_narrowing(directions.size());
            for (u32 bin = 0u; bin < value.m_directions; ++bin) {
                const double angle = 2.0 * std::numbers::pi * (double(bin) + .5) / value.m_directions;
                directions.push_back({static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))});
            }

            parity_bytes[level % 2u] = std::max(parity_bytes[level % 2u], u64{value.m_records} * sizeof(GpuInterval));
        }

        const auto &limits = state.m_device->getInfo().limits;
        const u64 groups_rows = u64{limits.maxComputeDispatchThreadGroups[0]} * 64u;
        const auto append = [&](const State::BounceOperation operation, const u32 count, const u32 calls,
                                const u32 level, const u32 parent) -> std::error_code {
            const u64 dispatch_rows = operation == State::BounceOperation::moments
                                          ? u64{limits.maxComputeDispatchThreadGroups[0]}
                                          : groups_rows;
            const u32 quantum = static_cast<u32>(std::min(dispatch_rows, u64{262144u / std::max(1u, calls)}));
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(quantum != 0u, std::errc::not_supported));
            for (u32 first = 0u; first < count;) {
                const u32 rows = std::min(quantum, count - first);
                GpuDispatch dispatch{level, parent, rows, 1u};
                if (operation == State::BounceOperation::combine) {
                    dispatch.m_min_x = direct_minimum[0];
                    dispatch.m_min_y = direct_minimum[1];
                    dispatch.m_count_x = direct_counts[0];
                    dispatch.m_count_y = direct_counts[1];
                }

                candidate->m_work.push_back({u64{rows} * calls, dispatch, operation, first});
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, add_(demand.m_trace_upper_bound, u64{rows} * calls));
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, add_(demand.m_record_count, rows));
                first += rows;
            }

            return {};
        };

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, append(State::BounceOperation::lookup, static_cast<u32>(demand.m_faces), 0u, 0u, 0u));
        if (demand.m_faces == 0u) {
            // Explicit empty table initialization still has a real submission/fence.
            candidate->m_work.push_back({0u, {}, State::BounceOperation::lookup, 0u});
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, append(State::BounceOperation::moments, static_cast<u32>(demand.m_faces), 128u, 0u, 0u));

        for (u32 level = 3u; level != 0u; --level) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, append(
                                   State::BounceOperation::coarse, levels[level].m_records, level == 3u ? 1u : 16u,
                                   level, level == 3u ? level : level + 1u));
        }

        const u32 rows = window.m_counts[0] * window.m_counts[1];
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, append(State::BounceOperation::refine, rows, 64u, 0u, 1u));

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, append(State::BounceOperation::combine, rows, 0u, 0u, 0u));

        demand.m_batches = safe_narrowing(candidate->m_work.size());
        demand.m_moment_bytes = std::max<u64>(1u, demand.m_faces) * 3u * sizeof(float);
        demand.m_lookup_bytes = demand.m_lookup_capacity * sizeof(u32);
        demand.m_indirect_bytes = u64{rows} * sizeof(WorldRadianceCoefficients);
        demand.m_combined_bytes = demand.m_indirect_bytes;
        demand.m_parity_bytes = parity_bytes[0] + parity_bytes[1];
        const u64 directions_bytes = directions.size() * sizeof(GpuDirection);
        demand.m_owned_bytes = sizeof(State::BounceBatch) +
                               sizeof(WorldRadianceGeneration) +
                               candidate->m_work.capacity() * sizeof(State::BounceWork) +
                               candidate->m_work.size() * sizeof(rhi::ComPtr<rhi::IShaderObject>);
        for (const u64 bytes:
            {demand.m_moment_bytes, demand.m_lookup_bytes, demand.m_indirect_bytes, demand.m_combined_bytes,
                parity_bytes[0], parity_bytes[1], u64{sizeof(levels)}, directions_bytes, 16u * u64{sizeof(u32)},
                settings.m_measure_gpu ? 16u * u64{demand.m_batches} : 0u}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_owned_bytes, bytes));
            demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, bytes);
        }

        // Shared input buffers have one owner ledger. The compact source and
        // retained discovery keys/bitset remain charged once by owner identity.
        demand.m_peak_bytes = state.m_batch->m_plan.m_demand.m_candidate_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, state.chargeGameplayFields(demand.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, demand.m_owned_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, settings.m_reserve_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, add_(demand.m_peak_bytes, directions.capacity() * sizeof(GpuDirection) + sizeof(levels)));
        std::vector<const void *> counted_sources;
        direct->source()->m_data->markInputs(counted_sources, direct->source().get());
        counted_sources.push_back(state.m_batch->m_static_input.get());
        const auto count_source = [&](const WorldRadianceSource &input) -> std::error_code {
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                input.m_data->charge(counted_sources, &input, demand.m_peak_bytes, demand.m_largest_resource_bytes));
            return {};
        };

        for (const auto &weak: state.m_generations) {
            if (const auto generation = weak.lock(); generation and generation != direct) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    add_(
                        demand.m_peak_bytes,
                        generation->bufferBytes() + generation->probeStateBytes() + sizeof(WorldRadianceGeneration)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*generation->source()));
                demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, generation->bufferBytes());
            }
        }

        for (const auto &weak: state.m_discoveries) {
            if (const auto value = weak.lock()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(
                                       demand.m_peak_bytes, value->bufferBytes() +
                                                                sizeof(WorldRadianceFaceDiscovery) +
                                                                value->m_support_keys.capacity() * sizeof(u32)));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*value->source()));
                demand.m_largest_resource_bytes = std::max(
                    {demand.m_largest_resource_bytes, value->m_bits->getDesc().size, value->m_keys->getDesc().size});
            }
        }

        demand.m_largest_resource_bytes = std::max(
            demand.m_largest_resource_bytes, state.m_batch->m_own_largest_resource_bytes);
        for (const auto &weak: state.m_projected_sources) {
            if (const auto retained = weak.lock())
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(*retained));
        }

        demand.m_admission_error = require_(
            demand.m_peak_bytes <= settings.m_budget_bytes and
                demand.m_largest_resource_bytes <= settings.m_max_resource_bytes and
                demand.m_largest_resource_bytes <= limits.maxBufferSize,
            std::errc::no_buffer_space);
        if (demand.m_admission_error)
            return demand;

        if (candidate->m_discovery->m_support_requested and
            (candidate->m_discovery->m_support_explicit or not state.m_support_screen_done)) {
            const hal::ProfileScope support_scope{"WorldRadiance.CountEmitterSupport"};
            const TimePoint cpu_begin = diagnostic_clock ? diagnostic_clock->now() : TimePoint{};
            const auto &data = *direct->source()->m_data;
            const auto &keys = candidate->m_discovery->m_support_keys;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   candidate->m_discovery->m_support_keys_ready and
                                   keys.size() == demand.m_faces and
                                   candidate->m_discovery->source().get() == direct->source().get() and
                                   slotFenceComplete(completed, candidate->m_discovery->completionFence())));

            WorldRadianceSupportCounts support{};
            support.m_emitter_bounds = data.m_emitters.size();
            support.m_cpu_key_capacity_bytes = keys.capacity() * sizeof(u32);
            support.m_key_readback_bytes = candidate->m_discovery->m_keys->getDesc().size;
            support.m_projection_quanta = data.m_projected ? data.m_projected->m_work_quanta : 0u;
            support.m_discovery_quanta = candidate->m_discovery->m_work_quanta;
            support.m_direct_quanta = state.m_batch->m_plan.m_strips.size();
            const bool sky = std::ranges::any_of(data.m_sky, [](const float value) { return value > 0.0f; });

            u64 maximum_calls = state.m_batch->m_plan.m_demand.m_query_upper_bound;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(maximum_calls, demand.m_faces * 128u));
            const auto maximum_tests = multiply_(maximum_calls, data.m_emitters.size());
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, maximum_tests);
            const auto included = [&](const ReceiverPage &box, const GpuDirection direction) {
                if (sky)
                    return true;

                for (const auto &emitter: data.m_emitters) {
                    ++support.m_support_tests; // Bounded by maximum_tests checked above.
                    if (pageMayReach_(box, emitter, direction))
                        return true;
                }

                return false;
            };

            const auto &plan = state.m_batch->m_plan;
            for (u32 quantum = 0u; quantum < plan.m_strips.size(); ++quantum) {
                const auto &strip = plan.m_strips[quantum];
                u64 baseline{}, kept{};
                for (u32 y = 0u; y < strip.m_count_y; ++y) {
                    for (u32 x = 0u; x < strip.m_count_x; ++x) {
                        const ReceiverPage box{
                            strip.m_min_x + static_cast<i32>(x), strip.m_min_y + static_cast<i32>(y), 1u, 1u};
                        for (u32 bin = 0u; bin < 256u; ++bin) {
                            if ((plan.m_pages[strip.m_page].m_mask[bin / 32u] & (u32{1u} << (bin % 32u))) == 0u)
                                continue;

                            ++baseline;
                            kept += included(box, plan.m_directions[bin]) ? 1u : 0u;
                        }
                    }
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(baseline == strip.m_queries and kept <= baseline));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(support.m_direct_baseline, baseline));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(support.m_direct_included, kept));

                const auto message = std::format(
                    "WORLD_SUPPORT_DIRECT epoch={} quantum={} page={} field_fence={} baseline_CURRENT_mask={} included={} "
                    "zero_proved={} receiver_box=1x1 no_queries_changed=true",
                    settings.m_request_epoch, quantum, strip.m_page, u64{direct->completionFence()}, baseline, kept,
                    baseline - kept);
                PPR_LOG_RAW(WorldRadiance, info, message);
            }

            for (u32 quantum = 0u; quantum < candidate->m_work.size(); ++quantum) {
                const auto &work = candidate->m_work[quantum];
                if (work.m_operation != State::BounceOperation::moments) {
                    ++support.m_other_bounce_quanta;
                    continue;
                }

                ++support.m_moment_quanta;
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(u64{work.m_first} + work.m_dispatch.m_count <= keys.size()));

                u64 baseline{}, kept{};
                for (u32 row = 0u; row < work.m_dispatch.m_count; ++row) {
                    const u32 key = keys[work.m_first + row], cell = key >> 2u, side = key & 3u;
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, require_(u64{cell} < u64{data.m_width} * data.m_height));

                    // The existing centre-support proof needs this same one-cell
                    // halo to enclose exact boundary face/empty-side centres.
                    const ReceiverPage box{
                        static_cast<i32>(cell % data.m_width) - 1, static_cast<i32>(cell / data.m_width) - 1, 3u, 3u};
                    for (u32 bin = 0u; bin < 256u; ++bin) {
                        const auto direction = plan.m_directions[bin];
                        const float component = side / 2u == 0u ? direction.m_x : direction.m_y;
                        if (side % 2u == 0u ? component >= 0.0f : component <= 0.0f)
                            continue;

                        ++baseline;
                        kept += included(box, direction) ? 1u : 0u;
                    }
                }

                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(baseline == work.m_queries and kept <= baseline));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(support.m_moment_baseline, baseline));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(support.m_moment_included, kept));

                const auto message = std::format(
                    "WORLD_SUPPORT_MOMENT epoch={} quantum={} first_face={} face_count={} baseline_outward128={} "
                    "included={} zero_proved={} future_actual_frame=UNMEASURED no_queries_changed=true",
                    settings.m_request_epoch, quantum, work.m_first, work.m_dispatch.m_count, baseline, kept,
                    baseline - kept);
                PPR_LOG_RAW(WorldRadiance, info, message);
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   support.m_direct_baseline == plan.m_demand.m_query_upper_bound and
                                   support.m_moment_baseline == demand.m_faces * 128u and
                                   support.m_support_tests <= *maximum_tests));

            // Existing work-call ceilings only. Product GPU/age thresholds and
            // calibrated equal-cost-per-ray screening belong to offline analysis.
            const u64 direct_page_rows = u64{std::min(32u, plan.m_parameters.m_refined_count_x)} *
                                         std::min(32u, plan.m_parameters.m_refined_count_y);
            const u64 direct_capacity = std::min<u64>(
                                            direct_page_rows, state.m_batch->m_settings.m_receiver_rows_per_quantum) *
                                        256u;
            constexpr u64 moment_capacity = 262144u;
            support.m_packed_direct_quanta = support.m_direct_included / direct_capacity +
                                             (support.m_direct_included % direct_capacity != 0u ? 1u : 0u);
            support.m_packed_moment_quanta = support.m_moment_included / moment_capacity +
                                             (support.m_moment_included % moment_capacity != 0u ? 1u : 0u);

            for (const u64 quanta:
                {support.m_projection_quanta, support.m_discovery_quanta, support.m_other_bounce_quanta,
                    support.m_packed_direct_quanta, support.m_packed_moment_quanta}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(support.m_optimistic_work_frames, quanta));
            }

            support.m_cpu_measured = diagnostic_clock != nullptr;
            if (diagnostic_clock) {
                support.m_cpu_elapsed = diagnostic_clock->now() - cpu_begin;
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(support.m_cpu_elapsed >= TimeSpan{}));
            }

            const auto identity = direct->source()->matterIdentity();
            const auto message = std::format(
                "WORLD_SUPPORT_COUNTS epoch={} source_instance={} list_owner={} discovery_fence={} field_fence={} "
                "job={} world={} content={} revision={} matter_pub={} joint={} device={} source_fence={} "
                "window_min=({},{}) counts=({},{}) faces={} emitter_bounds={} support_tests={} "
                "direct_baseline_CURRENT_masks={} direct_included={} direct_zero_proved={} moment_baseline128={} moment_included={} moment_zero_proved={} "
                "projection_quanta={} direct_quanta={} discovery_quanta={} moment_quanta={} other_bounce_quanta={} "
                "packed_direct={} packed_moments={} optimistic_REQUIRED_WORK_frames_lower_bound={} "
                "work_call_caps=({},{}) fixed_latency=UNMEASURED cadence=UNMEASURED "
                "GPU_cost_calibration=OFFLINE measured_GPU_age_PASS=false verdict=UNQUALIFIED_COUNTS_ONLY "
                "CPU_count_ms={} CPU_timing={} CPU_key_map_time=UNMEASURED GPU_key_copy_in_discovery_quantum=true "
                "CPU_keys_capacity_bytes={} key_copy_payload_bytes={} "
                "readback_allocation_bytes={} owned_model_bytes={} peak_model_bytes={} CPU_bound_bytes={} diagnostic_counts_only=true",
                settings.m_request_epoch, reinterpret_cast<std::uintptr_t>(direct->source().get()),
                reinterpret_cast<std::uintptr_t>(candidate->m_discovery->keysBuffer()),
                u64{candidate->m_discovery->completionFence()}, u64{direct->completionFence()},
                direct->cohort().m_job_id, direct->cohort().m_world_generation, direct->cohort().m_content_fingerprint,
                direct->cohort().m_geometry_revision, identity ? identity->m_particles.m_publication : 0u,
                identity ? identity->m_particles.m_joint_tick : 0u, identity ? identity->m_device_epoch : 0u,
                u64{direct->source()->matterCompletionFence()}, window.m_minimum[0], window.m_minimum[1],
                window.m_counts[0], window.m_counts[1], demand.m_faces, support.m_emitter_bounds,
                support.m_support_tests, support.m_direct_baseline, support.m_direct_included,
                support.m_direct_baseline - support.m_direct_included, support.m_moment_baseline,
                support.m_moment_included, support.m_moment_baseline - support.m_moment_included,
                support.m_projection_quanta, support.m_direct_quanta, support.m_discovery_quanta,
                support.m_moment_quanta, support.m_other_bounce_quanta, support.m_packed_direct_quanta,
                support.m_packed_moment_quanta, support.m_optimistic_work_frames, direct_capacity, moment_capacity,
                time::seconds(support.m_cpu_elapsed) * 1000.0, support.m_cpu_measured ? "explicit_clock" : "UNMEASURED",
                support.m_cpu_key_capacity_bytes, support.m_key_readback_bytes,
                12u * sizeof(u32) + candidate->m_discovery->m_keys->getDesc().size, demand.m_owned_bytes,
                demand.m_peak_bytes, data.m_emitters.capacity() * sizeof(EmitterBounds));
            PPR_LOG_RAW(WorldRadiance, info, message);

            demand.m_support_counts = support;
            if (not candidate->m_discovery->m_support_explicit)
                state.m_support_screen_done = true; // Only a successful actual count consumes the once-only screen.
        }

        // Immutable optional programs stay in this pass. No new session/cache;
        // a partial compile owns only completed pairs and can shut down safely.
        constexpr std::array names{
            "buildWorldFaceLookup", "bakeWorldFaceMomentsParallel", "solveWorldBounceCascade", "refineWorldBounce",
            "combineWorldBounce"};
        for (u32 index = 0u; index < names.size(); ++index) {
            if (state.m_pipelines[index + 3u])
                continue;

            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = state.m_module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            rhi::ComPtr<rhi::IShaderProgram> program;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                state.m_device->createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = program;
            rhi::ComPtr<rhi::IComputePipeline> compiled;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createComputePipeline(pipeline, compiled.writeRef()));

            state.m_programs[index + 3u] = std::move(program);
            state.m_pipelines[index + 3u] = std::move(compiled);
        }

        candidate->m_generation = std::shared_ptr<WorldRadianceGeneration>{new WorldRadianceGeneration{}};
        auto &generation = *candidate->m_generation;
        generation.m_source = direct->source();
        generation.m_device = state.m_device;
        generation.m_cohort = direct->cohort();
        generation.m_probe_min = window.m_minimum;
        generation.m_probe_counts = window.m_counts;
        generation.m_maximum_response = direct->maximumResponse();
        generation.m_refined_spacing = WorldRadianceRefinedSpacing::native_cell_centres;
        generation.m_transport = WorldRadianceTransport::direct_window;
        generation.m_bytes = demand.m_combined_bytes;
        generation.m_bounce_epoch = settings.m_request_epoch;
        generation.m_statistics.m_query_upper_bound = demand.m_trace_upper_bound;
        generation.m_statistics.m_expected_rows = demand.m_record_count;
        candidate->m_parameters = state.m_batch->m_plan.m_parameters;
        candidate->m_parameters.m_refined_min_x = window.m_minimum[0];
        candidate->m_parameters.m_refined_min_y = window.m_minimum[1];
        candidate->m_parameters.m_refined_count_x = window.m_counts[0];
        candidate->m_parameters.m_refined_count_y = window.m_counts[1];
        candidate->m_ids = state.m_batch->m_ids;
        candidate->m_materials = state.m_batch->m_materials;
        candidate->m_overlay = state.m_batch->m_overlay;
        candidate->m_overlay_lookup = state.m_batch->m_overlay_lookup;
        candidate->m_overlay_ancestors = state.m_batch->m_overlay_ancestors;
        candidate->m_static_input = state.m_batch->m_static_input;
        candidate->m_vacuum = state.m_batch->m_vacuum;
        candidate->m_hierarchy = state.m_batch->m_hierarchy;
        candidate->m_masks = state.m_batch->m_masks;

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, sizeof(levels), sizeof(GpuLevel), levels.data(), false, candidate->m_levels));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, directions_bytes, sizeof(GpuDirection), directions.data(), false,
                               candidate->m_directions));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, demand.m_lookup_bytes, sizeof(u32), nullptr, false, candidate->m_lookup));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, demand.m_moment_bytes, 3u * sizeof(float), nullptr, false, candidate->m_moments));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, demand.m_indirect_bytes, sizeof(WorldRadianceCoefficients), nullptr,
                               false, candidate->m_indirect));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, demand.m_combined_bytes, sizeof(WorldRadianceCoefficients), nullptr,
                               false, generation.m_coefficients));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, 8u * sizeof(u32), sizeof(u32), nullptr, false, candidate->m_counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(*state.m_device, 8u * sizeof(u32), 0u, nullptr, true, candidate->m_readback));
        for (u32 parity = 0u; parity < 2u; ++parity) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, parity_bytes[parity], sizeof(GpuInterval), nullptr, false,
                                   candidate->m_parity[parity]));
        }

        candidate->m_roots.resize(candidate->m_work.size());
        for (u32 index = 0u; index < candidate->m_work.size(); ++index) {
            const auto &work = candidate->m_work[index];
            auto &root = candidate->m_roots[index];
            const u32 program = 3u + static_cast<u32>(work.m_operation);
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createRootShaderObject(state.m_programs[program], root.writeRef()));
            rhi::ShaderCursor cursor{root}, constants{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_world"].getDereferenced(constants));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, constants.setData(&candidate->m_parameters, sizeof(GpuParameters)));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_dispatch"].getDereferenced(constants));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, constants.setData(&work.m_dispatch, sizeof(GpuDispatch)));

            const GpuBounceDispatch dispatch{
                static_cast<u32>(demand.m_faces), static_cast<u32>(demand.m_lookup_capacity), work.m_first, 0u};
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_bounce"].getDereferenced(constants));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, constants.setData(&dispatch, sizeof(dispatch)));

            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 14u>{
                     std::pair{"g_overlay", candidate->m_overlay.get()},
                     {"g_overlay_lookup", candidate->m_overlay_lookup.get()},
                     {"g_overlay_ancestors", candidate->m_overlay_ancestors.get()},
                     {"g_cell_ids", candidate->m_ids.get()}, {"g_materials", candidate->m_materials.get()},
                     {"g_vacuum", candidate->m_vacuum.get()}, {"g_hierarchy", candidate->m_hierarchy.get()},
                     {"g_levels", candidate->m_levels.get()}, {"g_directions", candidate->m_directions.get()},
                     {"g_page_masks", candidate->m_masks.get()}, {"g_counts", candidate->m_counts.get()},
                     {"g_face_keys", candidate->m_discovery->keysBuffer()},
                     {"g_face_lookup", candidate->m_lookup.get()}, {"g_face_moments", candidate->m_moments.get()}}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor[name].setBinding(rhi::Binding(buffer, range_(buffer))));
            }

            // Bind only the active operation's field views; the refined UAV
            // must never alias an inactive combined-input SRV in this root.
            if (work.m_operation == State::BounceOperation::coarse) {
                auto *const parent = candidate->m_parity[(work.m_dispatch.m_level + 1u) % 2u].get();
                auto *const current = candidate->m_parity[work.m_dispatch.m_level % 2u].get();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_parent"].setBinding(rhi::Binding(parent, range_(parent))));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_current"].setBinding(rhi::Binding(current, range_(current))));
            } else if (work.m_operation == State::BounceOperation::refine) {
                auto *const parent = candidate->m_parity[1].get();
                auto *const output = candidate->m_indirect.get();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_parent"].setBinding(rhi::Binding(parent, range_(parent))));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_refined"].setBinding(rhi::Binding(output, range_(output))));
            } else if (work.m_operation == State::BounceOperation::combine) {
                auto *const primary = direct->coefficientsBuffer();
                auto *const indirect = candidate->m_indirect.get();
                auto *const output = generation.m_coefficients.get();
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_bounce_direct"].setBinding(rhi::Binding(primary, range_(primary))));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_bounce_indirect"].setBinding(rhi::Binding(indirect, range_(indirect))));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, cursor["g_refined"].setBinding(rhi::Binding(output, range_(output))));
            }
        }

        if (settings.m_measure_gpu) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(state.m_device->getInfo().timestampFrequency != 0u, std::errc::not_supported));
            rhi::QueryPoolDesc queries{};
            queries.type = rhi::QueryType::Timestamp;
            queries.count = 2u * demand.m_batches;
            queries.label = "Single-bounce producer quanta";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createQueryPool(queries, candidate->m_queries.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, candidate->m_queries->reset(0u, queries.count));
        }

        state.m_bounce = std::move(candidate);
        return demand;
    }

    bool WorldRadiancePass::canEncodeBounce() const noexcept {
        return m_state and
               not m_state->m_unknown and
               not m_state->m_matter and
               m_state->m_bounce and
               not m_state->m_bounce->m_encoded and
               not m_state->m_bounce->m_submitted and
               m_state->m_bounce->m_next < m_state->m_bounce->m_work.size();
    }

    std::error_code WorldRadiancePass::encodeBounce(const PreRenderContext &context, const u64 actual_frame) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               canEncodeBounce() and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_device == &context.m_device,
                               std::errc::operation_not_permitted));
        const hal::ProfileScope scope{"WorldRadiance.EncodeSingleBounce"};
        auto &batch = *m_state->m_bounce;
        const auto &work = batch.m_work[batch.m_next];
        auto &encoder = context.m_encoder;
        batch.m_encoded = true;
        batch.m_actual_frame = actual_frame;
        if (batch.m_queries)
            encoder.writeTimestamp(batch.m_queries, 2u * batch.m_next);

        if (batch.m_next == 0u) {
            encoder.setBufferState(batch.m_lookup, rhi::ResourceState::CopyDestination);
            encoder.clearBuffer(batch.m_lookup, 0u, batch.m_lookup->getDesc().size);
        }

        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopyDestination);
        encoder.clearBuffer(batch.m_counts, 0u, 8u * sizeof(u32));
        for (auto *const buffer:
            {batch.m_overlay.get(), batch.m_overlay_lookup.get(), batch.m_overlay_ancestors.get(), batch.m_ids.get(),
                batch.m_materials.get(), batch.m_vacuum.get(), batch.m_hierarchy.get(), batch.m_masks.get(),
                batch.m_levels.get(), batch.m_directions.get(), batch.m_discovery->keysBuffer(),
                batch.m_direct->coefficientsBuffer()}) {
            encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);
        }

        encoder.setBufferState(batch.m_counts, rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(batch.m_lookup, rhi::ResourceState::UnorderedAccess);
        encoder.setBufferState(batch.m_moments, rhi::ResourceState::UnorderedAccess);
        if (work.m_operation == State::BounceOperation::coarse) {
            encoder.setBufferState(
                batch.m_parity[(work.m_dispatch.m_level + 1u) % 2u], rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_parity[work.m_dispatch.m_level % 2u], rhi::ResourceState::UnorderedAccess);
        } else if (work.m_operation == State::BounceOperation::refine) {
            encoder.setBufferState(batch.m_parity[1], rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_indirect, rhi::ResourceState::UnorderedAccess);
        } else if (work.m_operation == State::BounceOperation::combine) {
            encoder.setBufferState(batch.m_indirect, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::UnorderedAccess);
        }

        encoder.globalBarrier(); // Lookup construction/moment writes precede every dependent read.
        if (work.m_dispatch.m_count != 0u) {
            auto *const pass = encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(
                m_state->m_pipelines[3u + static_cast<u32>(work.m_operation)], batch.m_roots[batch.m_next]);
            const u32 groups = work.m_operation == State::BounceOperation::moments
                                   ? work.m_dispatch.m_count
                                   : (work.m_dispatch.m_count + 63u) / 64u;
            pass->dispatchCompute(groups, 1u, 1u);
            pass->end();
        }

        encoder.globalBarrier();
        encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
        encoder.copyBuffer(batch.m_readback, 0u, batch.m_counts, 0u, 8u * sizeof(u32));
        if (work.m_operation == State::BounceOperation::combine)
            encoder.setBufferState(batch.m_generation->m_coefficients, rhi::ResourceState::ShaderResource);
        if (batch.m_queries)
            encoder.writeTimestamp(batch.m_queries, 2u * batch.m_next + 1u);
        return {};
    }

    Expected<bool> WorldRadiancePass::advanceBounce(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_bounce,
                               std::errc::operation_not_permitted));
        auto &batch = *m_state->m_bounce;
        if (batch.m_next == batch.m_work.size())
            return true;

        if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence))
            return false;

        batch.m_completed = true;

        std::optional<double> milliseconds;
        if (batch.m_queries) {
            rhi::QueryResultState readiness{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, batch.m_queries->getResultState(2u * batch.m_next, 2u, &readiness));
            if (readiness == rhi::QueryResultState::Pending)
                return false;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(readiness == rhi::QueryResultState::Resolved, std::errc::state_not_recoverable));

            std::array<u64, 2u> ticks{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, batch.m_queries->getResult(2u * batch.m_next, 2u, ticks.data()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(ticks[1] >= ticks[0], std::errc::result_out_of_range));
            milliseconds = double(ticks[1] - ticks[0]) *
                           1000.0 /
                           double(m_state->m_device->getInfo().timestampFrequency);
        }

        std::array<u32, 8u> counts{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(counts.data(), mapped, sizeof(counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));
        const auto &work = batch.m_work[batch.m_next];
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               counts[0] == work.m_queries and
                                   counts[0] == counts[1] and
                                   counts[2] == 0u and
                                   counts[3] == work.m_dispatch.m_count and
                                   counts[4] == 0u and
                                   counts[7] == 0u,
                               std::errc::result_out_of_range));

        auto &statistics = batch.m_generation->m_statistics;
        statistics.m_queries_attempted += counts[0];
        statistics.m_queries_completed += counts[1];
        statistics.m_rows_written += counts[3];
        statistics.m_segments += counts[5];
        statistics.m_skipped_blocks += counts[6];
        if (milliseconds)
            batch.m_generation->m_max_gpu_milliseconds = std::max(
                batch.m_generation->m_max_gpu_milliseconds.value_or(0.0), *milliseconds);

        constexpr std::array operation_names{"lookup", "moments", "coarse", "refine", "combine"};
        const auto message = std::format(
            "WORLD_BOUNCE_QUANTUM frame={} epoch={} quantum={} quanta={} operation={} first={} rows={} rays={} "
            "segments={} skipped={} faces={} lookup_capacity={} fence={} GPU_ms={} timing_measured={} "
            "source_instance={} job={} world={} content={} revision={} CPU_prepare_uploads_excluded=true",
            batch.m_actual_frame, batch.m_settings.m_request_epoch, batch.m_next, batch.m_work.size(),
            operation_names[static_cast<u32>(work.m_operation)], work.m_first, counts[3], counts[0], counts[5],
            counts[6], batch.m_demand.m_faces, batch.m_demand.m_lookup_capacity, u64{batch.m_fence},
            milliseconds.value_or(-1.0), milliseconds.has_value(),
            reinterpret_cast<std::uintptr_t>(batch.m_generation->source().get()), batch.m_generation->cohort().m_job_id,
            batch.m_generation->cohort().m_world_generation, batch.m_generation->cohort().m_content_fingerprint,
            batch.m_generation->cohort().m_geometry_revision);
        PPR_LOG_RAW(WorldRadiance, info, message);

        const bool ready = batch.m_next + 1u == batch.m_work.size();
        if (ready) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(
                                   statistics.m_queries_attempted == batch.m_demand.m_trace_upper_bound and
                                       statistics.m_rows_written == batch.m_demand.m_record_count,
                                   std::errc::result_out_of_range));
            batch.m_generation->m_completion = batch.m_fence;
        }

        ++batch.m_next;
        if (not ready)
            batch.m_submitted = false;
        return ready;
    }

    Expected<std::shared_ptr<const WorldRadianceGeneration>> WorldRadiancePass::acquireBounce(
        const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_bounce and
                                   m_state->m_bounce->m_submitted,
                               std::errc::operation_not_permitted));
        auto &batch = *m_state->m_bounce;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               batch.m_next == batch.m_work.size() and slotFenceComplete(completed, batch.m_fence),
                               std::errc::device_or_resource_busy));
        if (not batch.m_published) {
            m_state->m_generations.push_back(batch.m_generation);
            batch.m_published = true;
        }
        return std::shared_ptr<const WorldRadianceGeneration>{batch.m_generation};
    }

    Expected<bool> WorldRadiancePass::cancelBounce(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                               std::errc::operation_not_permitted));
        if (not m_state->m_bounce)
            return true;
        const auto &batch = *m_state->m_bounce;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(not batch.m_encoded, std::errc::operation_in_progress));
        if (batch.m_submitted and not slotFenceComplete(completed, batch.m_fence))
            return false;

        m_state->m_bounce.reset();
        return true;
    }

    // ------------------------------------------------------------------
    // committed Matter snapshot, deterministic grouping and immutable source
    // ------------------------------------------------------------------

    std::error_code WorldRadiancePass::prepareMatterPipeline() {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               m_state and m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                               std::errc::operation_not_permitted));
        auto &state = *m_state;
        if (state.m_matter_programs[0])
            return {};

        const hal::ProfileScope scope{"WorldRadiance.PrepareMatterPipeline"};
        shader::SharedModule module;
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, state.m_shaders->loadModuleFromFile(
                               state.m_content_root / "shaders" / "world_matter_project.slang", "world_matter_project",
                               module.writeRef()));
        auto *const layout = module->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(layout != nullptr));
        for (const auto &[name, stride]: std::array<std::pair<const char *, u32>, 5u>{
                 std::pair{"g_matter", sizeof(GpuMatterFrame)}, {"g_metadata", sizeof(GridParticleMetadataRow)},
                 {"g_matter_input", sizeof(GpuMatterRecord)}, {"g_matter_materials", sizeof(GpuOriginalMaterial)},
                 {"g_matter_overlay", sizeof(GpuOverlay)}}) {
            slang::TypeLayoutReflection *element{};
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == name) {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(element and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == stride));
        }

        constexpr std::array names{
            "snapshotWorldMatter", "rankWorldMatter", "prefixWorldMatter", "scatterWorldMatter", "reduceWorldMatter",
            "finishWorldMatter"};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 6u> programs;
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 6u> pipelines;
        for (u32 index = 0u; index < names.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> entry;
            PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, module->findEntryPointByName(names[index], entry.writeRef()));
            slang::IComponentType *entries[]{entry.get()};
            rhi::ShaderProgramDesc description{};
            description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            description.slangGlobalScope = module.get();
            description.slangEntryPoints = entries;
            description.slangEntryPointCount = 1u;
            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance,
                state.m_device->createShaderProgram(description, programs[index].writeRef(), diagnostics.writeRef()));

            rhi::ComputePipelineDesc pipeline{};
            pipeline.program = programs[index];
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, state.m_device->createComputePipeline(pipeline, pipelines[index].writeRef()));
        }

        state.m_matter_module = std::move(module);
        state.m_matter_programs = std::move(programs);
        state.m_matter_pipelines = std::move(pipelines);
        return {};
    }

    Expected<WorldRadianceMatterDemand> WorldRadiancePass::prepareMatter(
        std::shared_ptr<const WorldRadianceSource> terrain, GridParticleGpuView particles,
        const WorldRadianceMatterSettings settings, const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   not m_state->m_matter and
                                   not m_state->m_discovery and
                                   not m_state->m_bounce and
                                   terrain and
                                   settings.m_request_epoch != 0u,
                               std::errc::operation_not_permitted));

        auto &state = *m_state;

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               not state.m_batch or (state.m_batch->m_published and
                                                        not state.m_batch->m_encoded and
                                                        (not state.m_batch->m_submitted or
                                                            slotFenceComplete(completed, state.m_batch->m_fence))),
                               std::errc::device_or_resource_busy));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                particles.m_metadata and
                particles.m_metadata->m_rows and
                particles.m_numerical and
                particles.m_readonly and
                particles.m_metadata->m_count <= kGridParticleCapacity and
                particles.m_metadata->m_moving_count <= particles.m_metadata->m_count and
                particles.m_metadata->m_rows->getDesc().elementSize == sizeof(GridParticleMetadataRow) and
                particles.m_metadata->m_rows->getDesc().size >=
                    std::max<u64>(1u, particles.m_metadata->m_count) * sizeof(GridParticleMetadataRow) and
                particles.m_numerical->getDesc().size >=
                    u64{particles.m_metadata->m_moving_count} * kGridParticleNumericalStride and
                particles.m_readonly->getDesc().size >=
                    u64{particles.m_metadata->m_moving_count} * kGridParticleReadonlyStride));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(particles.current(), std::errc::resource_unavailable_try_again));

        const hal::ProfileScope scope{"WorldRadiance.PrepareCommittedMatter"};
        const auto &identity = *terrain->m_data;
        const auto &base = identity.terrain();
        const auto canonical = identity.m_terrain ? identity.m_terrain : terrain;
        const GridParticlePresentationIdentity selected{
            particles.m_world, particles.m_publication, particles.m_terrain_revision, particles.m_joint_tick};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                not identity.m_projected and
                base.m_overlay.empty() and
                identity.m_world_identity.m_job_id != 0u and
                identity.m_world_identity.m_world_generation != 0u and
                identity.m_terrain_revision != 0u and
                selected.m_world == identity.m_world_identity and
                selected.m_terrain_revision == identity.m_terrain_revision and
                static_cast<float>(terrain->cellMetres()) == static_cast<float>(particles.m_metadata->m_cell_metres) and
                std::isfinite(particles.m_origin) and
                particles.m_origin == 3.0));

        if (state.m_matter_baseline and
            state.m_matter_baseline->m_data->m_world_identity == identity.m_world_identity and
            state.m_matter_baseline->m_data->m_terrain_revision == identity.m_terrain_revision) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(state.m_matter_baseline->matchesTerrainSource(*terrain)));
        }

        if (state.m_batch and state.m_batch->m_generation->source()->matchesTerrainSource(*terrain)) {
            const auto cohort = state.m_batch->m_generation->cohort();
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    selected.m_world ==
                    GridWorldIdentity{cohort.m_job_id, cohort.m_world_generation, cohort.m_content_fingerprint}));
        }

        auto candidate = std::make_unique<State::MatterBatch>();
        candidate->m_terrain = std::move(terrain);
        candidate->m_particles = std::move(particles);
        candidate->m_settings = settings;
        candidate->m_output = std::make_shared<ProjectedWorldInput>();
        candidate->m_output->m_identity = {selected, candidate->m_particles.m_device_epoch};
        if (state.m_matter_baseline and
            state.m_matter_baseline->matchesTerrainSource(*canonical) and
            state.m_matter_baseline->m_data->m_world_identity == identity.m_world_identity and
            state.m_matter_baseline->m_data->m_terrain_revision == identity.m_terrain_revision) {
            candidate->m_previous = state.m_matter_baseline;
            candidate->m_output->m_compared_identity = candidate->m_previous->matterIdentity();
        }

        auto &parameters = candidate->m_parameters;
        parameters.m_count = candidate->m_particles.m_metadata->m_count;
        parameters.m_moving_count = candidate->m_particles.m_metadata->m_moving_count;
        parameters.m_width = base.m_width;
        parameters.m_height = base.m_height;
        parameters.m_cell_metres = static_cast<float>(base.m_cell_metres);
        parameters.m_origin = static_cast<float>(candidate->m_particles.m_origin);

        const double min_x = base.m_minimum[0] / base.m_cell_metres, min_y = base.m_minimum[1] / base.m_cell_metres;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(std::floor(min_x) == min_x and std::floor(min_y) == min_y, std::errc::function_not_supported));
        parameters.m_minimum_x = static_cast<float>(min_x);
        parameters.m_minimum_y = static_cast<float>(min_y);

        parameters.m_blocks = (parameters.m_count + 255u) / 256u;
        parameters.m_lookup_capacity = 1u;
        while (u64{parameters.m_lookup_capacity} < 2u * u64{parameters.m_count}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(
                    parameters.m_lookup_capacity <= std::numeric_limits<u32>::max() / 2u, std::errc::value_too_large));

            parameters.m_lookup_capacity *= 2u;
        }

        const u64 native_cells = u64{base.m_width} * base.m_height;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(base.m_vacuum.size() >= native_cells));
        parameters.m_ancestor_words = safe_narrowing((base.m_vacuum.size() - native_cells + 31u) / 32u);
        parameters.m_palette_count = safe_narrowing(base.m_particle_materials.size());
        parameters.m_terrain_palette_count = safe_narrowing(base.m_materials.size());
        parameters.m_hierarchy_levels = safe_narrowing(base.m_hierarchy.size());
        parameters.m_old_count = candidate->m_previous ? candidate->m_previous->overlayCount() : 0u;
        parameters.m_old_lookup_capacity = candidate->m_previous ? candidate->m_previous->overlayLookupCapacity() : 1u;
        const auto &limits = state.m_device->getInfo().limits;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               std::max(parameters.m_blocks, (parameters.m_old_count + 255u) / 256u) <=
                                   limits.maxComputeDispatchThreadGroups[0],
                               std::errc::not_supported));

        candidate->m_work.push_back({State::MatterOperation::freeze, 0u});
        if (parameters.m_count != 0u) {
            for (u32 shift = 0u; shift < 32u; shift += 4u)
                candidate->m_work.push_back({State::MatterOperation::sort, shift});

            candidate->m_work.push_back({State::MatterOperation::reduce, 32u});
            candidate->m_work.push_back({State::MatterOperation::lookup, 0u});
        }

        if (parameters.m_count != 0u or parameters.m_old_count != 0u)
            candidate->m_work.push_back({State::MatterOperation::compare, 0u});

        auto &demand = candidate->m_demand;
        demand.m_input_rows = parameters.m_count;
        demand.m_overlay_capacity = std::max(1u, parameters.m_count);
        demand.m_lookup_capacity = parameters.m_lookup_capacity;
        demand.m_snapshot_bytes = std::max<u64>(1u, parameters.m_count) * sizeof(GpuMatterRecord);
        demand.m_sort_bytes = 2u * demand.m_snapshot_bytes;
        demand.m_scan_bytes = std::max<u64>(1u, parameters.m_count) * sizeof(u32) +
                              2u * std::max<u64>(1u, u64{parameters.m_blocks} * 16u) * sizeof(u32);
        demand.m_overlay_bytes = demand.m_overlay_capacity * sizeof(GpuOverlay);
        demand.m_lookup_bytes = demand.m_lookup_capacity * sizeof(u32);
        demand.m_occupancy_bytes = std::max<u64>(1u, parameters.m_ancestor_words) * sizeof(u32);
        demand.m_palette_bytes = std::max<u64>(1u, base.m_particle_materials.size()) * sizeof(GpuOriginalMaterial);
        demand.m_readback_bytes = 40u * sizeof(u32);
        demand.m_input_lease_bytes = candidate->m_particles.m_numerical->getDesc().size +
                                     candidate->m_particles.m_readonly->getDesc().size +
                                     candidate->m_particles.m_metadata->m_rows->getDesc().size +
                                     sizeof(GridParticleMetadataResource) +
                                     candidate->m_particles.m_metadata->m_materials.capacity() * sizeof(u32);

        demand.m_batches = safe_narrowing(candidate->m_work.size());
        demand.m_owned_bytes = sizeof(State::MatterBatch) +
                               sizeof(ProjectedWorldInput) +
                               sizeof(WorldRadianceSource) +
                               sizeof(WorldRadianceSource::Data) +
                               (base.m_emitters.size() + 1u) * sizeof(EmitterBounds) +
                               candidate->m_work.capacity() * sizeof(State::MatterWork) +
                               candidate->m_work.size() * sizeof(std::array<rhi::ComPtr<rhi::IShaderObject>, 3u>);
        for (const u64 bytes:
            {demand.m_sort_bytes, demand.m_scan_bytes, demand.m_overlay_bytes, demand.m_lookup_bytes,
                demand.m_occupancy_bytes, 2u * demand.m_readback_bytes, u64{sizeof(GpuOverlay) + sizeof(u32)},
                settings.m_measure_gpu ? u64{16u} * demand.m_batches : u64{0u}}) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_owned_bytes, bytes));
            demand.m_largest_resource_bytes = std::max(demand.m_largest_resource_bytes, bytes);
        }

        auto shared = state.m_static_input;
        if (shared and (shared->m_source != canonical or shared->m_device != state.m_device))
            shared.reset();

        const u64 static_bytes = base.m_packed_ids.size() * sizeof(u32) +
                                 base.m_materials.size() * sizeof(GpuMaterial) +
                                 base.m_vacuum.size() * sizeof(u32) +
                                 base.m_hierarchy.size() * sizeof(GpuHierarchy) +
                                 demand.m_palette_bytes;
        if (not shared) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, add_(demand.m_owned_bytes, static_bytes + sizeof(StaticWorldInput)));
            demand.m_largest_resource_bytes = std::max(
                {demand.m_largest_resource_bytes, u64{base.m_packed_ids.size() * sizeof(u32)},
                    u64{base.m_vacuum.size() * sizeof(u32)}, demand.m_palette_bytes});
        }

        demand.m_peak_bytes = demand.m_owned_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, state.chargeGameplayFields(demand.m_peak_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, settings.m_reserve_bytes));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, add_(demand.m_peak_bytes, demand.m_input_lease_bytes));
        std::vector<const void *> owners;
        const auto count_source = [&](const std::shared_ptr<const WorldRadianceSource> &source) -> std::error_code {
            if (source)
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldRadiance,
                    source->m_data->charge(owners, source.get(), demand.m_peak_bytes, demand.m_largest_resource_bytes));
            return {};
        };

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(candidate->m_terrain));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(state.m_matter_baseline));
        if (state.m_static_input) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                chargeOwner_(
                    owners, state.m_static_input.get(), state.m_static_input->m_bytes + sizeof(StaticWorldInput),
                    std::max(
                        state.m_static_input->m_vacuum->getDesc().size, state.m_static_input->m_ids->getDesc().size),
                    demand.m_peak_bytes, demand.m_largest_resource_bytes));
        }

        if (state.m_batch) {
            const auto &old = *state.m_batch;
            const auto source = old.m_generation->source();
            const u64 detached_inputs = old.m_static_input->m_bytes +
                                        sizeof(StaticWorldInput) +
                                        old.m_overlay->getDesc().size +
                                        old.m_overlay_lookup->getDesc().size +
                                        old.m_overlay_ancestors->getDesc().size;
            const u64 all = old.m_plan.m_demand.m_candidate_bytes;
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(all >= source->retainedCpuBytes() + detached_inputs));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, add_(demand.m_peak_bytes, all - source->retainedCpuBytes() - detached_inputs));
            if (not source->m_data->m_projected)
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, add_(
                                       demand.m_peak_bytes, old.m_overlay->getDesc().size +
                                                                old.m_overlay_lookup->getDesc().size +
                                                                old.m_overlay_ancestors->getDesc().size));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(source));

            // The current static donor may already belong to a newer Matter
            // request. The retained direct batch still owns this exact base.
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                chargeOwner_(
                    owners, old.m_static_input.get(), old.m_static_input->m_bytes + sizeof(StaticWorldInput),
                    std::max(
                        {old.m_static_input->m_ids->getDesc().size, old.m_static_input->m_materials->getDesc().size,
                            old.m_static_input->m_vacuum->getDesc().size,
                            old.m_static_input->m_hierarchy->getDesc().size,
                            old.m_static_input->m_particle_materials->getDesc().size}),
                    demand.m_peak_bytes, demand.m_largest_resource_bytes));
            demand.m_largest_resource_bytes = std::max(
                demand.m_largest_resource_bytes, old.m_own_largest_resource_bytes);
        }

        for (const auto &weak: state.m_generations) {
            if (const auto generation = weak.lock();
                generation and (not state.m_batch or generation != state.m_batch->m_generation)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    chargeOwner_(
                        owners, generation.get(),
                        generation->bufferBytes() + generation->probeStateBytes() + sizeof(WorldRadianceGeneration),
                        generation->bufferBytes(), demand.m_peak_bytes, demand.m_largest_resource_bytes));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(generation->source()));
            }
        }

        for (const auto &weak: state.m_discoveries) {
            if (const auto discovery = weak.lock()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, chargeOwner_(
                                       owners, discovery.get(),
                                       discovery->bufferBytes() +
                                           sizeof(WorldRadianceFaceDiscovery) +
                                           discovery->m_support_keys.capacity() * sizeof(u32),
                                       std::max(discovery->m_bits->getDesc().size, discovery->m_keys->getDesc().size),
                                       demand.m_peak_bytes, demand.m_largest_resource_bytes));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(discovery->source()));
            }
        }

        for (auto iterator = state.m_projected_sources.begin(); iterator != state.m_projected_sources.end();) {
            if (const auto source = iterator->lock()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, count_source(source));

                ++iterator;
            } else
                iterator = state.m_projected_sources.erase(iterator);
        }

        demand.m_admission_error = require_(
            demand.m_peak_bytes <= settings.m_budget_bytes and
                demand.m_largest_resource_bytes <= settings.m_max_resource_bytes and
                demand.m_largest_resource_bytes <= limits.maxBufferSize,
            std::errc::no_buffer_space);
        if (demand.m_admission_error)
            return demand;

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, prepareMatterPipeline());

        if (not shared) {
            auto owned = std::make_shared<StaticWorldInput>();
            owned->m_source = canonical;
            owned->m_device = state.m_device;
            const GpuOriginalMaterial unused{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, base.m_packed_ids.size() * sizeof(u32), sizeof(u32),
                                   base.m_packed_ids.data(), false, owned->m_ids));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, base.m_materials.size() * sizeof(GpuMaterial), sizeof(GpuMaterial),
                                   base.m_materials.data(), false, owned->m_materials));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, base.m_vacuum.size() * sizeof(u32), sizeof(u32),
                                   base.m_vacuum.data(), false, owned->m_vacuum));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, base.m_hierarchy.size() * sizeof(GpuHierarchy),
                                   sizeof(GpuHierarchy), base.m_hierarchy.data(), false, owned->m_hierarchy));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, demand.m_palette_bytes, sizeof(GpuOriginalMaterial),
                                   base.m_particle_materials.empty() ? &unused : base.m_particle_materials.data(),
                                   false, owned->m_particle_materials));
            owned->m_bytes = static_bytes;
            shared = owned;
        }

        candidate->m_base = shared;
        candidate->m_output->m_base = shared;
        candidate->m_output->m_capacity = parameters.m_lookup_capacity;
        candidate->m_output->m_ancestor_words = parameters.m_ancestor_words;
        candidate->m_output->m_bytes = demand.m_overlay_bytes + demand.m_lookup_bytes + demand.m_occupancy_bytes;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, demand.m_overlay_bytes, sizeof(GpuOverlay), nullptr, false,
                               candidate->m_output->m_rows));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(
                *state.m_device, demand.m_lookup_bytes, sizeof(u32), nullptr, false, candidate->m_output->m_lookup));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, demand.m_occupancy_bytes, sizeof(u32), nullptr, false,
                               candidate->m_output->m_ancestors));

        for (auto &buffer: candidate->m_sort)
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                buffer_(*state.m_device, demand.m_snapshot_bytes, sizeof(GpuMatterRecord), nullptr, false, buffer));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, std::max<u64>(1u, parameters.m_count) * sizeof(u32), sizeof(u32),
                               nullptr, false, candidate->m_ranks));
        for (auto *const buffer: {std::addressof(candidate->m_counts), std::addressof(candidate->m_prefix)})
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, buffer_(
                                   *state.m_device, std::max<u64>(1u, u64{parameters.m_blocks} * 16u) * sizeof(u32),
                                   sizeof(u32), nullptr, false, *buffer));

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, demand.m_readback_bytes, sizeof(u32), nullptr, false, candidate->m_summary));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(*state.m_device, demand.m_readback_bytes, 0u, nullptr, true, candidate->m_readback));

        const GpuOverlay empty_overlay{};
        const u32 zero{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, buffer_(
                               *state.m_device, sizeof(empty_overlay), sizeof(empty_overlay), &empty_overlay, false,
                               candidate->m_unused_overlay));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            buffer_(*state.m_device, sizeof(zero), sizeof(zero), &zero, false, candidate->m_unused_lookup));

        candidate->m_roots.resize(candidate->m_work.size());
        for (u32 quantum = 0u; quantum < candidate->m_work.size(); ++quantum) {
            const auto &work = candidate->m_work[quantum];
            const u32 parity = (work.m_shift / 4u) % 2u;
            std::array<u32, 3u> programs{};
            u32 steps{};
            switch (work.m_operation) {
                case State::MatterOperation::freeze:
                    programs = {2u, 0u, 0u};
                    steps = parameters.m_count == 0u ? 1u : 2u;
                    break;
                case State::MatterOperation::sort:
                    programs = {1u, 2u, 3u};
                    steps = 3u;
                    break;
                case State::MatterOperation::reduce:
                    programs = {1u, 2u, 4u};
                    steps = 3u;
                    break;
                case State::MatterOperation::lookup:
                case State::MatterOperation::compare:
                    programs = {5u, 0u, 0u};
                    steps = 1u;
                    break;
            }

            for (u32 step = 0u; step < steps; ++step) {
                const u32 program = programs[step];
                auto frame = parameters;
                frame.m_shift = work.m_shift;
                frame.m_phase = work.m_operation == State::MatterOperation::freeze   ? 0u
                                : work.m_operation == State::MatterOperation::lookup ? 0u
                                                                                     : 1u;

                auto &root = candidate->m_roots[quantum][step];
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    state.m_device->createRootShaderObject(state.m_matter_programs[program], root.writeRef()));
                rhi::ShaderCursor cursor{root}, constants{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, cursor["g_matter"].getDereferenced(constants));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, constants.setData(&frame, sizeof(frame)));

                const auto bind = [&](const char *const name, rhi::IBuffer *const buffer) -> std::error_code {
                    PPR_RETURN_ERROR_ON_FAIL(
                        WorldRadiance, cursor[name].setBinding(rhi::Binding(buffer, range_(buffer))));
                    return {};
                };

                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_summary", candidate->m_summary));
                if (program == 0u) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind("g_numerical", candidate->m_particles.m_numerical));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind("g_metadata", candidate->m_particles.m_metadata->m_rows));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind("g_matter_materials", shared->m_particle_materials));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_output", candidate->m_sort[0]));
                } else if (program == 1u or program == 2u or program == 3u or program == 4u) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_ranks", candidate->m_ranks));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_counts", candidate->m_counts));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_prefix", candidate->m_prefix));
                    if (program != 2u)
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance,
                            bind(
                                "g_matter_input",
                                candidate->m_sort[work.m_operation == State::MatterOperation::sort ? parity : 0u]));

                    if (program == 3u)
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance, bind("g_matter_output", candidate->m_sort[1u - parity]));

                    if (program == 4u) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance, bind("g_matter_materials", shared->m_particle_materials));
                        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_terrain_ids", shared->m_ids));
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance, bind("g_matter_terrain_materials", shared->m_materials));
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            WorldRadiance, bind("g_matter_overlay", candidate->m_output->m_rows));
                    }
                } else {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_overlay", candidate->m_output->m_rows));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind("g_matter_lookup", candidate->m_output->m_lookup));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance, bind("g_matter_ancestors", candidate->m_output->m_ancestors));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, bind("g_matter_hierarchy", shared->m_hierarchy));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        bind(
                            "g_matter_old_overlay", candidate->m_previous ? candidate->m_previous->overlayBuffer()
                                                                          : candidate->m_unused_overlay.get()));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        WorldRadiance,
                        bind(
                            "g_matter_old_lookup", candidate->m_previous ? candidate->m_previous->overlayLookupBuffer()
                                                                         : candidate->m_unused_lookup.get()));
                }
            }
        }

        if (settings.m_measure_gpu) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, require_(state.m_device->getInfo().timestampFrequency != 0u, std::errc::not_supported));
            rhi::QueryPoolDesc description{};
            description.type = rhi::QueryType::Timestamp;
            description.count = 2u * demand.m_batches;
            description.label = "World committed Matter projection quanta";
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance, state.m_device->createQueryPool(description, candidate->m_queries.writeRef()));
            PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, candidate->m_queries->reset(0u, description.count));
        }

        state.m_static_input = shared;
        const auto result = demand;
        state.m_matter = std::move(candidate);
        return result;
    }

    bool WorldRadiancePass::canEncodeMatter() const noexcept {
        return m_state and
               not m_state->m_unknown and
               m_state->m_matter and
               not m_state->m_matter->m_encoded and
               not m_state->m_matter->m_submitted and
               m_state->m_matter->m_next < m_state->m_matter->m_work.size();
    }

    std::error_code WorldRadiancePass::encodeMatter(
        const PreRenderContext &context, const u64 actual_frame, const float grouping_allowance_ms) {
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance, require_(
                               canEncodeMatter() and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   m_state->m_device == &context.m_device and
                                   (not m_state->m_last_matter_frame or *m_state->m_last_matter_frame < actual_frame),
                               std::errc::operation_not_permitted));

        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance,
            require_(
                std::isfinite(grouping_allowance_ms) and grouping_allowance_ms >= 0.0f, std::errc::invalid_argument));

        auto &batch = *m_state->m_matter;
        if (batch.m_next == 0u)
            PPR_RETURN_ERROR_ON_FAIL(
                WorldRadiance, require_(batch.m_particles.current(), std::errc::resource_unavailable_try_again));

        const hal::ProfileScope scope{"WorldRadiance.EncodeMatterQuantum"};
        auto &encoder = context.m_encoder;
        const auto &parameters = batch.m_parameters;
        batch.m_encoded_count = 1u;
        const auto previous = batch.m_previous ? batch.m_previous->m_data->m_projected.get() : nullptr;
        const bool same_size = previous and
                               previous->m_statistics.m_input_rows == parameters.m_count and
                               parameters.m_old_count <= parameters.m_count;
        const auto operation = batch.m_work[batch.m_next].m_operation;
        const double sort_cost = same_size ? std::max(batch.m_sort_cost_ms, previous->m_sort_cost_ms)
                                           : batch.m_sort_cost_ms;
        const bool measured_snapshot = operation == State::MatterOperation::freeze and
                                       same_size and
                                       previous->m_freeze_cost_ms > 0.0;
        if (sort_cost > 0.0 and (operation == State::MatterOperation::sort or measured_snapshot)) {
            // Snapshot writes a bounded record or a drop sentinel for every row.
            // Radix consumes only that fixed row range and preserves sticky errors.
            // The host validates the summary before allowing reduction.
            double predicted = measured_snapshot ? previous->m_freeze_cost_ms : sort_cost;
            while (batch.m_next + batch.m_encoded_count < batch.m_work.size() and
                   batch.m_work[batch.m_next + batch.m_encoded_count].m_operation == State::MatterOperation::sort and
                   (predicted + sort_cost) * 1.2 <= double{grouping_allowance_ms}) {
                predicted += sort_cost;
                ++batch.m_encoded_count;
            }
        }

        // Reduction and its row bounds have already passed host validation.
        // Compare checks every hash ordinal and preserves lookup's sticky error.
        if (operation == State::MatterOperation::lookup and
            same_size and
            previous->m_compared_identity and
            previous->m_lookup_cost_ms > 0.0 and
            previous->m_compare_cost_ms > 0.0 and
            (previous->m_lookup_cost_ms + previous->m_compare_cost_ms) * 1.2 <= double{grouping_allowance_ms} and
            batch.m_next + 1u < batch.m_work.size() and
            batch.m_work[batch.m_next + 1u].m_operation == State::MatterOperation::compare) {
            batch.m_encoded_count = 2u;
        }

        batch.m_actual_frame = actual_frame;
        batch.m_encoded = true;
        m_state->m_last_matter_frame = actual_frame;

        for (u32 index = batch.m_next; index < batch.m_next + batch.m_encoded_count; ++index) {
            const auto &work = batch.m_work[index];
            if (batch.m_queries)
                encoder.writeTimestamp(batch.m_queries, 2u * index);
            if (work.m_operation == State::MatterOperation::freeze) {
                for (auto *const buffer: {batch.m_output->m_lookup.get(), batch.m_output->m_ancestors.get()}) {
                    encoder.setBufferState(buffer, rhi::ResourceState::CopyDestination);
                    encoder.clearBuffer(buffer, 0u, buffer->getDesc().size);
                }
                if (parameters.m_count != 0u) {
                    encoder.setBufferState(batch.m_particles.m_numerical, rhi::ResourceState::ShaderResource);
                    encoder.setBufferState(batch.m_particles.m_metadata->m_rows, rhi::ResourceState::ShaderResource);
                }
            }

            for (auto *const buffer:
                {batch.m_base->m_ids.get(), batch.m_base->m_materials.get(), batch.m_base->m_particle_materials.get(),
                    batch.m_base->m_hierarchy.get()})
                encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);

            for (auto *const buffer:
                {batch.m_ranks.get(), batch.m_counts.get(), batch.m_prefix.get(), batch.m_summary.get(),
                    batch.m_output->m_rows.get(), batch.m_output->m_lookup.get(), batch.m_output->m_ancestors.get()})
                encoder.setBufferState(buffer, rhi::ResourceState::UnorderedAccess);

            encoder.setBufferState(batch.m_unused_overlay, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(batch.m_unused_lookup, rhi::ResourceState::ShaderResource);
            if (batch.m_previous) {
                encoder.setBufferState(batch.m_previous->overlayBuffer(), rhi::ResourceState::ShaderResource);
                encoder.setBufferState(batch.m_previous->overlayLookupBuffer(), rhi::ResourceState::ShaderResource);
            }

            std::array<u32, 3u> programs{};
            u32 steps{};
            switch (work.m_operation) {
                case State::MatterOperation::freeze:
                    programs = {2u, 0u, 0u};
                    steps = parameters.m_count == 0u ? 1u : 2u;
                    break;
                case State::MatterOperation::sort:
                    programs = {1u, 2u, 3u};
                    steps = 3u;
                    break;
                case State::MatterOperation::reduce:
                    programs = {1u, 2u, 4u};
                    steps = 3u;
                    break;
                case State::MatterOperation::lookup:
                case State::MatterOperation::compare:
                    programs = {5u, 0u, 0u};
                    steps = 1u;
                    break;
            }

            for (u32 step = 0u; step < steps; ++step) {
                const u32 program = programs[step];
                const u32 parity = work.m_operation == State::MatterOperation::sort ? (work.m_shift / 4u) % 2u : 0u;
                if (program == 0u)
                    encoder.setBufferState(batch.m_sort[0], rhi::ResourceState::UnorderedAccess);
                if (program == 1u or program == 4u)
                    encoder.setBufferState(batch.m_sort[parity], rhi::ResourceState::ShaderResource);
                if (program == 3u) {
                    encoder.setBufferState(batch.m_sort[parity], rhi::ResourceState::ShaderResource);
                    encoder.setBufferState(batch.m_sort[1u - parity], rhi::ResourceState::UnorderedAccess);
                }

                auto *const pass = encoder.beginComputePass();
                PPR_RETURN_ERROR_ON_FAIL(WorldRadiance, require_(pass != nullptr, std::errc::io_error));
                pass->bindPipeline(m_state->m_matter_pipelines[program], batch.m_roots[index][step]);
                const u32 rows = work.m_operation == State::MatterOperation::compare
                                     ? std::max(parameters.m_count, parameters.m_old_count)
                                     : parameters.m_count;
                pass->dispatchCompute(program == 2u ? 1u : (rows + 255u) / 256u, 1u, 1u);
                pass->end();
                encoder.globalBarrier();
            }

            if (batch.m_queries and index + 1u < batch.m_next + batch.m_encoded_count)
                encoder.writeTimestamp(batch.m_queries, 2u * index + 1u);
        }

        encoder.setBufferState(batch.m_summary, rhi::ResourceState::CopySource);
        encoder.setBufferState(batch.m_readback, rhi::ResourceState::CopyDestination);
        encoder.copyBuffer(batch.m_readback, 0u, batch.m_summary, 0u, batch.m_demand.m_readback_bytes);
        if (batch.m_queries)
            encoder.writeTimestamp(batch.m_queries, 2u * (batch.m_next + batch.m_encoded_count) - 1u);
        return {};
    }

    Expected<bool> WorldRadiancePass::advanceMatter(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and
                                   m_state->m_owner == std::this_thread::get_id() and
                                   not m_state->m_unknown and
                                   m_state->m_matter,
                               std::errc::operation_not_permitted));

        auto &batch = *m_state->m_matter;
        if (batch.m_next == batch.m_work.size())
            return true;
        if (not batch.m_submitted or not slotFenceComplete(completed, batch.m_fence))
            return false;

        batch.m_completed = true;

        std::optional<double> milliseconds;
        double freeze_cost_ms{}, sort_cost_ms{}, lookup_cost_ms{}, compare_cost_ms{};
        if (batch.m_queries) {
            // The largest group contains the snapshot followed by eight radix digits.
            const u32 query_count = 2u * batch.m_encoded_count;
            std::array<u64, 18u> ticks{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(query_count > 0u and query_count <= ticks.size(), std::errc::result_out_of_range));

            rhi::QueryResultState readiness{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                make_error_code(batch.m_queries->getResultState(2u * batch.m_next, query_count, &readiness)));
            if (readiness == rhi::QueryResultState::Pending)
                return false;

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                require_(readiness == rhi::QueryResultState::Resolved, std::errc::state_not_recoverable));

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                WorldRadiance,
                make_error_code(batch.m_queries->getResult(2u * batch.m_next, query_count, ticks.data())));
            const double tick_ms = 1000.0 / double(m_state->m_device->getInfo().timestampFrequency);
            for (u32 i = 0u; i < query_count; i += 2u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance, require_(
                                       ticks[i + 1u] >= ticks[i] and (i == 0u or ticks[i] >= ticks[i - 1u]),
                                       std::errc::result_out_of_range));
                const double cost = double(ticks[i + 1u] - ticks[i]) * tick_ms;
                const auto operation = batch.m_work[batch.m_next + i / 2u].m_operation;
                if (operation == State::MatterOperation::freeze)
                    freeze_cost_ms = cost;
                else if (operation == State::MatterOperation::sort)
                    sort_cost_ms = std::max(sort_cost_ms, cost);
                else if (operation == State::MatterOperation::lookup)
                    lookup_cost_ms = cost;
                else if (operation == State::MatterOperation::compare)
                    compare_cost_ms = cost;
            }

            milliseconds = double(ticks[query_count - 1u] - ticks[0]) * tick_ms;
        }

        std::array<u32, 40u> summary{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, m_state->m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(summary.data(), mapped, sizeof(summary));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, m_state->m_device->unmapBuffer(batch.m_readback));

        std::array<double, 3> emission_maximum{};
        for (u32 channel = 0u; channel < 3u; ++channel)
            emission_maximum[channel] = double(std::bit_cast<float>(summary[22u + channel]));

        const auto emission = admitWorldRadianceEmission(summary[8], emission_maximum);
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, emission);

        constexpr std::array names{
            "snapshot", "radix_rank_prefix_scatter", "head_prefix_reduce", "lookup_ancestors", "changed_cells"};
        const auto operation = batch.m_work[batch.m_next].m_operation;
        const auto message = std::format(
            "WORLD_MATTER_QUANTUM frame={} request={} quantum={} quanta={} grouped_quanta={} operation={} shift={} fence={} GPU_ms={} timing_measured={} input={} participating={} vacuum={} inactive={} invalid={} outside={} unsupported={} cells={} partial={} hard={} changed_cells={} status={} pub={} terrain={} joint={} device={} owned_bytes={} peak_bytes={} CPU_prepare_uploads_excluded=true emission_max_r={} emission_max_g={} emission_max_b={} emission_bound_r={} emission_bound_g={} emission_bound_b={}",
            batch.m_actual_frame, batch.m_settings.m_request_epoch, batch.m_next, batch.m_work.size(),
            batch.m_encoded_count, names[static_cast<u32>(operation)], batch.m_work[batch.m_next].m_shift,
            u64{batch.m_fence}, milliseconds.value_or(-1.0), milliseconds.has_value(), summary[1], summary[2],
            summary[3], summary[4], summary[5], summary[6], summary[7], summary[8], summary[34], summary[35],
            summary[11], summary[0], batch.m_output->m_identity.m_particles.m_publication,
            batch.m_output->m_identity.m_particles.m_terrain_revision,
            batch.m_output->m_identity.m_particles.m_joint_tick, batch.m_output->m_identity.m_device_epoch,
            batch.m_demand.m_owned_bytes, batch.m_demand.m_peak_bytes, emission_maximum[0], emission_maximum[1],
            emission_maximum[2], emission->m_bound[0], emission->m_bound[1], emission->m_bound[2]);
        PPR_LOG_RAW(WorldRadiance, info, message);

        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_((summary[0] & 32u) == 0u, std::errc::not_supported));
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, require_(summary[0] == 0u, std::errc::argument_out_of_domain));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               summary[1] == batch.m_parameters.m_count and
                                   summary[2] <= summary[1] and
                                   u64{summary[2]} + summary[36] == summary[1] and
                                   summary[3] <= summary[1] and
                                   summary[4] <= summary[1] and
                                   summary[8] <= batch.m_demand.m_overlay_capacity and
                                   summary[9] + summary[10] <= summary[8],
                               std::errc::result_out_of_range));

        if (batch.m_next == 0u) {
            // Both explicit owners and snapshot shader roots release Hot/meta
            // only AFTER the exact accepted read fence. Remaining stages read sortA/B.
            batch.m_particles = {};
            batch.m_roots[0] = {};
        }

        if (freeze_cost_ms > 0.0)
            batch.m_output->m_freeze_cost_ms = freeze_cost_ms;
        batch.m_sort_cost_ms = std::max(batch.m_sort_cost_ms, sort_cost_ms);

        if (lookup_cost_ms > 0.0)
            batch.m_output->m_lookup_cost_ms = lookup_cost_ms;
        if (compare_cost_ms > 0.0)
            batch.m_output->m_compare_cost_ms = compare_cost_ms;

        const bool ready = batch.m_next + batch.m_encoded_count == batch.m_work.size();
        if (ready) {
            auto &projection = *batch.m_output;
            projection.m_count = summary[8];
            projection.m_fence = batch.m_fence;
            projection.m_work_quanta = batch.m_work.size();
            projection.m_sort_cost_ms = batch.m_sort_cost_ms;
            projection.m_statistics = {
                summary[1], summary[2], summary[3], summary[4], summary[5], summary[6], summary[7], summary[8],
                summary[34], summary[35], summary[11]};
            projection.m_statistics.m_emission_maximum = emission->m_maximum;
            projection.m_statistics.m_emission_bound = emission->m_bound;

            const auto &input = *batch.m_terrain->m_data;
            const auto &base = input.terrain();
            auto data = std::make_unique<WorldRadianceSource::Data>();
            data->m_terrain = batch.m_base->m_source;
            data->m_projected = batch.m_output;

            data->m_world_identity = input.m_world_identity;
            data->m_terrain_revision = input.m_terrain_revision;
            data->m_width = base.m_width;
            data->m_height = base.m_height;
            data->m_cell_metres = base.m_cell_metres;
            data->m_minimum = base.m_minimum;
            data->m_sky = base.m_sky;
            data->m_surface_cells = base.m_surface_cells + summary[9] + summary[10];
            data->m_partial_cells = summary[9];

            data->m_emitters.reserve(base.m_emitters.size() + 1u);
            data->m_emitters.assign(base.m_emitters.begin(), base.m_emitters.end());
            if (summary[12] < summary[14] and summary[13] < summary[15])
                data->m_emitters.push_back(
                    {std::numeric_limits<u32>::max(), summary[12], summary[13], summary[14], summary[15]});

            const double path = 2.0 * (double(base.m_width) + double(base.m_height)) * base.m_cell_metres;
            const double crossings = double(base.m_width) + double(base.m_height) + 4.0;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double surface = double(std::bit_cast<float>(summary[16u + channel]));
                const double minimum_sigma = double(std::bit_cast<float>(summary[25u + channel]));
                const double positive_j = double(std::bit_cast<float>(summary[28u + channel]));
                const double zero_j = double(std::bit_cast<float>(summary[31u + channel]));
                const double maximum = std::max(
                    {base.m_transport_max[channel], surface, minimum_sigma > 0.0 ? positive_j / minimum_sigma : 0.0});
                const double zero = std::max(base.m_zero_emission[channel], zero_j);
                data->m_transport_max[channel] = maximum;
                data->m_zero_emission[channel] = zero;
                data->m_far_bound[channel] = maximum + zero * path;
                const double bound = data->m_far_bound[channel];
                data->m_underflow_loss[channel] = double(std::numeric_limits<float>::min()) *
                                                  ((2.0 + 2.0 * crossings) * bound +
                                                      2.0 * crossings +
                                                      512.0 +
                                                      2.0 *
                                                          double(batch.m_parameters.m_count) *
                                                          (bound * path + path + bound + 1.0));
                PPR_RETURN_UNEXPECTED_ON_FAIL(WorldRadiance, sourceFloat_(bound));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    WorldRadiance,
                    require_(
                        std::isfinite(data->m_underflow_loss[channel]) and data->m_underflow_loss[channel] <= 1e-5,
                        std::errc::argument_out_of_domain));
            }

            data->m_cpu_bytes = sizeof(WorldRadianceSource) +
                                sizeof(WorldRadianceSource::Data) +
                                sizeof(ProjectedWorldInput) +
                                data->m_emitters.capacity() * sizeof(EmitterBounds);
            batch.m_result = std::shared_ptr<const WorldRadianceSource>{new WorldRadianceSource{std::move(data)}};
        }

        batch.m_next += batch.m_encoded_count;
        if (not ready)
            batch.m_submitted = false;

        return ready;
    }

    Expected<std::shared_ptr<const WorldRadianceSource>> WorldRadiancePass::acquireMatter(
        const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(m_state and not m_state->m_unknown, std::errc::state_not_recoverable));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state->m_owner == std::this_thread::get_id() and m_state->m_matter,
                               std::errc::operation_not_permitted));

        auto &batch = *m_state->m_matter;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance,
            require_(
                batch.m_result and batch.m_next == batch.m_work.size() and slotFenceComplete(completed, batch.m_fence),
                std::errc::device_or_resource_busy));

        if (not batch.m_acquired) {
            m_state->m_projected_sources.push_back(batch.m_result);
            m_state->m_matter_baseline = batch.m_result;
            batch.m_acquired = true;
        }

        return batch.m_result;
    }

    Expected<bool> WorldRadiancePass::cancelMatter(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            WorldRadiance, require_(
                               m_state and m_state->m_owner == std::this_thread::get_id() and not m_state->m_unknown,
                               std::errc::operation_not_permitted));
        if (not m_state->m_matter)
            return true;

        const auto &batch = *m_state->m_matter;
        if (batch.m_encoded or (batch.m_submitted and not slotFenceComplete(completed, batch.m_fence)))
            return false;

        m_state->m_matter.reset();
        return true;
    }

    std::error_code WorldRadiancePass::shutdown(const GpuQuiescenceResult &proof) {
        if (not m_state)
            return {};
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance,
            require_(m_state->m_owner == std::this_thread::get_id(), std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldRadiance,
            proof.m_disposition == GpuQuiescence::kUnknown
                ? (proof.m_error ? proof.m_error : std::make_error_code(std::errc::state_not_recoverable))
                : std::error_code{});
        // External immutable generations retain only their own coefficient buffer
        // and copied source, not pass scratch. Caller destroys leases before device.
        m_state.reset();
        return proof.m_error;
    }
}
