module;
#include "pP/Macros.h"
export module engine.app:input.player;

import std;
import engine.core;
import :input.listener;
import :input.device;
import :input.key;
import :input.action;
import :input.mapping;

export namespace pP {
    enum class EPlayerKind : u8 {
        keyboard = 0,
        gamepad,
    };

    struct PlayerId {
        EPlayerKind m_kind{EPlayerKind::keyboard};
        u16 m_local_index{0u};
        u32 m_user_id{0u};

        [[nodiscard]] constexpr bool operator==(const PlayerId &other) const noexcept {
            return m_kind == other.m_kind
                && m_local_index == other.m_local_index
                && m_user_id == other.m_user_id;
        }

        [[nodiscard]] constexpr std::strong_ordering operator<=>(const PlayerId &other) const noexcept {
            if (const auto cmp = m_kind <=> other.m_kind; cmp != 0) [[unlikely]] return cmp;
            if (const auto cmp = m_local_index <=> other.m_local_index; cmp != 0) [[unlikely]] return cmp;
            return m_user_id <=> other.m_user_id;
        }
    };

    struct DeviceView {
        safe_ptr<const IInputDevice> m_device{};
    };

    struct PlayerIdentity {
        PlayerId m_id{};
        EPlayerKind m_source{EPlayerKind::keyboard};
        InputDeviceID m_device_id{};
        u32 m_user_id{0u};
    };

    struct InputFrameSnapshot {
        PlayerId m_player_id{};
        Array<InputMessage> m_messages{};
    };

    class Player : public safe_object {
        PlayerId m_id{};
        InputListener m_listener{};
        Array<DeviceView> m_device_views{};
        Array<InputMessage> m_frame_messages{};

    public:
        explicit Player(PlayerId id) noexcept;

        ~Player() noexcept;

        [[nodiscard]] const PlayerId &getId() const noexcept {
            return m_id;
        }

        [[nodiscard]] InputListener &getListener() noexcept {
            return m_listener;
        }

        [[nodiscard]] const InputListener &getListener() const noexcept {
            return m_listener;
        }

        void pushDeviceView(safe_ptr<const IInputDevice> device) noexcept;

        [[nodiscard]] ArrayView<const DeviceView> getDeviceViews() const noexcept;

        [[nodiscard]] std::optional<InputValue> getActionValue(const InputAction &action) const noexcept {
            return m_listener.getActionValue(action);
        }

        void addMapping(SharedInputMapping mapping, int priority) {
            m_listener.addMapping(std::move(mapping), priority);
        }

        void clearFrameMessages() noexcept {
            m_frame_messages.clear();
        }

        void pushFrameMessage(const InputMessage &message) noexcept {
            m_frame_messages.push_back(message);
        }

        [[nodiscard]] InputFrameSnapshot sample() const noexcept;
    };
}
