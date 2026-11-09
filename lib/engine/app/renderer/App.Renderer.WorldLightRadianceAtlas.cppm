module;
export module engine.app:renderer.world_light_radiance_atlas;

import :renderer.world_light_source_gpu;
import :renderer.world_light_types;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // per-cell radiance atlas
    // ------------------------------------------------------------------

    // Owns one packed RGB9E5 u32 per native cell, row-major width * height. A
    // compute pass writes emission + albedo * previous atlas value (one bounce)
    // through a temporal EMA, so the field's per-hit bounce can later sample one
    // value per cell instead of gathering the cascade directions. The single
    // buffer is read and written in place: each thread owns its own cell, so no
    // ping-pong copy is needed. encode() records work on the caller's encoder;
    // there is no readback or per-frame mapping.
    class WorldLightRadianceAtlas {
    public:
        WorldLightRadianceAtlas();
        ~WorldLightRadianceAtlas();
        WorldLightRadianceAtlas(const WorldLightRadianceAtlas &) = delete;
        WorldLightRadianceAtlas &operator=(const WorldLightRadianceAtlas &) = delete;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
            const std::filesystem::path &content_dir);

        // Records the atlas update from the dense source ids/overlay/materials on
        // the caller's encoder. The atlas is not cleared first: every cell is
        // rewritten each dispatch, and vacuum cells are written as exactly zero.
        [[nodiscard]] std::error_code encode(rhi::ICommandEncoder &encoder, const WorldLightSourceGpu &source);

        // Packed RGB9E5 atlas, one u32 per native cell, for explicit fenced test
        // readback only (never per frame). Null until initialize() succeeds.
        [[nodiscard]] rhi::IBuffer *cells() const noexcept;

        // Fraction of the previous atlas value retained per 60 Hz frame (0..1).
        // The shader blends with exp(-k * dt), k = -60 * ln(retention).
        void setTemporalRetention(float retention) noexcept;

        [[nodiscard]] std::error_code shutdown();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
