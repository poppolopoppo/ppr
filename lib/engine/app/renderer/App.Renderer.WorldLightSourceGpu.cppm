module;
export module engine.app:renderer.world_light_source_gpu;

import :renderer.world_light_source;
import :renderer.world_light_types;

import engine.core;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::lighting {
    // ------------------------------------------------------------------
    // dense world light source GPU mirror
    // ------------------------------------------------------------------

    // Shader-side world/scheduling constants, matched field-for-field by the
    // Slang `WorldLightSourceConstants` in world_light_source.slang. m_has_overlay
    // carries the number of overlay records staged for the current matter
    // projection (0 = none) so the clear/scatter/finalize passes can bounds-check
    // their record index; m_revision is a diagnostic mirror of the low 32 bits of
    // the committed source revision and is not read by the shaders.
    struct WorldLightSourceConstants {
        u32 m_width{}, m_height{};
        u32 m_chunk_edge{};
        u32 m_occupancy_words{};
        u32 m_revision{};
        u32 m_has_overlay{};
        u32 m_pad0{}, m_pad1{};
    };

    // Dense, RHI-backed mirror of the CPU `WorldLightSource`. Owns the same
    // dense cell ids, global material palette, dense Matter overlay and
    // occupancy bitset, plus the GPU buffers and clear/scatter/finalize/occupancy
    // compute passes. applyUpdate validates and stages an edit; encode records
    // the GPU work on the caller's encoder. Occupancy bits match the CPU oracle
    // exactly (id != 0, or overlay coverage/sigma/emission > 0). A rejected or
    // empty update leaves the previous GPU bytes and revision untouched.
    class WorldLightSourceGpu {
    public:
        WorldLightSourceGpu();
        ~WorldLightSourceGpu();
        WorldLightSourceGpu(const WorldLightSourceGpu &) = delete;
        WorldLightSourceGpu &operator=(const WorldLightSourceGpu &) = delete;

        [[nodiscard]] std::error_code initialize(
            IRhiService &rhi, IShaderService &shaders, const WorldLightPlan &plan,
            std::span<const WorldLightMaterial> materials, const std::filesystem::path &content_dir);

        [[nodiscard]] std::error_code applyUpdate(const WorldLightSourceUpdate &update);

        [[nodiscard]] std::error_code encode(rhi::ICommandEncoder &encoder);

        // GPU buffers, for explicit fenced test readback only (never per-frame
        // in production): dense u32 cell ids, dense GpuOverlayRow overlay, and
        // u32 occupancy words. Null until initialize() succeeds.
        [[nodiscard]] rhi::IBuffer *cellIds() const noexcept;
        [[nodiscard]] rhi::IBuffer *overlay() const noexcept;
        [[nodiscard]] rhi::IBuffer *occupancy() const noexcept;
        // Immutable palette on the device, indexed by cell id - 1 (id 0 = vacuum).
        [[nodiscard]] rhi::IBuffer *materialsGpu() const noexcept;

        // CPU mirrors: the global palette and the committed edit state.
        [[nodiscard]] std::span<const WorldLightMaterial> materials() const noexcept;
        [[nodiscard]] u64 revision() const noexcept;
        [[nodiscard]] WorldLightDirtyRect dirtyRect() const noexcept;

        // The committed terminal sky colour (linear RGB); zero means no sky. A
        // change is observable alongside a revision bump so the field can reset
        // only the coarsest cascade level.
        [[nodiscard]] std::array<float, 3> sky() const noexcept;

        // The committed sky solar hour driving the angular sky phase. A negative
        // hour disables the sky (a zero terminal contribution).
        [[nodiscard]] i32 skyHour() const noexcept;

        [[nodiscard]] std::error_code shutdown();

    private:
        // Grows the persistent host-mapped chunk staging buffer so it holds
        // chunk_count 128x128 slices: every pending chunk upload in one encoded
        // command buffer must read its own slice.
        [[nodiscard]] std::error_code ensureChunkStaging(u32 chunk_count);

        struct State;
        std::unique_ptr<State> m_state;
    };
}
