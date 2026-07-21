module;
#include "pP/Macros.h"

export module engine.tests.app:player_service;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    PPR_UNIT_TEST(player_service_keyboard_and_gamepad) {
        const safe_ptr<IPlayerService> input = IPlayerService::get();
        PPR_ASSERT(input.get() != nullptr);

        auto keyboard_result = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(keyboard_result.has_value());
        safe_ptr<Player> keyboard = *keyboard_result;
        PPR_ASSERT(keyboard.get() != nullptr);
        PPR_ASSERT(keyboard->getIdentity().m_kind == EPlayerKind::keyboard);

        const PlayerId keyboard_id = keyboard->getUserId();
        u32 count = 0u;
        input->enumeratePlayers([&](const SharedPlayer &) noexcept -> std::error_code {
            ++count;
            return default_value_v;
        });
        PPR_ASSERT(count >= 1u);

        keyboard = nullptr;
        PPR_ASSERT(input->removePlayer(keyboard_id) == default_value_v);
    };

    PPR_UNIT_TEST(gamepad_player_creation) {
        const safe_ptr<IPlayerService> input = IPlayerService::get();
        PPR_ASSERT(input.get() != nullptr);

        auto gamepad_result = input->addGamepadPlayer(0u);
        PPR_ASSERT(gamepad_result.has_value());
        safe_ptr<Player> gamepad = *gamepad_result;
        PPR_ASSERT(gamepad.get() != nullptr);
        PPR_ASSERT(gamepad->getIdentity().m_kind == EPlayerKind::gamepad);

        bool found = false;
        input->enumeratePlayers([&](const SharedPlayer &p) noexcept -> std::error_code {
            if (p->getUserId() == gamepad->getUserId()) {
                found = true;
            }
            return default_value_v;
        });
        PPR_ASSERT(found);

        const PlayerId gamepad_id = gamepad->getUserId();
        gamepad = nullptr;
        PPR_ASSERT(input->removePlayer(gamepad_id) == default_value_v);
    };

    PPR_UNIT_TEST(remove_nonexistent_player_returns_false) {
        const safe_ptr<IPlayerService> input = IPlayerService::get();
        const PlayerId fake_id{umax_v};
        PPR_ASSERT(input->removePlayer(fake_id) != default_value_v);
    };

    PPR_UNIT_TEST(get_or_create_keyboard_is_idempotent) {
        const safe_ptr<IPlayerService> input = IPlayerService::get();

        auto first_result = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(first_result.has_value());
        safe_ptr<Player> first = *first_result;
        PPR_ASSERT(first.get() != nullptr);

        auto second_result = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(second_result.has_value());
        safe_ptr<Player> second = *second_result;
        PPR_ASSERT(second.get() != nullptr);
        PPR_ASSERT(first.get() == second.get());

        const PlayerId player_id = first->getUserId();
        first = nullptr;
        second = nullptr;
        PPR_ASSERT(input->removePlayer(player_id) == default_value_v);
    };

    PPR_UNIT_TEST(player_service_callbacks) {
        const safe_ptr<IPlayerService> input = IPlayerService::get();

        u32 added_count = 0u;
        u32 removed_count = 0u;

        auto on_added = [&](const IPlayerService &, const Player &) noexcept -> std::error_code {
            ++added_count;
            return default_value_v;
        };
        auto on_removed = [&](const IPlayerService &, const Player &) noexcept -> std::error_code {
            ++removed_count;
            return default_value_v;
        };
        const auto added_handle = input->whenPlayerAdded(on_added);
        const auto removed_handle = input->whenPlayerRemoved(on_removed);

        auto player_result = input->getOrCreateKeyboardPlayer();
        PPR_ASSERT(player_result.has_value());
        safe_ptr<Player> player = *player_result;

        const PlayerId id = player->getUserId();
        player = nullptr;
        PPR_ASSERT(input->removePlayer(id) == default_value_v);

        PPR_ASSERT(added_count == 1u);
        PPR_ASSERT(removed_count == 1u);
    };

    PPR_UNIT_TEST(app_player_service) {
        _.recurse({
            player_service_keyboard_and_gamepad,
            gamepad_player_creation,
            remove_nonexistent_player_returns_false,
            get_or_create_keyboard_is_idempotent,
            player_service_callbacks,
        });
    };
}
