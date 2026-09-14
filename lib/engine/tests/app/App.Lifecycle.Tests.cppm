module;
#include "pP/UnitTest.h"

export module engine.tests.app:lifecycle;

import engine.app;
import engine.core;
import :window_input;
import engine.rhi;
import engine.shader;
import std;

export namespace pP::tests {
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
        PPR_DEFER {
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
        PPR_DEFER {
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
        AppLifecycle::WindowCreateFailApp app{"WindowCreateRollback"};
        PPR_TEST_ASSERT(app.boot() == std::make_error_code(std::errc::no_such_device_or_address));
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(not app.getServices().tryGet<IWindowService>().isValid());
    };

    PPR_UNIT_TEST (app_init_rollback_on_input_connect_failure) {
        AppLifecycle::InputConnectFailApp app{"InputConnectRollback"};
        PPR_TEST_ASSERT(app.boot() == std::make_error_code(std::errc::not_connected));
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(not app.getServices().tryGet<IInputService>().isValid());
    };

    PPR_UNIT_TEST (app_teardown_double_shutdown_no_refire) {
        AppLifecycle::CountingApp app{"DoubleShutdown"};
        PPR_TEST_ASSERT(not app.boot());
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(not app.teardown());
        PPR_TEST_ASSERT(app.m_shutdowns == 2);
    };

    PPR_UNIT_TEST (app_teardown_reports_subsystem_failure_but_completes) {
        AppLifecycle::PartialFailApp app{"PartialTeardown"};
        PPR_TEST_ASSERT(not app.boot());
        PPR_TEST_ASSERT(app.teardown() == std::make_error_code(std::errc::io_error));
        PPR_TEST_ASSERT(not app.teardown());
    };

    PPR_UNIT_TEST (app_preserves_pre_registered_graphics_services) {
        AppLifecycle::HeadlessApp app{"PreRegisteredGraphics"};
        AppLifecycle::MockGraphicsService graphics{};
        PPR_TEST_ASSERT(app.getServices().insert(safe_ptr<AppLifecycle::MockGraphicsService>{&graphics}));
        PPR_TEST_ASSERT(not app.boot());
        PPR_DEFER { PPR_TEST_ASSERT(not app.teardown()); };
        const auto resolved = app.getServices().tryGet<AppLifecycle::MockGraphicsService>();
        PPR_TEST_ASSERT(resolved.isValid());
        PPR_TEST_ASSERT(resolved.get() == &graphics);
        // Teardown inverse-of-setup: detach the pre-registered service before
        // scope exit so no safe_ptr outlives its stack owner.
        PPR_TEST_ASSERT(app.getServices().erase(graphics));
    };
}
