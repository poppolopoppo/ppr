module;
#include "pP/Macros.h"

export module engine.tests.app:player_service;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    PPR_UNIT_TEST(player_service_keyboard_and_gamepad) {
        const safe_ptr<IPlayerService> input = getDefaultPlayerService();
        PPR_ASSERT(input.get() != nullptr);

        safe_ptr<Player> keyboard = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(keyboard.get() != nullptr);
        PPR_ASSERT(keyboard->getId().m_kind == EPlayerKind::keyboard);

        const auto keyboard_id = keyboard->getId();
        u32 count = 0u;
        input->enumeratePlayers([&](const safe_ptr<Player> &) noexcept { ++count; });
        PPR_ASSERT(count >= 1u);

        keyboard = nullptr;
        PPR_ASSERT(input->removePlayer(keyboard_id));
    };

    PPR_UNIT_TEST(gamepad_player_creation) {
        const safe_ptr<IPlayerService> input = getDefaultPlayerService();
        PPR_ASSERT(input.get() != nullptr);

        safe_ptr<Player> gamepad = input->addGamepadPlayer(5u, 1u);
        PPR_ASSERT(gamepad.get() != nullptr);
        PPR_ASSERT(gamepad->getId().m_kind == EPlayerKind::gamepad);
        PPR_ASSERT(gamepad->getId().m_user_id == 5u);

        bool found = false;
        input->enumeratePlayers([&](const safe_ptr<Player> &p) noexcept {
            if (p->getId() == gamepad->getId()) {
                found = true;
            }
        });
        PPR_ASSERT(found);

        const PlayerId gamepad_id = gamepad->getId();
        gamepad = nullptr;
        PPR_ASSERT(input->removePlayer(gamepad_id));
    };

    PPR_UNIT_TEST(remove_nonexistent_player_returns_false) {
        const safe_ptr<IPlayerService> input = getDefaultPlayerService();
        const PlayerId fake_id{
            .m_kind = EPlayerKind::keyboard,
            .m_local_index = 999u,
            .m_user_id = 999u,
        };
        PPR_ASSERT(!input->removePlayer(fake_id));
    };

    PPR_UNIT_TEST(get_or_create_keyboard_is_idempotent) {
        const safe_ptr<IPlayerService> input = getDefaultPlayerService();

        safe_ptr<Player> first = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(first.get() != nullptr);

        safe_ptr<Player> second = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(second.get() != nullptr);
        PPR_ASSERT(first.get() == second.get());

        // cleanup
        const PlayerId player_id = first->getId();
        first = nullptr;
        second = nullptr;
        PPR_ASSERT(input->removePlayer(player_id));
    };

    PPR_UNIT_TEST(player_service_callbacks) {
        const safe_ptr<IPlayerService> input = getDefaultPlayerService();

        u32 added_count = 0u;
        u32 removed_count = 0u;

        auto on_added = [&](const IPlayerService &, const Player &) noexcept { ++added_count; };
        auto on_removed = [&](const IPlayerService &, const Player &) noexcept { ++removed_count; };
        const auto added_handle = input->whenPlayerAdded(on_added);
        const auto removed_handle = input->whenPlayerRemoved(on_removed);

        const PlayerId id{
            .m_kind = EPlayerKind::keyboard,
            .m_local_index = 0u,
            .m_user_id = 0u,
        };
        safe_ptr<Player> player = input->getOrCreateKeyboardPlayer();

        player = nullptr;
        PPR_ASSERT(input->removePlayer(id));

        PPR_ASSERT(added_count == 1u);
        PPR_ASSERT(removed_count == 1u);
    };

    PPR_UNIT_TEST(app_player_service) {
        resetDefaultPlayerService();
        _.recurse({
            player_service_keyboard_and_gamepad,
            gamepad_player_creation,
            remove_nonexistent_player_returns_false,
            get_or_create_keyboard_is_idempotent,
            player_service_callbacks,
        });
    };
}
