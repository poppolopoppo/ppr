module;
#include "pP/Macros.h"
export module engine.app:platform.glfw.player;

import :service.player;
import :player.graph;

import engine.core;
import std;

export namespace pP {
    class GlfwInput;

    class GlfwPlayer final : public IPlayerService {
        safe_ptr<GlfwInput> m_glfw_input{};
        PlayerGraph m_graph{};
        std::mt19937_64 m_id_generator{};

    public:
        GlfwPlayer() noexcept;

        std::error_code initialize(GlfwInput &glfw_input);
        std::error_code shutdown();

        // IPlayerService overrides
        [[nodiscard]] SharedPlayer getPlayer(const PlayerId &id) const noexcept override;
        void enumeratePlayers(Collector<SharedPlayer> each_player) const noexcept override;
        [[nodiscard]] Expected<SharedPlayer> getOrCreateKeyboardPlayer() override;
        [[nodiscard]] Expected<SharedPlayer> addGamepadPlayer(u32 controller_index) override;
        [[nodiscard]] std::error_code removePlayer(const PlayerId &id) override;
        [[nodiscard]] PlayerCallback::Handle whenPlayerAdded(PlayerCallback::Event on_added) override;
        [[nodiscard]] PlayerCallback::Handle whenPlayerRemoved(PlayerCallback::Event on_removed) override;
        void resetPlayers() noexcept;
    };
}
