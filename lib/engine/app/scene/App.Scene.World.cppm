module;

export module engine.app:scene.world;

import engine.core;
import engine.math;
import std;

export namespace pP {

    // ------------------------------------------------------------------
    // World interface: exposes collisions and picking generic interfaces
    // ------------------------------------------------------------------

    struct WorldHitResult {
        float3 m_position{};
        float3 m_normal{math::axis_z};
    };

    class IWorld : public safe_object {
    public:
        virtual ~IWorld() = default;

        virtual std::error_code initialize(ServicesStore &services) = 0;
        virtual std::error_code shutdown(ServicesStore &services) = 0;

        virtual std::error_code update(TimeSpan dt) = 0;

        [[nodiscard]] virtual std::optional<WorldHitResult> traceRay(Ray world_ray) const noexcept = 0;
    };

}
