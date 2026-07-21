module;
#include "pP/Macros.h"
module engine.app;

import :player.graph;
import :input.keyboard;
import :input.mouse;
import :input.gamepad;
import engine.core;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(PlayerGraph, info, none);

    SharedPlayer PlayerGraph::getPlayer(const PlayerId &id) const noexcept {
        if (const auto it = m_players.find(id); it != m_players.end()) [[likely]] {
            return safe_ptr{it->second.get()};
        }
        return default_value_v;
    }

    void PlayerGraph::enumeratePlayers(const Collector<safe_ptr<Player>> each_player) const noexcept {
        for (const auto &player: m_players | std::views::values) {
            each_player(safe_ptr{player.get()});
        }
    }

    std::optional<PlayerId> PlayerGraph::findPlayerForDevice(const InputDeviceID &device_id) const noexcept {
        if (const auto it = m_device_to_player.find(device_id);
            it != m_device_to_player.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    Expected<SharedPlayer> PlayerGraph::getOrCreateKeyboardPlayer(
        const IPlayerService &service, const PlayerId user_id, KeyboardDevice &keyboard, MouseDevice &mouse)
    {
        if (const auto it = m_players.find(user_id); it != m_players.end()) [[likely]] {
            PPR_ASSERT(it->second->getIdentity().m_device_id == keyboard.getInputDeviceID());
            return SharedPlayer{it->second.get()};
        }

        auto player = std::make_unique<Player>(PlayerIdentity{
            .m_user_id = user_id,
            .m_device_id = keyboard.getInputDeviceID(),
            .m_local_index = 0,
            .m_kind = EPlayerKind::keyboard,
        });
        player->pushDeviceView(safe_ptr<const IInputDevice>{&keyboard});
        player->pushDeviceView(safe_ptr<const IInputDevice>{&mouse});

        m_device_to_player.emplace(keyboard.getInputDeviceID(), user_id);
        m_device_to_player.emplace(mouse.getInputDeviceID(), user_id);

        auto ptr = safe_ptr{player.get()};
        m_players.emplace(user_id, std::move(player));

        PPR_RETURN_UNEXPECTED_ON_FAIL(PlayerGraph, m_when_player_added(service, *ptr));
        return ptr;
    }

    Expected<SharedPlayer> PlayerGraph::addGamepadPlayer(
        const IPlayerService &service, const PlayerId user_id, GamepadDevice &gamepad)
    {
        if (const auto it = m_players.find(user_id); it != m_players.end()) [[likely]] {
            PPR_ASSERT(it->second->getIdentity().m_device_id == gamepad.getInputDeviceID());
            return SharedPlayer{it->second.get()};
        }

        auto player = std::make_unique<Player>(PlayerIdentity{
            .m_user_id = user_id,
            .m_device_id = gamepad.getInputDeviceID(),
            .m_local_index = safe_narrowing(gamepad.getControllerIndex()),
            .m_kind = EPlayerKind::gamepad,
        });
        player->pushDeviceView(safe_ptr<const IInputDevice>{&gamepad});

        m_device_to_player.emplace(gamepad.getInputDeviceID(), user_id);

        auto ptr = safe_ptr{player.get()};
        m_players.emplace(user_id, std::move(player));

        PPR_RETURN_UNEXPECTED_ON_FAIL(PlayerGraph, m_when_player_added(service, *ptr));
        return ptr;
    }

    std::error_code PlayerGraph::removePlayer(const IPlayerService &service, const PlayerId &id) {
        if (const auto it = m_players.find(id); it != m_players.end()) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(PlayerGraph, m_when_player_removed(service, *it->second));

            for (auto dit = m_device_to_player.begin(); dit != m_device_to_player.end(); ) {
                if (dit->second == id) {
                    dit = m_device_to_player.erase(dit);
                } else {
                    ++dit;
                }
            }

            m_players.erase(it);
            return default_value_v;
        }
        return make_error_code(std::errc::invalid_argument);
    }

    auto PlayerGraph::whenPlayerAdded(IPlayerService::PlayerCallback::Event on_added)
        -> IPlayerService::PlayerCallback::Handle
    {
        return m_when_player_added.add(std::move(on_added));
    }

    auto PlayerGraph::whenPlayerRemoved(IPlayerService::PlayerCallback::Event on_removed)
        -> IPlayerService::PlayerCallback::Handle
    {
        return m_when_player_removed.add(std::move(on_removed));
    }

    void PlayerGraph::clear() noexcept {
        m_when_player_removed.clear();
        m_when_player_added.clear();
        m_players.clear();
        m_device_to_player.clear();
    }
}
