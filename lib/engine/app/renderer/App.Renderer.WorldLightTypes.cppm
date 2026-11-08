module;

export module engine.app:renderer.world_light_types;

import engine.core;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // world light field plan
    // ------------------------------------------------------------------

    inline constexpr u32 kWorldLightMaxLevels = 8u;

    // Immutable world-size and field-scheduling settings. Width/height are in
    // native cells; cell_metres is the native cell edge in metres. A zero
    // level_count requests the default six levels; a zero or non-finite
    // interval_scale requests the default 2.0x doubling ratio. samples_per_frame,
    // accumulation_samples and history_retention are runtime field settings the
    // plan carries but does not use to derive the layout.
    struct WorldLightSettings {
        u32 m_width{};
        u32 m_height{};
        float m_cell_metres{};
        u32 m_level_count{};
        u32 m_samples_per_frame{};
        u32 m_accumulation_samples{};
        float m_history_retention{};
        float m_interval_scale{};
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
        u32 m_level_count{};
        u32 m_width{};
        u32 m_height{};
        float m_cell_metres{};
        u64 m_radiance_samples{};
        u64 m_radiance_bytes{};
        u64 m_state_bytes{};
    };

    // Computes the persistent radiance-cascade layout for a world. Level l uses
    // spacing 2<<l cells and 4<<(2*l) directions; probes are ceil(edge/spacing)
    // per axis, so each level carries probes_x*probes_y*directions samples.
    // Intervals start at zero and grow by interval_scale each level so the
    // coarsest far bound covers 2*(width+height)*cell_metres (a loose bound on
    // the world diagonal). Radiance is 8 bytes per sample (RGB + scalar
    // transmittance); state is one u8 per direction sample plus one u32 per
    // probe. Rejects zero extents, non-finite cell size, excess levels and
    // arithmetic overflow with std::errc::invalid_argument.
    [[nodiscard]] inline Expected<WorldLightPlan> planWorldLight(const WorldLightSettings &settings) noexcept {
        constexpr u32 kDefaultLevelCount = 6u;
        constexpr float kDefaultIntervalScale = 2.0f;

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
        const float interval_scale = (std::isfinite(settings.m_interval_scale) and settings.m_interval_scale > 0.0f)
                                         ? settings.m_interval_scale
                                         : kDefaultIntervalScale;

        const double far_bound_m = 2.0 *
                                   (static_cast<double>(settings.m_width) + static_cast<double>(settings.m_height)) *
                                   static_cast<double>(settings.m_cell_metres);

        // far_0 = far_bound_m / interval_scale^(level_count - 1), so the last
        // level's far bound lands on the full world reach.
        double level_far_m = far_bound_m;
        for (u32 l = 1u; l < level_count; ++l) {
            level_far_m /= double{interval_scale};
        }

        WorldLightPlan plan{};
        plan.m_width = settings.m_width;
        plan.m_height = settings.m_height;
        plan.m_cell_metres = settings.m_cell_metres;
        plan.m_level_count = level_count;

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

            WorldLightLevel &level_out = plan.m_levels[level];
            level_out.m_index = level;
            level_out.m_spacing_cells = spacing;
            level_out.m_directions = directions;
            level_out.m_probes_x = static_cast<u32>(probes_x);
            level_out.m_probes_y = static_cast<u32>(probes_y);
            level_out.m_sample_offset = sample_offset;
            level_out.m_sample_count = sample_count;
            level_out.m_interval_near_m = level == 0u ? 0.0f : static_cast<float>(level_far_m / double{interval_scale});
            level_out.m_interval_far_m = static_cast<float>(level_far_m);

            sample_offset += sample_count;
            if (sample_offset < sample_count) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            total_probes += probe_count;
            if (total_probes < probe_count) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }

            level_far_m *= double{interval_scale};
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
