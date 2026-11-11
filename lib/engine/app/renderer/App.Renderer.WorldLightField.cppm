module;
export module engine.app:renderer.world_light_field;

import :renderer.world_light_types;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // cascade field constants
    // ------------------------------------------------------------------

    // One cascade level in shader constants, matched field-for-field by the
    // Slang WorldLightLevelGpu in world_light_field.slang. m_probe_offset and
    // m_sample_offset index the shared per-probe and per-sample regions.
    struct WorldLightLevelGpu {
        u32 m_spacing{}, m_directions{};
        u32 m_probe_offset{}, m_sample_offset{};
        u32 m_probes_x{}, m_probes_y{};
        float m_near_m{}, m_far_m{};
    };

    // Field constants, matched field-for-field by the Slang
    // WorldLightFieldConstants. m_origin is the world-space corner of native cell
    // (0,0). m_budget is the weighted ray budget for this frame (every stored
    // direction sample costs four rays). m_frame_delta is the actual field-update
    // interval in seconds.
    struct WorldLightFieldConstants {
        float m_origin_x{}, m_origin_y{};
        u32 m_width{}, m_height{};
        u32 m_level_count{};
        float m_cell_metres{};
        u32 m_source_revision{};
        u32 m_frame{};
        u32 m_budget{};
        float m_frame_delta{};
        // Explicit ABI padding: Slang ConstantBuffer arrays align the levels array
        // to 16 bytes. These are layout slots, not runtime controls.
        u32 m_pad0{}, m_pad1{};
        std::array<WorldLightLevelGpu, kWorldLightMaxLevels> m_levels{};

        // Angular sky at the coarsest level: the source sky hour (negative
        // disables the sky) and the reference sky-intensity scale.
        float m_sky_hour{};
        float m_sky_intensity{};
    };

    // ------------------------------------------------------------------
    // field view
    // ------------------------------------------------------------------

    // Immutable view of the current field for consumers (Task 8/10). m_radiance
    // is the merged field: update() folds every cascade level into level 0 at
    // trace time, so level 0 holds the near-to-far composite and shading samples
    // it alone (the remaining level regions stay exposed through m_constants for
    // debug views and tests but must not be re-composited). m_previous_radiance
    // is the other ping-pong buffer and the per-sample temporal-history input;
    // update() copies it into the write buffer before tracing. The atlas
    // producer reads m_radiance: the atlas is encoded before the field update, so
    // at that point m_radiance is the previous frame's merged field.
    // m_cell_ids/m_overlay/m_materials are the dense source the last update traced
    // (read-only from the consumers' point of view), used by the terrain
    // irradiance's in-place near trace. Borrowed pointers; the field owner and
    // source owner outlive every view.
    struct WorldLightFieldView {
        rhi::IBuffer *m_radiance{};
        rhi::IBuffer *m_previous_radiance{};
        const WorldLightFieldConstants *m_constants{};
        rhi::IBuffer *m_cell_ids{};
        rhi::IBuffer *m_overlay{};
        rhi::IBuffer *m_materials{};
    };

    // ------------------------------------------------------------------
    // temporal cascade field
    // ------------------------------------------------------------------

    // Owns the double-buffered cascade radiance field, per-direction sample
    // counts, and the round-robin work cursor. update() traces bounded direction
    // samples through the dense source and accumulates them into the running
    // per-direction mean; levels are serviced coarsest-to-finest so each traced
    // sample folds the current frame's already-merged parent suffix, and level 0
    // ends up as the fully merged near-to-far field. A zero per-direction count
    // is the persistent invalidation backlog: a source/sky revision clears counts
    // only, leaving the last published radiance/transmittance visible until that
    // sample is traced again. Half of every non-zero frame budget is reserved for
    // ordinary round-robin service so repeated revisions cannot starve refresh.
    // There is no readback, publication, or camera input.
    class WorldLightField {
    public:
        WorldLightField();
        ~WorldLightField();
        WorldLightField(const WorldLightField &) = delete;
        WorldLightField &operator=(const WorldLightField &) = delete;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
            const std::filesystem::path &content_dir);

        // Records the field update on the caller's encoder. sample_budget bounds
        // weighted ray work: every direction sample costs four rays (four sky
        // sub-rays at the coarsest level, four per-parent-corner connector rays
        // elsewhere). At most half the budget is spent on the current dirty/sky
        // revision and the rest is reserved for fair per-level ordinary service.
        // dt is the actual interval since the previous successful field update.
        // Coarse levels overwrite raw traced results; level 0 alone applies the
        // mature exp(-4*dt) temporal EMA. Every traced sample is folded into its
        // parent level's suffix, so the update leaves level 0 as the merged field
        // the view publishes.
        // The atlas holds one packed RGB9E5 radiance word per native cell; its
        // encode() must already be recorded earlier in the same command buffer,
        // and this update transitions it to a shader resource before the trace
        // samples it at each opaque hit. timing is an optional borrowed timestamp
        // pool owned by the caller (which must outlive GPU completion); when
        // non-null it must reserve indices 0..3, written as field entry, after
        // copy+barrier, after the dirty block, and after ordinary trace+barrier.
        [[nodiscard]] std::error_code update(
            rhi::ICommandEncoder &encoder, const class WorldLightSourceGpu &source, rhi::IBuffer &atlas, float dt,
            u32 sample_budget, rhi::IQueryPool *timing = nullptr);

        [[nodiscard]] WorldLightFieldView view() const noexcept;

        // Per-direction count buffer (one u32 per direction sample), for
        // explicit fenced test readback only; never read back per frame. Counts
        // are diagnostic visit metadata; zero is the dirty/fresh sentinel and the
        // persistent invalidation backlog.
        [[nodiscard]] rhi::IBuffer *state() const noexcept;

        // Selects which sampler the evaluate() test entry runs. Composite uses
        // the supplied direction; diffuse uses the normal; specular uses the
        // direction as a 2D reflection with the supplied roughness; irradiance
        // runs the stored level-0-bin reference irradiance used by the terrain
        // material response. ParentVisibility evaluates the level-0 -> level-1
        // parent connector fold at the supplied position/direction; it requires
        // >= 2 levels and the most recent update's source/atlas bindings.
        enum class EvaluateMode : u32 {
            kComposite = 0u,
            kDiffuse = 1u,
            kSpecular = 2u,
            kIrradiance = 3u,
            kParentVisibility = 4u,
        };

        // Records a one-thread sampler evaluation on the caller's encoder: the
        // composite/diffuse/specular/irradiance or parent-visibility response at
        // the supplied position/direction/normal, written to evaluateOutput()
        // (RGB, three floats). Rebinds the current field constants every call so
        // frame-dependent jitter matches the last update. Test-only; there is no
        // production readback path.
        [[nodiscard]] std::error_code evaluate(
            rhi::ICommandEncoder &encoder, const float2 &position, const float2 &direction, const float3 &normal,
            float roughness, EvaluateMode mode);

        // Small output buffer (3 floats) holding the last evaluate() result, for
        // explicit fenced test readback only.
        [[nodiscard]] rhi::IBuffer *evaluateOutput() const noexcept;

        // Complete ordinary rounds over the whole field (four packed-group
        // phase sweeps), and the current group cursor within one sweep (both for
        // diagnostics/tests only).
        [[nodiscard]] u64 cycle() const noexcept;
        [[nodiscard]] u64 cursor() const noexcept;

        [[nodiscard]] std::error_code shutdown();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
