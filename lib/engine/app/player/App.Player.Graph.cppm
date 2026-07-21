module;
#include "pP/Macros.h"
export module engine.app:player.graph;

import :input.player;
import :input.device;
import :service.player;
import engine.core;
import std;

export namespace pP {
    class KeyboardDevice;
    class MouseDevice;
    class GamepadDevice;

    class PlayerGraph : public safe_object {
        FlatMap<PlayerId, std::unique_ptr<Player> > m_players{};
        FlatMap<InputDeviceID, PlayerId> m_device_to_player{};
        IPlayerService::PlayerCallback m_when_player_added{};
        IPlayerService::PlayerCallback m_when_player_removed{};

    public:
        PlayerGraph() noexcept = default;

        [[nodiscard]] SharedPlayer getPlayer(const PlayerId &id) const noexcept;

        void enumeratePlayers(Collector<SharedPlayer> each_player) const noexcept;

        [[nodiscard]] std::optional<PlayerId> findPlayerForDevice(const InputDeviceID &device_id) const noexcept;

        [[nodiscard]] Expected<SharedPlayer> getOrCreateKeyboardPlayer(
            const IPlayerService &service, PlayerId user_id, KeyboardDevice &keyboard, MouseDevice &mouse);

        [[nodiscard]] Expected<SharedPlayer> addGamepadPlayer(
            const IPlayerService &service, PlayerId user_id, GamepadDevice &gamepad);

        [[nodiscard]] std::error_code removePlayer(const IPlayerService &service, const PlayerId &id);

        [[nodiscard]] IPlayerService::PlayerCallback::Handle
        whenPlayerAdded(IPlayerService::PlayerCallback::Event on_added);

        [[nodiscard]] IPlayerService::PlayerCallback::Handle
        whenPlayerRemoved(IPlayerService::PlayerCallback::Event on_removed);

        void clear() noexcept;
    };
}
