module;
#include "pP/Macros.h"
module engine.app;

import :platform.glfw.player;
import :player.graph;
import :input.keyboard;
import :input.mouse;
import :input.gamepad;
import :service.player;
import std;

namespace pP {
    GlfwPlayer::GlfwPlayer(GlfwInput &input) noexcept
        : m_input(&input), m_graph(&input.m_graph), m_id_generator(randomNumberGenerator()) {
        input.m_player_service = safe_ptr<IPlayerService>{this};
    }

    SharedPlayer GlfwPlayer::getPlayer(const PlayerId &id) const noexcept {
        return m_graph->getPlayer(id);
    }

    void GlfwPlayer::enumeratePlayers(const Collector<SharedPlayer> each_player) const noexcept {
        m_graph->enumeratePlayers(each_player);
    }

    Expected<SharedPlayer> GlfwPlayer::getOrCreateKeyboardPlayer() {
        KeyboardDevice &keyboard = m_input->m_keyboard;
        if (const auto existing_player = m_graph->findPlayerForDevice(keyboard.getInputDeviceID())) {
            return m_graph->getPlayer(existing_player.value());
        }

        const PlayerId user_id{m_id_generator()};
        return m_graph->getOrCreateKeyboardPlayer(
            *this, user_id, keyboard, m_input->m_mouse);
    }

    Expected<SharedPlayer> GlfwPlayer::addGamepadPlayer(const u32 controller_index) {
        if (controller_index >= m_input->m_gamepads.size()) [[unlikely]] {
            return std::unexpected{make_error_code(std::errc::invalid_argument)};
        }

        GamepadDevice &gamepad = m_input->m_gamepads[controller_index];
        PPR_ASSERT(gamepad.getControllerIndex() == controller_index);

        if (const auto existing_player = m_graph->findPlayerForDevice(gamepad.getInputDeviceID())) {
            return m_graph->getPlayer(existing_player.value());
        }

        const PlayerId user_id{m_id_generator()};
        return m_graph->addGamepadPlayer(*this, user_id, gamepad);
    }

    std::error_code GlfwPlayer::removePlayer(const PlayerId &id) {
        return m_graph->removePlayer(*this, id);
    }

    auto GlfwPlayer::whenPlayerAdded(PlayerCallback::Event on_added) -> PlayerCallback::Handle {
        return m_graph->whenPlayerAdded(std::move(on_added));
    }

    auto GlfwPlayer::whenPlayerRemoved(PlayerCallback::Event on_removed) -> PlayerCallback::Handle {
        return m_graph->whenPlayerRemoved(std::move(on_removed));
    }

    // ReSharper disable once CppMemberFunctionMayBeConst
    void GlfwPlayer::resetPlayers() noexcept {
        m_graph->clear();
    }

    safe_ptr<GlfwPlayer> GlfwPlayer::get() noexcept {
        static GlfwPlayer g_instance{GlfwInput::get()};
        return safe_ptr{&g_instance};
    }

    safe_ptr<IPlayerService> IPlayerService::get() noexcept {
        return GlfwPlayer::get();
    }
}
