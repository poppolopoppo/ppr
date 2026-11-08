module;
#include "pP/Macros.h"
#include <slang.h>

module engine.app;

import :renderer.lighting_pass;

import engine.core;
import engine.rhi;
import engine.math;
import engine.shader;
import std;

namespace pP {
    // TEMPORARY CPU-phase localization: flush consumed markers before watchdog termination.
    PPR_DEFINE_LOG_CATEGORY(RadiancePass, debug, immediate)
}

namespace pP::lighting {
    namespace {
        // ------------------------------------------------------------------
        // scalar CPU/GPU transport protocol
        // ------------------------------------------------------------------

        struct GpuOpticalCell {
            float m_extinction[3]{};
            float m_volume_emission[3]{};
            float m_surface_emission[3]{};
            u32 m_kind{};
        };

        constexpr u32 kTerrainNumericAmbiguity{32u};

        struct GpuTerrainVertex {
            float m_roundness{};
            u32 m_material{};
        };

        struct GpuTerrainParameters {
            u32 m_outside_material{};
            float m_outside_roundness{}, m_outside_r{}, m_outside_g{}, m_outside_b{};
            u32 m_reserved[3]{};
        };

        struct TerrainPlan {
            std::vector<GpuTerrainVertex> m_vertices{};
            GpuTerrainParameters m_parameters{};
            u64 m_bytes{}, m_largest{};
        };

        struct TerrainStorage {
            rhi::ComPtr<rhi::IBuffer> m_vertices{}, m_points{}, m_cases{};
        };

        struct TerrainPrecisionStorage {
            rhi::ComPtr<rhi::IBuffer> m_counts{}, m_readback{};
            TerrainPrecisionStatistics m_statistics{};
        };

        constexpr u64 kTerrainPrecisionCounterBytes{4u * sizeof(u32)};
        constexpr u64 kTerrainPrecisionOwnerBytes{
            sizeof(TerrainPrecisionStorage) + sizeof(std::unique_ptr<TerrainPrecisionStorage>)};
        constexpr u64 kTerrainPrecisionLeaseBytes{
            sizeof(TerrainPrecisionStatistics) + sizeof(std::unique_ptr<const TerrainPrecisionStatistics>)};
        constexpr u64 kTerrainPrecisionRetainedBytes{2u * kTerrainPrecisionCounterBytes + kTerrainPrecisionOwnerBytes};

        static_assert(sizeof(std::array<u32, 4>) == kTerrainPrecisionCounterBytes);

        static_assert(sizeof(GpuTerrainVertex) == 8u and alignof(GpuTerrainVertex) == 4u);
        static_assert(PPR_OFFSETOF(GpuTerrainVertex, m_material) == 4u);
        static_assert(std::is_standard_layout_v<GpuTerrainVertex> and std::is_trivially_copyable_v<GpuTerrainVertex>);
        static_assert(sizeof(GpuTerrainParameters) == 32u and alignof(GpuTerrainParameters) == 4u);
        static_assert(PPR_OFFSETOF(GpuTerrainParameters, m_outside_b) == 16u);
        static_assert(
            std::is_standard_layout_v<GpuTerrainParameters> and std::is_trivially_copyable_v<GpuTerrainParameters>);
        static_assert(sizeof(math::ContourPoint) == 8u and sizeof(math::ContourCase) == 72u);
        static_assert(PPR_OFFSETOF(math::ContourCase, m_segments) == 8u);
        static_assert(PPR_OFFSETOF(math::ContourCase, m_triangles) == 24u);

        struct GpuOpticalRay {
            float m_origin[2]{};
            float m_direction[2]{};
            float m_near_m{};
            float m_far_m{};
        };

        struct GpuOpticalResult {
            float m_radiance[3]{};
            float m_transmittance[3]{};
            u32 m_status{};
            u32 m_segments{};
        };

        struct GpuPackedInterval {
            u32 m_radiance_rg{}, m_radiance_b_transmission_r{}, m_transmission_gb{}, m_status{};
        };

        static_assert(sizeof(GpuPackedInterval) == 16u and alignof(GpuPackedInterval) == 4u);
        static_assert(std::is_standard_layout_v<GpuPackedInterval> and std::is_trivially_copyable_v<GpuPackedInterval>);
        static_assert(std::numeric_limits<float>::is_iec559 and std::numeric_limits<float>::digits == 24);
        static_assert(PPR_OFFSETOF(GpuPackedInterval, m_status) == 12u);

        struct GpuTraceParameters {
            u32 m_width{};
            u32 m_height{};
            u32 m_ray_count{};
            u32 m_boundary{};
            float m_cell_metres{};
            u32 m_hierarchy_levels{};
            u32 m_reserved[2]{};
        };

        struct GpuReceiverParameters {
            u32 m_count{}, m_parent_width{}, m_parent_height{}, m_parent_direction_offset{};
            float m_parent_spacing{}, m_parent_near_cells{};
            u32 m_angular_output{}, m_reserved{};
        };

        static_assert(sizeof(TerrainReceiverFeature) == 32u and alignof(TerrainReceiverFeature) == 4u);
        static_assert(PPR_OFFSETOF(TerrainReceiverFeature, m_visual_x) == 16u);
        static_assert(PPR_OFFSETOF(TerrainReceiverFeature, m_reserved) == 24u);
        static_assert(
            std::is_standard_layout_v<TerrainReceiverFeature> and std::is_trivially_copyable_v<TerrainReceiverFeature>);
        static_assert(sizeof(TerrainReceiverAngular) == 16u and alignof(TerrainReceiverAngular) == 4u);
        static_assert(PPR_OFFSETOF(TerrainReceiverAngular, m_status) == 12u);
        static_assert(
            std::is_standard_layout_v<TerrainReceiverAngular> and std::is_trivially_copyable_v<TerrainReceiverAngular>);
        static_assert(sizeof(TerrainReceiverResult) == 16u and alignof(TerrainReceiverResult) == 4u);
        static_assert(PPR_OFFSETOF(TerrainReceiverResult, m_status) == 12u);
        static_assert(sizeof(TerrainReceiverCellRange) == 8u and sizeof(TerrainReceiverComponentRange) == 16u);
        static_assert(PPR_OFFSETOF(TerrainReceiverComponentRange, m_corner_mask) == 12u);
        static_assert(
            std::is_standard_layout_v<TerrainReceiverResult> and std::is_trivially_copyable_v<TerrainReceiverResult>);
        static_assert(
            std::is_standard_layout_v<TerrainReceiverCellRange> and
            std::is_trivially_copyable_v<TerrainReceiverCellRange>);
        static_assert(
            std::is_standard_layout_v<TerrainReceiverComponentRange> and
            std::is_trivially_copyable_v<TerrainReceiverComponentRange>);
        static_assert(sizeof(GpuReceiverParameters) == 32u and alignof(GpuReceiverParameters) == 4u);
        static_assert(PPR_OFFSETOF(GpuReceiverParameters, m_parent_spacing) == 16u);
        static_assert(PPR_OFFSETOF(GpuReceiverParameters, m_angular_output) == 24u);

        struct GpuBackgroundViewParameters {
            float m_inverse[16]{};
            u32 m_width{}, m_height{}, m_source_width{}, m_source_height{};
            float m_min_x{}, m_min_y{}, m_cell_metres{};
            u32 m_reserved{};
        };
        static_assert(sizeof(GpuBackgroundViewParameters) == 96u and alignof(GpuBackgroundViewParameters) == 4u);
        static_assert(PPR_OFFSETOF(GpuBackgroundViewParameters, m_width) == 64u);
        static_assert(PPR_OFFSETOF(GpuBackgroundViewParameters, m_min_x) == 80u);

        struct GpuDirection {
            float m_x{}, m_y{};
        };

        struct GpuCascadeParameters {
            u32 m_probe_width{}, m_probe_height{}, m_directions{}, m_level{};
            float m_spacing{}, m_near_cells{}, m_terminal_far_cells{};
            u32 m_parent_width{};
            u32 m_parent_height{}, m_parent_direction_offset{}, m_direction_offset{}, m_dispatch_row_threads{};
            float m_parent_spacing{}, m_parent_near_cells{};
            u32 m_reserved[2]{};
        };

        struct GpuHierarchyLevel {
            u32 m_width{}, m_height{}, m_offset{}, m_shift{};
        };

        struct GpuHierarchyBuild {
            u32 m_level{}, m_row_threads{}, m_nodes{}, m_reserved{};
        };

        struct HierarchyDispatch {
            GpuHierarchyBuild m_parameters{};
            u32 m_groups_x{}, m_groups_y{};
        };

        struct HierarchyPlan {
            std::vector<GpuHierarchyLevel> m_levels{};
            std::vector<HierarchyDispatch> m_dispatches{};
            u64 m_flag_bytes{}, m_metadata_bytes{}, m_bytes{};
        };

        struct GpuHierarchy {
            rhi::ComPtr<rhi::IBuffer> m_flags{}, m_metadata{};
            std::vector<rhi::ComPtr<rhi::IShaderObject>> m_roots{};
            std::vector<HierarchyDispatch> m_dispatches{};
        };

        static_assert(sizeof(GpuHierarchyLevel) == 16u and alignof(GpuHierarchyLevel) == 4u);
        static_assert(PPR_OFFSETOF(GpuHierarchyLevel, m_shift) == 12u);
        static_assert(sizeof(GpuHierarchyBuild) == 16u and alignof(GpuHierarchyBuild) == 4u);
        static_assert(PPR_OFFSETOF(GpuHierarchyBuild, m_reserved) == 12u);

        static_assert(sizeof(GpuDirection) == 8u and alignof(GpuDirection) == 4u);
        static_assert(sizeof(GpuCascadeParameters) == 64u and alignof(GpuCascadeParameters) == 4u);
        static_assert(PPR_OFFSETOF(GpuCascadeParameters, m_parent_width) == 28u);
        static_assert(PPR_OFFSETOF(GpuCascadeParameters, m_parent_spacing) == 48u);

        static_assert(sizeof(GpuOpticalCell) == 40u and alignof(GpuOpticalCell) == 4u);
        static_assert(PPR_OFFSETOF(GpuOpticalCell, m_volume_emission) == 12u);
        static_assert(PPR_OFFSETOF(GpuOpticalCell, m_surface_emission) == 24u);
        static_assert(PPR_OFFSETOF(GpuOpticalCell, m_kind) == 36u);
        static_assert(sizeof(GpuOpticalRay) == 24u and alignof(GpuOpticalRay) == 4u);
        static_assert(PPR_OFFSETOF(GpuOpticalRay, m_near_m) == 16u);
        static_assert(sizeof(GpuOpticalResult) == 32u and alignof(GpuOpticalResult) == 4u);
        static_assert(PPR_OFFSETOF(GpuOpticalResult, m_status) == 24u);
        static_assert(sizeof(GpuTraceParameters) == 32u and alignof(GpuTraceParameters) == 4u);
        static_assert(PPR_OFFSETOF(GpuTraceParameters, m_cell_metres) == 16u);
        static_assert(PPR_OFFSETOF(GpuTraceParameters, m_hierarchy_levels) == 20u);

        // ------------------------------------------------------------------
        // single-purpose complete-cascade diagnostic file protocol
        // ------------------------------------------------------------------

        // inputs.bin contains this native-endian header followed by tightly
        // concatenated sections. Only memcpy accesses the potentially unaligned
        // payload. Floating values remain their actual uploaded representations.
        enum class CascadeDiagnosticSection : u32 {
            trace,
            terrain,
            receiver,
            cohort,
            geometry,
            levels,
            directions,
            cells,
            vertices,
            points,
            cases,
            features,
            labels,
            materials,
            count
        };

        struct CascadeDiagnosticRange {
            u64 m_offset{}, m_bytes{};
            u32 m_stride{}, m_count{};
        };

        struct CascadeDiagnosticHeader {
            u64 m_magic{0x3143474149444352ull}; // ASCII RCDIAGC1 on little-endian hosts.
            std::array<CascadeDiagnosticRange, static_cast<std::size_t>(CascadeDiagnosticSection::count)> m_sections{};
            u32 m_version{1u}, m_endian{std::endian::native == std::endian::little ? 1u : 2u};
        };

        struct CascadeDiagnosticLevel {
            u64 m_offset{std::numeric_limits<u64>::max()}, m_bytes{};
            GpuCascadeParameters m_parameters{};
            u32 m_executed{}, m_reserved{};
        };

        struct CascadeDiagnosticGeometry {
            u64 m_min_bits[2]{}, m_cell_metres_bits{};
            u32 m_width{}, m_height{}, m_outside_material{}, m_boundary{};
        };

        struct CascadeDiagnosticMaterial {
            u64 m_extinction_bits[3]{}, m_volume_bits[3]{}, m_surface_bits[3]{};
            u32 m_roundness_bits{}, m_material{}, m_kind{}, m_reserved{};
        };

        struct CascadeDiagnosticCapture {
            rhi::ComPtr<rhi::IBuffer> m_readback{};
            std::unique_ptr<std::byte[]> m_inputs{};
            CascadeDiagnosticHeader m_header{};
            u64 m_input_bytes{}, m_readback_bytes{}, m_retained_bytes{};
        };

        static_assert(sizeof(CascadeDiagnosticRange) == 24u and alignof(CascadeDiagnosticRange) == 8u);
        static_assert(PPR_OFFSETOF(CascadeDiagnosticRange, m_count) == 20u);
        static_assert(sizeof(CascadeDiagnosticHeader) == 352u and alignof(CascadeDiagnosticHeader) == 8u);
        static_assert(PPR_OFFSETOF(CascadeDiagnosticHeader, m_version) == 344u);
        static_assert(sizeof(CascadeDiagnosticLevel) == 88u and alignof(CascadeDiagnosticLevel) == 8u);
        static_assert(PPR_OFFSETOF(CascadeDiagnosticLevel, m_parameters) == 16u);
        static_assert(PPR_OFFSETOF(CascadeDiagnosticLevel, m_executed) == 80u);
        static_assert(sizeof(CascadeDiagnosticGeometry) == 40u and alignof(CascadeDiagnosticGeometry) == 8u);
        static_assert(sizeof(CascadeDiagnosticMaterial) == 88u and alignof(CascadeDiagnosticMaterial) == 8u);
        static_assert(PPR_OFFSETOF(CascadeDiagnosticMaterial, m_roundness_bits) == 72u);
        static_assert(sizeof(TerrainReceiverCohort) == 32u and alignof(TerrainReceiverCohort) == 8u);
        static_assert(std::numeric_limits<double>::is_iec559 and std::numeric_limits<double>::digits == 53);
        static_assert(
            std::is_trivially_copyable_v<CascadeDiagnosticHeader> and
            std::is_trivially_copyable_v<CascadeDiagnosticLevel> and
            std::is_trivially_copyable_v<CascadeDiagnosticGeometry> and
            std::is_trivially_copyable_v<CascadeDiagnosticMaterial>);

        // ------------------------------------------------------------------
        // checked scalar resource operations
        // ------------------------------------------------------------------

        std::error_code requireTrace_(const bool valid, const std::errc error = std::errc::invalid_argument) {
            return valid ? std::error_code{} : std::make_error_code(error);
        }

