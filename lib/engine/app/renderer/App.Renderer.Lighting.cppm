module;

export module engine.app:renderer.lighting;

import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // cell-constant numerical optical reference
    // ------------------------------------------------------------------

    using OpticalRgb = std::array<double, 3>;

    enum class OpticalKind : u8 {
        unavailable,
        vacuum,
        opaque,
        absorbing
    };
    enum class OpticalBoundary : u8 {
        unavailable,
        vacuum,
        opaque
    };

    // Linear HDR units. Extinction and volume source density are per metre.
    // Opaque emission is a surface event, evaluated once before transmission stops.
    // Unused fields must be zero; visual alpha/coverage never selects this kind.
    struct OpticalCell {
        OpticalRgb m_extinction{};
        OpticalRgb m_volume_emission{};
        OpticalRgb m_surface_emission{};
        OpticalKind m_kind{OpticalKind::unavailable};
    };

    struct OpticalGridView {
        std::span<const OpticalCell> m_cells{};
        std::array<double, 2> m_min_m{};
        double m_cell_size_m{};
        u32 m_width{};
        u32 m_height{};
        OpticalBoundary m_boundary{OpticalBoundary::unavailable};
    };

    struct OpticalRay {
        std::array<double, 2> m_origin_m{};
        std::array<double, 2> m_direction{};
        double m_near_m{};
        double m_far_m{};
    };

    struct RadianceInterval {
        OpticalRgb m_radiance{};
        OpticalRgb m_transmittance{1.0, 1.0, 1.0};
    };

    [[nodiscard]] std::error_code validateOpticalGrid(const OpticalGridView &grid);
    [[nodiscard]] std::expected<OpticalRay, std::error_code> normalizeOpticalRay(const OpticalRay &ray);

    [[nodiscard]] std::expected<RadianceInterval, std::error_code> composeIntervals(
        const RadianceInterval &near_interval, const RadianceInterval &far_interval);

    // Exact slab integration for small cell-constant fixtures, not contour/matter
    // geometry. Normalizes direction; distances remain metres. Visits all cells,
    // sorts positive-length intersections and has no silent work/step cap.
    // Parallel axes use half-open [min,max) ownership; corner-only hits contribute
    // no length. Starting inside opaque material evaluates its surface event at near.
    // Exterior is explicitly vacuum or black opaque; missing data is an error.
    [[nodiscard]] std::expected<RadianceInterval, std::error_code> traceOpticalReference(
        const OpticalGridView &grid, const OpticalRay &ray);

    // ------------------------------------------------------------------
    // immutable vacuum hierarchy and scalar hierarchical DDA
    // ------------------------------------------------------------------

    struct OpticalTraceResult {
        RadianceInterval m_interval{};
        u64 m_segments{};
        u64 m_vacuum_blocks{};
    };

    class OpticalHierarchy {
    public:
        // Captures validated optical rows. Later caller mutations cannot stale
        // this hierarchy: optical edits require another capture and owner swap.
        [[nodiscard]] static std::expected<OpticalHierarchy, std::error_code> capture(const OpticalGridView &grid);

        OpticalHierarchy(const OpticalHierarchy &) = default;
        OpticalHierarchy &operator=(const OpticalHierarchy &) = default;

        OpticalHierarchy(OpticalHierarchy &&) noexcept = default;
        OpticalHierarchy &operator=(OpticalHierarchy &&) noexcept = default;

    private:
        OpticalHierarchy() = default;

        friend std::expected<OpticalTraceResult, std::error_code> traceOpticalAccelerated(
            const OpticalHierarchy &hierarchy, const OpticalRay &ray);

        std::vector<OpticalCell> m_cells{};
        std::vector<std::vector<u8>> m_vacuum_levels{};

        std::array<double, 2> m_min_m{};
        double m_cell_size_m{};
        u32 m_width{};
        u32 m_height{};
        OpticalBoundary m_boundary{OpticalBoundary::unavailable};
    };

    // Scalar correctness path: largest fully in-domain vacuum blocks only;
    // all other cells integrate physical distance and opaque source-before-stop.
    // Ray-forward cell ownership uses O(log extent) boundary-event searches,
    // followed by exact positive block exits. At most width+height segments;
    // failure to progress returns an error, never a successful partial interval.
    // No runtime speed claim or production GPU integration is implied.
    [[nodiscard]] std::expected<OpticalTraceResult, std::error_code> traceOpticalAccelerated(
        const OpticalHierarchy &hierarchy, const OpticalRay &ray);

    // ------------------------------------------------------------------
    // fixed-lattice RC allocation admission
    // ------------------------------------------------------------------

    struct RcEstimateRequest {
        u64 m_interval_bytes{};
        u64 m_optical_cell_bytes{};
        u64 m_scalar_cell_bytes{};
        // All other simultaneously live storage: hierarchy, feedback, visual tiles,
        // display attachments/history, alignment/driver reserve, etc. Caller-owned.
        u64 m_additional_peak_bytes{};
        u64 m_additional_largest_resource_bytes{};
        u64 m_budget_bytes{};
        u64 m_max_resource_bytes{};
        u32 m_width{};
        u32 m_height{};
        u32 m_cascade_count{};
        // Simultaneously live optical/scalar output generations.
        u32 m_live_generations{};
        // Published/fence-retired C1 fields, excluding the working candidate.
        u32 m_retained_c1_generations{};
        u32 m_c0_tile_width{};
        u32 m_c0_tile_height{};
    };

    struct RcCascadeEstimate {
        u64 m_spacing_cells{};
        u64 m_directions{};
        u64 m_near_cells{};
        u64 m_far_cells{};
        std::array<u64, 2> m_nominal_probes{};
        std::array<u64, 2> m_padded_probes{};
        std::array<u64, 2> m_high_guards{};
        u64 m_logical_records{};
        u64 m_resident_records{};
        u64 m_bytes_per_generation{};
    };

    struct RcResourceEstimate {
        std::vector<RcCascadeEstimate> m_cascades{};
        // Conservative L1 upper bound on every top padded-probe/world-corner
        // Euclidean distance, never shorter than the nominal top far radius.
        // Top rays trace to actual world exit; this is a work bound, not storage.
        u64 m_terminal_far_cells{};
        u64 m_all_levels_bytes_per_generation{};
        u64 m_adjacent_live_bytes{};
        std::array<u64, 2> m_parity_buffer_bytes{};
        // Actual fixed even/odd allocation capacities, not just adjacent live data.
        u64 m_streaming_working_bytes{};
        u64 m_retained_c1_bytes{};
        u64 m_peak_bytes{};
        u64 m_largest_resource_bytes{};
        // A valid estimate remains inspectable when budget/resource admission fails.
        std::error_code m_admission_error{};
    };

    // p_i(k)=0.5+2^i*k in fine cells; upper bilinear support surrounds the
    // original lower probe, not its ray endpoint. Recursive floor/ceil closure
    // gives indices [0,ceil((extent-1)/spacing)] with no clamped lookups.
    // C0 streams one explicit tile. A single solve retains an adjacent working
    // cascade pair plus explicitly retained published C1 fields; each level is
    // logically global. Optical/scalar output generations are counted separately.
    // Only estimates CPU integers; never allocates GPU resources. A zero budget
    // or resource ceiling admits nothing. This is demand, not an 8-GiB allowance.
    [[nodiscard]] std::expected<RcResourceEstimate, std::error_code> estimateRcResources(
        const RcEstimateRequest &request);
}
