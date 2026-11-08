module;

export module engine.app:renderer.optical_geometry;

import :renderer.lighting;
import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // immutable rendered-macro optical qualification inputs
    // ------------------------------------------------------------------

    // Same selected material IDs and validated numeric optics, not an authored
    // catalog. Roundness affects the noise-free macro boundary only.
    struct OpticalTerrainMaterial {
        OpticalCell m_optics{};
        u32 m_material{};
        float m_roundness{};
    };

    // Canonical curve/source coefficient: binary32 RTE of the exact mean of
    // finite nonnegative FP32 endpoints in [0,1]. Preserves authored endpoints;
    // zero is +0, subnormal result bits are defined but do not admit GPU transport.
    // The Bezier reference promotes this coefficient to double before geometry.
    [[nodiscard]] std::expected<float, std::error_code> canonicalTerrainRoundnessMean(float a, float b);

    struct OpticalTerrainView {
        std::span<const u16> m_labels{};
        std::span<const OpticalTerrainMaterial> m_materials{};
        std::array<double, 2> m_min_m{};
        double m_cell_size_m{};
        u32 m_width{}, m_height{}, m_outside_material{};
        OpticalBoundary m_boundary{OpticalBoundary::unavailable};
    };

    class OpticalTerrainSnapshot {
    public:
        // Complete bounded fixture, not a production world allocation limit.
        static constexpr u32 kMaxQualificationSamples{4096u};

        [[nodiscard]] static std::expected<OpticalTerrainSnapshot, std::error_code> capture(
            const OpticalTerrainView &input);

        [[nodiscard]] OpticalTerrainView view() const noexcept;

    private:
        OpticalTerrainSnapshot() = default;

        std::vector<OpticalTerrainMaterial> m_materials{};
        std::vector<u16> m_labels{};
        std::array<double, 2> m_min_m{};
        double m_cell_size_m{};
        u32 m_width{}, m_height{}, m_outside_material{};
        OpticalBoundary m_boundary{OpticalBoundary::unavailable};
    };

    // ------------------------------------------------------------------
    // owned boundary receivers and visual continuation candidates
    // ------------------------------------------------------------------

    // Physical contact uses the owned dual-cell/curve key. The separate rounded
    // visual centre/direction are used ONLY for the FP32 continuation policy.
    struct TerrainReceiverFeature {
        i32 m_base_x{}, m_base_y{};
        u32 m_curve{}, m_component{};
        float m_visual_x{}, m_visual_y{};

        // Historical wire-slot name retained for existing layout/diagnostic
        // consumers: [0]/[1] now encode normalized visual-outward X/Y as FP32
        // bits (offsets24/28). Never physical contact, transport or gameplay.
        u32 m_reserved[2]{};
    };

    struct TerrainReceiverCellRange {
        u32 m_offset{}, m_count{};
    };

    struct TerrainReceiverComponentRange {
        u32 m_component{}, m_offset{}, m_count{}, m_corner_mask{};
    };

    class TerrainReceiverSet {
    public:
        // Initial complete scene: <=4096 samples, vacuum ring/halo, and common
        // nonzero-label source RGB per dual cell. Exposes every un-clipped outer
        // arc/straight midpoint; uncertain promoted midpoint precision is an
        // explicit domain failure. General differing-source curves remain pending.
        [[nodiscard]] static std::expected<TerrainReceiverSet, std::error_code> capture(
            std::shared_ptr<const OpticalTerrainSnapshot> geometry);

        [[nodiscard]] const std::shared_ptr<const OpticalTerrainSnapshot> &geometry() const noexcept;
        [[nodiscard]] std::span<const TerrainReceiverFeature> features() const noexcept;
        [[nodiscard]] std::span<const TerrainReceiverCellRange> cells() const noexcept;
        [[nodiscard]] std::span<const TerrainReceiverComponentRange> components() const noexcept;
        [[nodiscard]] std::span<const u32> candidates() const noexcept;
        [[nodiscard]] std::expected<std::array<double, 2>, std::error_code> point(u32 feature) const;

    private:
        std::shared_ptr<const OpticalTerrainSnapshot> m_geometry{};
        std::vector<TerrainReceiverFeature> m_features{};
        std::vector<TerrainReceiverCellRange> m_cells{};
        std::vector<TerrainReceiverComponentRange> m_components{};
        std::vector<u32> m_candidates{};
    };

    struct OpticalTerrainTrace {
        RadianceInterval m_interval{};
        std::optional<double> m_hit_m{};
        u64 m_dual_cells{};
        u32 m_material{};
    };

    // ------------------------------------------------------------------
    // physical and parametric reference queries
    // ------------------------------------------------------------------

    // Internal RC reference convention: fine-cell coordinates and the uploaded
    // raw direction, with no normalization. p(lambda)=origin+lambda*direction.
    // The physical hit distance is lambda*length(direction)*cell_size_m.
    struct OpticalTerrainParametricRay {
        std::array<double, 2> m_origin_cells{};
        std::array<double, 2> m_direction{};
        double m_near_lambda{}, m_far_lambda{};
    };

    [[nodiscard]] std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainParametricReference(
        const OpticalTerrainSnapshot &snapshot, const OpticalTerrainParametricRay &ray);

    // Owned midpoint witness: excludes only its algebraically known zero
    // contact. The first positive interval still tests the complete occupied
    // region; inward connectors emit immediately and later hits remain active.
    [[nodiscard]] std::expected<OpticalTerrainTrace, std::error_code> traceTerrainReceiverConnectorReference(
        const TerrainReceiverSet &receivers, u32 feature, std::array<double, 2> end_cells);

    // Segment start+t*(end-start), t in [0,1]. Only equal finite endpoints
    // denote identity; a collapsed/nonfinite direction is an explicit error.
    [[nodiscard]] std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainConnectorReference(
        const OpticalTerrainSnapshot &snapshot, std::array<double, 2> start_cells, std::array<double, 2> end_cells);

    // Enumerates intersected dual cells independently of GPU traversal. Analytic
    // quadratic events in double; no noise/AA/alpha or cell-box substitution.
    // Halo labels shape the half-cell in-world edge; the separate exterior
    // policy is vacuum or BLACK opaque, without incoming environment radiance.
    // Positive intervals use [near,far); isolated tangencies have no optical
    // length. Numerically uncertain tangent/collinear classifications report
    // argument_out_of_domain, never a successful miss or an epsilon repair.
    [[nodiscard]] std::expected<OpticalTerrainTrace, std::error_code> traceOpticalTerrainReference(
        const OpticalTerrainSnapshot &snapshot, const OpticalRay &ray);
}
