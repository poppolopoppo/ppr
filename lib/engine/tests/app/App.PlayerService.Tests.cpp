module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import std;

namespace pP::tests::detail {
    class PlayerServiceTestApp final : public Application {
    public:
        PlayerServiceTestApp()
            : Application(
                ApplicationDomain{
                    .m_needs_presence = true,
                    .m_needs_rendering = false,
                    .m_needs_user_interface = false,
                },
                "PlayerServiceTest",
                std::span<const char *const>{}) {
        }

        [[nodiscard]] std::error_code boot() {
            return initialize();
        }

        [[nodiscard]] std::error_code teardown() {
            return shutdown();
        }
    };

    PPR_UNIT_TEST (player_service_keyboard_and_gamepad) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER{PPR_TEST_ASSERT(test_app.teardown() == default_value_v); };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();
        PPR_TEST_ASSERT(input.get() != nullptr);

        auto keyboard_result = input->getOrCreateKeyboardPlayer();
        PPR_TEST_ASSERT(keyboard_result.has_value());
        safe_ptr<Player> keyboard = std::move(*keyboard_result);
        PPR_TEST_ASSERT(keyboard.get() != nullptr);
        PPR_TEST_ASSERT(keyboard->getIdentity().m_kind == EPlayerKind::keyboard);

        const PlayerId keyboard_id{keyboard->getUserId()};
        u32 count = 0u;
        input->enumeratePlayers([&](const SharedPlayer &) noexcept -> std::error_code {
            ++count;
            return default_value_v;
        });
        PPR_TEST_ASSERT(count >= 1u);

        keyboard = nullptr;
        PPR_TEST_ASSERT(input->removePlayer(keyboard_id) == default_value_v);
    };

    PPR_UNIT_TEST (gamepad_player_creation) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER{PPR_TEST_ASSERT(test_app.teardown() == default_value_v); };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();
        PPR_TEST_ASSERT(input.get() != nullptr);

        auto gamepad_result = input->addGamepadPlayer(0u);
        PPR_TEST_ASSERT(gamepad_result.has_value());
        safe_ptr<Player> gamepad = std::move(*gamepad_result);
        PPR_TEST_ASSERT(gamepad.get() != nullptr);
        PPR_TEST_ASSERT(gamepad->getIdentity().m_kind == EPlayerKind::gamepad);

        bool found = false;
        input->enumeratePlayers([&](const SharedPlayer &p) noexcept -> std::error_code {
            if (p->getUserId() == gamepad->getUserId()) {
                found = true;
            }
            return default_value_v;
        });
        PPR_TEST_ASSERT(found);

        const PlayerId gamepad_id{gamepad->getUserId()};
        gamepad = nullptr;
        PPR_TEST_ASSERT(input->removePlayer(gamepad_id) == default_value_v);
    };

    PPR_UNIT_TEST (remove_nonexistent_player_returns_false) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER{PPR_TEST_ASSERT(test_app.teardown() == default_value_v); };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();
        const PlayerId fake_id{max_v};
        PPR_TEST_ASSERT(input->removePlayer(fake_id) != default_value_v);
    };

    PPR_UNIT_TEST (get_or_create_keyboard_is_idempotent) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER{PPR_TEST_ASSERT(test_app.teardown() == default_value_v); };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();

        auto first_result = input->getOrCreateKeyboardPlayer();
        PPR_TEST_ASSERT(first_result.has_value());
        safe_ptr<Player> first = std::move(*first_result);
        PPR_TEST_ASSERT(first.get() != nullptr);

        auto second_result = input->getOrCreateKeyboardPlayer();
        PPR_TEST_ASSERT(second_result.has_value());
        safe_ptr<Player> second = std::move(*second_result);
        PPR_TEST_ASSERT(second.get() != nullptr);
        PPR_TEST_ASSERT(first.get() == second.get());

        const PlayerId player_id{first->getUserId()};
        first = nullptr;
        second = nullptr;
        PPR_TEST_ASSERT(input->removePlayer(player_id) == default_value_v);
    };

    PPR_UNIT_TEST (player_service_callbacks) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER{PPR_TEST_ASSERT(test_app.teardown() == default_value_v); };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();

        u32 added_count = 0u;
        u32 removed_count = 0u;

        auto on_added = [&](const Player &) noexcept -> std::error_code {
            ++added_count;
            return default_value_v;
        };
        auto on_removed = [&](const Player &) noexcept -> std::error_code {
            ++removed_count;
            return default_value_v;
        };
        const auto added_handle = input->whenPlayerAdded(on_added);
        const auto removed_handle = input->whenPlayerRemoved(on_removed);

        auto player_result = input->getOrCreateKeyboardPlayer();
        PPR_TEST_ASSERT(player_result.has_value());
        safe_ptr<Player> player = std::move(*player_result);

        const PlayerId id{player->getUserId()};
        player = nullptr;
        PPR_TEST_ASSERT(input->removePlayer(id) == default_value_v);

        PPR_TEST_ASSERT(added_count == 1u);
        PPR_TEST_ASSERT(removed_count == 1u);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest player_service = UnitTest::Named("player_service") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::player_service_keyboard_and_gamepad,
            detail::gamepad_player_creation,
            detail::remove_nonexistent_player_returns_false,
            detail::get_or_create_keyboard_is_idempotent,
            detail::player_service_callbacks,
        });
    };
} // namespace pP::tests
