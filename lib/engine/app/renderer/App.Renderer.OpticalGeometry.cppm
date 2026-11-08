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
