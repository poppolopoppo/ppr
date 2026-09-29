module;

export module engine.physics:scene;

import engine.core;
import std;

export namespace pP::physics {
    struct BodyHandle {
        u64 m_entity{};
        u64 m_serial{};

        [[nodiscard]] constexpr bool operator==(const BodyHandle &) const noexcept = default;
    };

    enum class EMotion : u32 { static_body, kinematic, dynamic };

    enum class EGeometry : u32 { box, circle };

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

    struct SensorDefinition {
        EGeometry m_geometry{EGeometry::box};
        float m_half_width{0.5f};
        float m_half_height{0.5f};
        u64 m_category_bits{1u};
        u64 m_mask_bits{~u64{0}};
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

    class Scene {
        struct Runtime;
        std::unique_ptr<Runtime> m_runtime;
        u64 m_next_serial{1u};

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

        [[nodiscard]] std::error_code initialize(Desc desc) noexcept;

        [[nodiscard]] std::error_code shutdown() noexcept;

        [[nodiscard]] std::error_code dispose() noexcept;

        /// Destroys bodies while chunk chains + chain body survive (ChunkColliders owns chain lifecycle).
        [[nodiscard]] std::error_code clear() noexcept;

        [[nodiscard]] std::error_code step() noexcept;

        [[nodiscard]] std::error_code createBodies(std::span<const u64> entities,
                                                   std::span<const BodyDefinition> definitions, std::span<BodyHandle> handles);

        [[nodiscard]] std::error_code destroyBody(BodyHandle handle) noexcept;

        /// Ordered points in world space; open chains use explicit exterior ghosts.
        [[nodiscard]] std::expected<ChainHandle, std::error_code> createChunkChain(ChainDefinition definition) noexcept;

        [[nodiscard]] std::expected<ChainHandle, std::error_code> replaceChunkChain(ChainHandle old,
                                                                                    ChainDefinition definition) noexcept;

        [[nodiscard]] std::error_code destroyChunkChain(ChainHandle handle) noexcept;

        [[nodiscard]] std::error_code attachSensor(BodyHandle handle, SensorDefinition definition) noexcept;

        [[nodiscard]] std::error_code drainSensorEvents(std::span<SensorEvent> destination, std::size_t &written) noexcept;

        /// Drains the last step once; a short destination leaves contacts available for retry.
        [[nodiscard]] std::error_code drainContactEvents(std::span<ContactEvent> destination, std::size_t &written) noexcept;

        [[nodiscard]] std::error_code enableSleep(BodyHandle handle, bool enabled) noexcept;

        [[nodiscard]] std::error_code setAwake(BodyHandle handle, bool awake) noexcept;

        [[nodiscard]] std::expected<bool, std::error_code> isAwake(BodyHandle handle) const noexcept;

        [[nodiscard]] std::expected<BodyState, std::error_code> state(BodyHandle handle) const noexcept;
    };
}
