module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :camera;
import :input.replay;
import :platform;
import :renderer;
import :service.input;
import :service.ui;
import :service.window;
import :viewport;
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

        m_content_dir = std::filesystem::directory_entry(hal::process::currentExecutablePath().parent_path());
        m_workingDir = std::filesystem::directory_entry(std::filesystem::current_path());
        m_installDir = m_content_dir;

        m_cached_window_service = m_services.get<IWindowService>();
        m_cached_input_service = m_services.get<IInputService>();
        PPR_ASSERT(m_cached_input_service.isValid());

        // Wrap the platform input in a replay decorator, then initialize the camera
        // service against the wrapped input so the free-cam controller sees replay.
        m_input_replay.setParent(m_services.get<IInputService>());
        m_input_replay.setMode(EInputReplayMode::replay);
        m_services.erase<IInputService>();
        std::ignore = m_services.insert(safe_ptr<IInputService>{&m_input_replay});
        m_cached_input_service = m_services.get<IInputService>();
        std::ignore = m_camera_service.initialize(*m_services.get<IInputService>());
        std::ignore = m_services.insert(safe_ptr<ICameraService>{&m_camera_service});

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

        m_camera_service.setDeviceType(rhi_service->getDevice().getDeviceType());

        if (auto renderer = std::make_unique<Renderer>()) {
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                renderer->initialize(
                    *rhi_service,
                    *m_cached_window_service,
                    *m_main_window,
                    m_content_dir.path()));

            m_resize_handle = m_main_window->m_when_resized.add(
                std23::function_ref{std23::nontype<&Application::onWindowResized_>, this});

            m_renderer = std::move(renderer);
        }

        // Set initial viewport configs from window framebuffer size
        m_scene_viewport.framebuffer_size = m_main_window->m_framebuffer_size;
        m_ui_viewport.framebuffer_size = m_main_window->m_framebuffer_size;
        m_camera_service.setViewportSize(m_main_window->m_framebuffer_size);

        if (auto ui = ui::createImGuiService()) {
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                ui->initialize(*rhi_service, *m_cached_window_service,
                    *m_cached_input_service, *m_main_window,
                    m_renderer->getSurfaceFormat()));

            std::ignore = m_ui_services.insert(safe_ptr{ui.get()});
            m_ui_service = std::move(ui);
        }

        auto [ctx, cancelFn] = context::withCancel(context::background());
        m_lifecycle = std::move(ctx);
        m_cancel = std::move(cancelFn);

        m_state = EState::initialized;
        return default_value_v;
    }

    std::error_code Application::onWindowResized_(const Window &window, int2) {
        const int2 fb = window.m_framebuffer_size;
        m_camera_service.setViewportSize(fb);
        m_scene_viewport.framebuffer_size = fb;
        m_ui_viewport.framebuffer_size = fb;

        std::error_code renderer_err;
        if (m_renderer) {
            renderer_err = m_renderer->onResize(fb);
        }

        std::error_code ui_err;
        if (m_ui_service) {
            ui_err = m_ui_service->onResize(fb);
        }

        return make_error_code({renderer_err, ui_err});
    }

    std::error_code Application::update() {
        PPR_RETURN_ERROR_ON_FAIL(App, m_lifecycle->error());

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_window_service->pollEvents());

        if (m_cached_window_service->getWindowShouldClose(*m_main_window)) [[unlikely]] {
            requestApplicationExit();
        }

        const TimePoint now = time::now();
        const TimeSpan dt = now - m_last_frame_time;
        m_last_frame_time = now;

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_input_service->postInputMessages(dt));

        m_camera_service.update(dt);

        if (m_ui_service) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, m_ui_service->newFrame(dt));
        }

        return default_value_v;
    }

