module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

namespace pP::tests::detail {
    namespace WindowInput {
        struct FakeInputService final : IInputService {
            KeyboardDevice m_keyboard{InputDeviceID{1u}};
            MouseDevice m_mouse{InputDeviceID{2u}};
            GamepadDevice m_gamepad{InputDeviceID{3u}};
            InputContext m_global{};
            std::vector<SharedInputContext> m_contexts{};
            float2 m_last_cursor{0.0f, 0.0f};

            [[nodiscard]] const KeyboardDevice &getKeyboard() const noexcept override {
                return m_keyboard;
            }

            [[nodiscard]] const MouseDevice &getMouse() const noexcept override {
                return m_mouse;
            }

            [[nodiscard]] const GamepadDevice &getGamepad(int) const noexcept override {
                return m_gamepad;
            }

            [[nodiscard]] SharedInputDevice getInputDeviceByID(const InputDeviceID &) const noexcept override {
                return {};
            }

            [[nodiscard]] std::error_code enumerateInputDevices(Collector<SharedInputDevice>) const noexcept override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputKeysSupported(Collector<InputKey>) const override {
                return default_value_v;
            }

            [[nodiscard]] InputContext &getGlobalInputContext() noexcept override {
                return m_global;
            }

            [[nodiscard]] bool hasInputContext(const InputContext &context) const noexcept override {
                if (&m_global == &context) {
                    return true;
                }
                return std::ranges::any_of(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
            }

            void addInputContext(SharedInputContext context) override {
                m_contexts.push_back(std::move(context));
            }

            bool removeInputContext(const InputContext &context) override {
                const auto it = std::ranges::find_if(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
                if (it == m_contexts.end()) {
                    return false;
                }
                m_contexts.erase(it);
                return true;
            }

            void assignInputContextToDevice(InputDeviceID, SharedInputContext) override {
            }

            void clearInputContextDeviceAssignments() override {
                m_contexts.clear();
            }

            [[nodiscard]] std::error_code enumerateInputContexts(Collector<InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
                Collector<IInputDevice, InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code pollInputDevices(TimeSpan) override {
                return default_value_v;
            }

            void resetInputDevices() noexcept override {
                m_keyboard.resetInputState();
                m_mouse.resetInputState();
                m_gamepad.resetInputState();
            }

            void postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) override {
                m_keyboard.postKeyboardCharacterInput(context, codepoint);
            }

            void postKeyboardKeyPressed(const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
            }

            void postMouseButtonPressed(const InputContext &context, const EMouseButton button, const bool pressed) override {
                m_mouse.postMouseButtonPressed(TimeSpan{}, context, button, pressed);
            }

            void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override {
                m_last_cursor = absolute_pos;
                m_mouse.postMouseCursorPosition(TimeSpan{}, context, absolute_pos);
            }

            void postMouseScrollWheel(const InputContext &context, const float2 &delta) override {
                m_mouse.postMouseScrollWheel(TimeSpan{}, context, delta);
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event) override {
                return {};
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event) override {
                return {};
            }
        };

        PPR_UNIT_TEST (mouse_keeps_client_space_with_nonzero_window_origin) {
            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{400, 300}, .m_window_size = int2{800, 600}}
            };
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}, safe_ptr<Window>{&window}};
                const float2 client{10.0f, 20.0f};
                window.m_when_mouse_moved(window, client);

                PPR_TEST_ASSERT(inputs.m_last_cursor.x == client.x && inputs.m_last_cursor.y == client.y);

