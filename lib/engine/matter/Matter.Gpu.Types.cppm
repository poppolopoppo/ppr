module;
#include "pP/Macros.h"

export module engine.matter:gpu.types;

import std;
import engine.core;

export namespace pP::matter {
    // ------------------------------------------------------------------
    // source material identities and fixed solver profile
    // ------------------------------------------------------------------

    enum class ESourceMaterial : u32 {
        liquid = 0,
        elastic = 1,
        sand = 2,
        visco = 3,
    };

    enum class EStaticShape : u32 {
        box = 0,
        circle = 1,
    };

    inline constexpr u32 kParticleDispatchSize = 64u;
    inline constexpr u32 kGridDispatchSize = 8u;
    inline constexpr u32 kSourceIterations = 2u;
    inline constexpr u32 kGameSubsteps = 8u;

    // ------------------------------------------------------------------
    // explicit GPU storage protocol
    // ------------------------------------------------------------------

    /// Source WGSL column components, with no implicit engine/Slang matrix ABI.
    /// A source product A*v is (c0x*x+c1x*y, c0y*x+c1y*y).
    struct SourceMatrix {
        float m_c0x{1.0f};
        float m_c0y{};
        float m_c1x{};
        float m_c1y{1.0f};
    };

    /// All source semantic particle fields, in cell/displacement units. This is
    /// the engine's 88-byte semantic record (CPU rows, snapshots, checkpoints and
    /// the v5 wire format); it is not the device storage layout, which is split
    /// into ParticleWrite and ParticleReadonly below.
    /// Stable ID, original material and physical inventory live in owned metadata.
    struct GpuParticle {
        float m_position_x{};
        float m_position_y{};
        float m_displacement_x{};
        float m_displacement_y{};
        SourceMatrix m_deformation_gradient{};
        SourceMatrix m_deformation_displacement{0.0f, 0.0f, 0.0f, 0.0f};
        float m_liquid_density{1.0f};
        float m_mass{1.0f};
        float m_material{};
        float m_volume{1.0f};
        float m_lambda{};
        float m_log_jp{1.0f};
        float m_color_r{};
        float m_color_g{};
        float m_color_b{};
        float m_enabled{1.0f};
    };

    // ------------------------------------------------------------------
    // device hot and cold row layout
    // ------------------------------------------------------------------

    /// Mutable 64-byte device row: exactly the fields the solver may advance in
    /// a dispatch. Deliberately reordered against the semantic record so the
    /// liquid scalars stay in the hot write set.
    struct ParticleWrite {
        float m_position_x{};
        float m_position_y{};
        float m_displacement_x{};
        float m_displacement_y{};
        SourceMatrix m_deformation_gradient{};
        SourceMatrix m_deformation_displacement{0.0f, 0.0f, 0.0f, 0.0f};
        float m_liquid_density{1.0f};
        float m_log_jp{1.0f};
        float m_lambda{};
        float m_enabled{1.0f};
    };

    /// Immutable 32-byte device row. Both reserved words are uploaded as zero and
    /// never participate in pack/unpack; they only keep the fixed 8-word stride.
    struct ParticleReadonly {
        float m_mass{1.0f};
        float m_volume{1.0f};
        float m_material{};
        float m_reserved0{};
        float m_color_r{};
        float m_color_g{};
        float m_color_b{};
        float m_reserved1{};
    };

    /// Named-field device pack. Never a byte copy: hot and cold reorder the record.
    [[nodiscard]] inline ParticleWrite packWrite(const GpuParticle &particle) noexcept {
        ParticleWrite row{};
        row.m_position_x = particle.m_position_x;
        row.m_position_y = particle.m_position_y;
        row.m_displacement_x = particle.m_displacement_x;
        row.m_displacement_y = particle.m_displacement_y;
        row.m_deformation_gradient = particle.m_deformation_gradient;
        row.m_deformation_displacement = particle.m_deformation_displacement;
        row.m_liquid_density = particle.m_liquid_density;
        row.m_log_jp = particle.m_log_jp;
        row.m_lambda = particle.m_lambda;
        row.m_enabled = particle.m_enabled;
        return row;
    }

