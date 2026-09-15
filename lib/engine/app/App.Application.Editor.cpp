module;

#include "pP/Macros.h"

module engine.app;

import :application_editor;
import :input.action;
import :input.listener;
import :window.viewport;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Editor, debug, none)

    namespace {
        enum EInputPriority : int {
            EInputPriority_ui = 0,
            EInputPriority_camera,
            EInputPriority_player,
        };
    }

    ApplicationEditor::ApplicationEditor(const std::string_view name, const std::span<const char *const> argv)
        : Application(ApplicationDomain{
            .m_is_headless = false,
            .m_is_interactive = true,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = true,
        }, name, argv) {
    }

    ApplicationEditor::~ApplicationEditor() noexcept = default;

    safe_ptr<const Camera> ApplicationEditor::getMainCamera() const noexcept {
        return m_camera.get();
    }

    safe_ptr<const Player> ApplicationEditor::getMainPlayer() const noexcept {
        return m_player.get();
    }

    safe_ptr<const WindowViewport> ApplicationEditor::getMainViewport() const noexcept {
        return m_main_viewport.get();
    }

    safe_ptr<Camera> ApplicationEditor::getMainCamera() noexcept {
        return m_camera.get();
    }

    safe_ptr<Player> ApplicationEditor::getMainPlayer() noexcept {
        return m_player.get();
    }

    safe_ptr<WindowViewport> ApplicationEditor::getMainViewport() noexcept {
        return m_main_viewport.get();
    }

    safe_ptr<const InputContext> ApplicationEditor::getMainInputContext() const noexcept {
        return safe_ptr(&m_main_input_context->m_context);
    }

    safe_ptr<InputContext> ApplicationEditor::getMainInputContext() noexcept {
        return safe_ptr(&m_main_input_context->m_context);
    }

    std::error_code ApplicationEditor::initialize() {
        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::initialize());

        // create main window:
        IWindowService &window_service = *getPlatform().getWindowService();

        safe_ptr<Window> main_window{};
        PPR_RETURN_ERROR_ON_FAIL(Editor, window_service.createWindow(WindowModel{
            .m_title = getName(),
            .m_window_size = int2{1280, 720},
            }, &main_window));

        std::ignore = window_service.setMainWindow(main_window);

        const safe_ptr<IInputService> input_service = getPlatform().getInputService();

        m_main_input_context = std::make_unique<WindowInputContext>(input_service, main_window);
        m_main_viewport = std::make_unique<WindowViewport>(main_window, ViewportLayout{});

        // create dummy player for the editor with a camera & a free camera controller:
        m_player = std::make_unique<Player>(PlayerIdentity{});
        m_camera = std::make_unique<Camera>(ECameraProjection::perspective);

        m_camera_input_mapping = std::make_unique<InputMapping>("camera_input_mapping");
        m_camera_controller = std::make_unique<FreeCameraController>();
        m_camera_controller->lookAt(float3{0.0f, 0.0f, -2.0f}, float3{0.0f, 0.0f, 0.0f}, math::axis_y);
        m_camera_controller->provideInputActionKeyMappings(*m_camera_input_mapping);

        // return camera inputs to player input listener, which is register to the window input context:
        m_player->getListener().addInputMapping(m_camera_input_mapping, EInputPriority_camera);
        m_main_input_context->m_context.addInputListener(&m_player->getListener(), EInputPriority_player);

        // initialize the renderer:
        ServicesStore &app_services = getServices();
        const safe_ptr<IRhiService> rhi_service{app_services.inject()};
        const safe_ptr<IShaderService> shader_service{app_services.inject()};

        // create dummy triangle render pass:
        m_triangle_pass = std::make_unique<TrianglePass>();
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->initialize(
            *rhi_service,
            *shader_service,
            getContentDir()));

        // create imgui backend service:
        m_ui_service = ui::createImGuiService();
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_ui_service->initialize(
            *m_main_input_context,
            *rhi_service,
            *shader_service,
            EInputPriority_ui));

        getServices().insert_or_assign(safe_ptr(m_ui_service));
        return default_value_v;
    }

    std::error_code ApplicationEditor::shutdown() {
        std::error_code first_err{};

        if (m_ui_service) {
            ServicesStore &app_services = getServices();
            PPR_VERIFY(app_services.erase(*m_ui_service));

            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_ui_service->shutdown());
            m_ui_service.reset();
        }

        if (m_triangle_pass) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->shutdown());
            m_triangle_pass.reset();
        }

        if (m_player) {
            m_player->getListener().clearInputMappings();
            m_player->clearFrameMessages();
        }

        safe_ptr<Window> main_window{};
        if (m_main_input_context) {
            main_window = m_main_input_context->m_window;
            m_main_input_context->m_context.clearInputListeners();
            m_main_input_context.reset();
        }

        if (m_main_viewport) {
            PPR_ASSERT(main_window == &m_main_viewport->getWindow());
            m_main_viewport.reset();
        }

        if (main_window) {
            IWindowService &window_service = *getPlatform().getWindowService();
            std::ignore = window_service.setMainWindow(nullptr);
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, window_service.destroyWindow(std::move(main_window)));
        }

        m_camera_input_mapping.reset();
        m_camera_controller.reset();
        m_camera.reset();

        PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, Application::shutdown());
        return first_err;
    }

    std::error_code ApplicationEditor::update(TimeSpan dt) {
        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::update(dt));

        if (m_main_viewport) [[likely]] {
            m_main_viewport->updateFromWindow();
        }

        m_camera->updateModel(dt, *m_camera_controller, m_main_viewport->getViewport());

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->update(dt, m_camera->getSnapshot()));

        if (m_ui_service) [[likely]] {
            PPR_RETURN_ERROR_ON_FAIL(Editor, m_ui_service->update(dt, *m_main_viewport));
        }

        return default_value_v;
    }

    std::error_code ApplicationEditor::render() {
        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::render());

        Renderer &renderer = getRenderer();
        PPR_RETURN_ERROR_ON_FAIL(Editor, renderer.renderAndPresent(m_main_viewport->getWindow(), {
            *m_triangle_pass,
            *m_ui_service
            }));

        return default_value_v;
    }
}
