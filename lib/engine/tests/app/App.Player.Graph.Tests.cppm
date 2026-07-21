module;
#include "pP/Macros.h"

export module engine.tests.app:player_graph;

import engine.core;
import engine.app;
import std;

namespace pP {
    struct GraphTestService : IPlayerService {
        SharedPlayer getPlayer(const PlayerId &) const noexcept override { return {}; }
        void enumeratePlayers(Collector<SharedPlayer>) const noexcept override {}
        Expected<SharedPlayer> getOrCreateKeyboardPlayer() override {
            return std::unexpected{make_error_code(std::errc::not_supported)};
        }
        Expected<SharedPlayer> addGamepadPlayer(u32) override {
            return std::unexpected{make_error_code(std::errc::not_supported)};
        }
        std::error_code removePlayer(const PlayerId &) override { return default_value_v; }
        PlayerCallback::Handle whenPlayerAdded(PlayerCallback::Event) override { return {}; }
        PlayerCallback::Handle whenPlayerRemoved(PlayerCallback::Event) override { return {}; }
    };

    [[nodiscard]] KeyboardDevice &getTestKeyboard() noexcept {
        static KeyboardDevice g_instance{InputDeviceID{100u}};
        return g_instance;
    }

    [[nodiscard]] MouseDevice &getTestMouse() noexcept {
        static MouseDevice g_instance{InputDeviceID{101u}};
        return g_instance;
    }

    [[nodiscard]] GamepadDevice &getTestGamepad() noexcept {
        static GamepadDevice g_instance{InputDeviceID{102u}, 0u};
        return g_instance;
    }

    [[nodiscard]] GraphTestService &getTestGraphService() noexcept {
        static GraphTestService g_instance{};
        return g_instance;
    }
}

export namespace pP::tests {
    PPR_UNIT_TEST(player_graph_empty_initially) {
        const PlayerGraph graph{};
        u32 count = 0u;
        graph.enumeratePlayers([&](const SharedPlayer &) noexcept -> std::error_code {
            ++count;
            return default_value_v;
        });
        PPR_ASSERT(count == 0u);
    };

