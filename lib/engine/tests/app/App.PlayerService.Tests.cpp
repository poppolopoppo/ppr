module;
#include "pP/UnitTest.h"

#if defined(_WIN32)
#include <windows.h>
#endif

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // player service
    // ------------------------------------------------------------------

    class PlayerServiceTestApp final : public Application {
    public:
        PlayerServiceTestApp()
            : Application(
                  ApplicationDomain{
                      .m_is_headless = true,
                      // NOTE: interactive stays on — GlfwPlatform builds the
                      // player service from the input service
                      // (GlfwPlayer::initialize(*m_input_service)), so
                      // non-interactive + presence would dereference null.
                      .m_is_interactive = true,
                      .m_needs_presence = true,
                      .m_needs_rendering = false,
                      .m_needs_user_interface = false,
                  },
                  "PlayerServiceTest", std::span<const char *const>{}) {
        }

        [[nodiscard]] std::error_code boot() {
            return initialize();
        }

        [[nodiscard]] std::error_code teardown() {
            return shutdown();
        }
    };

    // Windowed-path guard: one leaf keeps the original windowed domain so the
    // GlfwWindow + input + player boot path stays covered.
    class PlayerServiceWindowedTestApp final : public Application {
    public:
        PlayerServiceWindowedTestApp()
            : Application(
                  ApplicationDomain{
                      .m_needs_presence = true,
                      .m_needs_rendering = false,
                      .m_needs_user_interface = false,
                  },
                  "PlayerServiceWindowedTest", std::span<const char *const>{}) {
        }

        [[nodiscard]] std::error_code boot() {
            return initialize();
        }

        [[nodiscard]] std::error_code teardown() {
            return shutdown();
        }
    };

    PPR_UNIT_TEST (player_service_keyboard_and_gamepad) {
        PlayerServiceWindowedTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER {
            PPR_TEST_ASSERT(test_app.teardown() == default_value_v);
        };
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

    PPR_UNIT_TEST (native_window_wait_accepts_timeout_and_posted_wake) {
        // Two independent service lifetimes exercise native wait ownership
        // without making assertions about scheduler timing.
        for (u32 lifetime = 0u; lifetime != 2u; ++lifetime) {
            PlayerServiceWindowedTestApp test_app{};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                PPR_TEST_ASSERT(not test_app.teardown());
            };
            const safe_ptr<IWindowService> windows = test_app.getServices().get<IWindowService>();
            PPR_TEST_ASSERT(windows.get() != nullptr);
            PPR_TEST_ASSERT(not windows->pollEvents());
            PPR_TEST_ASSERT(not windows->waitEventsTimeout(TimeSpan{}));
            PPR_TEST_ASSERT(not windows->waitEventsTimeout(std::chrono::milliseconds{1}));

            windows->postEmptyEvent();
            PPR_TEST_ASSERT(not windows->waitEventsTimeout(std::chrono::milliseconds{50}));
            PPR_TEST_ASSERT(not windows->pollEvents());
        }
    };

    PPR_UNIT_TEST (native_window_center_keeps_decorations_in_work_area) {
        PlayerServiceWindowedTestApp test_app{};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER {
            std::ignore = test_app.teardown();
        };

        auto windows = test_app.getServices().get<IWindowService>();
        PPR_TEST_ASSERT(windows.get() != nullptr);
        safe_ptr<Window> window{};
        PPR_TEST_ASSERT(not windows->createWindow(
            WindowModel{.m_title = "Centered window test", .m_window_size = int2{512, 320}}, &window));
        PPR_DEFER {
            // Best-effort unwinding; normal cleanup below checks both results.
            if (window) {
                SharedWindow retiring{window};
                window.reset();
                std::ignore = windows->destroyWindow(std::move(retiring));
            }
        };

        unsigned monitor_count{};
        const std::error_code enumeration_error = windows->enumerateMonitors(
            [&](const SharedMonitor &monitor) -> std::error_code {
                ++monitor_count;
                const auto centered = windows->centerDisplayWindow(*window, *monitor);
                PPR_TEST_ASSERT(centered.has_value() and not centered->m_fullscreen);
                PPR_TEST_ASSERT(window->m_window_position.x == centered->m_position.x);
                PPR_TEST_ASSERT(window->m_window_position.y == centered->m_position.y);

#if defined(_WIN32)
                const HWND native = static_cast<HWND>(windows->getWindowNativeHandle(*window));
                const POINT monitor_center{
                    monitor->m_virtual_position.x + monitor->m_video_mode.m_resolution.x / 2,
                    monitor->m_virtual_position.y + monitor->m_video_mode.m_resolution.y / 2};
                const HMONITOR native_monitor = ::MonitorFromPoint(monitor_center, MONITOR_DEFAULTTONULL);
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                PPR_TEST_ASSERT(native_monitor != nullptr and ::GetMonitorInfoW(native_monitor, &info));

                RECT outer{};
                RECT client{};
                PPR_TEST_ASSERT(::GetWindowRect(native, &outer));
                PPR_TEST_ASSERT(::GetClientRect(native, &client));
                const LONG horizontal_room = info.rcWork.right - info.rcWork.left - (outer.right - outer.left);
                const LONG vertical_room = info.rcWork.bottom - info.rcWork.top - (outer.bottom - outer.top);
                const LONG expected_left = info.rcWork.left +
                                           (horizontal_room >= 0 ? horizontal_room / 2 : horizontal_room);
                const LONG expected_top = info.rcWork.top + (vertical_room >= 0 ? vertical_room / 2 : 0);
                PPR_TEST_ASSERT(std::abs(outer.left - expected_left) <= 1);
                PPR_TEST_ASSERT(std::abs(outer.top - expected_top) <= 1);
                PPR_TEST_ASSERT(outer.top >= info.rcWork.top and outer.right <= info.rcWork.right);
                PPR_TEST_ASSERT(centered->m_size.x == client.right - client.left);
                PPR_TEST_ASSERT(centered->m_size.y == client.bottom - client.top);
#endif

                return {};
            });
        PPR_TEST_ASSERT(not enumeration_error);

        PPR_TEST_ASSERT(monitor_count > 0u);

        SharedWindow retiring{window};
        window.reset();
        const std::error_code destroy_error = windows->destroyWindow(std::move(retiring));
        PPR_TEST_ASSERT(not destroy_error);

        windows.reset();
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST (gamepad_player_creation) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER {
            PPR_TEST_ASSERT(test_app.teardown() == default_value_v);
        };
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
        PPR_DEFER {
            PPR_TEST_ASSERT(test_app.teardown() == default_value_v);
        };
        const safe_ptr<IPlayerService> input = test_app.getServices().get<IPlayerService>();
        const PlayerId fake_id{max_v};
        PPR_TEST_ASSERT(input->removePlayer(fake_id) != default_value_v);
    };

    PPR_UNIT_TEST (get_or_create_keyboard_is_idempotent) {
        PlayerServiceTestApp test_app{};
        PPR_TEST_ASSERT(test_app.boot() == default_value_v);
        PPR_DEFER {
            PPR_TEST_ASSERT(test_app.teardown() == default_value_v);
        };
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
        PPR_DEFER {
            PPR_TEST_ASSERT(test_app.teardown() == default_value_v);
        };
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
    const UnitTest player_service = UnitTest::Named("player_service") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::player_service_keyboard_and_gamepad,
            detail::native_window_wait_accepts_timeout_and_posted_wake,
            detail::native_window_center_keeps_decorations_in_work_area,
            detail::gamepad_player_creation,
            detail::remove_nonexistent_player_returns_false,
            detail::get_or_create_keyboard_is_idempotent,
            detail::player_service_callbacks,
        });
    };

    const UnitTest &player_serviceTests() noexcept {
        return player_service;
    }
} // namespace pP::tests
