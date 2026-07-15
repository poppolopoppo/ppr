module;
#include "pP/Macros.h"
export module engine.app:service.player;

import engine.core;
import :input.player;

export namespace pP {
    // ------------------------------------------------------------------
    // player-centric input service
    // ------------------------------------------------------------------

    class IPlayerService : public virtual IService {
    public:
        using PlayerCallback = Callback<void(const IPlayerService &, const Player &)>;

        [[nodiscard]] virtual safe_ptr<Player> getPlayer(const PlayerId &id) const noexcept = 0;

        virtual void enumeratePlayers(Collector<safe_ptr<Player>> each_player) const noexcept = 0;

        [[nodiscard]] virtual safe_ptr<Player> getOrCreateKeyboardPlayer() = 0;

        [[nodiscard]] virtual safe_ptr<Player> addGamepadPlayer(u32 user_id, u32 controller_index) = 0;

        [[nodiscard]] virtual bool removePlayer(const PlayerId &id) = 0;

        [[nodiscard]] virtual PlayerCallback::Handle whenPlayerAdded(PlayerCallback::Event on_added) = 0;

        [[nodiscard]] virtual PlayerCallback::Handle whenPlayerRemoved(PlayerCallback::Event on_removed) = 0;
    };

    [[nodiscard]] safe_ptr<IPlayerService> getDefaultPlayerService() noexcept;

    // Test support: resets the singleton's internal player state for test isolation
    void resetDefaultPlayerService() noexcept;
}
