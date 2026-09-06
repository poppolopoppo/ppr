module;

#include "pP/Macros.h"

module engine.app;

import :application;
import :input.action;
import :input.listener;
import :platform;
import :renderer;
import :renderer.triangle_pass;
import :renderer.types;
import :scene.camera;
import :scene.camera.controller;
import :service.input;
import :service.ui;
import :service.window;
import :ui.imgui;
import :window.handle;
import :window.monitor;
import :window.viewport;
import engine.core;
import engine.math;
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
        if (m_cancel.isValid()) {
            m_cancel();
        }
    }

    std::error_code Application::run() {
        hal::disableSystemErrorReporting();
        hal::installDebugAssertHooks();

        PPR_RETURN_ERROR_ON_FAIL(App, initialize());
        PPR_DEFER{
            if (const std::error_code err = shutdown()) {
                if (std::uncaught_exceptions() == 0) {
                    throw std::system_error(err);



                }
            }
        };

        try {
            while (not m_lifecycle->error()) [[likely]] {
                if (const std::error_code err = update()) {
                    if (err == std::make_error_code(std::errc::operation_canceled)) {
                        break;
                    }
                    PPR_RETURN_ERROR_ON_FAIL(App, err);
                }
                if (const std::error_code err = render()) {
                    if (err == std::make_error_code(std::errc::operation_canceled)) {
                        break;
                    }
                    PPR_RETURN_ERROR_ON_FAIL(App, err);
                }
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

        auto [ctx, cancelFn] = context::withCancel(context::background());
        m_lifecycle = std::move(ctx);
        m_cancel = std::move(cancelFn);

        // Roll back the lifecycle pair and any window subscriptions on failure
        // below so a failed initialize() never leaks a live context.
        PPR_DEFER{
            if (m_state != EState::initialized) {
                m_resize_handle = {};
                m_focus_handle = {};
                m_close_handle = {};
                if (m_cancel.isValid()) {
                    m_cancel();
                }
                m_cancel = {};
                m_lifecycle = {};
            }
        };

        PPR_LOG(App, info, "starting application", {
            {"name", m_name},
            {"platform", hal::platformName()},
            {"args", opaqueValue(m_arguments)},
        });

        PPR_RETURN_ERROR_ON_FAIL(App, m_platform->initialize(*this));

        m_content_dir = std::filesystem::directory_entry(hal::process::currentExecutablePath().parent_path());
        m_workingDir = std::filesystem::directory_entry(std::filesystem::current_path());
        m_installDir = m_content_dir;

        m_cached_window_service = m_services.get<IWindowService>();
        m_cached_input_service = m_services.get<IInputService>();
        PPR_ASSERT(m_cached_input_service.isValid());

        // Shader service must be initialized before RHI to provide the shared global session
        const safe_ptr<IShaderService> shader_service = IShaderService::get();
        PPR_RETURN_ERROR_ON_FAIL(App, shader_service->initialize());
        std::ignore = m_services.insert(shader_service);

        const safe_ptr<IRhiService> rhi_service = IRhiService::get();
        PPR_RETURN_ERROR_ON_FAIL(App, rhi_service->initialize(rhi::DeviceType::Default, shader_service->getGlobalSession()));
        std::ignore = m_services.insert(rhi_service);

        if (auto result = m_cached_window_service->createWindow(WindowModel{
            .m_title = m_name,
            .m_window_size = int2{1280, 720},
        })) {
            m_main_window = std::move(*result);
            std::ignore = m_cached_window_service->setMainWindow(m_main_window);
        } else {
            return result.error();
        }

        PPR_RETURN_ERROR_ON_FAIL(App, m_renderer.initialize(*rhi_service));
        PPR_RETURN_ERROR_ON_FAIL(App, m_renderer.createWindowSurface(*m_cached_window_service, *m_main_window));
        PPR_RETURN_ERROR_ON_FAIL(App, m_triangle_pass.initialize(*rhi_service, m_content_dir.path()));

        m_resize_handle = m_cached_window_service->whenWindowResized(
            IWindowService::WindowResizedCallback::Event{std23::nontype<&Application::onWindowResized_>, this});

        m_focus_handle = m_cached_window_service->whenWindowFocused(
            IWindowService::WindowFocusedCallback::Event{std23::nontype<&Application::onWindowFocused_>, this});

        m_close_handle = m_cached_window_service->whenWindowClosed(
            IWindowService::WindowCallback::Event{std23::nontype<&Application::onWindowClosed_>, this});

        if (auto ui = ui::createImGuiService()) {
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                ui->initialize(*rhi_service, *m_cached_window_service,
                    *m_cached_input_service, *m_main_window,
                    m_renderer.getWindowSurfaceFormat(m_main_window->m_handle)));

            std::ignore = m_ui_services.insert(safe_ptr{ui.get()});
            m_ui_service = std::move(ui);
        }

        m_main_viewport = std::make_unique<WindowViewport>(m_main_window, ViewportLayout{});

        // Per-window input routing: window delegates -> input context -> scene listener -> controller actions.
        // WindowInputContext needs mutable delegate access while the window outlives the context (reset in shutdown).
        m_window_input = std::make_unique<WindowInputContext>(
            m_cached_input_service, safe_ptr<Window>{const_cast<Window *>(m_main_window.get())});
        m_scene_controller.provideInputActionKeyMappings(m_scene_controller_mapping);
        m_scene_listener.addInputMapping(safe_ptr<const InputMapping>{&m_scene_controller_mapping}, 0);
        m_window_input->m_context.addInputListener(safe_ptr<InputListener>{&m_scene_listener}, 0);

        m_state = EState::initialized;
        return default_value_v;
    }

    std::error_code Application::onWindowResized_(const Window &window, const int2 &) {
        const std::error_code surface_err = m_renderer.resizeWindowSurface(window.m_handle, window.m_framebuffer_size);

        std::error_code ui_err{};
        if (m_ui_service) {
            ui_err = m_ui_service->onResize(window.m_framebuffer_size);
        }

        // No camera/viewport propagation here: the WindowViewport refreshes owner-driven
        // in update(), and Camera::updateModel rejects degenerate viewports itself.
        return make_error_code({surface_err, ui_err});
    }

    std::error_code Application::onWindowFocused_(const Window &window [[maybe_unused]], bool focused) noexcept {
        m_focused = focused;
        return default_value_v;
    }

    std::error_code Application::onWindowClosed_(const Window &window [[maybe_unused]]) noexcept {
        requestApplicationExit();
        return default_value_v;
    }

    std::error_code Application::update() {
        if (const std::error_code lc = m_lifecycle->error()) {
            return lc;
        }

        // Pump deadline callbacks so withDeadline/withTimeout contexts can fire.
        TimerManager::mainTimer().tick();

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_window_service->pollEvents());

        const TimePoint now = time::now();
        const TimeSpan dt = now - m_last_frame_time;
        m_last_frame_time = now;

        PPR_RETURN_ERROR_ON_FAIL(App, m_cached_input_service->pollInputDevices(dt));

        if (m_main_viewport) [[likely]] {
            m_main_viewport->updateFromWindow();
        }

        m_scene_controller.updateCameraModel(dt, m_scene_camera_model);
        if (m_main_viewport) [[likely]] {
            // Degenerate viewports (minimized/zero window) are rejected inside updateModel;
            // the camera keeps its prior snapshot and render() submits nothing below.
            m_scene_camera.updateModel(dt, m_scene_camera_model, m_main_viewport->getViewport());
        }

        if (m_ui_service) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(App, m_ui_service->newFrame(dt));
        }

        return default_value_v;
    }

    std::error_code Application::render() {
        PPR_ASSERT(m_cached_window_service.isValid());
        PPR_ASSERT(m_main_window.isValid());
        PPR_ASSERT(m_main_viewport != nullptr);

        const std::optional<RenderView> render_view =
                makeRenderView(m_main_viewport->getViewport(), m_main_window->m_framebuffer_size);
        if (not render_view) [[unlikely]] {
            // Minimized/degenerate target: nothing to present this frame.
            return default_value_v;
        }

        const CameraSnapshot &snapshot = m_scene_camera.getSnapshot();
        const SceneView scene_view{snapshot, *render_view};

        auto scene_encode_draws = [this, &scene_view](rhi::IRenderPassEncoder &pass,
                                                      const DrawContext &draw_context) -> std::error_code {
            return m_triangle_pass.draw(pass, scene_view, draw_context.m_target);
        };
        DrawCallback scene_draws{scene_encode_draws};
        DrawSubmission scene_submission{*render_view, scene_draws};

        const WindowHandle window_handle = m_main_window->m_handle;
        const ColorPassOptions pass_options{};

        if (m_ui_service) [[likely]] {
            auto ui_encode_draws = [ui = m_ui_service.get()](rhi::IRenderPassEncoder &pass,
                                                             const DrawContext &draw_context) -> std::error_code {
                return ui->renderOverlay(pass, vector_cast<float>(draw_context.m_target.m_extent));
            };
            DrawCallback ui_draws{ui_encode_draws};
            const DrawSubmission submissions[] = {scene_submission, DrawSubmission{*render_view, ui_draws}};
            PPR_RETURN_ERROR_ON_FAIL(
                App, m_renderer.renderAndPresent(window_handle, std::span<const DrawSubmission>{submissions}, pass_options));
        } else {
            // UI hook point: a second named DrawSubmission joins the array above once a UI service is present.
            PPR_RETURN_ERROR_ON_FAIL(
                App,
                m_renderer.renderAndPresent(
                    window_handle, std::span<const DrawSubmission>{&scene_submission, 1}, pass_options));
        }

        return default_value_v;
    }

    std::error_code Application::shutdown() noexcept {
        if (m_state != EState::initialized) [[unlikely]] {
            return default_value_v;
        }
        m_state = EState::created;

        std::error_code shutdown_err{};
        const auto retain_error = [&shutdown_err](const std::error_code err) noexcept {
            if (not shutdown_err and err) {
                shutdown_err = err;
            }
        };

        // shutdown() is noexcept but most teardown callees are not proven
        // non-throwing: fold any exception into the error accumulator.
        const auto attempt = [&retain_error](std::invocable auto &&call) noexcept {
            try {
                if constexpr (std::is_void_v<std::invoke_result_t<decltype(call)>>) {
                    std::forward<decltype(call)>(call)();
                } else {
                    retain_error(std::forward<decltype(call)>(call)());
                }
            } catch (const std::exception &) {
                retain_error(std::make_error_code(std::errc::io_error));
            } catch (...) {
                retain_error(std::make_error_code(std::errc::state_not_recoverable));
            }
        };

        // Remove callbacks first so no events can reach teardown state.
        attempt([this] { m_resize_handle = {}; });
        attempt([this] { m_focus_handle = {}; });
        attempt([this] { m_close_handle = {}; });

        if (m_cancel.isValid()) {
            m_cancel();
        }
        m_cancel = {};
        m_lifecycle = {};

        // Detach scene input before the window and the input service go away.
        if (m_window_input) {
            attempt([this] {
                std::ignore = m_window_input->m_context.removeInputListener(m_scene_listener);
            });
            m_window_input.reset();
        }
        attempt([this] { m_scene_listener.clearInputMappings(); });
        attempt([this] { m_scene_controller_mapping.clearInputMappings(); });

        // Stop new submissions, then drain the queue before tearing down passes.
        attempt([this] { return m_renderer.waitForIdle(); });

        if (m_ui_service) {
            m_ui_services.erase<IUIService>();
            retain_error(m_ui_service->shutdown());
            m_ui_service.reset();
        }

        attempt([this] { return m_triangle_pass.shutdown(); });

        // Surface pairing: every createWindowSurface pairs with one idempotent destroy.
        if (m_main_window.isValid()) [[likely]] {
            attempt([this] { return m_renderer.destroyWindowSurface(m_main_window->m_handle); });
        }
        attempt([this] { return m_renderer.shutdown(); });

        m_main_viewport.reset();

        m_services.erase<IRhiService>();
        attempt([] { return IRhiService::get()->shutdown(); });

        attempt([] { return IShaderService::get()->shutdown(); });
        m_services.erase<IShaderService>();

        if (m_main_window.isValid()) [[likely]] {
            attempt([this] { return m_cached_window_service->destroyWindow(std::move(m_main_window)); });
        }
        m_cached_input_service.reset();
        m_cached_window_service.reset();

        attempt([this] { return m_platform->shutdown(*this); });

        return shutdown_err;
    }
}
