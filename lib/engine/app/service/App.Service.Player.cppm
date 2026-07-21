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
        [[nodiscard]] virtual SharedPlayer getPlayer(const PlayerId &id) const noexcept = 0;

        virtual void enumeratePlayers(Collector<SharedPlayer> each_player) const noexcept = 0;

        [[nodiscard]] virtual Expected<SharedPlayer> getOrCreateKeyboardPlayer() = 0;

        [[nodiscard]] virtual Expected<SharedPlayer> addGamepadPlayer(u32 controller_index) = 0;

        [[nodiscard]] virtual std::error_code removePlayer(const PlayerId &id) = 0;

        using PlayerCallback = Callback<std::error_code (const IPlayerService &, const Player &)>;

        [[nodiscard]] virtual PlayerCallback::Handle whenPlayerAdded(PlayerCallback::Event on_added) = 0;

        [[nodiscard]] virtual PlayerCallback::Handle whenPlayerRemoved(PlayerCallback::Event on_removed) = 0;
    };

    extern template class Callback<std::error_code (const IPlayerService &, const Player &)>;
}
