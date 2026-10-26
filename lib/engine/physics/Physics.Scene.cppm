module;
#include "pP/Macros.h"

export module engine.physics:scene;

import engine.core;
import std;

export namespace pP::physics {
    PPR_DECLARE_LOG_CATEGORY(Physics)

    // ------------------------------------------------------------------
    // body identity
    // ------------------------------------------------------------------

    struct BodyHandle {
        u64 m_entity{};
        u64 m_serial{};

        [[nodiscard]] constexpr bool operator==(const BodyHandle &) const noexcept = default;
    };

    enum class EMotion : u32 {
        static_body,
        kinematic,
        dynamic
    };

    enum class EGeometry : u32 {
        box,
        circle
    };

    // ------------------------------------------------------------------
    // body description
    // ------------------------------------------------------------------

    struct BodyDefinition {
        EMotion m_motion{EMotion::static_body};
        EGeometry m_geometry{EGeometry::box};
        float m_x{};
        float m_y{};
        float m_angle{};
        float m_velocity_x{};
        float m_velocity_y{};
        float m_angular_velocity{};
        float m_half_width{0.5f};
        float m_half_height{0.5f};
        float m_density{1.0f};
        float m_friction{0.6f};
        float m_restitution{};
        u64 m_category_bits{1u};
        u64 m_mask_bits{~u64{0}};
        i32 m_group_index{};
        bool m_enable_sleep{true};
        bool m_is_awake{true};
        bool m_is_bullet{};
        bool m_fixed_rotation{};
    };

    struct BodyState {
        float m_x{};
        float m_y{};
        float m_angle{};
        float m_velocity_x{};
        float m_velocity_y{};
        float m_angular_velocity{};

        [[nodiscard]] constexpr bool operator==(const BodyState &) const noexcept = default;
    };

    // ------------------------------------------------------------------
    // particle coupling snapshots and impulse publication
    // ------------------------------------------------------------------

    struct ParticleCouplingEpoch {
        u64 m_world{};
        u64 m_source_tick{};
        u64 m_terrain_revision{};

        [[nodiscard]] constexpr bool operator==(const ParticleCouplingEpoch &) const noexcept = default;
    };

    /// Numeric snapshot only; no Box2D handle or GPU type crosses this boundary.
    struct BodyBoundarySnapshot {
        BodyHandle m_handle{};
        BodyState m_state{};
        float m_com_x{};
        float m_com_y{};
        float m_half_width{};
        float m_half_height{};
        float m_mass{};
        float m_inertia{};
        EMotion m_motion{EMotion::static_body};
        EGeometry m_geometry{EGeometry::box};
    };

    /// Source state detects an intervening body mutation before any batch applies.
    /// Impulses are physical kg*m/s and kg*m^2/s, never velocity replacements.
    /// These are submitted impulses: Box2D's body constraints/inverse mass and
    /// existing velocity safeguard determine the actual resulting momentum.
    struct ParticleImpulse {
        BodyHandle m_handle{};
        BodyState m_source{};
        float m_x{};
        float m_y{};
        float m_angular{};
    };

    // ------------------------------------------------------------------
    // sensors + contacts
    // ------------------------------------------------------------------

    struct SensorDefinition {
        u64 m_category_bits{1u};
        u64 m_mask_bits{~u64{0}};
        EGeometry m_geometry{EGeometry::box};
        float m_half_width{0.5f};
        float m_half_height{0.5f};
        i32 m_group_index{};
        u32 m_kind{};
        bool m_enabled{true};
    };

    struct SensorEvent {
        u64 m_sensor_entity{};
        u64 m_visitor_entity{};
        u32 m_kind{};
        bool m_begin{};
    };

    /// Entity zero identifies the Scene-owned chunk-chain body.
    struct ContactEvent {
        u64 m_entity_a{};
        u64 m_entity_b{};
        float m_x{};
        float m_y{};
        float m_normal_x{};
        float m_normal_y{};
        bool m_begin{};
        bool m_hit{};
        float m_approach_speed{};
    };

    // ------------------------------------------------------------------
    // chunk chains
    // ------------------------------------------------------------------

    struct ChainPoint {
        float m_x{};
        float m_y{};
    };

    /// Serial identifies one chain in one Scene lifetime; zero is invalid.
    struct ChainHandle {
        u64 m_serial{};

        [[nodiscard]] constexpr bool operator==(const ChainHandle &) const noexcept = default;
    };