    /// Named-field cold pack; the reserved words are always written as zero.
    [[nodiscard]] inline ParticleReadonly packReadonly(const GpuParticle &particle) noexcept {
        ParticleReadonly row{};
        row.m_mass = particle.m_mass;
        row.m_volume = particle.m_volume;
        row.m_material = particle.m_material;
        row.m_reserved0 = 0.0f;
        row.m_color_r = particle.m_color_r;
        row.m_color_g = particle.m_color_g;
        row.m_color_b = particle.m_color_b;
        row.m_reserved1 = 0.0f;
        return row;
    }

    /// Reconstruction of the sole semantic record from one device row pair.
    [[nodiscard]] inline GpuParticle unpack(const ParticleWrite &write, const ParticleReadonly &readonly) noexcept {
        GpuParticle particle{};
        particle.m_position_x = write.m_position_x;
        particle.m_position_y = write.m_position_y;
        particle.m_displacement_x = write.m_displacement_x;
        particle.m_displacement_y = write.m_displacement_y;
        particle.m_deformation_gradient = write.m_deformation_gradient;
        particle.m_deformation_displacement = write.m_deformation_displacement;
        particle.m_liquid_density = write.m_liquid_density;
        particle.m_mass = readonly.m_mass;
        particle.m_material = readonly.m_material;
        particle.m_volume = readonly.m_volume;
        particle.m_lambda = write.m_lambda;
        particle.m_log_jp = write.m_log_jp;
        particle.m_color_r = readonly.m_color_r;
        particle.m_color_g = readonly.m_color_g;
        particle.m_color_b = readonly.m_color_b;
        particle.m_enabled = write.m_enabled;
        return particle;
    }

    /// Ordered static source collider. Only box/circle collider functionality
    /// is admitted; browser emission, mouse forces and drains are excluded.
    struct GpuStaticShape {
        float m_position_x{};
        float m_position_y{};
        float m_half_size_x{};
        float m_half_size_y{};
        float m_radius{};
        float m_rotation_degrees{};
        EStaticShape m_shape{EStaticShape::box};
        u32 m_reserved{};
    };

    /// Four full constant-buffer rows, mirrored by shader Parameters. Defaults
    /// are the pin's solver defaults; the game explicitly configures shared gravity.
    struct GpuParameters {
        u32 m_grid_width{};
        u32 m_grid_height{};
        u32 m_particle_count{};
        u32 m_shape_count{};
        float m_delta_time{1.0f / 480.0f};
        float m_elasticity_ratio{1.0f};
        float m_gravity_strength{2.5f};
        float m_liquid_relaxation{2.0f};
        float m_elastic_relaxation{1.5f};
        float m_liquid_viscosity{0.01f};
        u32 m_fixed_point_multiplier{10'000'000u};
        u32 m_use_grid_volume_for_liquid{1u};
        float m_friction_angle{30.0f};
        float m_plasticity{};
        u32 m_reserved0{};
        u32 m_reserved1{};
    };

    // ------------------------------------------------------------------
    // contour terrain and predicted-body extension protocol
    // ------------------------------------------------------------------

    /// One mask per unit dual cell; its origin is the lower-left sample centre
    /// in solver cells. Cases/points are uploaded from engine.math unchanged.
    /// The caller supplies the complete contour halo, including world edges.
    struct TerrainView {
        std::span<const u32> m_masks{};
        u32 m_width{};
        u32 m_height{};
        float m_origin_x{};
        float m_origin_y{};
    };

    /// Predicted END-pose boundary for one substep. Shape angles follow the
    /// source's world-to-local rotation convention (negative Box2D angle).
    /// COM and velocity use solver cells and cells/second; omega uses radians/s.
    struct GpuBodyBoundary {
        GpuStaticShape m_shape{};
        float m_com_x{};
        float m_com_y{};
        float m_velocity_x{};
        float m_velocity_y{};
        float m_angular_velocity{};
        u32 m_reserved0{};
        u32 m_reserved1{};
        u32 m_reserved2{};
    };

    /// Separate constant-buffer rows keep the upstream solver profile unchanged.
    struct GpuEnvironment {
        u32 m_terrain_width{};
        u32 m_terrain_height{};
        u32 m_body_count{};
        u32 m_body_base{};
        float m_terrain_origin_x{};
        float m_terrain_origin_y{};
        u32 m_has_terrain{};
        u32 m_reserved{};
    };

