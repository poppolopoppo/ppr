module;

export module engine.sim:worldgen.noise;

import engine.core;
import std;

export namespace pP::sim {
    // ------------------------------------------------------------------
    // noise vocabulary
    // ------------------------------------------------------------------

    enum class ENoiseAlgorithm : u8 {
        open_simplex2,
        perlin,
        cellular_distance,
        cellular_value
    };
    enum class ENoiseFractal : u8 {
        none,
        fbm,
        ridged,
        billow
    };

    struct NoiseFieldDesc {
        ENoiseAlgorithm m_algorithm{ENoiseAlgorithm::open_simplex2};
        ENoiseFractal m_fractal{ENoiseFractal::none};
        double m_frequency_per_metre{};
        u32 m_octaves{4u};
        double m_lacunarity{2.0};
        double m_gain{0.5};
        double m_warp_amplitude_m{};
        double m_warp_frequency_per_metre{0.006};
        double m_axis_scale_x{1.0};
        double m_axis_scale_y{1.0};
        double m_rotation_degrees{};
    };

    // Covers the fixed world's metre coordinates with room for local masks.
    // Compilation also checks octave/warp lattice coordinates against the
    // private noise implementation's signed integer domain.
    inline constexpr double kNoiseCoordinateLimitMetres = 4096.0;

    // ------------------------------------------------------------------
    // compiled noise field
    // ------------------------------------------------------------------

    class NoiseField {
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        explicit NoiseField(std::unique_ptr<Impl> impl) noexcept;
        friend Expected<NoiseField> compileNoiseField(const NoiseFieldDesc &, u64);

    public:
        ~NoiseField() noexcept;
        NoiseField(NoiseField &&) noexcept;
        NoiseField &operator=(NoiseField &&) noexcept;
        NoiseField(const NoiseField &) = delete;
        NoiseField &operator=(const NoiseField &) = delete;
        /// Immutable and allocation-free; safe to sample concurrently. Inputs
        /// are finite metre coordinates within kNoiseCoordinateLimitMetres.
        [[nodiscard]] float sample(double x_m, double y_m) const noexcept;
    };

    // ------------------------------------------------------------------
    // field compilation
    // ------------------------------------------------------------------

    [[nodiscard]] std::error_code validateNoiseFieldDescription(const NoiseFieldDesc &desc) noexcept;
    [[nodiscard]] Expected<NoiseField> compileNoiseField(const NoiseFieldDesc &desc, u64 stream_seed);
}