    struct ChainDefinition {
        std::span<const ChainPoint> m_points;
        ChainPoint m_ghost_before{};
        ChainPoint m_ghost_after{};
        u32 m_material_index{};
        u32 m_material_count{1u};
        bool m_loop{};
    };

    // ------------------------------------------------------------------
    // scene
    // ------------------------------------------------------------------

    class Scene {
        struct Runtime;
        std::unique_ptr<Runtime> m_runtime;
        u64 m_next_serial{1u};
        u64 m_next_chunk_update{1u};

    public:
        struct Desc {
            float m_gravity_x{};
            float m_gravity_y{-9.8f};
            float m_fixed_dt{1.0f / 60.0f};
            u32 m_substeps{4u};
            bool m_enable_sleep{true};
        };

        Scene() noexcept;

        Scene(const Scene &) = delete;

        Scene &operator=(const Scene &) = delete;

        ~Scene() noexcept;

        [[nodiscard]] std::error_code initialize(Desc desc);

        [[nodiscard]] std::error_code shutdown() noexcept;

        [[nodiscard]] std::error_code dispose() noexcept;

        /// Destroys bodies while chunk chains + chain body survive (ChunkColliders owns chain lifecycle).
        [[nodiscard]] std::error_code clear() noexcept;

        [[nodiscard]] std::error_code step();

        /// A collider cohort may temporarily own old and replacement chains.
        /// No physics step is admitted until it commits or cancels.
        [[nodiscard]] Expected<u64> beginChunkChainUpdate();
        [[nodiscard]] std::error_code finishChunkChainUpdate(u64 token);

        [[nodiscard]] std::error_code createBodies(
            std::span<const u64> entities, std::span<const BodyDefinition> definitions, std::span<BodyHandle> handles);

        [[nodiscard]] std::error_code destroyBody(BodyHandle handle) noexcept;

        /// Ordered points in world space; open chains use explicit exterior ghosts.
        [[nodiscard]] std::expected<ChainHandle, std::error_code> createChunkChain(ChainDefinition definition);

        [[nodiscard]] std::expected<ChainHandle, std::error_code> replaceChunkChain(
            ChainHandle old, ChainDefinition definition);

        [[nodiscard]] std::error_code destroyChunkChain(ChainHandle handle) noexcept;

        [[nodiscard]] std::error_code attachSensor(BodyHandle handle, SensorDefinition definition);

        [[nodiscard]] std::error_code drainSensorEvents(
            std::span<SensorEvent> destination, std::size_t &written) noexcept;

        /// Drains the last step once; a short destination leaves contacts available for retry.
        [[nodiscard]] std::error_code drainContactEvents(std::span<ContactEvent> destination, std::size_t &written);

        [[nodiscard]] std::error_code enableSleep(BodyHandle handle, bool enabled) noexcept;

        [[nodiscard]] std::error_code setAwake(BodyHandle handle, bool awake) noexcept;

        /// Updates an existing movable body; identity, shapes and contacts survive.
        [[nodiscard]] std::error_code setLinearVelocity(BodyHandle handle, float x_m_per_s, float y_m_per_s) noexcept;
        /// Dynamic-body locomotion switches between ordinary gravity and climbing.
        [[nodiscard]] std::error_code setGravityEnabled(BodyHandle handle, bool enabled) noexcept;

        [[nodiscard]] std::expected<bool, std::error_code> isAwake(BodyHandle handle) const noexcept;

        [[nodiscard]] std::expected<BodyState, std::error_code> state(BodyHandle handle) const noexcept;

        [[nodiscard]] Expected<BodyBoundarySnapshot> boundarySnapshot(BodyHandle handle) const;

        /// Arms one world/tick/terrain identity. Physics stepping and terrain
        /// cohort admission pause until its impulse publication or cancellation.
        [[nodiscard]] std::error_code beginParticleCoupling(ParticleCouplingEpoch epoch);
        [[nodiscard]] std::error_code validateParticleImpulses(
            ParticleCouplingEpoch epoch, std::span<const ParticleImpulse> impulses) const;
        [[nodiscard]] std::error_code applyParticleImpulses(
            ParticleCouplingEpoch epoch, std::span<const ParticleImpulse> impulses);
        /// Commits one-way coupling without applying impulses or waking bodies.
        [[nodiscard]] std::error_code commitParticleCoupling(ParticleCouplingEpoch epoch);
        [[nodiscard]] std::error_code cancelParticleCoupling(ParticleCouplingEpoch epoch);
    };
}
