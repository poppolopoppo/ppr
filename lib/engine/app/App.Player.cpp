module;
#include "pP/Macros.h"
module engine.app;

import :input.player;
import std;

namespace pP {
    Player::Player(const PlayerId id) noexcept
        : m_id{id} {
    }

    Player::~Player() noexcept = default;

    void Player::pushDeviceView(safe_ptr<const IInputDevice> device) noexcept {
        m_device_views.push_back(DeviceView{.m_device = device});
    }

    [[nodiscard]] ArrayView<const DeviceView> Player::getDeviceViews() const noexcept {
        return ArrayView<const DeviceView>{m_device_views.data(), m_device_views.size()};
    }

    [[nodiscard]] InputFrameSnapshot Player::sample() const noexcept {
        InputFrameSnapshot snapshot;
        snapshot.m_player_id = m_id;
        snapshot.m_messages = m_frame_messages;
        return snapshot;
    }
}