    // Structured-buffer and constant-buffer contracts are separate. Every field
    // uses scalar four-byte alignment; no C++ bool, Mango vector, or float3 padding.
    static_assert(std::is_standard_layout_v<SourceMatrix> and std::is_trivially_copyable_v<SourceMatrix>);
    static_assert(sizeof(SourceMatrix) == 16u and alignof(SourceMatrix) == 4u);
    static_assert(PPR_OFFSETOF(SourceMatrix, m_c0y) == 4u and PPR_OFFSETOF(SourceMatrix, m_c1x) == 8u);
    static_assert(std::is_standard_layout_v<GpuParticle> and std::is_trivially_copyable_v<GpuParticle>);
    static_assert(sizeof(GpuParticle) == 88u and alignof(GpuParticle) == 4u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_deformation_gradient) == 16u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_deformation_displacement) == 32u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_liquid_density) == 48u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_lambda) == 64u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_color_r) == 72u);
    static_assert(PPR_OFFSETOF(GpuParticle, m_enabled) == 84u);
    static_assert(std::is_standard_layout_v<ParticleWrite> and std::is_trivially_copyable_v<ParticleWrite>);
    static_assert(sizeof(ParticleWrite) == 64u and alignof(ParticleWrite) == 4u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_position_x) == 0u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_position_y) == 4u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_displacement_x) == 8u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_displacement_y) == 12u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_deformation_gradient) == 16u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_deformation_displacement) == 32u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_liquid_density) == 48u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_log_jp) == 52u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_lambda) == 56u);
    static_assert(PPR_OFFSETOF(ParticleWrite, m_enabled) == 60u);
    static_assert(std::is_standard_layout_v<ParticleReadonly> and std::is_trivially_copyable_v<ParticleReadonly>);
    static_assert(sizeof(ParticleReadonly) == 32u and alignof(ParticleReadonly) == 4u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_mass) == 0u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_volume) == 4u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_material) == 8u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_reserved0) == 12u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_color_r) == 16u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_color_g) == 20u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_color_b) == 24u);
    static_assert(PPR_OFFSETOF(ParticleReadonly, m_reserved1) == 28u);
    static_assert(std::is_standard_layout_v<GpuStaticShape> and std::is_trivially_copyable_v<GpuStaticShape>);
    static_assert(sizeof(GpuStaticShape) == 32u and alignof(GpuStaticShape) == 4u);
    static_assert(PPR_OFFSETOF(GpuStaticShape, m_radius) == 16u and PPR_OFFSETOF(GpuStaticShape, m_shape) == 24u);
    static_assert(std::is_standard_layout_v<GpuParameters> and std::is_trivially_copyable_v<GpuParameters>);
    static_assert(sizeof(GpuParameters) == 64u and alignof(GpuParameters) == 4u);
    static_assert(PPR_OFFSETOF(GpuParameters, m_delta_time) == 16u);
    static_assert(PPR_OFFSETOF(GpuParameters, m_elastic_relaxation) == 32u);
    static_assert(PPR_OFFSETOF(GpuParameters, m_fixed_point_multiplier) == 40u);
    static_assert(PPR_OFFSETOF(GpuParameters, m_friction_angle) == 48u);

    static_assert(std::is_standard_layout_v<GpuBodyBoundary> and std::is_trivially_copyable_v<GpuBodyBoundary>);
    static_assert(sizeof(GpuBodyBoundary) == 64u and alignof(GpuBodyBoundary) == 4u);
    static_assert(PPR_OFFSETOF(GpuBodyBoundary, m_com_x) == 32u);
    static_assert(PPR_OFFSETOF(GpuBodyBoundary, m_angular_velocity) == 48u);
    static_assert(PPR_OFFSETOF(GpuEnvironment, m_reserved) == 28u);
    static_assert(std::is_standard_layout_v<GpuEnvironment> and std::is_trivially_copyable_v<GpuEnvironment>);
    static_assert(sizeof(GpuEnvironment) == 32u and alignof(GpuEnvironment) == 4u);
    static_assert(PPR_OFFSETOF(GpuEnvironment, m_terrain_origin_x) == 16u);
}
