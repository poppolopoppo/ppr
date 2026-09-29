module;

export module engine.sim:footprint;

import engine.core;
import std;

export namespace pP::sim {
    enum class EMotionKind : u32 { static_body, kinematic, dynamic };
    enum class EBodyGeometry : u32 { box, circle };

    struct BodyState {
        float m_x{};
        float m_y{};
        float m_angle{};
        float m_velocity_x{};
        float m_velocity_y{};
        float m_angular_velocity{};
    };

    struct BodyDefinition {
        EMotionKind m_motion{EMotionKind::static_body};
        EBodyGeometry m_geometry{EBodyGeometry::box};
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

    struct SolidFootprint {
        u32 m_x{};
        u32 m_y{};
        u32 m_width{};
        u32 m_height{};
    };

    struct Sensor {
        u32 m_kind{};
        bool m_enabled{true};
        u64 m_category_bits{1u};
        u64 m_mask_bits{~u64{0}};
    };

    static_assert(std::is_standard_layout_v<BodyState> and std::is_trivially_copyable_v<BodyState>);
    static_assert(std::is_standard_layout_v<BodyDefinition> and std::is_trivially_copyable_v<BodyDefinition>);
    static_assert(std::is_standard_layout_v<SolidFootprint> and std::is_trivially_copyable_v<SolidFootprint>);
    static_assert(std::is_standard_layout_v<Sensor> and std::is_trivially_copyable_v<Sensor>);
    static_assert(sizeof(BodyState) == 24u);
    static_assert(sizeof(BodyDefinition) == 56u);
    static_assert(sizeof(SolidFootprint) == 16u);
    static_assert(sizeof(Sensor) == 24u);
}
