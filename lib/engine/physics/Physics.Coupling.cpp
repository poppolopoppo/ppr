module;

module engine.physics;

import :coupling;
import std;

namespace pP::physics {
    std::error_code Coupling::publishUpdates(Scene &scene, const std::span<const BodyHandle> handles,
                                             const std::span<BodyState> states, const std::span<SensorEvent> sensors,
                                             std::size_t &sensor_count, const std::span<WakeRecord> wakes,
                                             std::size_t &wake_count, const std::span<ContactRecord> contacts,
                                             std::size_t &contact_count) noexcept {
        sensor_count = 0u;
        wake_count = 0u;
        contact_count = 0u;
        if (handles.size() != states.size() or handles.size() > wakes.size()) {
            return std::make_error_code(std::errc::no_buffer_space);
        }
        try {
            std::vector<PreviousAwake> next;
            std::vector<WakeRecord> transitions;
            std::vector<BodyState> pending;
            next.reserve(handles.size());
            transitions.reserve(handles.size());
            pending.reserve(handles.size());
            for (std::size_t index = 0u; index < handles.size(); ++index) {
                const BodyHandle handle = handles[index];
                if (handle.m_entity == 0u or (index != 0u and handles[index - 1u].m_entity >= handle.m_entity)) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                const auto state = scene.state(handle);
                const auto awake = scene.isAwake(handle);
                if (not state) {
                    return state.error();
                }
                if (not awake) {
                    return awake.error();
                }
                pending.push_back(*state);
                next.push_back({handle, *awake});
                const auto previous = std::ranges::find_if(m_previous, [handle](const PreviousAwake &entry) {
                    return entry.m_handle == handle;
                });
                if (previous != m_previous.end() and previous->m_awake != *awake) {
                    transitions.push_back({handle.m_entity, *awake});
                }
            }
            std::vector<SensorEvent> events(sensors.size());
            std::size_t count{};
            if (const std::error_code error = scene.drainSensorEvents(events, count); error) {
                return error;
            }
            events.resize(count);
            std::ranges::sort(events, {}, [](const SensorEvent &event) {
                return std::tuple{event.m_sensor_entity, event.m_visitor_entity, event.m_kind, event.m_begin};
            });
            std::size_t contacts_written{};
            if (not contacts.empty()) {
                if (const std::error_code error = scene.drainContactEvents(contacts, contacts_written); error) {
                    return error;
                }
            }
            std::ranges::copy(events, sensors.begin());
            std::ranges::copy(pending, states.begin());
            std::ranges::copy(transitions, wakes.begin());
            sensor_count = count;
            wake_count = transitions.size();
            contact_count = contacts_written;
            m_previous = std::move(next);
            return {};
        } catch (const std::bad_alloc &) {
            return std::make_error_code(std::errc::not_enough_memory);
        }
    }

    std::error_code Coupling::applyBoundedForce(Scene &, BodyHandle, float, float, float) noexcept {
        return std::make_error_code(std::errc::function_not_supported);
    }

    std::expected<BodyState, std::error_code> Coupling::sampleBoundaryVelocity(
        const Scene &, BodyHandle, float, float) const noexcept {
        return std::unexpected{std::make_error_code(std::errc::function_not_supported)};
    }
}