    PPR_UNIT_TEST(player_graph_add_keyboard_player) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        auto result = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());
        PPR_ASSERT(result.has_value());
        SharedPlayer player = *result;
        PPR_ASSERT(player.get() != nullptr);
        PPR_ASSERT(player->getIdentity().m_kind == EPlayerKind::keyboard);
        PPR_ASSERT(player->getIdentity().m_user_id == user_id);
        PPR_ASSERT(player->getIdentity().m_device_id == InputDeviceID{100u});
    };

    PPR_UNIT_TEST(player_graph_get_player_by_id) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        std::ignore = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());

        SharedPlayer retrieved = graph.getPlayer(user_id);
        PPR_ASSERT(retrieved.get() != nullptr);
    };

    PPR_UNIT_TEST(player_graph_get_nonexistent_player_returns_null) {
        const PlayerGraph graph{};
        SharedPlayer retrieved = graph.getPlayer(PlayerId{999u});
        PPR_ASSERT(retrieved.get() == nullptr);
    };

    PPR_UNIT_TEST(player_graph_find_player_for_device) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        std::ignore = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());

        auto found = graph.findPlayerForDevice(InputDeviceID{100u});
        PPR_ASSERT(found.has_value());
        PPR_ASSERT(*found == user_id);

        found = graph.findPlayerForDevice(InputDeviceID{101u});
        PPR_ASSERT(found.has_value());
        PPR_ASSERT(*found == user_id);
    };

    PPR_UNIT_TEST(player_graph_find_nonexistent_device_returns_nullopt) {
        const PlayerGraph graph{};
        auto found = graph.findPlayerForDevice(InputDeviceID{999u});
        PPR_ASSERT(!found.has_value());
    };

    PPR_UNIT_TEST(player_graph_enumerate_players) {
        PlayerGraph graph{};
        GraphTestService service{};

        std::ignore = graph.getOrCreateKeyboardPlayer(service, PlayerId{1u}, getTestKeyboard(), getTestMouse());
            std::ignore = graph.addGamepadPlayer(service, PlayerId{2u}, getTestGamepad());

        u32 count = 0u;
        bool has_keyboard = false;
        bool has_gamepad = false;
        graph.enumeratePlayers([&](const SharedPlayer &p) noexcept -> std::error_code {
            ++count;
            if (p->getIdentity().m_kind == EPlayerKind::keyboard) has_keyboard = true;
            if (p->getIdentity().m_kind == EPlayerKind::gamepad) has_gamepad = true;
            return default_value_v;
        });
        PPR_ASSERT(count == 2u);
        PPR_ASSERT(has_keyboard);
        PPR_ASSERT(has_gamepad);
    };

    PPR_UNIT_TEST(player_graph_remove_player) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        std::ignore = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());

        auto err = graph.removePlayer(service, user_id);
        PPR_ASSERT(err == default_value_v);

        SharedPlayer retrieved = graph.getPlayer(user_id);
        PPR_ASSERT(retrieved.get() == nullptr);
    };

    PPR_UNIT_TEST(player_graph_remove_nonexistent_player_fails) {
        PlayerGraph graph{};
        GraphTestService service{};
        auto err = graph.removePlayer(service, PlayerId{999u});
        PPR_ASSERT(err != default_value_v);
    };

    PPR_UNIT_TEST(player_graph_clear_removes_all_players) {
        PlayerGraph graph{};
        GraphTestService service{};

        std::ignore = graph.getOrCreateKeyboardPlayer(service, PlayerId{1u}, getTestKeyboard(), getTestMouse());
        std::ignore = graph.addGamepadPlayer(service, PlayerId{2u}, getTestGamepad());

        graph.clear();

        u32 count = 0u;
        graph.enumeratePlayers([&](const SharedPlayer &) noexcept -> std::error_code {
            ++count;
            return default_value_v;
        });
        PPR_ASSERT(count == 0u);
    };

    PPR_UNIT_TEST(player_graph_get_or_create_keyboard_is_idempotent) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        auto first = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());
        PPR_ASSERT(first.has_value());

        auto second = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());
        PPR_ASSERT(second.has_value());
        PPR_ASSERT(first->get() == second->get());
    };

    PPR_UNIT_TEST(player_graph_when_player_added_callback) {
        PlayerGraph graph{};
        GraphTestService service{};

        u32 call_count = 0u;
        auto on_added = [&](const IPlayerService &, const Player &) noexcept -> std::error_code {
            ++call_count;
            return default_value_v;
        };
        auto added_handle = graph.whenPlayerAdded(on_added);

        std::ignore = graph.getOrCreateKeyboardPlayer(service, PlayerId{42u}, getTestKeyboard(), getTestMouse());
        PPR_ASSERT(call_count == 1u);
    };

    PPR_UNIT_TEST(player_graph_when_player_removed_callback) {
        PlayerGraph graph{};
        GraphTestService service{};

        const PlayerId user_id{42u};
        std::ignore = graph.getOrCreateKeyboardPlayer(service, user_id, getTestKeyboard(), getTestMouse());

        u32 call_count = 0u;
        auto on_removed = [&](const IPlayerService &, const Player &) noexcept -> std::error_code {
            ++call_count;
            return default_value_v;
        };
        auto removed_handle = graph.whenPlayerRemoved(on_removed);

        std::ignore = graph.removePlayer(service, user_id);
        PPR_ASSERT(call_count == 1u);
    };

    PPR_UNIT_TEST(player_graph_clear_also_clears_callbacks) {
        PlayerGraph graph{};
        GraphTestService service{};

        u32 added_count = 0u;
        auto on_added = [&](const IPlayerService &, const Player &) noexcept -> std::error_code {
            ++added_count;
            return default_value_v;
        };
        auto added_handle = graph.whenPlayerAdded(on_added);
        added_handle.release();

        graph.clear();

        std::ignore = graph.getOrCreateKeyboardPlayer(service, PlayerId{42u}, getTestKeyboard(), getTestMouse());
        PPR_ASSERT(added_count == 0u);
    };

    PPR_UNIT_TEST(app_player_graph) {
        _.recurse({
            player_graph_empty_initially,
            player_graph_add_keyboard_player,
            player_graph_get_player_by_id,
            player_graph_get_nonexistent_player_returns_null,
            player_graph_find_player_for_device,
            player_graph_find_nonexistent_device_returns_nullopt,
            player_graph_enumerate_players,
            player_graph_remove_player,
            player_graph_remove_nonexistent_player_fails,
            player_graph_clear_removes_all_players,
            player_graph_get_or_create_keyboard_is_idempotent,
            player_graph_when_player_added_callback,
            player_graph_when_player_removed_callback,
            player_graph_clear_also_clears_callbacks,
        });
    };
}
