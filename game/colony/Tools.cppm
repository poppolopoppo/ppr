export module game.colony.tools;

import engine.core;
import std;

export namespace pP::colony {
    // ------------------------------------------------------------------
    // physical brush settings and commands
    // ------------------------------------------------------------------

    struct ToolSettings {
        std::string m_spawn_material{"water"};
        double m_radius_m{2.0};
        double m_acceleration_m_s2{100.0};
        u32 m_spawn_count{64u};
    };

    enum class EParticleToolKind : u8 {
        spawn,
        destroy,
    };

    struct ParticleToolCommand {
        double m_x_m{};
        double m_y_m{};
        double m_radius_m{};
        u32 m_count{};
        u16 m_material{};
        EParticleToolKind m_kind{EParticleToolKind::spawn};
    };

    /// Signed radial acceleration: positive pushes away, negative pulls inward.
    /// A zero acceleration disables the brush without retaining a gesture.
    struct RadialForce {
        double m_x_m{};
        double m_y_m{};
        double m_radius_m{};
        double m_acceleration_m_s2{};
    };

    [[nodiscard]] std::error_code validateToolSettings(const ToolSettings &settings);
}
