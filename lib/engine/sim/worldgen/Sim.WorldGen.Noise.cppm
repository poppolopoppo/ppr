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

        // Slice 1a: canonical definition — the sole authored noise schema.
        inline static constexpr auto describe() {
            using namespace config;
            const auto metadata = [](const std::string_view name, const std::string_view description,
                                      const std::string_view units = {}, const std::optional<double> slider_min = {},
                                      const std::optional<double> slider_max = {}) constexpr {
                Metadata result{name, description, "World generation", EMutability::startup, ESource::file};
                result.m_units = units;
                result.m_slider_min = slider_min;
                result.m_slider_max = slider_max;
                return result;
            };
            return std::tuple{
                field(
                    &NoiseFieldDesc::m_algorithm, metadata("algorithm", "Base noise algorithm"),
                    EnumDomain<ENoiseAlgorithm, 4>{
                        {EnumEntry<ENoiseAlgorithm>{ENoiseAlgorithm::open_simplex2, "open_simplex2"},
                            EnumEntry<ENoiseAlgorithm>{ENoiseAlgorithm::perlin, "perlin"},
                            EnumEntry<ENoiseAlgorithm>{ENoiseAlgorithm::cellular_distance, "cellular_distance"},
                            EnumEntry<ENoiseAlgorithm>{ENoiseAlgorithm::cellular_value, "cellular_value"}}},
                    EPresence::required),
                field(
                    &NoiseFieldDesc::m_fractal, metadata("fractal", "Fractal octave combination"),
                    EnumDomain<ENoiseFractal, 4>{
                        {EnumEntry<ENoiseFractal>{ENoiseFractal::none, "none"},
                            EnumEntry<ENoiseFractal>{ENoiseFractal::fbm, "fbm"},
                            EnumEntry<ENoiseFractal>{ENoiseFractal::ridged, "ridged"},
                            EnumEntry<ENoiseFractal>{ENoiseFractal::billow, "billow"}}}),
                field(
                    &NoiseFieldDesc::m_frequency_per_metre,
                    metadata("frequency_per_metre", "Base spatial frequency", "1/m", 0.0, 0.1),
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true}, EPresence::required),
                field(
                    &NoiseFieldDesc::m_octaves, metadata("octaves", "Number of fractal noise octaves"),
                    Range<u32>{1u, 16u}),
                field(
                    &NoiseFieldDesc::m_lacunarity, metadata("lacunarity", "Frequency multiplier between octaves"),
                    FloatDomain{1.0, 4.0}),
                field(
                    &NoiseFieldDesc::m_gain, metadata("gain", "Amplitude multiplier between octaves"),
                    FloatDomain{0.0, 1.0}),
                field(
                    &NoiseFieldDesc::m_axis_scale_x, metadata("axis_scale_x", "Horizontal sampling scale"),
                    FloatDomain{0.125, 8.0}),
                field(
                    &NoiseFieldDesc::m_axis_scale_y, metadata("axis_scale_y", "Vertical sampling scale"),
                    FloatDomain{0.125, 8.0}),
                field(
                    &NoiseFieldDesc::m_rotation_degrees,
                    metadata("rotation_degrees", "Sampling coordinate rotation", "degrees"),
                    FloatDomain{-180.0, 180.0}),
                field(
                    &NoiseFieldDesc::m_warp_amplitude_m,
                    metadata("warp_amplitude_m", "Domain warp displacement", "m", 0.0, 100.0),
                    FloatDomain{0.0, 4096.0}),
                field(
                    &NoiseFieldDesc::m_warp_frequency_per_metre,
                    metadata("warp_frequency_per_metre", "Domain warp spatial frequency", "1/m", 0.0, 0.1),
                    FloatDomain{0.0, (std::numeric_limits<double>::max)(), false, true})};
        }
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

    [[nodiscard]] std::error_code validateNoiseFieldDescription(const NoiseFieldDesc &desc) noexcept;
    [[nodiscard]] Expected<NoiseField> compileNoiseField(const NoiseFieldDesc &desc, u64 stream_seed);
}
