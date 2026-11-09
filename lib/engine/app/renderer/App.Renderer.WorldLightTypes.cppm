module;

export module engine.app:renderer.world_light_types;

import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // world light field plan
    // ------------------------------------------------------------------

    inline constexpr u32 kWorldLightMaxLevels = 8u;

    // Reference slider defaults (docs/oni_v22.slang: the penumbra width and sky
    // intensity uniforms). A zero or non-finite setting falls back to these.
    inline constexpr float kWorldLightDefaultPenumbraWidth = 0.5f;
    inline constexpr float kWorldLightDefaultSkyIntensity = 5.0f;

    // Immutable world-size and field-scheduling settings. Width/height are in
    // native cells; cell_metres is the native cell edge in metres. A zero
    // level_count requests the default six levels. penumbra_width is the
    // reference cascade interval scale (cell-space c0 = width * penumbra *
    // 1e-3); sky_intensity scales the angular sky integral at the coarsest
    // level. samples_per_frame, accumulation_samples and history_retention are
    // runtime field settings the plan carries but does not use to derive the
    // layout.
    struct WorldLightSettings {
        u32 m_width{};
        u32 m_height{};
        float m_cell_metres{};
        u32 m_level_count{};
        u32 m_samples_per_frame{};
        u32 m_accumulation_samples{};
        float m_history_retention{};
        float m_penumbra_width{kWorldLightDefaultPenumbraWidth};
        float m_sky_intensity{kWorldLightDefaultSkyIntensity};
    };

    // One radiance-cascade level. Level l is spaced 2^(l+1) cells apart with
    // 4^(l+1) directions; each level stores its own interval radiance
    // [m_interval_near_m, m_interval_far_m). m_sample_offset indexes the shared
    // radiance sample buffer; a level's sample count is probes_x*probes_y
    // directions, equal across levels for a square power-of-two world.
    struct WorldLightLevel {
        u32 m_index{};
        u32 m_spacing_cells{};
        u32 m_directions{};
        u32 m_probes_x{};
        u32 m_probes_y{};
        u64 m_sample_offset{};
        u64 m_sample_count{};
        float m_interval_near_m{};
        float m_interval_far_m{};
    };

    struct WorldLightPlan {
        std::array<WorldLightLevel, kWorldLightMaxLevels> m_levels{};
        u64 m_radiance_samples{};
        u64 m_radiance_bytes{};
        u64 m_state_bytes{};
        u32 m_level_count{};
        u32 m_width{};
        u32 m_height{};
        float m_cell_metres{};
        float m_penumbra_width{};
        float m_sky_intensity{};
    };

    // Computes the persistent radiance-cascade layout for a world. Level l uses
    // spacing 2<<l cells and 4<<(2*l) directions; probes are ceil(edge/spacing)
    // per axis, so each level carries probes_x*probes_y*directions samples.
    // Intervals follow the reference cascade exactly: with
    // c0 = width_cells * penumbra_width * 1e-3 * cell_metres,
    // intervalLength(l) = c0 * (scale + 1) where scale is 0 for level 0 and
    // 4^l/2 for level l >= 1, giving c0, 3c0, 9c0, 33c0, 129c0, 513c0 ... The
    // near bound is intervalLength(l) and the far bound intervalLength(l+1), so
    // level 0 starts at c0 (not zero) and the coarsest far bound is
    // c0 * (2^(2N-1) + 1) (for 1024x1024 at 1 m, c0 = 0.512 m and the reach is
    // 1049.088 m). Radiance is 8 bytes per sample (RGB + scalar transmittance);
    // state is one u8 per direction sample plus one u32 per probe. Rejects zero
    // extents, non-finite cell size or penumbra, excess levels and arithmetic
    // overflow with std::errc::invalid_argument.
    [[nodiscard]] inline Expected<WorldLightPlan> planWorldLight(const WorldLightSettings &settings) noexcept {
        constexpr u32 kDefaultLevelCount = 6u;

        if (settings.m_width == 0u or settings.m_height == 0u) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (not std::isfinite(settings.m_cell_metres) or settings.m_cell_metres <= 0.0f) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        if (settings.m_level_count > kWorldLightMaxLevels) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        const u32 level_count = settings.m_level_count == 0u ? kDefaultLevelCount : settings.m_level_count;
        const float penumbra_width = (std::isfinite(settings.m_penumbra_width) and settings.m_penumbra_width > 0.0f)
                                         ? settings.m_penumbra_width
                                         : kWorldLightDefaultPenumbraWidth;
        const float sky_intensity = (std::isfinite(settings.m_sky_intensity) and settings.m_sky_intensity >= 0.0f)
                                        ? settings.m_sky_intensity
                                        : kWorldLightDefaultSkyIntensity;

        // c0 is the finest interval length in metres. It is proportional to the
        // viewport width in cells, so the cell-space interval lengths
        // (c0/cell_metres) are a function of the world edge and the penumbra
        // width alone: 0.512 cells at the reference 1024-cell world and 0.5
        // penumbra.
        const double c0_m = static_cast<double>(settings.m_width) *
                            static_cast<double>(penumbra_width) *
                            1e-3 *
                            static_cast<double>(settings.m_cell_metres);
        if (not std::isfinite(c0_m) or c0_m <= 0.0) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }

        // intervalLength(l) = c0 * (4^l / 2 + 1) for l >= 1, and c0 for l = 0.
        // The +1 of level 0 is the in-place near interval the shading trace
        // covers; every higher level doubles the angular resolution while its
        // interval length grows by four (the squared angular count), so the
        // cascade tiles the distance axis without overlap.
        const auto interval_length_m = [&](const u32 cascade) noexcept {
            const u64 scale = cascade == 0u ? 0u : (u64{1} << (2u * cascade - 1u));
            return c0_m * static_cast<double>(scale + 1u);
        };

        WorldLightPlan plan{};
        plan.m_width = settings.m_width;
        plan.m_height = settings.m_height;
        plan.m_cell_metres = settings.m_cell_metres;
        plan.m_level_count = level_count;
        plan.m_penumbra_width = penumbra_width;
        plan.m_sky_intensity = sky_intensity;

        u64 sample_offset = 0u;
        u64 total_probes = 0u;

        for (u32 level = 0u; level < level_count; ++level) {
            const u32 spacing = 2u << level;
            const u32 directions = 4u << (2u * level);

            const u64 probes_x = (u64{settings.m_width} + spacing - 1u) / spacing;
            const u64 probes_y = (u64{settings.m_height} + spacing - 1u) / spacing;
            const u64 probe_count = probes_x * probes_y;

            const u64 sample_count = probe_count * u64{directions};
            if (sample_count / probe_count != u64{directions}) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            const double near_m = interval_length_m(level);
            const double far_m = interval_length_m(level + 1u);
            if (not std::isfinite(near_m) or not std::isfinite(far_m) or far_m < near_m) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            WorldLightLevel &level_out = plan.m_levels[level];
            level_out.m_index = level;
            level_out.m_spacing_cells = spacing;
            level_out.m_directions = directions;
            level_out.m_probes_x = static_cast<u32>(probes_x);
            level_out.m_probes_y = static_cast<u32>(probes_y);
            level_out.m_sample_offset = sample_offset;
            level_out.m_sample_count = sample_count;
            level_out.m_interval_near_m = static_cast<float>(near_m);
            level_out.m_interval_far_m = static_cast<float>(far_m);

            sample_offset += sample_count;
            if (sample_offset < sample_count) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            total_probes += probe_count;
            if (total_probes < probe_count) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
        }

        plan.m_radiance_samples = sample_offset;

        const u64 radiance_bytes = sample_offset * 8u;
        if (radiance_bytes / 8u != sample_offset) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        plan.m_radiance_bytes = radiance_bytes;

        const u64 count_bytes = sample_offset * u64{sizeof(u8)};
        const u64 phase_bytes = total_probes * u64{sizeof(u32)};
        if (phase_bytes / u64{sizeof(u32)} != total_probes) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        const u64 state_bytes = count_bytes + phase_bytes;
        if (state_bytes < count_bytes) {
            return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
        }
        plan.m_state_bytes = state_bytes;

        return plan;
    }
}
