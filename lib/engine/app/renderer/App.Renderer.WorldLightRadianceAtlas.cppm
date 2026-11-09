module;
export module engine.app:renderer.world_light_radiance_atlas;

import :renderer.world_light_field;
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
    // compute pass writes the incident radiance L_i of the previous frame's
    // merged field at each cell centre (never emission or albedo), then applies
    // a temporal EMA, so the field's per-hit bounce and the terrain ambient term
    // can later sample one value per cell instead of gathering the cascade
    // directions. The single buffer is read and written in place: each thread
    // owns its own cell, so no ping-pong copy is needed. encode() records work on
    // the caller's encoder; there is no readback or per-frame mapping.
    class WorldLightRadianceAtlas {
    public:
        WorldLightRadianceAtlas();
        ~WorldLightRadianceAtlas();
        WorldLightRadianceAtlas(const WorldLightRadianceAtlas &) = delete;
        WorldLightRadianceAtlas &operator=(const WorldLightRadianceAtlas &) = delete;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
            const std::filesystem::path &content_dir);

        // Records the atlas update from the dense source ids/overlay and the
        // merged field on the caller's encoder. `field` must expose a complete
        // merged field: because the atlas is encoded before the field update in
        // the frame, its m_radiance is the previous frame's merged field, the
        // one-update feedback the design wants. The atlas is not cleared first:
        // every cell is rewritten each dispatch, and vacuum cells are written as
        // exactly zero.
        [[nodiscard]] std::error_code encode(
            rhi::ICommandEncoder &encoder, const WorldLightSourceGpu &source, const WorldLightFieldView &field);

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
