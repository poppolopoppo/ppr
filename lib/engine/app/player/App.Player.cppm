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

    using PlayerId = Numeric<u64, class Player>;

    struct PlayerIdentity {
        PlayerId m_user_id{};
        InputDeviceID m_device_id{};
        u16 m_local_index{0};
        EPlayerKind m_kind{EPlayerKind::keyboard};

        [[nodiscard]] constexpr bool operator==(const PlayerIdentity &other) const noexcept = default;

        [[nodiscard]] constexpr std::strong_ordering operator<=>(const PlayerIdentity &other) const noexcept {
            if (const auto cmp = m_kind <=> other.m_kind; cmp != 0) [[unlikely]] return cmp;
            if (const auto cmp = m_local_index <=> other.m_local_index; cmp != 0) [[unlikely]] return cmp;
            return m_user_id <=> other.m_user_id;
        }
    };

    struct InputFrameSnapshot {
        PlayerIdentity m_player_id{};
        StableVector<InputMessage> m_messages{};
    };

    class Player : public safe_object {
        PlayerIdentity m_id{};
        InputListener m_listener{};

        StableVectorInplace<SharedInputDevice> m_device_views{};
        StableVector<InputMessage> m_frame_messages{};

    public:
        explicit Player(PlayerIdentity id) noexcept;

        ~Player() noexcept;

        [[nodiscard]] const PlayerIdentity &getIdentity() const noexcept {
            return m_id;
        }

        [[nodiscard]] const PlayerId &getUserId() const noexcept {
            return m_id.m_user_id;
        }

        [[nodiscard]] InputListener &getListener() noexcept {
            return m_listener;
        }

        [[nodiscard]] const InputListener &getListener() const noexcept {
            return m_listener;
        }

        void pushDeviceView(SharedInputDevice device) noexcept;

        [[nodiscard]] const StableVectorInplace<SharedInputDevice> &getDeviceViews() const noexcept {
            return m_device_views;
        }

        [[nodiscard]] std::optional<InputValue> getActionValue(const InputAction &action) const noexcept;

        void addMapping(SharedInputMapping mapping, int priority);

        void clearFrameMessages() noexcept;

        void pushFrameMessage(const InputMessage &message) noexcept;

        [[nodiscard]] InputFrameSnapshot sample() const noexcept;
    };

    using SharedPlayer = safe_ptr<Player>;
}