        std::error_code traceMultiply_(const u64 a, const u64 b, u64 &result) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(b == 0u or a <= std::numeric_limits<u64>::max() / b, std::errc::value_too_large));
            result = a * b;
            return {};
        }

        std::error_code traceAdd_(const u64 a, const u64 b, u64 &result) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(a <= std::numeric_limits<u64>::max() - b, std::errc::value_too_large));
            result = a + b;
            return {};
        }

        std::error_code writeDiagnosticBytes_(
            const std::filesystem::path &path, const void *const data, const u64 bytes) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  bytes <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()),
                                  std::errc::value_too_large));

            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(stream.is_open(), std::errc::io_error));
            stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
            stream.close();
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(not stream.fail(), std::errc::io_error));
            return {};
        }

        std::error_code writeDiagnosticBuffer_(
            rhi::IDevice &device, rhi::IBuffer *const buffer, const std::filesystem::path &path, const u64 bytes) {
            void *mapped{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, device.mapBuffer(buffer, rhi::CpuAccessMode::Read, &mapped));
            bool needs_unmap = true;
            PPR_DEFER {
                if (needs_unmap) {
                    // Best-effort cleanup while an exception reaches the caller.
                    PPR_LOG_WARNING_ON_FAIL(RadiancePass, device.unmapBuffer(buffer));
                }
            };

            std::error_code error = writeDiagnosticBytes_(path, mapped, bytes);
            PPR_RETAIN_ERROR_ON_FAIL(RadiancePass, error, device.unmapBuffer(buffer));
            needs_unmap = false;
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, error);
            return {};
        }

        u32 intervalStride_(const GpuIntervalStorage storage) noexcept {
            return storage == GpuIntervalStorage::rgb16f ? static_cast<u32>(sizeof(GpuPackedInterval))
                                                         : static_cast<u32>(sizeof(GpuOpticalResult));
        }

        // Decode only this readback protocol; no exported half arithmetic type.
        float decodeHalf_(const u32 bits) noexcept {
            const u32 sign = (bits & 32768u) << 16u;
            const u32 exponent = (bits >> 10u) & 31u;
            u32 fraction = bits & 1023u;
            if (exponent == 31u) {
                return std::bit_cast<float>(sign | 0x7f800000u | (fraction << 13u));
            }

            if (exponent == 0u) {
                if (fraction == 0u) {
                    return std::bit_cast<float>(sign);
                }

                u32 float_exponent = 113u;
                while ((fraction & 1024u) == 0u) {
                    fraction <<= 1u;
                    --float_exponent;
                }

                return std::bit_cast<float>(sign | (float_exponent << 23u) | ((fraction & 1023u) << 13u));
            }

            return std::bit_cast<float>(sign | ((exponent + 112u) << 23u) | (fraction << 13u));
        }

        // GPU arithmetic may flush FP32 subnormal operands/results. This is a
        // qualification-domain guard, independent of binary16 storage precision.
        bool normalOrZero_(const float value) noexcept {
            const u32 magnitude = std::bit_cast<u32>(value) & 0x7fffffffu;
            return magnitude == 0u or (magnitude >= 0x00800000u and magnitude < 0x7f800000u);
        }

        std::error_code toFloat_(const double value, float &result) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    std::isfinite(value) and std::abs(value) <= double((std::numeric_limits<float>::max)()),
                    std::errc::result_out_of_range));

            result = static_cast<float>(value);
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    std::isfinite(result) and (value == 0.0 or result != 0.0f), std::errc::result_out_of_range));
            return {};
        }

        std::error_code validateTraceEvents_(const GpuOpticalRay &ray, const GpuTraceParameters &parameters) {
            if (ray.m_near_m == ray.m_far_m) {
                return {};
            }

            const double reference_length = std::hypot(double(ray.m_direction[0]), double(ray.m_direction[1]));
            const float actual_length = std::sqrt(
                ray.m_direction[0] * ray.m_direction[0] + ray.m_direction[1] * ray.m_direction[1]);
            const std::array<double, 2> reference_direction{
                double(ray.m_direction[0]) / reference_length, double(ray.m_direction[1]) / reference_length};
            const std::array<float, 2> actual_direction{
                ray.m_direction[0] / actual_length, ray.m_direction[1] / actual_length};
            const std::array<u32, 2> extent{parameters.m_width, parameters.m_height};
            double reference_near = double(ray.m_near_m) / double(parameters.m_cell_metres);
            double reference_far = double(ray.m_far_m) / double(parameters.m_cell_metres);
            float actual_near = ray.m_near_m / parameters.m_cell_metres;
            float actual_far = ray.m_far_m / parameters.m_cell_metres;
            bool reference_axis_collapsed = false;

            // Evaluate the shader's actual FP32 clipping arithmetic separately
            // from double arithmetic over the same uploaded values. A positive
            // world intersection must not silently become an empty FP32 interval.
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const float origin = ray.m_origin[axis];
                if (ray.m_direction[axis] == 0.0f) {
                    if (origin < 0.0f or origin >= static_cast<float>(extent[axis])) {
                        return {};
                    }
                    continue;
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(
                                      std::isfinite(actual_direction[axis]) and actual_direction[axis] != 0.0f,
                                      std::errc::result_out_of_range));
                const double first = -double(origin) / reference_direction[axis];
                const double last = (double(extent[axis]) - double(origin)) / reference_direction[axis];
                reference_axis_collapsed = reference_axis_collapsed or first == last;
                reference_near = std::max(reference_near, std::min(first, last));
                reference_far = std::min(reference_far, std::max(first, last));

                const float actual_first = -origin / actual_direction[axis];
                const float actual_last = (static_cast<float>(extent[axis]) - origin) / actual_direction[axis];
                actual_near = std::max(actual_near, std::min(actual_first, actual_last));
                actual_far = std::min(actual_far, std::max(actual_first, actual_last));
            }

            if (reference_near > reference_far) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(not reference_axis_collapsed, std::errc::result_out_of_range));
            if (reference_near == reference_far) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  std::isfinite(actual_near) and std::isfinite(actual_far) and actual_near < actual_far,
                                  std::errc::result_out_of_range));

            struct PlaneEvent {
                double m_reference{};
                float m_actual{};
            };
            std::vector<PlaneEvent> events{};
            const u64 capacity = u64{extent[0]} + extent[1] + 4u;
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(capacity <= events.max_size(), std::errc::value_too_large));
            events.reserve(static_cast<std::size_t>(capacity));
            events.push_back({reference_near, actual_near});
            events.push_back({reference_far, actual_far});

            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                if (ray.m_direction[axis] == 0.0f) {
                    continue;
                }

                for (u64 plane = 0u; plane <= extent[axis]; ++plane) {
                    const double reference_event = (double(plane) - double(ray.m_origin[axis])) /
                                                   reference_direction[axis];
                    if (reference_event < reference_near or reference_event > reference_far) {
                        continue;
                    }

                    const float actual_event = (static_cast<float>(plane) - ray.m_origin[axis]) /
                                               actual_direction[axis];
                    events.push_back({reference_event, actual_event});
                }
            }

            std::ranges::sort(events, {}, &PlaneEvent::m_reference);

            // Preserve positive segments between either axis's planes and the
            // requested/world boundaries. Exact corner ties remain ties; this
            // also rejects a rounded-away partial cell at an interval endpoint.
            for (std::size_t index = 1u; index < events.size(); ++index) {
                const PlaneEvent &previous = events[index - 1u];
                const PlaneEvent &current = events[index];
                const bool ordered = previous.m_reference == current.m_reference ? previous.m_actual == current.m_actual
                                                                                 : previous.m_actual < current.m_actual;
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(ordered, std::errc::result_out_of_range));
            }

            return {};
        }

        std::expected<std::vector<GpuOpticalCell>, std::error_code> packCells_(
            const OpticalGridView &grid, const bool require_normal) {
            std::vector<GpuOpticalCell> cells(grid.m_cells.size());
            for (std::size_t index = 0u; index < cells.size(); ++index) {
                const auto &source = grid.m_cells[index];
                auto &destination = cells[index];
                destination.m_kind = source.m_kind == OpticalKind::vacuum   ? 0u
                                     : source.m_kind == OpticalKind::opaque ? 1u
                                                                            : 2u;
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        RadiancePass, toFloat_(source.m_extinction[channel], destination.m_extinction[channel]));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        RadiancePass,
                        toFloat_(source.m_volume_emission[channel], destination.m_volume_emission[channel]));
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        RadiancePass,
                        toFloat_(source.m_surface_emission[channel], destination.m_surface_emission[channel]));

                    // Opaque source loads share the same FTZ domain in both
                    // geometry modes; FP16 subnormals remain normal FP32 here.
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        RadiancePass,
                        requireTrace_(
                            normalOrZero_(destination.m_surface_emission[channel]), std::errc::argument_out_of_domain));

                    if (require_normal) {
                        PPR_RETURN_UNEXPECTED_ON_FAIL(
                            RadiancePass, requireTrace_(
                                              normalOrZero_(destination.m_extinction[channel]) and
                                                  normalOrZero_(destination.m_volume_emission[channel]),
                                              std::errc::argument_out_of_domain));
                    }
                }
            }

            return cells;
        }

        rhi::BufferRange traceRange_(rhi::IBuffer *const buffer) noexcept {
            return {.offset = 0u, .size = buffer->getDesc().size};
        }

        std::error_code traceBuffer_(
            rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial, const bool readback,
            rhi::ComPtr<rhi::IBuffer> &result) {
            rhi::BufferDesc description{};
            description.size = bytes;
            description.elementSize = stride;
            description.memoryType = readback ? rhi::MemoryType::ReadBack : rhi::MemoryType::DeviceLocal;
            description.usage = readback ? rhi::BufferUsage::CopyDestination
                                         : static_cast<rhi::BufferUsage>(
                                               static_cast<u32>(rhi::BufferUsage::ShaderResource) |
                                               static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
                                               static_cast<u32>(rhi::BufferUsage::CopySource) |
                                               static_cast<u32>(rhi::BufferUsage::CopyDestination));
            description.defaultState = readback ? rhi::ResourceState::CopyDestination
                                                : rhi::ResourceState::UnorderedAccess;
            description.label = "RadianceCascadePass optical qualification";
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, device.createBuffer(description, initial, result.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    result != nullptr and result->getDesc().size == bytes and result->getDesc().elementSize == stride));
            return {};
        }

        // ------------------------------------------------------------------
        // bounded precision counters under the existing batch fence owner
        // ------------------------------------------------------------------

        std::expected<std::unique_ptr<TerrainPrecisionStorage>, std::error_code> createTerrainPrecision_(
            rhi::IDevice &device, const u64 query_upper_bound) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  query_upper_bound > 0u and query_upper_bound <= std::numeric_limits<u32>::max(),
                                  std::errc::value_too_large));

            auto result = std::make_unique<TerrainPrecisionStorage>();
            result->m_statistics.m_query_upper_bound = query_upper_bound;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass,
                traceBuffer_(device, kTerrainPrecisionCounterBytes, sizeof(u32), nullptr, false, result->m_counts));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass,
                traceBuffer_(device, kTerrainPrecisionCounterBytes, 0u, nullptr, true, result->m_readback));
            return result;
        }

        std::error_code bindTerrainPrecision_(rhi::ShaderCursor &cursor, const TerrainPrecisionStorage *const storage) {
            if (storage == nullptr) {
                return {};
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(storage->m_counts->getDesc().elementSize == sizeof(u32)));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, cursor["g_terrain_precision_counts"].setBinding(
                                  rhi::Binding(storage->m_counts, traceRange_(storage->m_counts))));
            return {};
        }

        void beginTerrainPrecision_(rhi::ICommandEncoder &encoder, TerrainPrecisionStorage *const storage) {
            if (storage == nullptr) {
                return;
            }

            // Re-encoding a completed batch starts a new counter epoch. An
            // abandoned or unknown epoch follows the existing batch disposition.
            encoder.setBufferState(storage->m_counts, rhi::ResourceState::CopyDestination);
            encoder.clearBuffer(storage->m_counts, 0u, kTerrainPrecisionCounterBytes);
            encoder.setBufferState(storage->m_counts, rhi::ResourceState::UnorderedAccess);
        }

        void copyTerrainPrecision_(rhi::ICommandEncoder &encoder, const TerrainPrecisionStorage *const storage) {
            if (storage == nullptr) {
                return;
            }

            encoder.setBufferState(storage->m_counts, rhi::ResourceState::CopySource);
            encoder.setBufferState(storage->m_readback, rhi::ResourceState::CopyDestination);
            encoder.copyBuffer(storage->m_readback, 0u, storage->m_counts, 0u, kTerrainPrecisionCounterBytes);
        }


        // ------------------------------------------------------------------
        // checked hierarchy resources and ordered GPU construction
        // ------------------------------------------------------------------

        std::expected<HierarchyPlan, std::error_code> hierarchyPlan_(
            rhi::IDevice &device, const u32 initial_width, const u32 initial_height) {
            HierarchyPlan plan{};
            const auto &limits = device.getInfo().limits;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  limits.maxComputeThreadsPerGroup >= 64u and
                                      limits.maxComputeThreadGroupSize[0] >= 64u and
                                      limits.maxComputeDispatchThreadGroups[0] > 0u and
                                      limits.maxComputeDispatchThreadGroups[1] > 0u,
                                  std::errc::function_not_supported));

            u64 width = initial_width;
            u64 height = initial_height;
            u64 offset{};
            for (u32 shift = 0u;; ++shift) {
                u64 count{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceMultiply_(width, height, count));
                u64 end{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(offset, count, end));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass,
                    requireTrace_(end <= std::numeric_limits<u32>::max() and shift < 32u, std::errc::value_too_large));

                const u64 groups = count / 64u + (count % 64u == 0u ? 0u : 1u);
                const u64 groups_x = std::min<u64>(
                    {groups, limits.maxComputeDispatchThreadGroups[0], std::numeric_limits<u32>::max() / 64u});
                const u64 groups_y = groups / groups_x + (groups % groups_x == 0u ? 0u : 1u);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass, requireTrace_(
                                      groups_y <= limits.maxComputeDispatchThreadGroups[1] and
                                          groups_x * groups_y <= std::numeric_limits<u32>::max() / 64u,
                                      std::errc::function_not_supported));

                plan.m_levels.push_back(
                    {static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(offset), shift});
                plan.m_dispatches.push_back(
                    {{shift, static_cast<u32>(groups_x * 64u), static_cast<u32>(count), 0u}, static_cast<u32>(groups_x),
                        static_cast<u32>(groups_y)});
                offset = end;
                if (width == 1u and height == 1u) {
                    break;
                }

                width = (width + 1u) / 2u;
                height = (height + 1u) / 2u;
            }

            PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceMultiply_(offset, sizeof(u32), plan.m_flag_bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceMultiply_(plan.m_levels.size(), sizeof(GpuHierarchyLevel), plan.m_metadata_bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceAdd_(plan.m_flag_bytes, plan.m_metadata_bytes, plan.m_bytes));
            return plan;
        }

        std::expected<GpuHierarchy, std::error_code> createHierarchy_(
            rhi::IDevice &device, rhi::IShaderProgram *const program, rhi::IBuffer *const cells,
            const HierarchyPlan &plan) {
            GpuHierarchy hierarchy{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceBuffer_(device, plan.m_flag_bytes, sizeof(u32), nullptr, false, hierarchy.m_flags));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceBuffer_(
                                  device, plan.m_metadata_bytes, sizeof(GpuHierarchyLevel), plan.m_levels.data(), false,
                                  hierarchy.m_metadata));
            hierarchy.m_dispatches = plan.m_dispatches;

            for (const auto &dispatch: plan.m_dispatches) {
                rhi::ComPtr<rhi::IShaderObject> root{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, device.createRootShaderObject(program, root.writeRef()));
                rhi::ShaderCursor cursor{root};
                rhi::ShaderCursor constants{};
                PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, cursor["g_build"].getDereferenced(constants));
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass, constants.setData(&dispatch.m_parameters, sizeof(dispatch.m_parameters)));

                // Only build-entry resources are bound. In particular the SRV
                // traversal alias is never active beside the RW construction view.
                for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 3>{
                         std::pair{"g_cells", cells}, {"g_hierarchy", hierarchy.m_metadata.get()},
                         {"g_flags_write", hierarchy.m_flags.get()}}) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
                }

                hierarchy.m_roots.push_back(std::move(root));
            }

            return hierarchy;
        }

        std::error_code encodeHierarchy_(
            rhi::ICommandEncoder &encoder, rhi::IComputePipeline *const pipeline, const GpuHierarchy &hierarchy) {
            const hal::ProfileScope scope{"App.Lighting.EncodeVacuumHierarchy"};
            encoder.setBufferState(hierarchy.m_metadata, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(hierarchy.m_flags, rhi::ResourceState::UnorderedAccess);
            for (std::size_t index = 0u; index < hierarchy.m_dispatches.size(); ++index) {
                const auto &dispatch = hierarchy.m_dispatches[index];
                auto *const pass = encoder.beginComputePass();
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
                pass->bindPipeline(pipeline, hierarchy.m_roots[index]);
                pass->dispatchCompute(dispatch.m_groups_x, dispatch.m_groups_y, 1u);
                pass->end();

                // A parent reads only the previous completed level and writes a
                // disjoint range. No workgroup-local or in-place reduction races.
                encoder.globalBarrier();
            }

            encoder.setBufferState(hierarchy.m_flags, rhi::ResourceState::ShaderResource);
            return {};
        }

        // ------------------------------------------------------------------
        // rounded terrain capture resources and entry-only bindings
        // ------------------------------------------------------------------

        std::vector<OpticalCell> terrainCells_(const OpticalTerrainSnapshot &snapshot) {
            const auto view = snapshot.view();
            std::vector<OpticalCell> cells{};
            cells.reserve(view.m_labels.size());
            for (const u16 id: view.m_labels) {
                const auto found = std::ranges::find(view.m_materials, id, &OpticalTerrainMaterial::m_material);
                PPR_ASSERT(found != view.m_materials.end());
                cells.push_back(found->m_optics);
            }

            return cells;
        }

        std::expected<TerrainPlan, std::error_code> terrainPlan_(const OpticalTerrainSnapshot &snapshot) {
            const auto view = snapshot.view();
            TerrainPlan result{};
            result.m_vertices.reserve(view.m_labels.size());

            for (const u16 id: view.m_labels) {
                const auto found = std::ranges::find(view.m_materials, id, &OpticalTerrainMaterial::m_material);
                PPR_ASSERT(found != view.m_materials.end());
                result.m_vertices.push_back({found->m_roundness, id});
            }

            const auto halo = std::ranges::find(
                view.m_materials, view.m_outside_material, &OpticalTerrainMaterial::m_material);
            PPR_ASSERT(halo != view.m_materials.end());
            result.m_parameters.m_outside_material = view.m_outside_material;
            result.m_parameters.m_outside_roundness = halo->m_roundness;
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, toFloat_(halo->m_optics.m_surface_emission[0], result.m_parameters.m_outside_r));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, toFloat_(halo->m_optics.m_surface_emission[1], result.m_parameters.m_outside_g));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, toFloat_(halo->m_optics.m_surface_emission[2], result.m_parameters.m_outside_b));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  normalOrZero_(result.m_parameters.m_outside_r) and
                                      normalOrZero_(result.m_parameters.m_outside_g) and
                                      normalOrZero_(result.m_parameters.m_outside_b),
                                  std::errc::argument_out_of_domain));

            const u64 vertices = result.m_vertices.size() * sizeof(GpuTerrainVertex);
            const u64 points = math::contourPoints().size_bytes(), cases = math::contourCases().size_bytes();
            PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(vertices, points, result.m_bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(result.m_bytes, cases, result.m_bytes));
            result.m_largest = std::max({vertices, points, cases});
            return result;
        }

        std::expected<TerrainStorage, std::error_code> createTerrain_(rhi::IDevice &device, const TerrainPlan &plan) {
            TerrainStorage result{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceBuffer_(
                                  device, plan.m_vertices.size() * sizeof(GpuTerrainVertex), sizeof(GpuTerrainVertex),
                                  plan.m_vertices.data(), false, result.m_vertices));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceBuffer_(
                                  device, math::contourPoints().size_bytes(), sizeof(math::ContourPoint),
                                  math::contourPoints().data(), false, result.m_points));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceBuffer_(
                                  device, math::contourCases().size_bytes(), sizeof(math::ContourCase),
                                  math::contourCases().data(), false, result.m_cases));
            return result;
        }

        std::error_code bindTerrain_(
            rhi::ShaderCursor &cursor, const TerrainStorage &storage, const TerrainPlan &plan) {
            rhi::ShaderCursor constants{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_terrain"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(&plan.m_parameters, sizeof(plan.m_parameters)));

            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 3>{
                     std::pair{"g_terrain_vertices", storage.m_vertices.get()},
                     {"g_contour_points", storage.m_points.get()}, {"g_contour_cases", storage.m_cases.get()}}) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
            }

            return {};
        }

        void terrainStates_(rhi::ICommandEncoder &encoder, const TerrainStorage &storage) {
            encoder.setBufferState(storage.m_vertices, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(storage.m_points, rhi::ResourceState::ShaderResource);
            encoder.setBufferState(storage.m_cases, rhi::ResourceState::ShaderResource);
        }

        std::error_code checkTraceLayout_(slang::ProgramLayout &layout) {
            const std::array<std::pair<const char *, std::size_t>, 17> fields{
                std::pair{"g_cells", sizeof(GpuOpticalCell)}, {"g_rays", sizeof(GpuOpticalRay)},
                {"g_results", sizeof(GpuOpticalResult)}, {"g_trace", sizeof(GpuTraceParameters)},
                {"g_directions", sizeof(GpuDirection)}, {"g_upper", sizeof(GpuOpticalResult)},
                {"g_cascade", sizeof(GpuCascadeParameters)}, {"g_hierarchy", sizeof(GpuHierarchyLevel)},
                {"g_build", sizeof(GpuHierarchyBuild)}, {"g_packed_results", sizeof(GpuPackedInterval)},
                {"g_packed_upper", sizeof(GpuPackedInterval)}, {"g_terrain_vertices", sizeof(GpuTerrainVertex)},
                {"g_terrain", sizeof(GpuTerrainParameters)}, {"g_receiver_features", sizeof(TerrainReceiverFeature)},
                {"g_receiver_results", sizeof(TerrainReceiverResult)}, {"g_receivers", sizeof(GpuReceiverParameters)},
                {"g_receiver_angular", sizeof(TerrainReceiverAngular)}};
            for (const auto &[name, size]: fields) {
                slang::TypeLayoutReflection *element{};
                for (unsigned index = 0u; index < layout.getParameterCount(); ++index) {
                    auto *const parameter = layout.getParameterByIndex(index);
                    if (std::string_view{parameter->getName()} == name) {
                        element = parameter->getTypeLayout()->getElementTypeLayout();
                        break;
                    }
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(
                                      element != nullptr and
                                      element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == size and
                                      element->getFieldCount() == size / 4u));
                // Every protocol member is a scalar. Verify all actual offsets,
                // not just matching aggregate sizes on the active backend.
                for (unsigned index = 0u; index < element->getFieldCount(); ++index) {
                    auto *const field = element->getFieldByIndex(index);
                    PPR_RETURN_ERROR_ON_FAIL(
                        RadiancePass,
                        requireTrace_(field->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM) == u64{index} * 4u));
                }
            }

            for (const auto &[name, bytes]: std::array<std::pair<const char *, std::size_t>, 2>{
                     std::pair{"g_contour_points", sizeof(math::ContourPoint)},
                     {"g_contour_cases", sizeof(math::ContourCase)}}) {
                slang::TypeLayoutReflection *element{};
                for (unsigned index = 0u; index < layout.getParameterCount(); ++index) {
                    auto *const parameter = layout.getParameterByIndex(index);
                    if (std::string_view{parameter->getName()} == name) {
                        element = parameter->getTypeLayout()->getElementTypeLayout();
                        break;
                    }
                }

                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    requireTrace_(element and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes));
            }

            return {};
        }
    }

    // ------------------------------------------------------------------
    // pass storage and initialization
    // ------------------------------------------------------------------

    // Optional immutable source resources for exact-view backwall reconstruction.
    // These are the same uploaded rows used by the cascades, not a second catalog.
    struct TerrainBackgroundSource {
        TerrainStorage m_terrain{};
        rhi::ComPtr<rhi::IBuffer> m_cells{}, m_means{};
        GpuTraceParameters m_trace{};
        GpuTerrainParameters m_parameters{};
        TerrainPrecision m_precision{};
        u32 m_width{}, m_height{};
    };

    struct RadianceCascadePass::State {
        struct BackgroundBatch {
            std::shared_ptr<TerrainBackgroundGpuView> m_view{};
            rhi::ComPtr<rhi::IShaderObject> m_root{};
            rhi::ComPtr<rhi::IBuffer> m_counts{}, m_readback{};
            GpuBackgroundViewParameters m_parameters{};
            bool m_published{};
        };
        std::unique_ptr<BackgroundBatch> m_background_batch{};
        std::vector<std::weak_ptr<const TerrainBackgroundGpuView>> m_background_views{};

        std::unique_ptr<CascadeDiagnosticCapture> m_cascade_diagnostic{};
        std::array<shader::ComPtr<slang::IComponentType>, 6> m_terrain_components{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 6> m_terrain_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 6> m_terrain_pipelines{};
        TerrainStorage m_terrain{};
        std::unique_ptr<TerrainPrecisionStorage> m_precision_storage{};
        std::shared_ptr<TerrainReceiverGpuGeneration> m_receiver{};
        std::vector<std::weak_ptr<const TerrainReceiverGpuGeneration>> m_receiver_generations{};
        rhi::ComPtr<rhi::IShaderObject> m_receiver_root{}, m_background_mean_root{};
        GpuReceiverParameters m_receiver_parameters{};
        bool m_receiver_published{};


        rhi::ComPtr<rhi::IShaderProgram> m_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_pipeline{};
        rhi::ComPtr<rhi::IShaderProgram> m_packed_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_packed_pipeline{};
        rhi::ComPtr<rhi::IShaderProgram> m_hierarchy_program{};
        rhi::ComPtr<rhi::IComputePipeline> m_hierarchy_pipeline{};
        GpuHierarchy m_hierarchy{};
        rhi::ComPtr<rhi::IBuffer> m_cells{};
        rhi::ComPtr<rhi::IBuffer> m_rays{};
        rhi::ComPtr<rhi::IBuffer> m_output{};
        rhi::ComPtr<rhi::IBuffer> m_readback{};
        rhi::ComPtr<rhi::IBuffer> m_segments{};
        rhi::ComPtr<rhi::IShaderObject> m_root{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 2> m_cascade_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 2> m_cascade_pipelines{};
        std::array<rhi::ComPtr<rhi::IShaderProgram>, 2> m_packed_cascade_programs{};
        std::array<rhi::ComPtr<rhi::IComputePipeline>, 2> m_packed_cascade_pipelines{};
        std::array<rhi::ComPtr<rhi::IBuffer>, 2> m_parity{};
        rhi::ComPtr<rhi::IBuffer> m_directions{};

        struct CascadeLevel {
            rhi::ComPtr<rhi::IShaderObject> m_root{};
            u32 m_groups_x{}, m_groups_y{}, m_records{}, m_level{};
        };
        std::vector<CascadeLevel> m_levels{};

        rhi::IDevice *m_device{};
        u64 m_buffer_bytes{};
        u64 m_largest_buffer_bytes{};
        FenceCompletionValue m_last_use{kFenceValueNone};
        GpuTraceParameters m_parameters{};
        u32 m_diagnostic_count{};
        GpuIntervalStorage m_storage{GpuIntervalStorage::fp32};
        TerrainPrecision m_precision{TerrainPrecision::strict_fp32_qualification};
        bool m_terrain_mode{};
        bool m_cascade_mode{};
        bool m_prepared{};
        bool m_encoded{};
        bool m_submitted{};
        bool m_unknown{};
    };

    RadianceCascadePass::RadianceCascadePass() noexcept = default;

    RadianceCascadePass::~RadianceCascadePass() {
        if (m_state and (m_state->m_encoded or m_state->m_unknown or m_state->m_last_use != kFenceValueNone)) {
            hal::fatalExit("radiance-pass-gpu-completion-unknown");
        }
    }

    std::error_code RadianceCascadePass::initialize(
        rhi::IDevice &device, IShaderService &shaders, const std::filesystem::path &content_root,
        const TerrainPrecisionSelection precision) {
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(not m_state, std::errc::operation_in_progress));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              precision.m_precision == TerrainPrecision::strict_fp32_qualification or
                              precision.m_precision == TerrainPrecision::qualified_fp64_refinement_experimental));
        const bool refined = precision.m_precision == TerrainPrecision::qualified_fp64_refinement_experimental;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not refined or (device.getInfo().deviceType == rhi::DeviceType::Vulkan and
                                                 device.hasFeature(rhi::Feature::Double) and
                                                 precision.m_rte64_qualified_for_device_session),
                              std::errc::not_supported));

        // The caller's assertion owns the checked RTE64 evidence for this exact
        // device/session/configuration. No backend/driver is implicitly qualified.

        // TEMPORARY localization only; timestamps/thread IDs belong to the logger.
        PPR_LOG(RadiancePass, info, "RC-INIT before State allocation");
        auto candidate = std::make_unique<State>();
        candidate->m_device = &device;
        candidate->m_precision = precision.m_precision;
        PPR_LOG(RadiancePass, info, "RC-INIT after State allocation");

        shader::SharedModule module{};
        PPR_LOG(RadiancePass, info, "RC-INIT before loadModuleFromFile radiance_cascades");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            shaders.loadModuleFromFile(
                content_root / "shaders" / (refined ? "radiance_cascades_refined.slang" : "radiance_cascades.slang"),
                refined ? "radiance_cascades_refined" : "radiance_cascades", module.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after loadModuleFromFile radiance_cascades");

        PPR_LOG(RadiancePass, info, "RC-INIT before getLayout");
        auto *const layout = module->getLayout();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(layout != nullptr));
        PPR_LOG(RadiancePass, info, "RC-INIT after getLayout");

        PPR_LOG(RadiancePass, info, "RC-INIT before checkTraceLayout");
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, checkTraceLayout_(*layout));
        if (refined) {
            slang::TypeLayoutReflection *element{};
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == "g_terrain_precision_counts") {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    element != nullptr and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == sizeof(u32)));
        }
        for (const auto &[name, bytes]: std::array<std::pair<const char *, u64>, 5>{
                 std::pair{"g_background_view", sizeof(GpuBackgroundViewParameters)},
                 {"g_background_means", sizeof(TerrainReceiverResult)},
                 {"g_background_source", sizeof(TerrainReceiverResult)}, {"g_background_view_results", 16u},
                 {"g_background_view_counts", sizeof(u32)}}) {
            slang::TypeLayoutReflection *element{};
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{parameter->getName()} == name) {
                    element = parameter->getTypeLayout()->getElementTypeLayout();
                    break;
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(element and element->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == bytes));

            if (std::string_view{name} == "g_background_view") {
                constexpr std::array<u64, 9> offsets{0u, 64u, 68u, 72u, 76u, 80u, 84u, 88u, 92u};
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(element->getFieldCount() == offsets.size()));
                for (unsigned index = 0u; index < offsets.size(); ++index) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        RadiancePass, requireTrace_(
                                          element->getFieldByIndex(index)->getOffset(
                                              SLANG_PARAMETER_CATEGORY_UNIFORM) == offsets[index]));
                }
            }
        }

        PPR_LOG(RadiancePass, info, "RC-INIT after checkTraceLayout");

        shader::ComPtr<slang::IEntryPoint> entry{};
        PPR_LOG(RadiancePass, info, "RC-INIT before findEntryPointByName traceOptical");
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, module->findEntryPointByName("traceOptical", entry.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after findEntryPointByName traceOptical");

        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = module.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;
        shader::Diagnose diagnostics{};
        PPR_LOG(RadiancePass, info, "RC-INIT before createShaderProgram traceOptical");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            device.createShaderProgram(description, candidate->m_program.writeRef(), diagnostics.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createShaderProgram traceOptical");

        rhi::ComputePipelineDesc pipeline{};
        pipeline.program = candidate->m_program;
        PPR_LOG(RadiancePass, info, "RC-INIT before createComputePipeline traceOptical");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, device.createComputePipeline(pipeline, candidate->m_pipeline.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createComputePipeline traceOptical");

        const std::array<const char *, 4> cascade_entries{
            "cascadeTop", "cascadeMerge", "cascadeTopPacked", "cascadeMergePacked"};
        for (std::size_t index = 0u; index < cascade_entries.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> cascade_entry{};
            PPR_LOG(
                RadiancePass, info, "RC-INIT before findEntryPointByName",
                {{"entry", std::string_view{cascade_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, module->findEntryPointByName(cascade_entries[index], cascade_entry.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after findEntryPointByName",
                {{"entry", std::string_view{cascade_entries[index]}}});

            slang::IComponentType *cascade_components[]{cascade_entry.get()};
            description.slangEntryPoints = cascade_components;
            auto &program = index < 2u ? candidate->m_cascade_programs[index]
                                       : candidate->m_packed_cascade_programs[index - 2u];
            auto &compute_pipeline = index < 2u ? candidate->m_cascade_pipelines[index]
                                                : candidate->m_packed_cascade_pipelines[index - 2u];
            PPR_LOG(
                RadiancePass, info, "RC-INIT before createShaderProgram",
                {{"entry", std::string_view{cascade_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, device.createShaderProgram(description, program.writeRef(), diagnostics.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after createShaderProgram",
                {{"entry", std::string_view{cascade_entries[index]}}});

            pipeline.program = program;
            PPR_LOG(
                RadiancePass, info, "RC-INIT before createComputePipeline",
                {{"entry", std::string_view{cascade_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, device.createComputePipeline(pipeline, compute_pipeline.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after createComputePipeline",
                {{"entry", std::string_view{cascade_entries[index]}}});
        }

        shader::ComPtr<slang::IEntryPoint> hierarchy_entry{};
        PPR_LOG(RadiancePass, info, "RC-INIT before findEntryPointByName buildVacuumHierarchy");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, module->findEntryPointByName("buildVacuumHierarchy", hierarchy_entry.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after findEntryPointByName buildVacuumHierarchy");

        slang::IComponentType *hierarchy_entries[]{hierarchy_entry.get()};
        description.slangEntryPoints = hierarchy_entries;
        PPR_LOG(RadiancePass, info, "RC-INIT before createShaderProgram buildVacuumHierarchy");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            device.createShaderProgram(description, candidate->m_hierarchy_program.writeRef(), diagnostics.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createShaderProgram buildVacuumHierarchy");

        pipeline.program = candidate->m_hierarchy_program;
        PPR_LOG(RadiancePass, info, "RC-INIT before createComputePipeline buildVacuumHierarchy");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, device.createComputePipeline(pipeline, candidate->m_hierarchy_pipeline.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createComputePipeline buildVacuumHierarchy");

        shader::ComPtr<slang::IEntryPoint> packed_entry{};
        PPR_LOG(RadiancePass, info, "RC-INIT before findEntryPointByName traceOpticalPacked");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, module->findEntryPointByName("traceOpticalPacked", packed_entry.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after findEntryPointByName traceOpticalPacked");

        slang::IComponentType *packed_entries[]{packed_entry.get()};
        description.slangEntryPoints = packed_entries;
        PPR_LOG(RadiancePass, info, "RC-INIT before createShaderProgram traceOpticalPacked");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            device.createShaderProgram(description, candidate->m_packed_program.writeRef(), diagnostics.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createShaderProgram traceOpticalPacked");

        pipeline.program = candidate->m_packed_program;
        PPR_LOG(RadiancePass, info, "RC-INIT before createComputePipeline traceOpticalPacked");
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, device.createComputePipeline(pipeline, candidate->m_packed_pipeline.writeRef()));
        PPR_LOG(RadiancePass, info, "RC-INIT after createComputePipeline traceOpticalPacked");

        // The pinned Slang2026.17.1 direct SPIR-V emitter predates the
        // per-variable precise fix. Scope PRECISE to these linked programs;
        // old cell/packed programs and the service/session defaults stay intact.
        const std::array<const char *, 6> terrain_entries{
            "traceTerrain", "cascadeTerrainTop", "cascadeTerrainMerge", "terrainReceivers", "terrainBackgroundMeans",
            "terrainBackgroundView"};
        for (std::size_t index = 0u;
            device.getInfo().deviceType == rhi::DeviceType::Vulkan and index < terrain_entries.size(); ++index) {
            shader::ComPtr<slang::IEntryPoint> terrain_entry{};
            PPR_LOG(
                RadiancePass, info, "RC-INIT before findEntryPointByName",
                {{"entry", std::string_view{terrain_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, module->findEntryPointByName(terrain_entries[index], terrain_entry.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after findEntryPointByName",
                {{"entry", std::string_view{terrain_entries[index]}}});

            slang::IComponentType *parts[]{module.get(), terrain_entry.get()};
            shader::ComPtr<slang::IComponentType> composite{};
            PPR_LOG(
                RadiancePass, info, "RC-INIT before createCompositeComponentType",
                {{"entry", std::string_view{terrain_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, module->getSession()->createCompositeComponentType(
                                  parts, 2, composite.writeRef(), diagnostics.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after createCompositeComponentType",
                {{"entry", std::string_view{terrain_entries[index]}}});

            slang::CompilerOptionEntry option{};
            option.name = slang::CompilerOptionName::FloatingPointMode;
            option.value.kind = slang::CompilerOptionValueKind::Int;
            option.value.intValue0 = SLANG_FLOATING_POINT_MODE_PRECISE;
            PPR_LOG(
                RadiancePass, info, "RC-INIT before linkWithOptions PRECISE",
                {{"entry", std::string_view{terrain_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                composite->linkWithOptions(
                    candidate->m_terrain_components[index].writeRef(), 1u, &option, diagnostics.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after linkWithOptions PRECISE",
                {{"entry", std::string_view{terrain_entries[index]}}});

            rhi::ShaderProgramDesc terrain_description{};
            terrain_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
            terrain_description.slangGlobalScope = candidate->m_terrain_components[index].get();
            terrain_description.slangEntryPointCount = 0u;
            PPR_LOG(
                RadiancePass, info, "RC-INIT before createShaderProgram",
                {{"entry", std::string_view{terrain_entries[index]}}});
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                device.createShaderProgram(
                    terrain_description, candidate->m_terrain_programs[index].writeRef(), diagnostics.writeRef()));
            PPR_LOG(
                RadiancePass, info, "RC-INIT after createShaderProgram",
                {{"entry", std::string_view{terrain_entries[index]}}});
        }

        PPR_LOG(RadiancePass, info, "RC-INIT before commit");
        m_state = std::move(candidate);
        PPR_LOG(RadiancePass, info, "RC-INIT return success");
        return {};
    }

    // ------------------------------------------------------------------
    // public bounded source selection and exact linked-code observation
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::prepareTrace(
        const OpticalGridView &grid, const std::span<const OpticalRay> rays, const u64 budget_bytes,
        const u64 reserve_bytes, const u64 max_resource_bytes, const FenceCompletionValue completed,
        const GpuIntervalStorage storage) {
        return prepareTrace_(grid, rays, budget_bytes, reserve_bytes, max_resource_bytes, completed, storage, nullptr);
    }

    std::error_code RadianceCascadePass::prepareCascades(
        const OpticalGridView &grid, const RcEstimateRequest &request, const FenceCompletionValue completed,
        const GpuIntervalStorage storage) {
        return prepareCascades_(grid, request, completed, storage, nullptr);
    }

    std::error_code RadianceCascadePass::prepareTerrainTrace(
        const OpticalTerrainSnapshot &terrain, const std::span<const OpticalRay> rays, const u64 budget_bytes,
        const u64 reserve_bytes, const u64 max_resource_bytes, const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                m_state->m_device->getInfo().deviceType == rhi::DeviceType::Vulkan, std::errc::not_supported));

        const auto view = terrain.view();
        const auto cells = terrainCells_(terrain);
        const OpticalGridView grid{
            cells, view.m_min_m, view.m_cell_size_m, view.m_width, view.m_height, view.m_boundary};
        return prepareTrace_(
            grid, rays, budget_bytes, reserve_bytes, max_resource_bytes, completed, GpuIntervalStorage::fp32, &terrain);
    }

    std::error_code RadianceCascadePass::prepareTerrainCascades(
        const OpticalTerrainSnapshot &terrain, const RcEstimateRequest &request, const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                m_state->m_device->getInfo().deviceType == rhi::DeviceType::Vulkan, std::errc::not_supported));

        const auto view = terrain.view();
        const auto cells = terrainCells_(terrain);
        const OpticalGridView grid{
            cells, view.m_min_m, view.m_cell_size_m, view.m_width, view.m_height, view.m_boundary};
        return prepareCascades_(grid, request, completed, GpuIntervalStorage::fp32, &terrain);
    }

    std::array<bool, 3> RadianceCascadePass::terrainPipelineReadinessForQualification() const noexcept {
        if (not m_state) {
            return {};
        }

        return {
            m_state->m_terrain_pipelines[0] != nullptr, m_state->m_terrain_pipelines[1] != nullptr,
            m_state->m_terrain_pipelines[2] != nullptr};
    }

    std::error_code RadianceCascadePass::writeTerrainSpirvForQualification(
        const std::filesystem::path &directory, const bool receiver_only) {
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                m_state->m_device->getInfo().deviceType == rhi::DeviceType::Vulkan, std::errc::not_supported));

        std::error_code filesystem_error{};
        std::filesystem::create_directories(directory, filesystem_error);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, filesystem_error);

        const std::array<const char *, 4> names{
            "traceTerrain.spv", "cascadeTerrainTop.spv", "cascadeTerrainMerge.spv", "terrainReceivers.spv"};
        for (std::size_t index = receiver_only ? 3u : 0u; index < (receiver_only ? 4u : 3u); ++index) {
            shader::ComPtr<slang::IBlob> code{};
            shader::Diagnose diagnostics{};
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                m_state->m_terrain_components[index]->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(code and code->getBufferSize() >= sizeof(u32)));

            u32 magic{};
            std::memcpy(&magic, code->getBufferPointer(), sizeof(magic));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(magic == 0x07230203u, std::errc::not_supported));

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    code->getBufferSize() <= static_cast<u64>((std::numeric_limits<std::streamsize>::max)()),
                    std::errc::value_too_large));

            std::ofstream stream{directory / names[index], std::ios::binary | std::ios::trunc};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(stream.is_open(), std::errc::io_error));
            stream.write(
                static_cast<const char *>(code->getBufferPointer()),
                static_cast<std::streamsize>(code->getBufferSize()));
            stream.close();
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(not stream.fail(), std::errc::io_error));
        }

        return {};
    }

    // ------------------------------------------------------------------
    // validated upload admission and encoding
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::prepareTrace_(
        const OpticalGridView &grid, const std::span<const OpticalRay> rays, const u64 budget_bytes,
        const u64 reserve_bytes, const u64 max_resource_bytes, const FenceCompletionValue completed,
        const GpuIntervalStorage storage, const OpticalTerrainSnapshot *const terrain) {
        const hal::ProfileScope scope{"App.Lighting.PrepareGpuTrace"};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(storage == GpuIntervalStorage::fp32 or storage == GpuIntervalStorage::rgb16f));
        const bool packed = storage == GpuIntervalStorage::rgb16f;
        const u32 stride = intervalStride_(storage);

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not state.m_encoded and not state.m_unknown and slotFenceComplete(completed, state.m_last_use),
                std::errc::device_or_resource_busy));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, validateOpticalGrid(grid));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not rays.empty() and
                              rays.size() <= std::numeric_limits<u32>::max() and
                              grid.m_cells.size() <= std::numeric_limits<u32>::max() and
                              u64{grid.m_width} + grid.m_height <= std::numeric_limits<u32>::max()));

        const auto hierarchy_plan = terrain ? std::expected<HierarchyPlan, std::error_code>{HierarchyPlan{}}
                                            : hierarchyPlan_(*state.m_device, grid.m_width, grid.m_height);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, hierarchy_plan ? std::error_code{} : hierarchy_plan.error());

        const auto terrain_plan = terrain ? terrainPlan_(*terrain)
                                          : std::expected<TerrainPlan, std::error_code>{TerrainPlan{}};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, terrain_plan ? std::error_code{} : terrain_plan.error());

        u64 cell_bytes{};
        u64 ray_bytes{};
        u64 result_bytes{};
        u64 segment_bytes{};
        u64 readback_bytes{};
        u64 candidate_bytes{};
        u64 peak_bytes{};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(grid.m_cells.size(), sizeof(GpuOpticalCell), cell_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(rays.size(), sizeof(GpuOpticalRay), ray_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(rays.size(), stride, result_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(rays.size(), packed ? sizeof(u32) : 0u, segment_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(result_bytes, segment_bytes, readback_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(cell_bytes, ray_bytes, candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, hierarchy_plan->m_bytes, candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, terrain_plan->m_bytes, candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, result_bytes, candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, segment_bytes, candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, readback_bytes, candidate_bytes));
        const bool refined = terrain != nullptr and
                             state.m_precision == TerrainPrecision::qualified_fp64_refinement_experimental;
        if (refined) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceAdd_(candidate_bytes, kTerrainPrecisionRetainedBytes, candidate_bytes));
        }

        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, state.m_buffer_bytes, peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak_bytes, reserve_bytes, peak_bytes));
        u64 retained_largest{};
        std::erase_if(state.m_receiver_generations, [](const auto &generation) { return generation.expired(); });
        for (const auto &weak: state.m_receiver_generations) {
            if (const auto generation = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak_bytes, generation->bufferBytes(), peak_bytes));
                retained_largest = std::max(retained_largest, generation->largestBufferBytes());
            }
        }


        std::erase_if(state.m_background_views, [](const auto &view) { return view.expired(); });
        for (const auto &weak: state.m_background_views) {
            if (const auto view = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak_bytes, view->bufferBytes(), peak_bytes));
                retained_largest = std::max(retained_largest, view->largestBufferBytes());
            }
        }

        const auto &limits = state.m_device->getInfo().limits;
        const u64 largest = std::max(
            {cell_bytes, ray_bytes, result_bytes, segment_bytes, readback_bytes, hierarchy_plan->m_flag_bytes,
                hierarchy_plan->m_metadata_bytes, terrain_plan->m_largest,
                refined ? kTerrainPrecisionCounterBytes : 0u});
        const u64 groups = rays.size() / 64u + (rays.size() % 64u == 0u ? 0u : 1u);
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                peak_bytes <= budget_bytes and
                    std::max({largest, state.m_largest_buffer_bytes, retained_largest}) <= max_resource_bytes and
                    largest <= limits.maxBufferSize,
                std::errc::no_buffer_space));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              groups <= limits.maxComputeDispatchThreadGroups[0] and
                                  limits.maxComputeThreadsPerGroup >= 64u and
                                  limits.maxComputeThreadGroupSize[0] >= 64u,
                              std::errc::function_not_supported));

        GpuTraceParameters parameters{};
        parameters.m_hierarchy_levels = static_cast<u32>(hierarchy_plan->m_levels.size());
        parameters.m_width = grid.m_width;
        parameters.m_height = grid.m_height;
        parameters.m_ray_count = static_cast<u32>(rays.size());
        parameters.m_boundary = grid.m_boundary == OpticalBoundary::opaque ? 1u : 0u;
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(grid.m_cell_size_m, parameters.m_cell_metres));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                parameters.m_cell_metres > 0.0f and grid.m_width <= (1u << 24u) and grid.m_height <= (1u << 24u),
                std::errc::result_out_of_range));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                terrain != nullptr or normalOrZero_(parameters.m_cell_metres), std::errc::argument_out_of_domain));

        const auto cells = packCells_(grid, terrain == nullptr);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cells ? std::error_code{} : cells.error());

        std::vector<GpuOpticalRay> packed_rays(rays.size());
        for (std::size_t index = 0u; index < rays.size(); ++index) {
            const auto normalized = normalizeOpticalRay(rays[index]);
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, normalized ? std::error_code{} : normalized.error());
            auto &destination = packed_rays[index];
            for (std::size_t axis = 0u; axis < 2u; ++axis) {
                const double local_cells = (normalized->m_origin_m[axis] - grid.m_min_m[axis]) / grid.m_cell_size_m;
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(local_cells, destination.m_origin[axis]));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, toFloat_(normalized->m_direction[axis], destination.m_direction[axis]));
            }

            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(normalized->m_near_m, destination.m_near_m));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(normalized->m_far_m, destination.m_far_m));

            // Inspect uploaded bits before any GPU empty/axis identity. A
            // nonzero FP32 subnormal is unavailable to this arithmetic domain.
            for (const float component: std::bit_cast<std::array<float, 6>>(destination)) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(normalOrZero_(component), std::errc::argument_out_of_domain));
            }

            const float near_cells = destination.m_near_m / parameters.m_cell_metres;
            const float far_cells = destination.m_far_m / parameters.m_cell_metres;
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  std::isfinite(near_cells) and
                                      std::isfinite(far_cells) and
                                      (normalized->m_near_m == normalized->m_far_m or
                                          (destination.m_far_m > destination.m_near_m and far_cells > near_cells)),
                                  std::errc::result_out_of_range));

            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, validateTraceEvents_(destination, parameters));
        }

        // Compile only the selected terrain entry after admission/input checks.
        // A failed candidate leaves the prior prepared batch and pipeline intact.
        if (terrain != nullptr and not state.m_terrain_pipelines[0]) {
            const hal::ProfileScope pipeline_scope{"App.Lighting.CreateTerrainTracePipeline"};
            rhi::ComPtr<rhi::IComputePipeline> candidate{};
            rhi::ComputePipelineDesc description{};
            description.program = state.m_terrain_programs[0];
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, state.m_device->createComputePipeline(description, candidate.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(candidate != nullptr));

            state.m_terrain_pipelines[0] = std::move(candidate);
        }

        // Candidate resources have never been submitted and roll back through
        // owning values on every failure. The prior prepared batch stays intact.
        rhi::ComPtr<rhi::IBuffer> gpu_cells, gpu_rays, gpu_output, gpu_readback, gpu_segments;
        auto precision_storage = refined ? createTerrainPrecision_(*state.m_device, rays.size())
                                         : std::expected<std::unique_ptr<TerrainPrecisionStorage>, std::error_code>{
                                               nullptr};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, precision_storage ? std::error_code{} : precision_storage.error());

        rhi::ComPtr<rhi::IShaderObject> root{};
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceBuffer_(*state.m_device, cell_bytes, sizeof(GpuOpticalCell), cells->data(), false, gpu_cells));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceBuffer_(*state.m_device, ray_bytes, sizeof(GpuOpticalRay), packed_rays.data(), false, gpu_rays));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, result_bytes, stride, nullptr, false, gpu_output));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, readback_bytes, 0u, nullptr, true, gpu_readback));
        if (packed) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceBuffer_(*state.m_device, segment_bytes, sizeof(u32), nullptr, false, gpu_segments));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, state.m_device->createRootShaderObject(
                              terrain  ? state.m_terrain_programs[0]
                              : packed ? state.m_packed_program
                                       : state.m_program,
                              root.writeRef()));

        auto hierarchy = terrain
                             ? std::expected<GpuHierarchy, std::error_code>{GpuHierarchy{}}
                             : createHierarchy_(*state.m_device, state.m_hierarchy_program, gpu_cells, *hierarchy_plan);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, hierarchy ? std::error_code{} : hierarchy.error());

        auto terrain_storage = terrain ? createTerrain_(*state.m_device, *terrain_plan)
                                       : std::expected<TerrainStorage, std::error_code>{TerrainStorage{}};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, terrain_storage ? std::error_code{} : terrain_storage.error());

        rhi::ShaderCursor cursor{root};
        rhi::ShaderCursor constants{};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_trace"].getDereferenced(constants));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(&parameters, sizeof(parameters)));
        for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 3>{
                 std::pair{"g_cells", gpu_cells.get()}, {"g_rays", gpu_rays.get()},
                 {packed ? "g_packed_results" : "g_results", gpu_output.get()}}) {
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
        }

        if (terrain) {
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrain_(cursor, *terrain_storage, *terrain_plan));
        } else {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                cursor["g_flags"].setBinding(rhi::Binding(hierarchy->m_flags, traceRange_(hierarchy->m_flags))));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, cursor["g_hierarchy"].setBinding(
                                  rhi::Binding(hierarchy->m_metadata, traceRange_(hierarchy->m_metadata))));
        }

        if (packed) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, cursor["g_segments"].setBinding(rhi::Binding(gpu_segments, traceRange_(gpu_segments))));
        }

        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrainPrecision_(cursor, precision_storage->get()));

        state.m_precision_storage = std::move(*precision_storage);
        state.m_terrain = std::move(*terrain_storage);
        state.m_terrain_mode = terrain != nullptr;
        state.m_segments = std::move(gpu_segments);
        state.m_storage = storage;
        state.m_hierarchy = std::move(*hierarchy);
        state.m_cells = std::move(gpu_cells);
        state.m_rays = std::move(gpu_rays);
        state.m_output = std::move(gpu_output);
        state.m_readback = std::move(gpu_readback);
        state.m_root = std::move(root);
        state.m_parameters = parameters;
        state.m_buffer_bytes = candidate_bytes;
        state.m_largest_buffer_bytes = largest;
        state.m_levels.clear();
        state.m_parity = {};
        state.m_directions.setNull();
        state.m_diagnostic_count = 0u;
        state.m_cascade_diagnostic.reset();
        state.m_receiver.reset();
        state.m_receiver_root.setNull();
        state.m_background_mean_root.setNull();
        state.m_receiver_published = false;
        state.m_cascade_mode = false;
        state.m_last_use = kFenceValueNone;
        state.m_background_batch.reset();
        state.m_prepared = true;
        state.m_submitted = false;
        return {};
    }

    // ------------------------------------------------------------------
    // fixed-parity FP32 cascade admission
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::prepareCascades_(
        const OpticalGridView &grid, const RcEstimateRequest &request, const FenceCompletionValue completed,
        const GpuIntervalStorage storage, const OpticalTerrainSnapshot *const terrain,
        std::shared_ptr<const TerrainReceiverSet> receivers, const TerrainReceiverCohort cohort,
        const bool qualification_readback, const TerrainReceiverOutput output,
        const TerrainBackgroundOutput background) {
        const hal::ProfileScope scope{"App.Lighting.PrepareCascades"};
        PPR_LOG(
            RadiancePass, info, "RC-PREP enter checked input/planning/admission",
            {{"width", grid.m_width}, {"height", grid.m_height}, {"cascades", request.m_cascade_count}});
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(storage == GpuIntervalStorage::fp32 or storage == GpuIntervalStorage::rgb16f));
        const bool packed = storage == GpuIntervalStorage::rgb16f;
        const u32 stride = intervalStride_(storage);
        const bool angular = output == TerrainReceiverOutput::angular16;
        const bool backwall = background == TerrainBackgroundOutput::c0_mean;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(background == TerrainBackgroundOutput::disabled or backwall));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(not backwall or (receivers and grid.m_width >= 2u and grid.m_height >= 2u)));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(output == TerrainReceiverOutput::mean_only or angular));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(not angular or bool(receivers)));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(not qualification_readback or (terrain != nullptr and receivers and not packed)));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not state.m_encoded and not state.m_unknown and slotFenceComplete(completed, state.m_last_use),
                std::errc::device_or_resource_busy));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(grid.m_cells.size() <= kMaxCompleteFixtureCells, std::errc::function_not_supported));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, validateOpticalGrid(grid));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              request.m_width == grid.m_width and
                                  request.m_height == grid.m_height and
                                  request.m_interval_bytes == stride and
                                  request.m_optical_cell_bytes == sizeof(GpuOpticalCell) and
                                  request.m_scalar_cell_bytes == 0u and
                                  request.m_live_generations == 1u and
                                  request.m_retained_c1_generations == 0u and
                                  request.m_c0_tile_width >= grid.m_width and
                                  request.m_c0_tile_height >= grid.m_height,
                              std::errc::function_not_supported));

        u32 minimum_cascades = 1u;
        u64 minimum_outer = 1u;
        const double diagonal = std::hypot(double(grid.m_width), double(grid.m_height));

        while (double(minimum_outer) < diagonal) {
            minimum_outer *= 2u;
            ++minimum_cascades;
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(request.m_cascade_count == minimum_cascades, std::errc::function_not_supported));

        const auto layout = estimateRcResources(request);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, layout ? std::error_code{} : layout.error());
        const auto &limits = state.m_device->getInfo().limits;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              limits.maxComputeThreadsPerGroup >= 64u and
                                  limits.maxComputeThreadGroupSize[0] >= 64u and
                                  limits.maxComputeDispatchThreadGroups[0] > 0u and
                                  limits.maxComputeDispatchThreadGroups[1] > 0u,
                              std::errc::function_not_supported));

        const auto hierarchy_plan = terrain ? std::expected<HierarchyPlan, std::error_code>{HierarchyPlan{}}
                                            : hierarchyPlan_(*state.m_device, grid.m_width, grid.m_height);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, hierarchy_plan ? std::error_code{} : hierarchy_plan.error());

        const auto terrain_plan = terrain ? terrainPlan_(*terrain)
                                          : std::expected<TerrainPlan, std::error_code>{TerrainPlan{}};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, terrain_plan ? std::error_code{} : terrain_plan.error());

        u64 direction_count{};
        for (const auto &cascade: layout->m_cascades) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    cascade.m_directions <= std::numeric_limits<u32>::max() and
                        cascade.m_resident_records <= std::numeric_limits<u32>::max() and
                        double(float(double(cascade.m_spacing_cells) + 0.5)) == double(cascade.m_spacing_cells) + 0.5,
                    std::errc::result_out_of_range));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(direction_count, cascade.m_directions, direction_count));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(direction_count <= std::numeric_limits<u32>::max(), std::errc::value_too_large));
        const u64 c0_records = layout->m_cascades[0].m_resident_records;
        u64 direction_bytes{};
        u64 readback_bytes{};
        u64 segment_bytes{};
        u64 cell_bytes{};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(direction_count, sizeof(GpuDirection), direction_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceMultiply_(c0_records + 1u, stride + (packed ? sizeof(u32) : 0u), readback_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceMultiply_(c0_records + 1u, packed ? sizeof(u32) : 0u, segment_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(grid.m_cells.size(), sizeof(GpuOpticalCell), cell_bytes));

        std::array<u64, 6> receiver_bytes{};
        u64 angular_validation_bytes{};
        u64 background_mean_bytes{};
        GpuReceiverParameters receiver_parameters{};
        if (receivers) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  terrain != nullptr and
                                  receivers->geometry().get() == terrain and
                                  not receivers->features().empty() and
                                  layout->m_cascades.size() >= 2u));
            const auto &parent = layout->m_cascades[1];
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(parent.m_directions == 16u));
            receiver_parameters.m_count = static_cast<u32>(receivers->features().size());
            receiver_parameters.m_parent_width = static_cast<u32>(parent.m_padded_probes[0]);
            receiver_parameters.m_parent_height = static_cast<u32>(parent.m_padded_probes[1]);
            receiver_parameters.m_parent_direction_offset = 8u;
            receiver_parameters.m_parent_spacing = static_cast<float>(parent.m_spacing_cells);
            receiver_parameters.m_parent_near_cells = static_cast<float>(parent.m_near_cells);
            receiver_parameters.m_angular_output = angular ? 1u : 0u;
            const u64 groups = (u64{receiver_parameters.m_count} + 63u) / 64u;
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(groups <= limits.maxComputeDispatchThreadGroups[0], std::errc::function_not_supported));

            for (u32 index = 0u; index < receiver_parameters.m_count; ++index) {
                const auto point = receivers->point(index);
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, point ? std::error_code{} : point.error());
                for (u32 axis = 0u; axis < 2u; ++axis) {
                    const double lattice = ((*point)[axis] - 0.5) / double(parent.m_spacing_cells);
                    PPR_RETURN_ERROR_ON_FAIL(
                        RadiancePass, requireTrace_(
                                          lattice >= 0.0 and std::ceil(lattice) < double(parent.m_padded_probes[axis]),
                                          std::errc::function_not_supported));
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceMultiply_(receivers->features().size(), sizeof(TerrainReceiverFeature), receiver_bytes[0]));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceMultiply_(receivers->features().size(), sizeof(TerrainReceiverResult), receiver_bytes[1]));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceMultiply_(receivers->cells().size(), sizeof(TerrainReceiverCellRange), receiver_bytes[2]));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceMultiply_(
                    receivers->components().size(), sizeof(TerrainReceiverComponentRange), receiver_bytes[3]));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceMultiply_(receivers->candidates().size(), sizeof(u32), receiver_bytes[4]));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(std::ranges::all_of(std::span{receiver_bytes}.first<5>(), [](const u64 bytes) {
                    return bytes != 0u;
                })));
            readback_bytes = receiver_bytes[1];
            if (angular) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(receiver_bytes[1], 16u, receiver_bytes[5]));
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(readback_bytes, receiver_bytes[5], readback_bytes));
                // A mapped validation scratch record and optional returned CPU
                // snapshot coexist with mean rows; reserve that peak explicitly.
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(receiver_bytes[5], receiver_bytes[1], angular_validation_bytes));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(angular_validation_bytes, sizeof(TerrainReceiverAngular), angular_validation_bytes));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(
                        angular_validation_bytes,
                        sizeof(std::vector<TerrainReceiverAngular>) + sizeof(std::vector<TerrainReceiverResult>),
                        angular_validation_bytes));
            }
        }

        if (backwall) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceMultiply_(grid.m_cells.size(), sizeof(TerrainReceiverResult), background_mean_bytes));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(readback_bytes, background_mean_bytes, readback_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceAdd_(angular_validation_bytes, background_mean_bytes, angular_validation_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceAdd_(
                                  angular_validation_bytes,
                                  sizeof(TerrainBackgroundSource) + sizeof(std::vector<TerrainReceiverResult>),
                                  angular_validation_bytes));
        }

        // Diagnostic copies retain all executed levels, including failed rows.
        // Charge the exact-size CPU payload and owner as well as the GPU copy
        // target before allocating either. No diagnostic data is shader-bound.
        CascadeDiagnosticHeader diagnostic_header{};
        u64 diagnostic_input_bytes{}, diagnostic_readback_bytes{}, diagnostic_retained_bytes{};
        if (qualification_readback) {
            diagnostic_input_bytes = sizeof(CascadeDiagnosticHeader);
            const auto add_section = [&](const CascadeDiagnosticSection section, const u64 count,
                                         const u32 element_bytes) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(count <= std::numeric_limits<u32>::max(), std::errc::value_too_large));

                auto &range = diagnostic_header.m_sections[static_cast<std::size_t>(section)];
                range.m_offset = diagnostic_input_bytes;
                range.m_count = static_cast<u32>(count);
                range.m_stride = element_bytes;
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(count, element_bytes, range.m_bytes));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(diagnostic_input_bytes, range.m_bytes, diagnostic_input_bytes));
                return {};
            };

            const auto terrain_view = terrain->view();
            for (const auto &[section, count, element_bytes]:
                std::array<std::tuple<CascadeDiagnosticSection, u64, u32>, 14>{
                    std::tuple<CascadeDiagnosticSection, u64, u32>{
                        CascadeDiagnosticSection::trace, 1u, sizeof(GpuTraceParameters)},
                    {CascadeDiagnosticSection::terrain, 1u, sizeof(GpuTerrainParameters)},
                    {CascadeDiagnosticSection::receiver, 1u, sizeof(GpuReceiverParameters)},
                    {CascadeDiagnosticSection::cohort, 1u, sizeof(TerrainReceiverCohort)},
                    {CascadeDiagnosticSection::geometry, 1u, sizeof(CascadeDiagnosticGeometry)},
                    {CascadeDiagnosticSection::levels, layout->m_cascades.size(), sizeof(CascadeDiagnosticLevel)},
                    {CascadeDiagnosticSection::directions, direction_count, sizeof(GpuDirection)},
                    {CascadeDiagnosticSection::cells, grid.m_cells.size(), sizeof(GpuOpticalCell)},
                    {CascadeDiagnosticSection::vertices, terrain_plan->m_vertices.size(), sizeof(GpuTerrainVertex)},
                    {CascadeDiagnosticSection::points, math::contourPoints().size(), sizeof(math::ContourPoint)},
                    {CascadeDiagnosticSection::cases, math::contourCases().size(), sizeof(math::ContourCase)},
                    {CascadeDiagnosticSection::features, receivers->features().size(), sizeof(TerrainReceiverFeature)},
                    {CascadeDiagnosticSection::labels, terrain_view.m_labels.size(), sizeof(u16)},
                    {CascadeDiagnosticSection::materials, terrain_view.m_materials.size(),
                        sizeof(CascadeDiagnosticMaterial)}}) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, add_section(section, count, element_bytes));
            }

            for (std::size_t index = backwall ? 0u : 1u; index < layout->m_cascades.size(); ++index) {
                u64 bytes{};
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceMultiply_(layout->m_cascades[index].m_resident_records, sizeof(GpuOpticalResult), bytes));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(diagnostic_readback_bytes, bytes, diagnostic_readback_bytes));
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceAdd_(diagnostic_input_bytes, sizeof(CascadeDiagnosticCapture), diagnostic_retained_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceAdd_(diagnostic_retained_bytes, sizeof(state.m_cascade_diagnostic), diagnostic_retained_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceAdd_(diagnostic_retained_bytes, diagnostic_readback_bytes, diagnostic_retained_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    diagnostic_input_bytes <= std::numeric_limits<std::size_t>::max(), std::errc::value_too_large));
        }

        const bool refined = terrain != nullptr and
                             state.m_precision == TerrainPrecision::qualified_fp64_refinement_experimental;
        u64 precision_query_bound{};
        u64 precision_retained_bytes{};
        if (refined) {
            for (std::size_t index = receivers and not backwall ? 1u : 0u; index < layout->m_cascades.size(); ++index) {
                const bool top = index + 1u == layout->m_cascades.size();
                u64 queries{};
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceMultiply_(layout->m_cascades[index].m_resident_records, top ? 1u : 8u, queries));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(precision_query_bound, queries, precision_query_bound));
            }

            if (receivers) {
                u64 receiver_queries{};
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceMultiply_(receivers->features().size(), 64u, receiver_queries));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(precision_query_bound, receiver_queries, precision_query_bound));
            }

            // Each of the four atomic counters increments <= once/query.
            // This complete-generation bound prevents wrap in every dispatch.
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                requireTrace_(
                    precision_query_bound > 0u and precision_query_bound <= std::numeric_limits<u32>::max(),
                    std::errc::value_too_large));
            precision_retained_bytes = kTerrainPrecisionRetainedBytes + (receivers ? kTerrainPrecisionLeaseBytes : 0u);
        }

        RcEstimateRequest admission = request;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, precision_retained_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, diagnostic_retained_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, segment_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, hierarchy_plan->m_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, terrain_plan->m_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, direction_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, readback_bytes, admission.m_additional_peak_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, state.m_buffer_bytes, admission.m_additional_peak_bytes));
        std::erase_if(state.m_receiver_generations, [](const auto &generation) { return generation.expired(); });
        for (const auto &weak: state.m_receiver_generations) {
            if (const auto generation = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceAdd_(
                                      admission.m_additional_peak_bytes, generation->bufferBytes(),
                                      admission.m_additional_peak_bytes));
                admission.m_additional_largest_resource_bytes = std::max(
                    admission.m_additional_largest_resource_bytes, generation->largestBufferBytes());
            }
        }

        std::erase_if(state.m_background_views, [](const auto &view) { return view.expired(); });
        for (const auto &weak: state.m_background_views) {
            if (const auto view = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(
                        admission.m_additional_peak_bytes, view->bufferBytes(), admission.m_additional_peak_bytes));
                admission.m_additional_largest_resource_bytes = std::max(
                    admission.m_additional_largest_resource_bytes, view->largestBufferBytes());
            }
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, angular_validation_bytes, admission.m_additional_peak_bytes));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceAdd_(admission.m_additional_peak_bytes, background_mean_bytes, admission.m_additional_peak_bytes));
        admission.m_additional_largest_resource_bytes = std::max(
            admission.m_additional_largest_resource_bytes, background_mean_bytes);

        for (const u64 bytes: receiver_bytes) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceAdd_(admission.m_additional_peak_bytes, bytes, admission.m_additional_peak_bytes));
            admission.m_additional_largest_resource_bytes = std::max(
                admission.m_additional_largest_resource_bytes, bytes);
        }

        if (receivers) {
            // Conservatively reserve another copy of every initialized input
            // for backend upload staging; this is transient, not lease storage.
            u64 upload_bytes{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(cell_bytes, direction_bytes, upload_bytes));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(upload_bytes, terrain_plan->m_bytes, upload_bytes));
            for (const std::size_t index: {0u, 2u, 3u, 4u}) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(upload_bytes, receiver_bytes[index], upload_bytes));
            }
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceAdd_(admission.m_additional_peak_bytes, upload_bytes, admission.m_additional_peak_bytes));
        }

        admission.m_additional_largest_resource_bytes = std::max(
            {admission.m_additional_largest_resource_bytes, direction_bytes, readback_bytes, segment_bytes,
                hierarchy_plan->m_flag_bytes, hierarchy_plan->m_metadata_bytes, terrain_plan->m_largest,
                state.m_largest_buffer_bytes, diagnostic_input_bytes, diagnostic_readback_bytes,
                refined ? kTerrainPrecisionCounterBytes : 0u});
        admission.m_max_resource_bytes = std::min(admission.m_max_resource_bytes, limits.maxBufferSize);
        const auto admitted = estimateRcResources(admission);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, admitted ? std::error_code{} : admitted.error());
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, admitted->m_admission_error);
        PPR_LOG(
            RadiancePass, info, "RC-PREP resource admission finished",
            {{"cell_bytes", cell_bytes}, {"direction_bytes", direction_bytes}, {"readback_bytes", readback_bytes},
                {"parity0_bytes", admitted->m_parity_buffer_bytes[0]},
                {"parity1_bytes", admitted->m_parity_buffer_bytes[1]}});

        const auto cells = packCells_(grid, terrain == nullptr);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cells ? std::error_code{} : cells.error());
        if (receivers) {
            // Interim base8/doubling domain: each C1 merge term is at least1/8;
            // midpoint fraction guards enforce [1/8,7/8], so the final receiver
            // measure is at least1/1024 in the actual floating evaluation. Each
            // positive source must survive every selected stage as normal FP32.
            const int exponent = 10 + 3 * (static_cast<int>(request.m_cascade_count) - 2);
            const float source_floor = std::ldexp(std::numeric_limits<float>::min(), exponent);
            const auto admitted_source = [source_floor](const float value) {
                return value == 0.0f or (normalOrZero_(value) and value >= source_floor);
            };

            for (const auto &cell: *cells) {
                for (const float channel: cell.m_surface_emission) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        RadiancePass, requireTrace_(admitted_source(channel), std::errc::argument_out_of_domain));
                }
            }
            for (const float channel:
                {terrain_plan->m_parameters.m_outside_r, terrain_plan->m_parameters.m_outside_g,
                    terrain_plan->m_parameters.m_outside_b}) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(admitted_source(channel), std::errc::argument_out_of_domain));
            }
        }

        std::vector<GpuDirection> directions{};
        directions.reserve(static_cast<std::size_t>(direction_count));
        std::vector<u32> direction_offsets{};
        for (const auto &cascade: layout->m_cascades) {
            direction_offsets.push_back(static_cast<u32>(directions.size()));

            for (u64 bin = 0u; bin < cascade.m_directions; ++bin) {
                const double angle = 2.0 * std::numbers::pi * (double(bin) + 0.5) / double(cascade.m_directions);
                directions.push_back({static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))});
                if (terrain != nullptr) {
                    const auto &direction = directions.back();
                    PPR_RETURN_ERROR_ON_FAIL(
                        RadiancePass, requireTrace_(
                                          normalOrZero_(direction.m_x) and
                                              normalOrZero_(direction.m_y) and
                                              std::max(std::abs(direction.m_x), std::abs(direction.m_y)) >= 0.5f,
                                          std::errc::argument_out_of_domain));
                }
            }
        }

        GpuTraceParameters trace{};
        trace.m_hierarchy_levels = static_cast<u32>(hierarchy_plan->m_levels.size());
        trace.m_width = grid.m_width;
        trace.m_height = grid.m_height;
        trace.m_ray_count = receivers ? receiver_parameters.m_count : static_cast<u32>(c0_records);
        trace.m_boundary = grid.m_boundary == OpticalBoundary::opaque ? 1u : 0u;
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(grid.m_cell_size_m, trace.m_cell_metres));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(trace.m_cell_metres > 0.0f));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(terrain != nullptr or normalOrZero_(trace.m_cell_metres), std::errc::argument_out_of_domain));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              std::isfinite(static_cast<float>(admitted->m_terminal_far_cells) * trace.m_cell_metres),
                              std::errc::result_out_of_range));

        // The estimator bounds every top padded probe against every world
        // corner by an integer L1 distance. Uploaded raw directions have length
        // at least 1/2, so twice that bound is boundary-complete in lambda units.
        float terrain_terminal_lambda{};
        if (terrain != nullptr) {
            const double terminal_lambda = double(admitted->m_terminal_far_cells) * 2.0;
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(terminal_lambda, terrain_terminal_lambda));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  normalOrZero_(terrain_terminal_lambda) and
                                      terrain_terminal_lambda > 0.0f and
                                      double(terrain_terminal_lambda) >= terminal_lambda,
                                  std::errc::result_out_of_range));
        }

        std::vector<State::CascadeLevel> levels{};
        for (std::size_t index = 0u; index < layout->m_cascades.size(); ++index) {
            const auto &cascade = layout->m_cascades[index];
            const u64 group_count = cascade.m_resident_records / 64u +
                                    (cascade.m_resident_records % 64u == 0u ? 0u : 1u);
            const u64 groups_x = std::min<u64>(
                {group_count, limits.maxComputeDispatchThreadGroups[0], std::numeric_limits<u32>::max() / 64u});
            const u64 groups_y = group_count / groups_x + (group_count % groups_x == 0u ? 0u : 1u);
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, requireTrace_(
                                  groups_y <= limits.maxComputeDispatchThreadGroups[1] and
                                      groups_x * groups_y <= std::numeric_limits<u32>::max() / 64u,
                                  std::errc::function_not_supported));

            State::CascadeLevel level{};
            level.m_groups_x = static_cast<u32>(groups_x);
            level.m_groups_y = static_cast<u32>(groups_y);
            level.m_records = static_cast<u32>(cascade.m_resident_records);
            level.m_level = static_cast<u32>(index);
            levels.push_back(std::move(level));
        }

        PPR_LOG(
            RadiancePass, info, "RC-PREP checked input/planning finished",
            {{"levels", levels.size()}, {"directions", directions.size()}, {"receivers", receiver_parameters.m_count}});

        // Keep completed family members for reuse or retry. Pipeline creation
        // never replaces the prior prepared batch, even if the second entry fails.
        if (terrain != nullptr and (not state.m_terrain_pipelines[1] or
                                       not state.m_terrain_pipelines[2] or
                                       (receivers and not state.m_terrain_pipelines[3]))) {
            const hal::ProfileScope pipeline_scope{"App.Lighting.CreateTerrainCascadePipelines"};
            constexpr std::array<const char *, 4> entry_names{
                "traceTerrain", "cascadeTerrainTop", "cascadeTerrainMerge", "terrainReceivers"};
            for (std::size_t index = 1u; index < (receivers ? 4u : 3u); ++index) {
                if (state.m_terrain_pipelines[index]) {
                    continue;
                }

                rhi::ComPtr<rhi::IComputePipeline> candidate{};
                rhi::ComputePipelineDesc description{};
                description.program = state.m_terrain_programs[index];
                PPR_LOG(
                    RadiancePass, info, "RC-PREP before lazy createComputePipeline",
                    {{"entry", std::string_view{entry_names[index]}}});
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, state.m_device->createComputePipeline(description, candidate.writeRef()));
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(candidate != nullptr));
                PPR_LOG(
                    RadiancePass, info, "RC-PREP after lazy createComputePipeline",
                    {{"entry", std::string_view{entry_names[index]}}});

                state.m_terrain_pipelines[index] = std::move(candidate);
            }
        }

        PPR_LOG(RadiancePass, info, "RC-PREP before main GPU allocation group");
        auto precision_storage = refined ? createTerrainPrecision_(*state.m_device, precision_query_bound)
                                         : std::expected<std::unique_ptr<TerrainPrecisionStorage>, std::error_code>{
                                               nullptr};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, precision_storage ? std::error_code{} : precision_storage.error());

        std::array<rhi::ComPtr<rhi::IBuffer>, 2> parity{};
        rhi::ComPtr<rhi::IBuffer> gpu_cells, gpu_directions, readback, gpu_segments;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceBuffer_(*state.m_device, cell_bytes, sizeof(GpuOpticalCell), cells->data(), false, gpu_cells));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            traceBuffer_(
                *state.m_device, direction_bytes, sizeof(GpuDirection), directions.data(), false, gpu_directions));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, readback_bytes, 0u, nullptr, true, readback));
        if (packed) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, traceBuffer_(*state.m_device, segment_bytes, sizeof(u32), nullptr, false, gpu_segments));
        }

        for (std::size_t index = 0u; index < parity.size(); ++index) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceBuffer_(
                    *state.m_device, admitted->m_parity_buffer_bytes[index], stride, nullptr, false, parity[index]));
        }

        auto hierarchy = terrain
                             ? std::expected<GpuHierarchy, std::error_code>{GpuHierarchy{}}
                             : createHierarchy_(*state.m_device, state.m_hierarchy_program, gpu_cells, *hierarchy_plan);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, hierarchy ? std::error_code{} : hierarchy.error());

        auto terrain_storage = terrain ? createTerrain_(*state.m_device, *terrain_plan)
                                       : std::expected<TerrainStorage, std::error_code>{TerrainStorage{}};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, terrain_storage ? std::error_code{} : terrain_storage.error());
        PPR_LOG(RadiancePass, info, "RC-PREP after main GPU allocation group");

        std::unique_ptr<CascadeDiagnosticCapture> diagnostic{};
        if (qualification_readback) {
            diagnostic = std::make_unique<CascadeDiagnosticCapture>();
            diagnostic->m_header = diagnostic_header;
            diagnostic->m_input_bytes = diagnostic_input_bytes;
            diagnostic->m_readback_bytes = diagnostic_readback_bytes;
            diagnostic->m_retained_bytes = diagnostic_retained_bytes;
            diagnostic->m_inputs = std::make_unique<std::byte[]>(static_cast<std::size_t>(diagnostic_input_bytes));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                traceBuffer_(*state.m_device, diagnostic_readback_bytes, 0u, nullptr, true, diagnostic->m_readback));

            std::memcpy(diagnostic->m_inputs.get(), &diagnostic_header, sizeof(diagnostic_header));
            const auto copy_section = [&](const CascadeDiagnosticSection section, const void *const data) {
                const auto &range = diagnostic_header.m_sections[static_cast<std::size_t>(section)];
                std::memcpy(diagnostic->m_inputs.get() + range.m_offset, data, static_cast<std::size_t>(range.m_bytes));
            };

            const auto terrain_view = terrain->view();
            CascadeDiagnosticGeometry geometry{};
            geometry.m_min_bits[0] = std::bit_cast<u64>(terrain_view.m_min_m[0]);
            geometry.m_min_bits[1] = std::bit_cast<u64>(terrain_view.m_min_m[1]);
            geometry.m_cell_metres_bits = std::bit_cast<u64>(terrain_view.m_cell_size_m);
            geometry.m_width = terrain_view.m_width;
            geometry.m_height = terrain_view.m_height;
            geometry.m_outside_material = terrain_view.m_outside_material;
            geometry.m_boundary = trace.m_boundary;
            copy_section(CascadeDiagnosticSection::geometry, &geometry);
            copy_section(CascadeDiagnosticSection::trace, &trace);
            copy_section(CascadeDiagnosticSection::terrain, &terrain_plan->m_parameters);
            copy_section(CascadeDiagnosticSection::receiver, &receiver_parameters);
            copy_section(CascadeDiagnosticSection::cohort, &cohort);
            copy_section(CascadeDiagnosticSection::directions, directions.data());
            copy_section(CascadeDiagnosticSection::cells, cells->data());
            copy_section(CascadeDiagnosticSection::vertices, terrain_plan->m_vertices.data());
            copy_section(CascadeDiagnosticSection::points, math::contourPoints().data());
            copy_section(CascadeDiagnosticSection::cases, math::contourCases().data());
            copy_section(CascadeDiagnosticSection::features, receivers->features().data());
            copy_section(CascadeDiagnosticSection::labels, terrain_view.m_labels.data());

            const auto &palette_range =
                diagnostic_header.m_sections[static_cast<std::size_t>(CascadeDiagnosticSection::materials)];
            for (std::size_t index = 0u; index < terrain_view.m_materials.size(); ++index) {
                const auto &authored = terrain_view.m_materials[index];
                CascadeDiagnosticMaterial material{};
                for (std::size_t channel = 0u; channel < 3u; ++channel) {
                    material.m_extinction_bits[channel] = std::bit_cast<u64>(authored.m_optics.m_extinction[channel]);
                    material.m_volume_bits[channel] = std::bit_cast<u64>(authored.m_optics.m_volume_emission[channel]);
                    material.m_surface_bits[channel] = std::bit_cast<u64>(
                        authored.m_optics.m_surface_emission[channel]);
                }

                material.m_roundness_bits = std::bit_cast<u32>(authored.m_roundness);
                material.m_material = authored.m_material;
                material.m_kind = authored.m_optics.m_kind == OpticalKind::vacuum   ? 0u
                                  : authored.m_optics.m_kind == OpticalKind::opaque ? 1u
                                                                                    : 2u;
                std::memcpy(
                    diagnostic->m_inputs.get() + palette_range.m_offset + index * sizeof(material), &material,
                    sizeof(material));
            }
        }

        if (backwall and not state.m_terrain_pipelines[4]) {
            rhi::ComputePipelineDesc description{};
            description.program = state.m_terrain_programs[4];
            description.label = "terrain C0 mean reduction";
            rhi::ComPtr<rhi::IComputePipeline> pipeline{};
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, state.m_device->createComputePipeline(description, pipeline.writeRef()));
            state.m_terrain_pipelines[4] = std::move(pipeline);
        }

        std::shared_ptr<TerrainReceiverGpuGeneration> receiver_generation{};
        rhi::ComPtr<rhi::IShaderObject> receiver_root{}, background_mean_root{};
        if (receivers) {
            PPR_LOG(
                RadiancePass, info, "RC-PREP before receiver GPU allocation group",
                {{"features", receiver_parameters.m_count}, {"result_bytes", receiver_bytes[1]}});
            receiver_generation = std::make_shared<TerrainReceiverGpuGeneration>();
            receiver_generation->m_receivers = receivers;
            receiver_generation->m_device = state.m_device;
            receiver_generation->m_cohort = cohort;
            receiver_generation->m_angular_direction_offset = angular ? receiver_parameters.m_parent_direction_offset
                                                                      : 0u;
            receiver_generation->m_angular_direction_count = angular ? 16u : 0u;
            if (refined) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(receiver_generation->m_bytes, kTerrainPrecisionLeaseBytes, receiver_generation->m_bytes));
            }
            const std::array<const void *, 5> data{
                receivers->features().data(), nullptr, receivers->cells().data(), receivers->components().data(),
                receivers->candidates().data()};
            const std::array<u32, 5> strides{
                sizeof(TerrainReceiverFeature), sizeof(TerrainReceiverResult), sizeof(TerrainReceiverCellRange),
                sizeof(TerrainReceiverComponentRange), sizeof(u32)};
            const std::array<rhi::ComPtr<rhi::IBuffer> *, 5> buffers{
                std::addressof(receiver_generation->m_features), std::addressof(receiver_generation->m_results),
                std::addressof(receiver_generation->m_cells), std::addressof(receiver_generation->m_components),
                std::addressof(receiver_generation->m_candidates)};
            for (std::size_t index = 0u; index < buffers.size(); ++index) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceBuffer_(
                        *state.m_device, receiver_bytes[index], strides[index], data[index], false, *buffers[index]));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(receiver_generation->m_bytes, receiver_bytes[index], receiver_generation->m_bytes));
                receiver_generation->m_largest = std::max(receiver_generation->m_largest, receiver_bytes[index]);
            }

            if (angular) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceBuffer_(
                                      *state.m_device, receiver_bytes[5], sizeof(TerrainReceiverAngular), nullptr,
                                      false, receiver_generation->m_angular));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(receiver_generation->m_bytes, receiver_bytes[5], receiver_generation->m_bytes));
                receiver_generation->m_largest = std::max(receiver_generation->m_largest, receiver_bytes[5]);
            }

            if (backwall) {
                receiver_generation->m_background = std::make_shared<TerrainBackgroundSource>();
                auto &source = *receiver_generation->m_background;
                source.m_trace = trace;
                source.m_parameters = terrain_plan->m_parameters;
                source.m_precision = state.m_precision;
                source.m_width = grid.m_width;
                source.m_height = grid.m_height;
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, traceBuffer_(
                                      *state.m_device, background_mean_bytes, sizeof(TerrainReceiverResult), nullptr,
                                      false, source.m_means));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    traceAdd_(
                        receiver_generation->m_bytes, background_mean_bytes + sizeof(TerrainBackgroundSource),
                        receiver_generation->m_bytes));
                receiver_generation->m_largest = std::max(receiver_generation->m_largest, background_mean_bytes);

                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, state.m_device->createRootShaderObject(
                                      state.m_terrain_programs[4], background_mean_root.writeRef()));
                rhi::ShaderCursor background_cursor{background_mean_root};
                rhi::ShaderCursor background_constants{};
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, background_cursor["g_trace"].getDereferenced(background_constants));
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, background_constants.setData(&trace, sizeof(trace)));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    background_cursor["g_upper"].setBinding(rhi::Binding(parity[0], traceRange_(parity[0]))));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, background_cursor["g_background_means"].setBinding(
                                      rhi::Binding(source.m_means, traceRange_(source.m_means))));
            }

            PPR_LOG(
                RadiancePass, info, "RC-PREP after receiver GPU allocation group",
                {{"bytes", receiver_generation->m_bytes}});

            PPR_LOG(RadiancePass, info, "RC-PREP before createRootShaderObject terrainReceivers");
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                state.m_device->createRootShaderObject(state.m_terrain_programs[3], receiver_root.writeRef()));
            PPR_LOG(RadiancePass, info, "RC-PREP after createRootShaderObject terrainReceivers");

            rhi::ShaderCursor cursor{receiver_root};
            rhi::ShaderCursor constants{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_trace"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(&trace, sizeof(trace)));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_receivers"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, constants.setData(&receiver_parameters, sizeof(receiver_parameters)));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrain_(cursor, *terrain_storage, *terrain_plan));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrainPrecision_(cursor, precision_storage->get()));
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 6>{
                     std::pair{"g_cells", gpu_cells.get()}, {"g_directions", gpu_directions.get()},
                     {"g_upper", parity[1].get()}, {"g_receiver_features", receiver_generation->m_features.get()},
                     {"g_receiver_results", receiver_generation->m_results.get()},
                     {"g_receiver_angular",
                         angular ? receiver_generation->m_angular.get() : receiver_generation->m_results.get()}}) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
            }
        }

        PPR_LOG(
            RadiancePass, info, "RC-PREP before level root/binding group",
            {{"levels", levels.size()}, {"top_groups_x", levels.back().m_groups_x},
                {"top_groups_y", levels.back().m_groups_y}});
        u64 diagnostic_copy_offset{};
        for (std::size_t index = 0u; index < layout->m_cascades.size(); ++index) {
            const auto &cascade = layout->m_cascades[index];
            const bool top = index + 1u == layout->m_cascades.size();
            const auto &parent = top ? cascade : layout->m_cascades[index + 1u];
            auto &level = levels[index];

            GpuCascadeParameters parameters{};
            parameters.m_probe_width = static_cast<u32>(cascade.m_padded_probes[0]);
            parameters.m_probe_height = static_cast<u32>(cascade.m_padded_probes[1]);
            parameters.m_directions = static_cast<u32>(cascade.m_directions);
            parameters.m_level = static_cast<u32>(index);
            parameters.m_spacing = static_cast<float>(cascade.m_spacing_cells);
            parameters.m_near_cells = static_cast<float>(cascade.m_near_cells);
            parameters.m_terminal_far_cells = terrain != nullptr ? terrain_terminal_lambda
                                                                 : static_cast<float>(admitted->m_terminal_far_cells);
            parameters.m_parent_width = static_cast<u32>(parent.m_padded_probes[0]);
            parameters.m_parent_height = static_cast<u32>(parent.m_padded_probes[1]);
            parameters.m_direction_offset = direction_offsets[index];
            parameters.m_parent_direction_offset = top ? direction_offsets[index] : direction_offsets[index + 1u];
            parameters.m_dispatch_row_threads = level.m_groups_x * 64u;
            parameters.m_parent_spacing = static_cast<float>(parent.m_spacing_cells);
            parameters.m_parent_near_cells = static_cast<float>(parent.m_near_cells);

            if (diagnostic) {
                const auto &range =
                    diagnostic_header.m_sections[static_cast<std::size_t>(CascadeDiagnosticSection::levels)];
                CascadeDiagnosticLevel saved{};
                saved.m_parameters = parameters;
                if (index != 0u or backwall) {
                    saved.m_offset = diagnostic_copy_offset;
                    saved.m_bytes = u64{level.m_records} * sizeof(GpuOpticalResult);
                    saved.m_executed = 1u;
                    diagnostic_copy_offset += saved.m_bytes;
                }

                std::memcpy(diagnostic->m_inputs.get() + range.m_offset + index * sizeof(saved), &saved, sizeof(saved));
            }

            if (receivers and not backwall and index == 0u) {
                continue;
            }

            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass,
                state.m_device->createRootShaderObject(
                    terrain ? state.m_terrain_programs[top ? 1u : 2u]
                            : (packed ? state.m_packed_cascade_programs : state.m_cascade_programs)[top ? 0u : 1u],
                    level.m_root.writeRef()));
            rhi::ShaderCursor cursor{level.m_root};
            rhi::ShaderCursor constants{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_trace"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(&trace, sizeof(trace)));

            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor["g_cascade"].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(&parameters, sizeof(parameters)));
            for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 4>{
                     std::pair{"g_cells", gpu_cells.get()}, {"g_directions", gpu_directions.get()},
                     {packed ? "g_packed_upper" : "g_upper", parity[(index + 1u) % 2u].get()},
                     {packed ? "g_packed_results" : "g_results", parity[index % 2u].get()}}) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
            }

            if (terrain) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrain_(cursor, *terrain_storage, *terrain_plan));
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrainPrecision_(cursor, precision_storage->get()));
            } else {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    cursor["g_flags"].setBinding(rhi::Binding(hierarchy->m_flags, traceRange_(hierarchy->m_flags))));
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, cursor["g_hierarchy"].setBinding(
                                      rhi::Binding(hierarchy->m_metadata, traceRange_(hierarchy->m_metadata))));
            }

            if (packed) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass,
                    cursor["g_segments"].setBinding(rhi::Binding(gpu_segments, traceRange_(gpu_segments))));
            }
        }

        PPR_LOG(RadiancePass, info, "RC-PREP after level root/binding group", {{"levels", levels.size()}});
        PPR_ASSERT(not diagnostic or diagnostic_copy_offset == diagnostic_readback_bytes);

        u64 owned_bytes{};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(admitted->m_streaming_working_bytes, cell_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, direction_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, readback_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, segment_bytes, owned_bytes));

        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, hierarchy_plan->m_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, terrain_plan->m_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, diagnostic_retained_bytes, owned_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceAdd_(owned_bytes, refined ? kTerrainPrecisionRetainedBytes : 0u, owned_bytes));

        if (receiver_generation) {
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, receiver_generation->m_bytes, owned_bytes));
        }

        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(owned_bytes, angular_validation_bytes, owned_bytes));

        PPR_LOG(RadiancePass, info, "RC-PREP before commit", {{"owned_bytes", owned_bytes}});
        state.m_precision_storage = std::move(*precision_storage);
        state.m_cascade_diagnostic = std::move(diagnostic);
        state.m_receiver = std::move(receiver_generation);
        state.m_receiver_root = std::move(receiver_root);
        state.m_background_mean_root = std::move(background_mean_root);
        state.m_receiver_parameters = receiver_parameters;
        state.m_receiver_published = false;
        state.m_terrain = std::move(*terrain_storage);
        state.m_terrain_mode = terrain != nullptr;

        state.m_hierarchy = std::move(*hierarchy);
        state.m_segments = std::move(gpu_segments);
        state.m_storage = storage;
        state.m_root.setNull();
        state.m_rays.setNull();
        state.m_cells = std::move(gpu_cells);
        state.m_directions = std::move(gpu_directions);
        state.m_parity = std::move(parity);
        state.m_output = state.m_parity[0];
        state.m_readback = std::move(readback);
        state.m_levels = std::move(levels);
        state.m_parameters = trace;
        state.m_buffer_bytes = owned_bytes;
        state.m_largest_buffer_bytes = std::max(
            {cell_bytes, direction_bytes, readback_bytes, segment_bytes, hierarchy_plan->m_flag_bytes,
                hierarchy_plan->m_metadata_bytes, terrain_plan->m_largest, admitted->m_parity_buffer_bytes[0],
                admitted->m_parity_buffer_bytes[1], diagnostic_input_bytes, diagnostic_readback_bytes,
                refined ? kTerrainPrecisionCounterBytes : 0u});
        if (state.m_receiver) {
            state.m_largest_buffer_bytes = std::max(state.m_largest_buffer_bytes, state.m_receiver->m_largest);
        }
        state.m_diagnostic_count = receivers ? 0u : 1u;
        state.m_cascade_mode = true;
        state.m_last_use = kFenceValueNone;
        state.m_background_batch.reset();
        state.m_prepared = true;
        state.m_submitted = false;
        PPR_LOG(RadiancePass, info, "RC-PREP return success");
        return {};
    }

    std::error_code RadianceCascadePass::encodeCascades(const PreRenderContext &context) {
        const hal::ProfileScope scope{"App.Lighting.EncodeCascades"};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              state.m_cascade_mode and
                                  state.m_prepared and
                                  not state.m_encoded and
                                  not state.m_unknown and
                                  state.m_device == &context.m_device and
                                  slotFenceComplete(context.m_completed_fence_value, state.m_last_use),
                              std::errc::device_or_resource_busy));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not state.m_terrain_mode or
                                  (state.m_terrain_pipelines[1] != nullptr and state.m_terrain_pipelines[2] != nullptr),
                              std::errc::operation_not_permitted));

        const bool packed = state.m_storage == GpuIntervalStorage::rgb16f;
        const u32 stride = intervalStride_(state.m_storage);

        state.m_encoded = true;
        state.m_submitted = false;
        beginTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());

        if (packed) {
            context.m_encoder.setBufferState(state.m_segments, rhi::ResourceState::UnorderedAccess);
        }

        context.m_encoder.setBufferState(state.m_cells, rhi::ResourceState::ShaderResource);
        if (state.m_terrain_mode) {
            terrainStates_(context.m_encoder, state.m_terrain);
        } else {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, encodeHierarchy_(context.m_encoder, state.m_hierarchy_pipeline, state.m_hierarchy));
        }

        context.m_encoder.setBufferState(state.m_directions, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(state.m_readback, rhi::ResourceState::CopyDestination);
        if (state.m_cascade_diagnostic) {
            context.m_encoder.setBufferState(
                state.m_cascade_diagnostic->m_readback, rhi::ResourceState::CopyDestination);
        }

        for (auto iterator = state.m_levels.rbegin(); iterator != state.m_levels.rend(); ++iterator) {
            const auto &level = *iterator;
            if (state.m_receiver and not state.m_receiver->m_background and level.m_level == 0u) {
                continue;
            }

            const bool top = iterator == state.m_levels.rbegin();
            auto *const output = state.m_parity[level.m_level % 2u].get();
            context.m_encoder.setBufferState(output, rhi::ResourceState::UnorderedAccess);
            if (not top) {
                context.m_encoder.setBufferState(
                    state.m_parity[(level.m_level + 1u) % 2u], rhi::ResourceState::ShaderResource);
            }

            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(
                state.m_terrain_mode
                    ? state.m_terrain_pipelines[top ? 1u : 2u]
                    : (packed ? state.m_packed_cascade_pipelines : state.m_cascade_pipelines)[top ? 0u : 1u],
                level.m_root);
            pass->dispatchCompute(level.m_groups_x, level.m_groups_y, 1u);
            pass->end();

            context.m_encoder.globalBarrier();
            if (state.m_cascade_diagnostic) {
                const auto &diagnostic = *state.m_cascade_diagnostic;
                const auto &range =
                    diagnostic.m_header.m_sections[static_cast<std::size_t>(CascadeDiagnosticSection::levels)];
                CascadeDiagnosticLevel saved{};
                std::memcpy(
                    &saved, diagnostic.m_inputs.get() + range.m_offset + level.m_level * sizeof(saved), sizeof(saved));
                PPR_ASSERT(saved.m_executed != 0u and saved.m_bytes == u64{level.m_records} * sizeof(GpuOpticalResult));

                // This copy observes the completed level before its parity
                // storage can be reused. Its consumer still reads the same SRV.
                context.m_encoder.setBufferState(output, rhi::ResourceState::CopySource);
                context.m_encoder.copyBuffer(diagnostic.m_readback, saved.m_offset, output, 0u, saved.m_bytes);
                context.m_encoder.setBufferState(output, rhi::ResourceState::ShaderResource);
            }

            if (level.m_level == 1u and not state.m_receiver) {
                context.m_encoder.setBufferState(output, rhi::ResourceState::CopySource);
                context.m_encoder.copyBuffer(
                    state.m_readback, u64{state.m_parameters.m_ray_count} * stride, output, 0u, stride);
            }
        }

        if (state.m_receiver and state.m_receiver->m_background) {
            auto &source = *state.m_receiver->m_background;
            context.m_encoder.setBufferState(state.m_parity[0], rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(source.m_means, rhi::ResourceState::UnorderedAccess);
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_terrain_pipelines[4], state.m_background_mean_root);
            pass->dispatchCompute((source.m_width * source.m_height + 63u) / 64u, 1u, 1u);
            pass->end();

            context.m_encoder.globalBarrier();
            context.m_encoder.setBufferState(source.m_means, rhi::ResourceState::CopySource);
            const u64 offset = state.m_receiver->m_results->getDesc().size +
                               (state.m_receiver->m_angular ? state.m_receiver->m_angular->getDesc().size : 0u);
            context.m_encoder.copyBuffer(state.m_readback, offset, source.m_means, 0u, source.m_means->getDesc().size);
            context.m_encoder.setBufferState(source.m_means, rhi::ResourceState::ShaderResource);
        }

        if (state.m_receiver) {
            context.m_encoder.setBufferState(state.m_parity[1], rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(state.m_receiver->m_cells, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(state.m_receiver->m_components, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(state.m_receiver->m_candidates, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(state.m_receiver->m_features, rhi::ResourceState::ShaderResource);
            context.m_encoder.setBufferState(state.m_receiver->m_results, rhi::ResourceState::UnorderedAccess);
            if (state.m_receiver->m_angular) {
                context.m_encoder.setBufferState(state.m_receiver->m_angular, rhi::ResourceState::UnorderedAccess);
            }
            auto *const pass = context.m_encoder.beginComputePass();
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
            pass->bindPipeline(state.m_terrain_pipelines[3], state.m_receiver_root);
            pass->dispatchCompute((state.m_receiver_parameters.m_count + 63u) / 64u, 1u, 1u);
            pass->end();

            context.m_encoder.globalBarrier();
            context.m_encoder.setBufferState(state.m_receiver->m_results, rhi::ResourceState::CopySource);
            context.m_encoder.copyBuffer(
                state.m_readback, 0u, state.m_receiver->m_results, 0u, state.m_receiver->m_results->getDesc().size);
            context.m_encoder.setBufferState(state.m_receiver->m_results, rhi::ResourceState::ShaderResource);
            if (state.m_receiver->m_angular) {
                context.m_encoder.setBufferState(state.m_receiver->m_angular, rhi::ResourceState::CopySource);
                context.m_encoder.copyBuffer(
                    state.m_readback, state.m_receiver->m_results->getDesc().size, state.m_receiver->m_angular, 0u,
                    state.m_receiver->m_angular->getDesc().size);
                context.m_encoder.setBufferState(state.m_receiver->m_angular, rhi::ResourceState::ShaderResource);
            }
            copyTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());
            return {};
        }

        context.m_encoder.setBufferState(state.m_output, rhi::ResourceState::CopySource);
        context.m_encoder.copyBuffer(
            state.m_readback, 0u, state.m_output, 0u, u64{state.m_parameters.m_ray_count} * stride);
        if (packed) {
            context.m_encoder.setBufferState(state.m_segments, rhi::ResourceState::CopySource);
            context.m_encoder.copyBuffer(
                state.m_readback, (u64{state.m_parameters.m_ray_count} + state.m_diagnostic_count) * stride,
                state.m_segments, 0u, state.m_segments->getDesc().size);
        }

        copyTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());
        return {};
    }

    // ------------------------------------------------------------------
    // direct trace qualification encoding
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::encodeTrace(const PreRenderContext &context) {
        const hal::ProfileScope scope{"App.Lighting.EncodeGpuTrace"};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(not state.m_background_batch, std::errc::operation_not_permitted));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not state.m_cascade_mode and
                                  state.m_prepared and
                                  not state.m_encoded and
                                  not state.m_unknown and
                                  state.m_device == &context.m_device and
                                  slotFenceComplete(context.m_completed_fence_value, state.m_last_use),
                              std::errc::device_or_resource_busy));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not state.m_terrain_mode or state.m_terrain_pipelines[0] != nullptr,
                              std::errc::operation_not_permitted));

        const bool packed = state.m_storage == GpuIntervalStorage::rgb16f;

        // Once any commands reference this batch, even a later encode failure
        // needs explicit abandonment or a submission disposition before reuse.
        state.m_encoded = true;
        state.m_submitted = false;
        beginTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());

        if (packed) {
            context.m_encoder.setBufferState(state.m_segments, rhi::ResourceState::UnorderedAccess);
        }

        context.m_encoder.setBufferState(state.m_cells, rhi::ResourceState::ShaderResource);
        if (state.m_terrain_mode) {
            terrainStates_(context.m_encoder, state.m_terrain);
        } else {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, encodeHierarchy_(context.m_encoder, state.m_hierarchy_pipeline, state.m_hierarchy));
        }

        context.m_encoder.setBufferState(state.m_rays, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(state.m_output, rhi::ResourceState::UnorderedAccess);
        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
        pass->bindPipeline(
            state.m_terrain_mode ? state.m_terrain_pipelines[0]
            : packed             ? state.m_packed_pipeline
                                 : state.m_pipeline,
            state.m_root);
        const u32 count = state.m_parameters.m_ray_count;
        pass->dispatchCompute(count / 64u + (count % 64u == 0u ? 0u : 1u), 1u, 1u);
        pass->end();

        context.m_encoder.globalBarrier();
        context.m_encoder.setBufferState(state.m_output, rhi::ResourceState::CopySource);
        context.m_encoder.setBufferState(state.m_readback, rhi::ResourceState::CopyDestination);
        context.m_encoder.copyBuffer(state.m_readback, 0u, state.m_output, 0u, state.m_output->getDesc().size);
        if (packed) {
            context.m_encoder.setBufferState(state.m_segments, rhi::ResourceState::CopySource);
            context.m_encoder.copyBuffer(
                state.m_readback, state.m_output->getDesc().size, state.m_segments, 0u,
                state.m_segments->getDesc().size);
        }

        copyTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());
        state.m_encoded = true;
        state.m_submitted = false;
        return {};
    }

    // ------------------------------------------------------------------
    // explicit submission settlement, readback, and teardown
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::notifySubmitted(const FenceCompletionValue value) {
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                m_state and m_state->m_encoded and value != kFenceValueNone, std::errc::operation_not_permitted));
        m_state->m_encoded = false;
        m_state->m_submitted = true;
        m_state->m_last_use = value;
        return {};
    }

    std::error_code RadianceCascadePass::discardUnsubmitted() {
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(m_state and m_state->m_encoded, std::errc::operation_not_permitted));
        m_state->m_encoded = false;
        return {};
    }

    std::error_code RadianceCascadePass::notifySubmitFailed() {
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(m_state and m_state->m_encoded, std::errc::operation_not_permitted));
        m_state->m_encoded = false;
        m_state->m_unknown = true;
        return {};
    }

    std::expected<std::optional<TerrainPrecisionStatistics>, std::error_code>
    RadianceCascadePass::readTerrainPrecisionStatistics(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(not state.m_unknown, std::errc::state_not_recoverable));
        if (not state.m_precision_storage or not state.m_submitted or state.m_encoded) {
            return std::optional<TerrainPrecisionStatistics>{};
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              state.m_last_use != kFenceValueNone and slotFenceComplete(completed, state.m_last_use),
                              std::errc::resource_unavailable_try_again));

        std::array<u32, 4> words{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            state.m_device->mapBuffer(state.m_precision_storage->m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(words.data(), mapped, sizeof(words));
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, state.m_device->unmapBuffer(state.m_precision_storage->m_readback));

        auto statistics = state.m_precision_storage->m_statistics;
        statistics.m_attempted = words[0];
        statistics.m_qualified = words[1];
        statistics.m_unresolved = words[2];
        statistics.m_bound_error = words[3];
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              statistics.m_query_upper_bound <= std::numeric_limits<u32>::max() and
                                  u64{statistics.m_attempted} <= statistics.m_query_upper_bound and
                                  u64{statistics.m_qualified} + statistics.m_unresolved == statistics.m_attempted and
                                  statistics.m_bound_error <= statistics.m_unresolved,
                              std::errc::value_too_large));
        state.m_precision_storage->m_statistics = statistics;
        return std::optional<TerrainPrecisionStatistics>{statistics};
    }

    std::expected<GpuTraceSnapshot, std::error_code> RadianceCascadePass::readCompleted(
        const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              m_state and m_state->m_submitted and not m_state->m_encoded and not m_state->m_unknown,
                              std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(not state.m_receiver and not state.m_background_batch, std::errc::operation_not_permitted));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(slotFenceComplete(completed, state.m_last_use), std::errc::resource_unavailable_try_again));

        const auto precision = readTerrainPrecisionStatistics(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, precision ? std::error_code{} : precision.error());
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not *precision or ((**precision).m_unresolved == 0u and (**precision).m_bound_error == 0u),
                std::errc::argument_out_of_domain));

        std::vector<GpuOpticalResult> rows(std::size_t(state.m_parameters.m_ray_count) + state.m_diagnostic_count);
        GpuTraceSnapshot result{};
        result.m_intervals.resize(state.m_parameters.m_ray_count);
        result.m_segments.resize(state.m_parameters.m_ray_count);
        if (state.m_storage == GpuIntervalStorage::rgb16f) {
            result.m_packed_words.resize(rows.size());
        }

        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, state.m_device->mapBuffer(state.m_readback, rhi::CpuAccessMode::Read, &mapped));

        if (state.m_storage == GpuIntervalStorage::rgb16f) {
            const auto *const bytes = static_cast<const std::byte *>(mapped);
            const std::size_t segment_offset = rows.size() * sizeof(GpuPackedInterval);
            for (std::size_t index = 0u; index < rows.size(); ++index) {
                GpuPackedInterval stored{};
                std::memcpy(&stored, bytes + index * sizeof(stored), sizeof(stored));
                result.m_packed_words[index] = {
                    stored.m_radiance_rg, stored.m_radiance_b_transmission_r, stored.m_transmission_gb,
                    stored.m_status};
                auto &row = rows[index];
                row.m_radiance[0] = decodeHalf_(stored.m_radiance_rg & 65535u);
                row.m_radiance[1] = decodeHalf_(stored.m_radiance_rg >> 16u);
                row.m_radiance[2] = decodeHalf_(stored.m_radiance_b_transmission_r & 65535u);
                row.m_transmittance[0] = decodeHalf_(stored.m_radiance_b_transmission_r >> 16u);
                row.m_transmittance[1] = decodeHalf_(stored.m_transmission_gb & 65535u);
                row.m_transmittance[2] = decodeHalf_(stored.m_transmission_gb >> 16u);
                row.m_status = stored.m_status;
                std::memcpy(&row.m_segments, bytes + segment_offset + index * sizeof(u32), sizeof(u32));
            }
        } else {
            std::memcpy(rows.data(), mapped, rows.size() * sizeof(GpuOpticalResult));
        }

        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, state.m_device->unmapBuffer(state.m_readback));
        state.m_last_use = kFenceValueNone;

        for (std::size_t index = 0u; index < rows.size(); ++index) {
            const auto &row = rows[index];
            if (row.m_status != 0u) {
                PPR_LOG(
                    RadiancePass, error, "GPU optical trace did not complete",
                    {{"ray", index}, {"status", row.m_status}, {"segments", row.m_segments}});
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  row.m_status == 0u, (row.m_status & kTerrainNumericAmbiguity) != 0u
                                                          ? std::errc::argument_out_of_domain
                                                          : std::errc::result_out_of_range));
            RadianceInterval *destination{};
            if (index < result.m_intervals.size()) {
                result.m_segments[index] = row.m_segments;
                destination = &result.m_intervals[index];
            } else {
                destination = &result.m_c1_probe0_bin0.emplace();
            }

            for (std::size_t channel = 0u; channel < 3u; ++channel) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass, requireTrace_(
                                      std::isfinite(row.m_radiance[channel]) and
                                          row.m_radiance[channel] >= 0.0f and
                                          std::isfinite(row.m_transmittance[channel]) and
                                          row.m_transmittance[channel] >= 0.0f and
                                          row.m_transmittance[channel] <= 1.0f,
                                      std::errc::result_out_of_range));
                destination->m_radiance[channel] = static_cast<double>(row.m_radiance[channel]);
                destination->m_transmittance[channel] = static_cast<double>(row.m_transmittance[channel]);
            }
        }

        return result;
    }

    // ------------------------------------------------------------------
    // bounded terrain receiver production and immutable leases
    // ------------------------------------------------------------------

    std::error_code validateTerrainReceiverAngular(
        const std::span<const TerrainReceiverAngular> rows, const u32 receiver_count) {
        const hal::ProfileScope scope{"App.Lighting.ValidateAngularReceivers"};
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, receiver_count != 0u and rows.size() == u64{receiver_count} * 16u
                              ? std::error_code{}
                              : std::make_error_code(std::errc::invalid_argument));

        for (const auto &row: rows) {
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, row.m_status == 0u ? std::error_code{}
                                                 : std::make_error_code(
                                                       (row.m_status & kTerrainNumericAmbiguity) != 0u
                                                           ? std::errc::argument_out_of_domain
                                                           : std::errc::result_out_of_range));

            for (const float channel: row.m_radiance) {
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, std::isfinite(channel) and channel >= 0.0f
                                      ? std::error_code{}
                                      : std::make_error_code(std::errc::result_out_of_range));
            }
        }

        return {};
    }

    const std::shared_ptr<const TerrainReceiverSet> &TerrainReceiverGpuGeneration::receivers() const noexcept {
        return m_receivers;
    }

    TerrainReceiverCohort TerrainReceiverGpuGeneration::cohort() const noexcept {
        return m_cohort;
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::featuresBuffer() const noexcept {
        return m_features.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::resultsBuffer() const noexcept {
        return m_results.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::cellsBuffer() const noexcept {
        return m_cells.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::componentsBuffer() const noexcept {
        return m_components.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::candidatesBuffer() const noexcept {
        return m_candidates.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::c1Buffer() const noexcept {
        return m_c1.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::backgroundMeanBuffer() const noexcept {
        return m_background ? m_background->m_means.get() : nullptr;
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::angularBuffer() const noexcept {
        return m_angular.get();
    }

    rhi::IBuffer *TerrainReceiverGpuGeneration::directionsBuffer() const noexcept {
        return m_directions.get();
    }

    u32 TerrainReceiverGpuGeneration::angularDirectionOffset() const noexcept {
        return m_angular_direction_offset;
    }

    u32 TerrainReceiverGpuGeneration::angularDirectionCount() const noexcept {
        return m_angular_direction_count;
    }

    u64 TerrainReceiverGpuGeneration::bufferBytes() const noexcept {
        return m_bytes;
    }

    u64 TerrainReceiverGpuGeneration::largestBufferBytes() const noexcept {
        return m_largest;
    }

    bool TerrainReceiverGpuGeneration::matchesDevice(const rhi::IDevice &device) const noexcept {
        return m_device == &device;
    }

    const TerrainPrecisionStatistics *TerrainReceiverGpuGeneration::precisionStatistics() const noexcept {
        return m_precision_statistics.get();
    }

    std::error_code RadianceCascadePass::prepareTerrainReceivers(
        std::shared_ptr<const TerrainReceiverSet> receivers, const TerrainReceiverCohort cohort,
        const RcEstimateRequest &request, const FenceCompletionValue completed, const bool qualification_readback,
        const TerrainReceiverOutput output, const TerrainBackgroundOutput background) {
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(receivers and receivers->geometry() and cohort.m_geometry_revision != 0u));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                m_state->m_device->getInfo().deviceType == rhi::DeviceType::Vulkan, std::errc::not_supported));

        const auto &terrain = *receivers->geometry();
        const auto view = terrain.view();
        PPR_LOG(
            RadiancePass, info, "RC-PREP before terrainCells",
            {{"width", view.m_width}, {"height", view.m_height}, {"features", receivers->features().size()}});
        const auto cells = terrainCells_(terrain);
        PPR_LOG(RadiancePass, info, "RC-PREP after terrainCells", {{"cells", cells.size()}});

        const OpticalGridView grid{
            cells, view.m_min_m, view.m_cell_size_m, view.m_width, view.m_height, view.m_boundary};
        return prepareCascades_(
            grid, request, completed, GpuIntervalStorage::fp32, &terrain, std::move(receivers), cohort,
            qualification_readback, output, background);
    }

    // ------------------------------------------------------------------
    // fenced raw observation of the failed receiver dependency chain
    // ------------------------------------------------------------------

    std::error_code RadianceCascadePass::writeTerrainCascadeReadbackForQualification(
        const std::filesystem::path &directory, const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              m_state and
                                  m_state->m_cascade_diagnostic and
                                  m_state->m_receiver and
                                  m_state->m_cascade_mode and
                                  m_state->m_storage == GpuIntervalStorage::fp32 and
                                  m_state->m_submitted and
                                  not m_state->m_encoded and
                                  not m_state->m_unknown,
                              std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(slotFenceComplete(completed, state.m_last_use), std::errc::resource_unavailable_try_again));
        const auto precision = readTerrainPrecisionStatistics(completed);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, precision ? std::error_code{} : precision.error());
        const auto &diagnostic = *state.m_cascade_diagnostic;

        std::error_code filesystem_error{};
        std::filesystem::create_directories(directory, filesystem_error);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, filesystem_error);
        std::ignore = std::filesystem::remove(directory / "manifest.txt", filesystem_error);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, filesystem_error);

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            writeDiagnosticBytes_(directory / "inputs.bin", diagnostic.m_inputs.get(), diagnostic.m_input_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            writeDiagnosticBuffer_(
                *state.m_device, diagnostic.m_readback, directory / "cascades.bin", diagnostic.m_readback_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, writeDiagnosticBuffer_(
                              *state.m_device, state.m_readback, directory / "receivers.bin",
                              u64{state.m_receiver_parameters.m_count} * sizeof(TerrainReceiverResult)));

        // Publish the manifest only after every raw file and its own close
        // succeeded. Raw statuses are exported without filtering or acceptance.
        std::ofstream manifest{directory / "manifest.tmp", std::ios::trunc};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(manifest.is_open(), std::errc::io_error));
        manifest
            << "schema=ppr-terrain-cascade-diagnostic-v1\n"
            << "diagnostic_only=true physical_acceptance=false\n"
            << "endian=" << (diagnostic.m_header.m_endian == 1u ? "little" : "big") << '\n'
            << "inputs_header=magic:u64,sections:14*(offset:u64,bytes:u64,stride:u32,count:u32),version:u32,endian:u32\n"
            << "cascade_record=radiance:3*f32,transmission:3*f32,status:u32,segments:u32\n"
            << "receiver_record=mean:3*f32,status:u32\n"
            << "level_record=copy_offset:u64,copy_bytes:u64,parameters:16*u32_bits,executed:u32,reserved:u32\n"
            << "cascade_parameters=probe_width,probe_height,directions,level,spacing_f32,near_f32,terminal_far_f32,parent_width,parent_height,parent_direction_offset,direction_offset,dispatch_row_threads,parent_spacing_f32,parent_near_f32,reserved0,reserved1\n"
            << "trace_parameters=width,height,ray_count,boundary,cell_metres_f32,hierarchy_levels,reserved0,reserved1\n"
            << "terrain_parameters=outside_material,outside_roundness_f32,outside_r_f32,outside_g_f32,outside_b_f32,reserved0,reserved1,reserved2\n"
            << "receiver_parameters=count,parent_width,parent_height,parent_direction_offset,parent_spacing_f32,parent_near_f32,angular_output,reserved1\n"
            << "cohort=job:u64,world:u64,content:u64,geometry_revision:u64\n"
            << "geometry=min_m:2*f64_bits,cell_metres:f64_bits,width:u32,height:u32,outside_material:u32,boundary:u32\n"
            << "material=extinction:3*f64_bits,volume_emission:3*f64_bits,surface_emission:3*f64_bits,roundness:f32_bits,material:u32,kind:u32,reserved:u32\n"
            << "feature=base_x:i32,base_y:i32,curve:u32,component:u32,visual_x:f32_bits,visual_y:f32_bits,visual_outward_x:f32_bits,visual_outward_y:f32_bits\n"
            << "cell=extinction:3*f32,volume_emission:3*f32,surface_emission:3*f32,kind:u32\n"
            << "vertex=roundness:f32_bits,material:u32\n"
            << "point=xy:2*f32\n"
            << "case=segment_count:u32,triangle_count:u32,segments:2*(a:u32,b:u32),triangles:3*(a:u32,b:u32,c:u32,solid_corners:u32)\n"
            << std::format(
                   "inputs_bytes={} cascade_bytes={} receiver_bytes={} charged_retention_bytes={} submitted_fence={}\n",
                   diagnostic.m_input_bytes, diagnostic.m_readback_bytes,
                   u64{state.m_receiver_parameters.m_count} * sizeof(TerrainReceiverResult),
                   diagnostic.m_retained_bytes, u64{state.m_last_use});

        manifest << "precision_mode="
                 << (state.m_precision == TerrainPrecision::strict_fp32_qualification
                            ? "strict_fp32_qualification"
                            : "qualified_fp64_refinement_experimental")
                 << '\n';
        if (*precision) {
            const auto &statistics = **precision;
            manifest << std::format(
                "precision_query_upper_bound={} attempted={} qualified={} unresolved={} bound_error={} counter_gpu_bytes={} counter_readback_bytes={} counter_owner_bytes={} receiver_statistics_reserve_bytes={}\n",
                statistics.m_query_upper_bound, statistics.m_attempted, statistics.m_qualified, statistics.m_unresolved,
                statistics.m_bound_error, kTerrainPrecisionCounterBytes, kTerrainPrecisionCounterBytes,
                kTerrainPrecisionOwnerBytes, kTerrainPrecisionLeaseBytes);
        }

        constexpr std::array<const char *, 14> section_names{
            "trace", "terrain", "receiver", "cohort", "geometry", "levels", "directions", "cells", "vertices", "points",
            "cases", "features", "labels", "materials"};
        for (std::size_t index = 0u; index < section_names.size(); ++index) {
            const auto &range = diagnostic.m_header.m_sections[index];
            manifest << std::format(
                "section={} offset={} bytes={} stride={} count={}\n", section_names[index], range.m_offset,
                range.m_bytes, range.m_stride, range.m_count);
        }

        const auto &level_range =
            diagnostic.m_header.m_sections[static_cast<std::size_t>(CascadeDiagnosticSection::levels)];
        for (u32 index = 0u; index < level_range.m_count; ++index) {
            CascadeDiagnosticLevel level{};
            std::memcpy(
                &level, diagnostic.m_inputs.get() + level_range.m_offset + index * sizeof(level), sizeof(level));
            manifest << std::format(
                "level={} executed={} offset={} bytes={} width={} height={} directions={}\n", index, level.m_executed,
                level.m_offset, level.m_bytes, level.m_parameters.m_probe_width, level.m_parameters.m_probe_height,
                level.m_parameters.m_directions);
        }

        const auto &feature_range =
            diagnostic.m_header.m_sections[static_cast<std::size_t>(CascadeDiagnosticSection::features)];
        TerrainReceiverFeature first{};
        std::memcpy(&first, diagnostic.m_inputs.get() + feature_range.m_offset, sizeof(first));
        manifest << std::format(
            "receiver0_base=({},{}) curve={} component={} visual_bits=({:08x},{:08x})\n", first.m_base_x,
            first.m_base_y, first.m_curve, first.m_component, std::bit_cast<u32>(first.m_visual_x),
            std::bit_cast<u32>(first.m_visual_y));

        manifest << "complete=true\n";
        manifest.close();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(not manifest.fail(), std::errc::io_error));

        std::filesystem::rename(directory / "manifest.tmp", directory / "manifest.txt", filesystem_error);
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, filesystem_error);
        return {};
    }

    // ------------------------------------------------------------------
    // validated receiver observation and publication
    // ------------------------------------------------------------------

    std::expected<std::vector<TerrainReceiverResult>, std::error_code> RadianceCascadePass::readTerrainReceivers(
        const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              m_state and
                                  m_state->m_receiver and
                                  not m_state->m_background_batch and
                                  m_state->m_submitted and
                                  not m_state->m_encoded and
                                  not m_state->m_unknown,
                              std::errc::operation_not_permitted));
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(slotFenceComplete(completed, state.m_last_use), std::errc::resource_unavailable_try_again));

        const auto precision = readTerrainPrecisionStatistics(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, precision ? std::error_code{} : precision.error());
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not *precision or ((**precision).m_unresolved == 0u and (**precision).m_bound_error == 0u),
                std::errc::argument_out_of_domain));

        std::vector<TerrainReceiverResult> rows(state.m_receiver_parameters.m_count);
        std::vector<TerrainReceiverAngular> angular(state.m_receiver->m_angular ? rows.size() * 16u : 0u);
        const auto source = state.m_receiver->m_background;
        std::vector<TerrainReceiverResult> background(source ? std::size_t{source->m_width} * source->m_height : 0u);
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, state.m_device->mapBuffer(state.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(rows.data(), mapped, rows.size() * sizeof(TerrainReceiverResult));
        if (state.m_receiver->m_angular) {
            const auto *const bytes = static_cast<const std::byte *>(mapped) +
                                      rows.size() * sizeof(TerrainReceiverResult);
            std::memcpy(angular.data(), bytes, angular.size() * sizeof(TerrainReceiverAngular));
        }
        if (source) {
            const auto *const bytes = static_cast<const std::byte *>(mapped) +
                                      rows.size() * sizeof(TerrainReceiverResult) +
                                      angular.size() * sizeof(TerrainReceiverAngular);
            std::memcpy(background.data(), bytes, background.size() * sizeof(TerrainReceiverResult));
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, state.m_device->unmapBuffer(state.m_readback));

        for (const auto &row: background) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  row.m_status == 0u, (row.m_status & kTerrainNumericAmbiguity) != 0u
                                                          ? std::errc::argument_out_of_domain
                                                          : std::errc::result_out_of_range));
            for (const float channel: row.m_mean) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass,
                    requireTrace_(std::isfinite(channel) and channel >= 0.0f, std::errc::result_out_of_range));
            }
        }

        if (state.m_receiver->m_angular) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, validateTerrainReceiverAngular(angular, state.m_receiver_parameters.m_count));
        }

        for (std::size_t index = 0u; index < rows.size(); ++index) {
            const auto &row = rows[index];
            if (row.m_status != 0u) {
                PPR_LOG(
                    RadiancePass, error, "GPU terrain receiver did not complete",
                    {{"receiver", index}, {"status", row.m_status}});
            }
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(
                                  row.m_status == 0u, (row.m_status & kTerrainNumericAmbiguity) != 0u
                                                          ? std::errc::argument_out_of_domain
                                                          : std::errc::result_out_of_range));
            for (const float channel: row.m_mean) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass,
                    requireTrace_(std::isfinite(channel) and channel >= 0.0f, std::errc::result_out_of_range));
            }
        }

        return rows;
    }

    std::expected<std::vector<TerrainReceiverAngular>, std::error_code> RadianceCascadePass::readTerrainReceiverAngular(
        const FenceCompletionValue completed) {
        const auto validated = readTerrainReceivers(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, validated ? std::error_code{} : validated.error());
        State &state = *m_state;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(state.m_receiver->m_angular != nullptr, std::errc::operation_not_permitted));

        std::vector<TerrainReceiverAngular> rows(std::size_t(state.m_receiver_parameters.m_count) * 16u);
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, state.m_device->mapBuffer(state.m_readback, rhi::CpuAccessMode::Read, &mapped));
        const auto *const bytes = static_cast<const std::byte *>(mapped) +
                                  validated->size() * sizeof(TerrainReceiverResult);
        std::memcpy(rows.data(), bytes, rows.size() * sizeof(TerrainReceiverAngular));
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, state.m_device->unmapBuffer(state.m_readback));
        return rows;
    }

    std::expected<std::shared_ptr<const TerrainReceiverGpuGeneration>, std::error_code>
    RadianceCascadePass::acquireTerrainReceiverView(const FenceCompletionValue completed) {
        const auto validated = readTerrainReceivers(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, validated ? std::error_code{} : validated.error());
        State &state = *m_state;
        auto &generation = *state.m_receiver;
        if (not state.m_receiver_published) {
            if (state.m_precision_storage) {
                generation.m_precision_statistics = std::make_unique<const TerrainPrecisionStatistics>(
                    state.m_precision_storage->m_statistics);
            }

            u64 bytes{};
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, traceAdd_(generation.m_bytes, state.m_parity[1]->getDesc().size, bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(bytes, state.m_directions->getDesc().size, bytes));
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass, requireTrace_(state.m_buffer_bytes >= bytes, std::errc::state_not_recoverable));

            if (generation.m_background) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(bytes, state.m_cells->getDesc().size, bytes));
                for (auto *const buffer:
                    {state.m_terrain.m_vertices.get(), state.m_terrain.m_points.get(), state.m_terrain.m_cases.get()}) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, traceAdd_(bytes, buffer->getDesc().size, bytes));
                }
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    RadiancePass, requireTrace_(state.m_buffer_bytes >= bytes, std::errc::state_not_recoverable));
            }

            // Reserve the tracking record before transferring buffers. Each
            // external lease remains charged until its last strong owner retires.
            state.m_receiver_generations.push_back(state.m_receiver);
            generation.m_bytes = bytes;
            generation.m_largest = std::max(
                {generation.m_largest, state.m_parity[1]->getDesc().size, state.m_directions->getDesc().size});
            generation.m_c1 = std::move(state.m_parity[1]);
            generation.m_directions = std::move(state.m_directions);
            if (generation.m_background) {
                auto &source = *generation.m_background;
                source.m_cells = std::move(state.m_cells);
                source.m_terrain = std::move(state.m_terrain);
                for (auto *const buffer:
                    {source.m_cells.get(), source.m_terrain.m_vertices.get(), source.m_terrain.m_points.get(),
                        source.m_terrain.m_cases.get()})
                    generation.m_largest = std::max(generation.m_largest, buffer->getDesc().size);
            }
            state.m_buffer_bytes -= bytes;
            state.m_levels.clear();
            state.m_receiver_root.setNull();
            state.m_background_mean_root.setNull();
            state.m_receiver_published = true;
            state.m_prepared = false;
        }

        return std::shared_ptr<const TerrainReceiverGpuGeneration>{state.m_receiver};
    }

    // ------------------------------------------------------------------
    // completed exact-view backwall resources and preparation
    // ------------------------------------------------------------------

    std::error_code validateTerrainBackgroundCompletion(
        const TerrainBackgroundStatistics &statistics, const u32 width, const u32 height) {
        u64 pixels{}, samples{}, queries{};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(width != 0u and height != 0u));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(width, height, pixels));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(pixels, 5u, samples));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(samples, 4u, queries));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              queries <= std::numeric_limits<u32>::max() and
                                  statistics.m_query_upper_bound == queries and
                                  statistics.m_samples == samples and
                                  statistics.m_attempted <= queries and
                                  u64{statistics.m_qualified} + statistics.m_unresolved == statistics.m_attempted and
                                  statistics.m_bound_error <= statistics.m_unresolved and
                                  statistics.m_outside_plane <= samples and
                                  statistics.m_invalid_values <= samples,
                              std::errc::state_not_recoverable));

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                statistics.m_unresolved == 0u and statistics.m_bound_error == 0u and statistics.m_invalid_values == 0u,
                std::errc::argument_out_of_domain));
        return {};
    }

    const std::shared_ptr<const TerrainReceiverGpuGeneration> &TerrainBackgroundGpuView::source() const noexcept {
        return m_source;
    }

    rhi::IBuffer *TerrainBackgroundGpuView::resultsBuffer() const noexcept {
        return m_results.get();
    }

    const TerrainBackgroundStatistics &TerrainBackgroundGpuView::statistics() const noexcept {
        return m_statistics;
    }

    bool TerrainBackgroundGpuView::matches(
        const CameraSnapshot &camera, const int2 extent, const rhi::IDevice &device) const noexcept {
        if (not m_source or not m_source->matchesDevice(device) or extent.x != m_extent.x or extent.y != m_extent.y) {
            return false;
        }

        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                if (std::bit_cast<u32>(camera.m_invert_view_projection[row][column]) !=
                    std::bit_cast<u32>(m_camera.m_invert_view_projection[row][column])) {
                    return false;
                }
            }
        }
        return true;
    }

    u64 TerrainBackgroundGpuView::bufferBytes() const noexcept {
        return m_bytes;
    }

    u64 TerrainBackgroundGpuView::largestBufferBytes() const noexcept {
        return m_results ? m_results->getDesc().size : 0u;
    }

    std::error_code RadianceCascadePass::prepareTerrainBackgroundView(
        std::shared_ptr<const TerrainReceiverGpuGeneration> source, const CameraSnapshot &camera, const int2 extent,
        const u64 budget_bytes, const u64 reserve_bytes, const u64 max_resource_bytes,
        const FenceCompletionValue completed) {
        const hal::ProfileScope scope{"App.Lighting.PrepareBackgroundView"};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(m_state != nullptr, std::errc::not_connected));
        State &state = *m_state;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not state.m_encoded and not state.m_unknown and slotFenceComplete(completed, state.m_last_use),
                std::errc::device_or_resource_busy));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              source and
                              source->matchesDevice(*state.m_device) and
                              source->m_background and
                              extent.x > 0 and
                              extent.y > 0));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              state.m_device->hasFeature("standard-sample-locations-4") and
                                  state.m_device->hasFeature("sample-rate-shading"),
                              std::errc::not_supported));
        const auto &field = *source->m_background;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              field.m_width >= 2u and field.m_height >= 2u and field.m_precision == state.m_precision,
                              std::errc::not_supported));

        u64 pixels{}, samples{}, queries{}, output_bytes{}, candidate_bytes{}, peak{};
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceMultiply_(static_cast<u64>(extent.x), static_cast<u64>(extent.y), pixels));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(pixels, 5u, samples));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(samples, 4u, queries));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceMultiply_(samples, 16u, output_bytes));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(queries <= std::numeric_limits<u32>::max(), std::errc::value_too_large));
        const bool refined = state.m_precision == TerrainPrecision::qualified_fp64_refinement_experimental;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceAdd_(
                              output_bytes,
                              64u +
                                  sizeof(State::BackgroundBatch) +
                                  sizeof(TerrainBackgroundGpuView) +
                                  (refined ? kTerrainPrecisionRetainedBytes : 0u),
                              candidate_bytes));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(candidate_bytes, reserve_bytes, peak));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak, state.m_buffer_bytes, peak));

        u64 largest = std::max(output_bytes, state.m_largest_buffer_bytes);
        bool source_counted = false;
        std::erase_if(state.m_receiver_generations, [](const auto &generation) { return generation.expired(); });
        for (const auto &weak: state.m_receiver_generations) {
            if (const auto generation = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak, generation->bufferBytes(), peak));
                largest = std::max(largest, generation->largestBufferBytes());
                source_counted = source_counted or generation.get() == source.get();
            }
        }
        if (not source_counted) {
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak, source->bufferBytes(), peak));
            largest = std::max(largest, source->largestBufferBytes());
        }

        std::erase_if(state.m_background_views, [](const auto &view) { return view.expired(); });
        for (const auto &weak: state.m_background_views) {
            if (const auto view = weak.lock()) {
                PPR_RETURN_ERROR_ON_FAIL(RadiancePass, traceAdd_(peak, view->bufferBytes(), peak));
                largest = std::max(largest, view->largestBufferBytes());
            }
        }

        const auto &limits = state.m_device->getInfo().limits;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                peak <= budget_bytes and largest <= max_resource_bytes and output_bytes <= limits.maxBufferSize,
                std::errc::no_buffer_space));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            requireTrace_(
                limits.maxComputeThreadsPerGroup >= 64u and
                    limits.maxComputeThreadGroupSize[0] >= 8u and
                    limits.maxComputeThreadGroupSize[1] >= 8u and
                    (u64{static_cast<u32>(extent.x)} + 7u) / 8u <= limits.maxComputeDispatchThreadGroups[0] and
                    (u64{static_cast<u32>(extent.y)} + 7u) / 8u <= limits.maxComputeDispatchThreadGroups[1],
                std::errc::not_supported));

        auto candidate = std::make_unique<State::BackgroundBatch>();
        candidate->m_view = std::make_shared<TerrainBackgroundGpuView>();
        auto &view = *candidate->m_view;
        view.m_source = std::move(source);
        view.m_camera = camera;
        view.m_extent = extent;
        view.m_bytes = output_bytes + sizeof(TerrainBackgroundGpuView);
        view.m_statistics.m_query_upper_bound = queries;
        auto &parameters = candidate->m_parameters;
        parameters.m_width = static_cast<u32>(extent.x);
        parameters.m_height = static_cast<u32>(extent.y);
        parameters.m_source_width = field.m_width;
        parameters.m_source_height = field.m_height;
        const auto optical = view.m_source->receivers()->geometry()->view();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(optical.m_min_m[0], parameters.m_min_x));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(optical.m_min_m[1], parameters.m_min_y));
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, toFloat_(optical.m_cell_size_m, parameters.m_cell_metres));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              normalOrZero_(parameters.m_min_x) and
                              normalOrZero_(parameters.m_min_y) and
                              normalOrZero_(parameters.m_cell_metres) and
                              parameters.m_cell_metres > 0.0f and
                              double(parameters.m_min_x) == optical.m_min_m[0] and
                              double(parameters.m_min_y) == optical.m_min_m[1] and
                              double(parameters.m_cell_metres) == optical.m_cell_size_m));
        for (u32 row = 0u; row < 4u; ++row) {
            for (u32 column = 0u; column < 4u; ++column) {
                const float value = camera.m_invert_view_projection[row][column];
                PPR_RETURN_ERROR_ON_FAIL(
                    RadiancePass, requireTrace_(normalOrZero_(value), std::errc::argument_out_of_domain));
                parameters.m_inverse[row * 4u + column] = value;
            }
        }

        // Bounded2D receiver domain: affine orthographic homogeneous W=1,
        // camera rays parallel to +Z and a forward plane intersection inside
        // the near/far segment. Pan/zoom preserve these exact zero identities.
        const auto &inverse = camera.m_invert_view_projection;
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
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
                              std::errc::argument_out_of_domain));

        if (not state.m_terrain_pipelines[5]) {
            rhi::ComputePipelineDesc description{};
            description.program = state.m_terrain_programs[5];
            description.label = "validated exact-view cavern backwall lighting";
            rhi::ComPtr<rhi::IComputePipeline> pipeline{};
            PPR_RETURN_ERROR_ON_FAIL(
                RadiancePass, state.m_device->createComputePipeline(description, pipeline.writeRef()));
            state.m_terrain_pipelines[5] = std::move(pipeline);
        }

        auto precision = refined ? createTerrainPrecision_(*state.m_device, queries)
                                 : std::expected<std::unique_ptr<TerrainPrecisionStorage>, std::error_code>{nullptr};
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, precision ? std::error_code{} : precision.error());
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, output_bytes, 16u, nullptr, false, view.m_results));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, 32u, sizeof(u32), nullptr, false, candidate->m_counts));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, traceBuffer_(*state.m_device, 32u, sizeof(u32), nullptr, true, candidate->m_readback));
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass,
            state.m_device->createRootShaderObject(state.m_terrain_programs[5], candidate->m_root.writeRef()));
        rhi::ShaderCursor cursor{candidate->m_root};
        for (const auto &[name, data, size]: std::array<std::tuple<const char *, const void *, std::size_t>, 3>{
                 std::tuple{"g_background_view", &parameters, sizeof(parameters)},
                 {"g_trace", &field.m_trace, sizeof(field.m_trace)},
                 {"g_terrain", &field.m_parameters, sizeof(field.m_parameters)}}) {
            rhi::ShaderCursor constants{};
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor[name].getDereferenced(constants));
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, constants.setData(data, size));
        }

        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, bindTerrainPrecision_(cursor, precision->get()));
        for (const auto &[name, buffer]: std::array<std::pair<const char *, rhi::IBuffer *>, 7>{
                 std::pair{"g_cells", field.m_cells.get()}, {"g_terrain_vertices", field.m_terrain.m_vertices.get()},
                 {"g_contour_points", field.m_terrain.m_points.get()},
                 {"g_contour_cases", field.m_terrain.m_cases.get()}, {"g_background_source", field.m_means.get()},
                 {"g_background_view_results", view.m_results.get()},
                 {"g_background_view_counts", candidate->m_counts.get()}}) {
            PPR_RETURN_ERROR_ON_FAIL(RadiancePass, cursor[name].setBinding(rhi::Binding(buffer, traceRange_(buffer))));
        }

        if (not source_counted) {
            state.m_receiver_generations.push_back(view.m_source);
        }

        // The old submitted cohort is complete. Clear roots before dropping its
        // accounting; externally published sources/views retain immutable buffers.
        state.m_levels.clear();
        state.m_receiver_root.setNull();
        state.m_background_mean_root.setNull();
        state.m_root.setNull();
        state.m_receiver.reset();
        state.m_cascade_diagnostic.reset();
        state.m_cells.setNull();
        state.m_terrain = {};
        state.m_parity = {};
        state.m_directions.setNull();
        state.m_rays.setNull();
        state.m_output.setNull();
        state.m_readback.setNull();
        state.m_segments.setNull();
        state.m_hierarchy = {};
        state.m_precision_storage = std::move(*precision);
        state.m_background_batch = std::move(candidate);
        state.m_buffer_bytes = candidate_bytes;
        state.m_largest_buffer_bytes = output_bytes;
        state.m_prepared = true;
        state.m_submitted = false;
        state.m_cascade_mode = false;
        state.m_last_use = kFenceValueNone;
        return {};
    }

    std::error_code RadianceCascadePass::encodeTerrainBackgroundView(const PreRenderContext &context) {
        const hal::ProfileScope scope{"App.Lighting.EncodeBackgroundView"};
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              m_state and
                                  m_state->m_background_batch and
                                  m_state->m_prepared and
                                  not m_state->m_encoded and
                                  not m_state->m_unknown and
                                  m_state->m_device == &context.m_device and
                                  slotFenceComplete(context.m_completed_fence_value, m_state->m_last_use),
                              std::errc::device_or_resource_busy));
        auto &state = *m_state;
        auto &batch = *state.m_background_batch;

        state.m_encoded = true;
        state.m_submitted = false;
        beginTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());
        context.m_encoder.setBufferState(batch.m_counts, rhi::ResourceState::UnorderedAccess);
        context.m_encoder.clearBuffer(batch.m_counts, traceRange_(batch.m_counts));
        context.m_encoder.setBufferState(batch.m_view->m_results, rhi::ResourceState::UnorderedAccess);
        const auto &field = *batch.m_view->m_source->m_background;
        for (auto *const buffer:
            {field.m_cells.get(), field.m_terrain.m_vertices.get(), field.m_terrain.m_points.get(),
                field.m_terrain.m_cases.get(), field.m_means.get()}) {
            context.m_encoder.setBufferState(buffer, rhi::ResourceState::ShaderResource);
        }
        context.m_encoder.globalBarrier();

        auto *const pass = context.m_encoder.beginComputePass();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, requireTrace_(pass != nullptr, std::errc::io_error));
        pass->bindPipeline(state.m_terrain_pipelines[5], batch.m_root);
        pass->dispatchCompute((batch.m_parameters.m_width + 7u) / 8u, (batch.m_parameters.m_height + 7u) / 8u, 1u);
        pass->end();

        context.m_encoder.globalBarrier();
        context.m_encoder.setBufferState(batch.m_view->m_results, rhi::ResourceState::ShaderResource);
        context.m_encoder.setBufferState(batch.m_counts, rhi::ResourceState::CopySource);
        context.m_encoder.setBufferState(batch.m_readback, rhi::ResourceState::CopyDestination);
        context.m_encoder.copyBuffer(batch.m_readback, 0u, batch.m_counts, 0u, 32u);
        copyTerrainPrecision_(context.m_encoder, state.m_precision_storage.get());
        return {};
    }

    std::expected<std::shared_ptr<const TerrainBackgroundGpuView>, std::error_code>
    RadianceCascadePass::acquireTerrainBackgroundView(const FenceCompletionValue completed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              m_state and
                                  m_state->m_background_batch and
                                  m_state->m_submitted and
                                  not m_state->m_encoded and
                                  not m_state->m_unknown,
                              std::errc::operation_not_permitted));
        auto &state = *m_state;
        auto &batch = *state.m_background_batch;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(slotFenceComplete(completed, state.m_last_use), std::errc::resource_unavailable_try_again));

        // Publication makes the view immutable. A repeat acquisition may only
        // validate lifecycle/completion and return the same strong lease.
        if (batch.m_published) {
            return std::shared_ptr<const TerrainBackgroundGpuView>{batch.m_view};
        }

        const auto precision = readTerrainPrecisionStatistics(completed);
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, precision ? std::error_code{} : precision.error());
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(
                not *precision or ((**precision).m_unresolved == 0u and (**precision).m_bound_error == 0u),
                std::errc::argument_out_of_domain));

        std::array<u32, 8> counts{};
        void *mapped{};
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, state.m_device->mapBuffer(batch.m_readback, rhi::CpuAccessMode::Read, &mapped));
        std::memcpy(counts.data(), mapped, sizeof(counts));
        PPR_RETURN_UNEXPECTED_ON_FAIL(RadiancePass, state.m_device->unmapBuffer(batch.m_readback));
        const u64 expected_samples = u64{batch.m_parameters.m_width} * batch.m_parameters.m_height * 5u;
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass, requireTrace_(
                              counts[0] <= batch.m_view->m_statistics.m_query_upper_bound and
                                  u64{counts[1]} + counts[2] == counts[0] and
                                  counts[3] <= counts[2] and
                                  counts[4] <= counts[5] and
                                  counts[5] == expected_samples and
                                  counts[7] == 0u,
                              std::errc::state_not_recoverable));
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            requireTrace_(counts[2] == 0u and counts[3] == 0u and counts[6] == 0u, std::errc::argument_out_of_domain));

        auto &statistics = batch.m_view->m_statistics;
        statistics.m_attempted = counts[0];
        statistics.m_qualified = counts[1];
        statistics.m_unresolved = counts[2];
        statistics.m_bound_error = counts[3];
        statistics.m_outside_plane = counts[4];
        statistics.m_samples = counts[5];
        statistics.m_invalid_values = counts[6];
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            RadiancePass,
            validateTerrainBackgroundCompletion(statistics, batch.m_parameters.m_width, batch.m_parameters.m_height));

        if (not batch.m_published) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                RadiancePass,
                requireTrace_(state.m_buffer_bytes >= batch.m_view->bufferBytes(), std::errc::state_not_recoverable));
            state.m_background_views.push_back(batch.m_view);
            state.m_buffer_bytes -= batch.m_view->bufferBytes();
            batch.m_root.setNull();
            batch.m_published = true;
            state.m_prepared = false;
        }
        return std::shared_ptr<const TerrainBackgroundGpuView>{batch.m_view};
    }

    std::error_code RadianceCascadePass::shutdown(const FenceCompletionValue completed) {
        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              not m_state or (not m_state->m_encoded and
                                                 not m_state->m_unknown and
                                                 slotFenceComplete(completed, m_state->m_last_use)),
                              std::errc::device_or_resource_busy));
        m_state.reset();
        return {};
    }

    std::error_code RadianceCascadePass::shutdown(const GpuQuiescenceResult &proof) {
        if (not m_state) {
            return {};
        }

        PPR_RETURN_ERROR_ON_FAIL(
            RadiancePass, requireTrace_(
                              proof.m_disposition == GpuQuiescence::kIdleProven or
                                  proof.m_disposition == GpuQuiescence::kRemovalConfirmed,
                              std::errc::state_not_recoverable));
        m_state.reset();
        PPR_RETURN_ERROR_ON_FAIL(RadiancePass, proof.m_error);
        return {};
    }
}