                const float2 &stored = inputs.m_mouse.m_cursor_pos.m_raw.m_absolute;
                PPR_TEST_ASSERT(stored.x == client.x && stored.y == client.y);
            }
            std::ignore = window.release();
        };

        PPR_UNIT_TEST (char_posted_after_poll_clear_survives_to_consumer) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            // Frame start: transient clear runs before window-event dispatch.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            // Dispatch: character arrives via window event.
            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('a'));
            // Consumer (ImGui::NewFrame) observes it in the same frame.
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);
            // Next frame start clears it exactly once.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };

        PPR_UNIT_TEST (reset_clears_pending_characters) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('b'));
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);

            device.resetInputState();
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };
    }


    namespace AppLifecycle {
        constexpr ApplicationDomain kHeadlessDomain{
            .m_is_headless = true,
            .m_is_interactive = false,
            .m_needs_presence = false,
            .m_needs_rendering = false,
            .m_needs_user_interface = false,
        };

        struct HeadlessApp : Application {
            explicit HeadlessApp(const std::string_view name)
                : Application(kHeadlessDomain, name, std::span<const char *const>{}) {
            }

            [[nodiscard]] std::error_code boot() { return initialize(); }
            [[nodiscard]] std::error_code teardown() { return shutdown(); }
        };

        struct WindowCreateFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

        protected:
            [[nodiscard]] std::error_code initialize() override {
                if (const auto ec = Application::initialize()) {
                    return ec;
                }
                return std::make_error_code(std::errc::no_such_device_or_address);
            }
        };

        struct InputConnectFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

        protected:
            [[nodiscard]] std::error_code initialize() override {
                if (const auto ec = Application::initialize()) {
                    return ec;
                }
                return std::make_error_code(std::errc::not_connected);
            }
        };

        struct CountingApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            int m_shutdowns{0};

        protected:
            [[nodiscard]] std::error_code shutdown() override {
                ++m_shutdowns;
                return Application::shutdown();
            }
        };

        struct PartialFailApp final : HeadlessApp {
            using HeadlessApp::HeadlessApp;

            bool m_fail_once{true};

        protected:
            [[nodiscard]] std::error_code shutdown() override {
                const auto ec = Application::shutdown();
                if (m_fail_once) {
                    m_fail_once = false;
                    if (not ec) {
                        return std::make_error_code(std::errc::io_error);
                    }
                }
                return ec;
            }
        };

        struct MockGraphicsService : IService {
            int m_tag{7};
        };
    }

    PPR_UNIT_TEST (renderer_triangle_reinit_ok) {
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER{
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();

        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));
        {
            Renderer renderer{};
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_TEST_ASSERT(not renderer.shutdown());
            PPR_TEST_ASSERT(not renderer.initialize(*rhi));
            PPR_TEST_ASSERT(not renderer.shutdown());
        }
        {
            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.shutdown());
        }
    };

    PPR_UNIT_TEST (imgui_live_shutdown_idempotent) {
        const auto shader = IShaderService::get();
        const auto rhi = IRhiService::get();
        std::ignore = rhi->shutdown();
        std::ignore = shader->shutdown();
        PPR_DEFER{
            std::ignore = rhi->shutdown();
            std::ignore = shader->shutdown();

        };
        PPR_TEST_ASSERT(not shader->initialize());
        PPR_TEST_ASSERT(not rhi->initialize(rhi::DeviceType::Default, *shader));

        WindowInput::FakeInputService inputs{};
        Window window{
            WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
            WindowModel{.m_window_position = int2{10, 20}, .m_window_size = int2{800, 600}}
        };
        window.m_framebuffer_size = int2{800, 600};
        {
            WindowInputContext routed{safe_ptr<IInputService>{&inputs}, safe_ptr<Window>{&window}};
            auto ui = ui::createImGuiService();
            PPR_TEST_ASSERT(ui != nullptr);
            PPR_TEST_ASSERT(not ui->initialize(routed, *rhi, *shader, 0));
            PPR_TEST_ASSERT(ui->getContext() != nullptr);
            PPR_TEST_ASSERT(not ui->shutdown());
            PPR_TEST_ASSERT(not ui->shutdown());
            // ImGuiService::~ImGuiService ENSUREs a live context, so a shut-down
            // instance cannot be destroyed while assert hooks are installed; the
            // emptied shell is released here. Dtor ownership stays engine-side.
            std::ignore = ui.release();
        }
        std::ignore = window.release();
    };

    PPR_UNIT_TEST (app_init_rollback_on_window_create_failure) {
        AppLifecycle::WindowCreateFailApp test_app{"WindowCreateRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::no_such_device_or_address));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IWindowService>().isValid());
    };

    PPR_UNIT_TEST (app_init_rollback_on_input_connect_failure) {
        AppLifecycle::InputConnectFailApp test_app{"InputConnectRollback"};
        PPR_TEST_ASSERT(test_app.boot() == std::make_error_code(std::errc::not_connected));
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.getServices().tryGet<IInputService>().isValid());
    };

    PPR_UNIT_TEST (app_teardown_double_shutdown_no_refire) {
        AppLifecycle::CountingApp test_app{"DoubleShutdown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(test_app.m_shutdowns == 2);
    };

    PPR_UNIT_TEST (app_teardown_reports_subsystem_failure_but_completes) {
        AppLifecycle::PartialFailApp test_app{"PartialTeardown"};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(test_app.teardown() == std::make_error_code(std::errc::io_error));
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST (app_preserves_pre_registered_graphics_services) {
        AppLifecycle::HeadlessApp test_app{"PreRegisteredGraphics"};
        AppLifecycle::MockGraphicsService graphics{};
        PPR_TEST_ASSERT(test_app.getServices().insert(safe_ptr<AppLifecycle::MockGraphicsService>{&graphics}));
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
        const auto resolved = test_app.getServices().tryGet<AppLifecycle::MockGraphicsService>();
        PPR_TEST_ASSERT(resolved.isValid());
        PPR_TEST_ASSERT(resolved.get() == &graphics);
        // Teardown inverse-of-setup: detach the pre-registered service before
        // scope exit so no safe_ptr outlives its stack owner.
        PPR_TEST_ASSERT(test_app.getServices().erase(graphics));
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest window_input = UnitTest::Named("window_input") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::WindowInput::mouse_keeps_client_space_with_nonzero_window_origin,
            detail::WindowInput::char_posted_after_poll_clear_survives_to_consumer,
            detail::WindowInput::reset_clears_pending_characters,
        });
    };
} // namespace pP::tests

namespace pP::tests {
    extern const UnitTest renderer_triangle_reinit_ok = detail::renderer_triangle_reinit_ok;
    extern const UnitTest imgui_live_shutdown_idempotent = detail::imgui_live_shutdown_idempotent;
    extern const UnitTest app_init_rollback_on_window_create_failure = detail::app_init_rollback_on_window_create_failure;
    extern const UnitTest app_init_rollback_on_input_connect_failure = detail::app_init_rollback_on_input_connect_failure;
    extern const UnitTest app_teardown_double_shutdown_no_refire = detail::app_teardown_double_shutdown_no_refire;
    extern const UnitTest app_teardown_reports_subsystem_failure_but_completes = detail::app_teardown_reports_subsystem_failure_but_completes;
    extern const UnitTest app_preserves_pre_registered_graphics_services = detail::app_preserves_pre_registered_graphics_services;
} // namespace pP::tests
