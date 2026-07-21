module;
#include "pP/Macros.h"
module engine.app;

import :input.player;
import std;

namespace pP {
    Player::Player(const PlayerIdentity id) noexcept
        : m_id{id} {
    }

    Player::~Player() noexcept = default;

    void Player::pushDeviceView(SharedInputDevice device) noexcept {
        m_device_views.pushBack(std::move(device));
    }

    std::optional<InputValue> Player::getActionValue(const InputAction &action) const noexcept {
        return m_listener.getActionValue(action);
    }

    void Player::addMapping(SharedInputMapping mapping, int priority) {
        m_listener.addMapping(std::move(mapping), priority);
    }

    void Player::clearFrameMessages() noexcept {
        m_frame_messages.clear();
    }

    void Player::pushFrameMessage(const InputMessage &message) noexcept {
        m_frame_messages.pushBack(message);
    }

    [[nodiscard]] InputFrameSnapshot Player::sample() const noexcept {
        return {
            .m_player_id = m_id,
            .m_messages = m_frame_messages,
        };
    }
}
