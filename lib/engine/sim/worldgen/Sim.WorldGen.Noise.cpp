module;

#include "pP/Macros.h"
#include <FastNoiseLite.h>

module engine.sim;

import :worldgen.noise;
import engine.core;
import std;

namespace pP::sim {
    using pP::make_error_code;
    PPR_DEFINE_LOG_CATEGORY(WorldNoise, info, none);

    struct NoiseField::Impl {
        FastNoiseLite m_noise;
        FastNoiseLite m_warp;
        bool m_warp_enabled{};
    };

    namespace {
        [[nodiscard]] int noiseSeed(u64 seed) noexcept {
            seed += 0x9e3779b97f4a7c15ull;
            seed = (seed ^ (seed >> 30u)) * 0xbf58476d1ce4e5b9ull;
            seed = (seed ^ (seed >> 27u)) * 0x94d049bb133111ebull;
            seed ^= seed >> 31u;
            static_assert(sizeof(int) == sizeof(u32));
            return std::bit_cast<int>(static_cast<u32>(seed ^ (seed >> 32u)));
        }

        [[nodiscard]] bool validNoiseDescription(const NoiseFieldDesc &desc) noexcept {
            if (desc.m_algorithm > ENoiseAlgorithm::cellular_distance or
                desc.m_fractal > ENoiseFractal::ridged or
                desc.m_octaves == 0u or
                desc.m_octaves > 16u or
                not std::isfinite(desc.m_frequency_per_metre) or
                desc.m_frequency_per_metre < static_cast<double>((std::numeric_limits<float>::min)()) or
                not std::isfinite(desc.m_lacunarity) or
                desc.m_lacunarity < 1.0 or
                desc.m_lacunarity > 4.0 or
                not std::isfinite(desc.m_gain) or
                desc.m_gain < 0.0 or
                desc.m_gain > 1.0 or
                not std::isfinite(desc.m_warp_amplitude_m) or
                desc.m_warp_amplitude_m < 0.0 or
                desc.m_warp_amplitude_m > kNoiseCoordinateLimitMetres or
                not std::isfinite(desc.m_warp_frequency_per_metre) or
                desc.m_warp_frequency_per_metre < static_cast<double>((std::numeric_limits<float>::min)())) {
                return false;
            }
            const double octave_scale = std::pow(desc.m_lacunarity, static_cast<double>(desc.m_octaves - 1u));
            const double coordinates = kNoiseCoordinateLimitMetres + 2.0 * desc.m_warp_amplitude_m;
            // Leave margin for the upstream simplex rotations and integer
            // neighbour arithmetic; reject before narrowing to float/int.
            const double limit = static_cast<double>((std::numeric_limits<i32>::max)()) / 8.0;
            return coordinates * desc.m_frequency_per_metre * octave_scale <= limit and
                   coordinates * desc.m_warp_frequency_per_metre * octave_scale <= limit;
        }
    }

    NoiseField::NoiseField(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {
    }
    NoiseField::~NoiseField() noexcept = default;
    NoiseField::NoiseField(NoiseField &&) noexcept = default;
    NoiseField &NoiseField::operator=(NoiseField &&) noexcept = default;

    float NoiseField::sample(double x_m, double y_m) const noexcept {
        PPR_ASSERT(m_impl != nullptr);
        PPR_ASSERT(std::isfinite(x_m) and std::isfinite(y_m));
        PPR_ASSERT(std::abs(x_m) <= kNoiseCoordinateLimitMetres and std::abs(y_m) <= kNoiseCoordinateLimitMetres);
        if (m_impl->m_warp_enabled) {
            m_impl->m_warp.DomainWarp(x_m, y_m);
        }
        return m_impl->m_noise.GetNoise(x_m, y_m);
    }

    std::error_code validateNoiseFieldDescription(const NoiseFieldDesc &desc) noexcept {
        if (not validNoiseDescription(desc)) {
            PPR_RETURN_ERROR_ON_FAIL(WorldNoise, std::errc::invalid_argument);
        }
        return {};
    }

    Expected<NoiseField> compileNoiseField(const NoiseFieldDesc &desc, const u64 stream_seed) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(WorldNoise, validateNoiseFieldDescription(desc));

        auto impl = std::make_unique<NoiseField::Impl>();
        impl->m_noise.SetSeed(noiseSeed(stream_seed));
        switch (desc.m_algorithm) {
            case ENoiseAlgorithm::open_simplex2:
                impl->m_noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
                break;
            case ENoiseAlgorithm::perlin: impl->m_noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin); break;
            case ENoiseAlgorithm::cellular_distance:
                impl->m_noise.SetNoiseType(FastNoiseLite::NoiseType_Cellular);
                impl->m_noise.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_EuclideanSq);
                impl->m_noise.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance);
                break;
        }
        const FastNoiseLite::FractalType fractals[] = {
            FastNoiseLite::FractalType_None, FastNoiseLite::FractalType_FBm, FastNoiseLite::FractalType_Ridged};
        impl->m_noise.SetFractalType(fractals[static_cast<std::size_t>(desc.m_fractal)]);
        impl->m_noise.SetFrequency(static_cast<float>(desc.m_frequency_per_metre));
        impl->m_noise.SetFractalOctaves(static_cast<int>(desc.m_octaves));
        impl->m_noise.SetFractalLacunarity(static_cast<float>(desc.m_lacunarity));
        impl->m_noise.SetFractalGain(static_cast<float>(desc.m_gain));

        impl->m_warp_enabled = desc.m_warp_amplitude_m > 0.0;
        impl->m_warp.SetSeed(noiseSeed(stream_seed ^ 0x776172705f666e6cull));
        impl->m_warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
        impl->m_warp.SetFractalType(FastNoiseLite::FractalType_DomainWarpProgressive);
        impl->m_warp.SetFractalOctaves(static_cast<int>(desc.m_octaves));
        impl->m_warp.SetFractalLacunarity(static_cast<float>(desc.m_lacunarity));
        impl->m_warp.SetFractalGain(static_cast<float>(desc.m_gain));
        impl->m_warp.SetFrequency(static_cast<float>(desc.m_warp_frequency_per_metre));
        impl->m_warp.SetDomainWarpAmp(static_cast<float>(desc.m_warp_amplitude_m));
        return NoiseField{std::move(impl)};
    }
}