std::error_code Application::render() {
    PPR_ASSERT(m_cached_window_service.isValid());
    PPR_ASSERT(m_main_window.isValid());

    if (m_renderer) [[likely]] {
        const int2 fb = m_main_window->m_framebuffer_size;

        // m_renderer is a std::unique_ptr<Renderer> member; this raw capture is
        // valid because render() is synchronous and m_renderer outlives the call.
        const auto scene_draw = [this, renderer = m_renderer.get()]
                                (rhi::IRenderPassEncoder &pass, const rhi::Viewport &viewport, const rhi::ScissorRect &scissor) -> std::error_code {
            return renderer->drawTriangle(pass, m_camera_service.camera(), viewport, scissor);
        };
        const ViewportEntry scene_entry{
            .viewport = rhi::Viewport{
                0.0f, 0.0f,
                static_cast<float>(fb.x),
                static_cast<float>(fb.y),
                0.0f, 1.0f},
            .scissor = rhi::ScissorRect{
                0, 0,
                static_cast<u32>(fb.x),
                static_cast<u32>(fb.y)},
            .draw = scene_draw,
        };

        // ui_store is a safe_ptr to m_ui_services (an Application member), used only
        // within this render() call, so m_ui_services outlives the lambda. Debug
        // mode ref-counts the capture; release mode is a plain pointer copy.
        const auto ui_draw = [ui_store = safe_ptr<ServicesStore>(&m_ui_services),
                              ui_size = float2{static_cast<float>(fb.x), static_cast<float>(fb.y)}]
                             (rhi::IRenderPassEncoder &pass, const rhi::Viewport &, const rhi::ScissorRect &) -> std::error_code {
            auto ui = ui_store->get<IUIService>();
            return ui->renderOverlay(pass, ui_size);
        };
        const ViewportEntry ui_entry{
            .viewport = rhi::Viewport{
                0.0f, 0.0f,
                static_cast<float>(fb.x),
                static_cast<float>(fb.y),
                0.0f, 1.0f},
            .scissor = rhi::ScissorRect{
                0, 0,
                static_cast<u32>(fb.x),
                static_cast<u32>(fb.y)},
            .draw = ui_draw,
        };

        const ViewportEntry viewports[] = {scene_entry, ui_entry};
        PPR_RETURN_ERROR_ON_FAIL(App, m_renderer->render(viewports));
    } else {
        m_cached_window_service->swapWindowBuffers(*m_main_window);
    }

    return default_value_v;
}

    std::error_code Application::shutdown() noexcept {
        if (m_state >= EState::terminated) [[unlikely]] {
            return default_value_v;
        }

        m_camera_service.deactivateController();
        m_input_replay.detachParent();

        m_resize_handle.release();

        PPR_DEFER {
            // Drop service-store refs to member-owned services (m_input_replay,
            // m_camera_service) before those members are destroyed.
            m_services.erase<IInputService>();
            m_services.erase<ICameraService>();

            m_ui_service.reset();
            m_renderer.reset();

            m_main_window.reset();
            m_cached_input_service.reset();
            m_cached_window_service.reset();
        };

        m_state = EState::terminated;
        m_cancel();

        if (m_ui_service) {
            m_ui_services.erase<IUIService>();
            PPR_RETURN_ERROR_ON_FAIL(App, m_ui_service->shutdown());
            m_ui_service.reset();
        }

        if (m_renderer) {
            PPR_RETURN_ERROR_ON_FAIL(App, m_renderer->shutdown());
            m_renderer.reset();
        }

        m_services.erase<IRhiService>();
        PPR_RETURN_ERROR_ON_FAIL(App, IRhiService::get()->shutdown());

        PPR_RETURN_ERROR_ON_FAIL(App, IShaderService::get()->shutdown());
        m_services.erase<IShaderService>();

        if (m_main_window.isValid()) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, m_cached_window_service->destroyWindow(std::move(m_main_window)));
        }
        m_cached_input_service.reset();
        m_cached_window_service.reset();

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->shutdown(*this));

        return default_value_v;
    }
}
