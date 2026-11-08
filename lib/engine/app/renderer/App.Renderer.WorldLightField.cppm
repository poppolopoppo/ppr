module;
export module engine.app:renderer.world_light_field;

import :renderer.world_light_types;

import engine.core;
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
    // (0,0). Scalar pad fields keep the C++ layout identical to Slang's
    // ConstantBuffer<...> (array padding would otherwise widen pad[3]).
    struct WorldLightFieldConstants {
        float m_origin_x{}, m_origin_y{};
        u32 m_width{}, m_height{};
        u32 m_level_count{};
        float m_cell_metres{};
        u32 m_source_revision{};
        u32 m_frame{};
        u32 m_budget{};
        u32 m_pad0{}, m_pad1{}, m_pad2{};
        std::array<WorldLightLevelGpu, kWorldLightMaxLevels> m_levels{};
    };

    // ------------------------------------------------------------------
    // field view
    // ------------------------------------------------------------------

    // Immutable view of the current field for consumers (Task 8/10): the current
    // and previous radiance buffers (8 B/sample) and the CPU constants mirror.
    // Borrowed pointers; the field owner outlives every view.
    struct WorldLightFieldView {
        rhi::IBuffer *m_radiance{};
        rhi::IBuffer *m_previous_radiance{};
        const WorldLightFieldConstants *m_constants{};
    };

    // ------------------------------------------------------------------
    // temporal cascade field
    // ------------------------------------------------------------------

    // Owns the double-buffered cascade radiance field, per-direction sample
    // counts, and the round-robin work cursor. update() traces one bounded range
    // of direction samples per level through the dense source and accumulates
    // them into the running per-direction mean; a source revision change resets
    // only the intersecting probes (both buffers) before the cursor reprocesses
    // them. There is no readback, publication, or camera input.
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
        // the number of direction samples traced this frame; over-budget work
        // simply lengthens the cycle. dt is the injected frame time (reserved for
        // the Task 11 retention EMA; Task 6 accumulates a plain running mean).
        [[nodiscard]] std::error_code update(
            rhi::ICommandEncoder &encoder, const class WorldLightSourceGpu &source, float dt, u32 sample_budget);

        [[nodiscard]] WorldLightFieldView view() const noexcept;

        // Per-direction sample count buffer (one u32 per direction sample), for
        // explicit fenced test readback only; never read back per frame.
        [[nodiscard]] rhi::IBuffer *state() const noexcept;

        // Complete round-robin passes over the whole field, and the current
        // cursor position within one pass (both for diagnostics/tests only).
        [[nodiscard]] u64 cycle() const noexcept;
        [[nodiscard]] u64 cursor() const noexcept;

        [[nodiscard]] std::error_code shutdown();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
}
