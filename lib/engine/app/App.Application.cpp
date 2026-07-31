module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :platform;
import :renderer;
import :service.input;
import :service.ui;
import :service.window;
import :window.handle;
import :window.monitor;
import engine.core;
import engine.rhi;
import engine.shader;
import std;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(App, debug, none)

    Application::Application(const std::string_view name, const std::span<const char *const> argv)
        : m_platform(IPlatform::get()),
          m_arguments(argv.begin(), argv.end()),
          m_name(name) {
        PPR_ASSERT(m_platform != nullptr);
    }

    Application::~Application() noexcept = default;

    void Application::requestApplicationExit() noexcept {
        m_should_close = true;
    }

    std::error_code Application::run() {
        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();

        PPR_RETURN_ERROR_ON_FAIL(App, initialize());
        PPR_DEFER {
            if (const std::error_code err = shutdown()) {
                throw std::system_error(err);
            }
        };

        try {
            while (not m_should_close) [[likely]] {
                PPR_RETURN_ERROR_ON_FAIL(App, update());
                PPR_RETURN_ERROR_ON_FAIL(App, render());
            }
        } catch (const std::system_error &e) {
            return e.code();
        } catch (const std::invalid_argument &) {
            return std::make_error_code(std::errc::invalid_argument);
        } catch (const std::bad_alloc &) {
            return std::make_error_code(std::errc::not_enough_memory);
        }

        return default_value_v;
    }

    std::error_code Application::initialize() {
        PPR_ASSERT(m_state == EState::created);

        m_state = EState::terminated;

        PPR_LOG(App, info, "starting application", {
            {"name", m_name},
            {"platform", hal::platformName()},
            {"args", [this]() noexcept -> opaque::TransformView {
                return opaque::TransformView(m_arguments);
            }},
        });

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->initialize(*this));

        m_cached_window_service = m_services.get<IWindowService>();
        m_cached_input_service = m_services.get<IInputService>();
        PPR_ASSERT(m_cached_input_service.isValid());

        if (auto result = m_cached_window_service->createWindow(WindowModel{
            .m_title = m_name,
            .m_window_size = int2{1280, 720},
        })) {
            m_main_window = std::move(*result);
            m_cached_window_service->setMainWindow(*m_main_window);
        } else {
            return result.error();
        }

        // Shader service must be initialized before RHI to provide the shared global session
        const safe_ptr<IShaderService> shader_service = IShaderService::get();
        PPR_RETURN_ERROR_ON_FAIL(App, shader_service->initialize());
        std::ignore = m_services.insert(shader_service);

        const safe_ptr<IRhiService> rhi_service = IRhiService::get();
        PPR_RETURN_ERROR_ON_FAIL(App, rhi_service->initialize(rhi::DeviceType::Default, shader_service->getGlobalSession()));
        std::ignore = m_services.insert(rhi_service);

        if (auto renderer = std::make_unique<Renderer>()) {
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                renderer->initialize(
                    *rhi_service,
                    *m_cached_window_service,
                    *m_main_window,
                    m_contentDir.path()));

            m_resize_handle = m_main_window->m_when_resized.add([self{safe_ptr{this}}](const Window &window, const int2 &) -> std::error_code {
                const int2 fb = window.m_framebuffer_size;
                if (self->m_renderer) {
                    PPR_RETURN_ERROR_ON_FAIL(App, self->m_renderer->onResize(fb));
                }
                if (self->m_ui_service) {
                    PPR_RETURN_ERROR_ON_FAIL(App, self->m_ui_service->onResize(fb));
                }
                return default_value_v;
            });

            m_renderer = std::move(renderer);
        }

        if (auto ui = ui::createImGuiService()) {
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                ui->initialize(*rhi_service, *m_cached_window_service,
                    *m_cached_input_service, *m_main_window,
                    m_renderer->getSurfaceFormat()));

            std::ignore = m_services.insert(safe_ptr{ui.get()});
            m_ui_service = std::move(ui);
        }

        auto [ctx, cancelFn] = context::withCancel(context::background());
        m_lifecycle = std::move(ctx);
        m_cancel = std::move(cancelFn);

        m_state = EState::initialized;
        return default_value_v;
    }

    std::error_code Application::update() {
        PPR_RETURN_ERROR_ON_FAIL(App, m_lifecycle->error());

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_window_service->pollEvents());

        if (m_cached_window_service->getWindowShouldClose(*m_main_window)) [[unlikely]] {
            requestApplicationExit();
            return default_value_v;
        }

        const TimePoint now = time::now();
        const TimeSpan dt = now - m_last_frame_time;
        m_last_frame_time = now;

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_input_service->postInputMessages(dt));

        if (m_ui_service) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, m_ui_service->newFrame(dt));
        }

        return default_value_v;
    }

    std::error_code Application::render() {
        PPR_ASSERT(m_cached_window_service.isValid());
        PPR_ASSERT(m_main_window.isValid());

        if (m_renderer) [[likely]] {
            std::optional<Renderer::OverlayCallback> overlay;
            if (m_ui_service) {
                overlay = Renderer::OverlayCallback{std23::nontype<&IUIService::renderOverlay>, m_ui_service.get()};
            }
            PPR_RETURN_ERROR_ON_FAIL(App, m_renderer->render(std::move(overlay)));
        } else {
            m_cached_window_service->swapWindowBuffers(*m_main_window);
        }

        return default_value_v;
    }

    std::error_code Application::shutdown() noexcept {
        if (m_state >= EState::terminated) [[unlikely]] {
            return default_value_v;
        }

        m_resize_handle.release();

        PPR_DEFER {
            m_ui_service.reset();
            m_renderer.reset();

            m_main_window.reset();
            m_cached_input_service.reset();
            m_cached_window_service.reset();
        };

        m_state = EState::terminated;
        m_cancel();

        if (m_ui_service) {
            m_services.erase<IUIService>();
            PPR_RETURN_ERROR_ON_FAIL(App, m_ui_service->shutdown());
            m_ui_service.reset();
        }

        if (m_renderer) {
            PPR_RETURN_ERROR_ON_FAIL(App, m_renderer->shutdown());
            m_renderer.reset();
        }

        m_services.erase<IRhiService>();
        PPR_RETURN_ERROR_ON_FAIL(App, IRhiService::get()->shutdown());

        m_services.erase<IShaderService>();

        m_main_window.reset();
        m_cached_input_service.reset();
        m_cached_window_service.reset();

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->shutdown(*this));

        return default_value_v;
    }
}
