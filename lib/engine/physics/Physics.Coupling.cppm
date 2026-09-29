module;

export module engine.physics:coupling;

import :scene;
import std;

export namespace pP::physics {
    class Coupling {
        struct PreviousAwake {
            BodyHandle m_handle{};
            bool m_awake{};
        };

        std::vector<PreviousAwake> m_previous{};

    public:
        struct WakeRecord {
            u64 m_entity{};
            bool m_awake{};
        };

        using ContactRecord = ContactEvent;

        /// Publish into caller-owned arrays. Handles must be sorted by entity.
        [[nodiscard]] std::error_code publishUpdates(Scene &scene, std::span<const BodyHandle> handles,
                                                     std::span<BodyState> states, std::span<SensorEvent> sensors,
                                                     std::size_t &sensor_count, std::span<WakeRecord> wakes,
                                                     std::size_t &wake_count, std::span<ContactRecord> contacts,
                                                     std::size_t &contact_count) noexcept;

        /// Reserved boundary for bounded particle forces and torque (not wired).
        [[nodiscard]] std::error_code applyBoundedForce(Scene &scene, BodyHandle handle,
                                                        float force_x, float force_y, float torque) noexcept;

        /// Reserved boundary velocity sample (not wired).
        [[nodiscard]] std::expected<BodyState, std::error_code> sampleBoundaryVelocity(
            const Scene &scene, BodyHandle handle, float x, float y) const noexcept;
    };
}
