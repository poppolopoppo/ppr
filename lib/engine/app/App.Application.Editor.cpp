module;

#include "pP/Macros.h"

module engine.app;

import :application_editor;
import :input.action;
import :input.device;
import :input.key;
import :input.listener;
import :input.routing;
import :renderer.triangle_pass;
import :service.input;
import :window.viewport;

import engine.core;
import engine.image;
import engine.mesh;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Editor, debug, none)

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

        m_player = std::make_unique<Player>(PlayerIdentity{});
        m_camera = std::make_unique<Camera>(ECameraProjection::perspective);

        // Camera controller is a pure motion integrator; the
        // background-drag latch lives in Routing (m_bg_state).
        m_camera_input_mapping = std::make_unique<InputMapping>("camera_input_mapping");
        m_camera_controller = std::make_unique<FreeCameraController>();
        m_camera_controller->lookAt(float3{0.0f, 0.0f, -2.0f}, float3{0.0f, 0.0f, 0.0f}, math::axis_y);
        m_camera_controller->provideInputActionKeyMappings(*m_camera_input_mapping);

        m_player->getListener().addInputMapping(m_camera_input_mapping, static_cast<int>(EInputMappingPriority::camera));

        const safe_ptr<IInputService> input_service = getPlatform().getInputService();

        m_main_input_context = std::make_unique<WindowInputContext>(input_service);

        m_input_background_latch = std::make_unique<InputBackgroundLatch>(
            static_cast<int>(EInputListenerPriority::ui),
            safe_ptr{&m_player->getListener()},
            static_cast<int>(EInputListenerPriority::player),
            static_cast<int>(EInputListenerPriority::detector));

        m_device_disconnected_handle = input_service->whenDeviceDisconnected([this](const IInputDevice &) noexcept {
            // Session reset (Routing latch) is separate from motion reset (controller).
            m_input_background_latch->resetInputState();
            if (m_camera_controller) {
                m_camera_controller->resetInputState();
            }
            return default_value_v;
        });

        ServicesStore &app_services = getServices();
        const safe_ptr<IRhiService> rhi_service{app_services.inject()};
        const safe_ptr<IShaderService> shader_service{app_services.inject()};

        m_triangle_pass = std::make_unique<TrianglePass>();
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->initialize(
            *rhi_service,
            *shader_service,
            getContentDir()));

        m_ui_service = ui::createImGuiService();
        PPR_RETURN_ERROR_ON_FAIL(Editor, m_ui_service->initialize(
            *m_main_input_context,
            *rhi_service,
            *shader_service,
            m_input_background_latch->m_foreground_priority));

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_input_background_latch->initialize(m_main_input_context->m_context, m_ui_service->getInputListener()));

        IWindowService &window_service = *getPlatform().getWindowService();

        safe_ptr<Window> main_window{};
        PPR_RETURN_ERROR_ON_FAIL(Editor, window_service.createWindow(WindowModel{
            .m_title = getName(),
            .m_window_size = int2{1280, 720},
            }, &main_window));

        SharedMonitor main_monitor = window_service.getWindowFullscreenMonitor(*main_window);
        if (not main_monitor) {
            main_monitor = window_service.getPrimaryMonitor();
        }
        if (main_monitor and PPR_ENSURE(main_monitor->m_video_mode.m_refresh_rate > 0)) {
            const TimeDuration min_frame_duration{1.0 / main_monitor->m_video_mode.m_refresh_rate};

            PPR_LOG(Editor, info, "set maximum refresh rate", {
                {"width", main_monitor->m_video_mode.m_resolution.x},
                {"height", main_monitor->m_video_mode.m_resolution.y},
                {"refresh_rate", main_monitor->m_video_mode.m_refresh_rate},
                });

            setTargetFrameDuration(min_frame_duration);
        }

        m_main_viewport = std::make_unique<WindowViewport>(main_window, ViewportLayout{});

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_main_input_context->initialize(main_window));

        std::ignore = window_service.setMainWindow(main_window);

        // Window-blur drag clear: focus loss releases background hold.
        main_window->m_when_focused.subscribe<&ApplicationEditor::onMainWindowFocused_>(this);

        getServices().insert_or_assign(safe_ptr(m_ui_service));
        return default_value_v;
    }

    std::error_code ApplicationEditor::loadScene(const std::filesystem::path &dir, const std::string_view file) {
        PPR_RETURN_ERROR_ON_FAIL(Editor, unloadScene());
        if (not m_triangle_pass) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        Expected<mesh::SceneAsset> scene = mesh::importAndConvert(dir, file);
        if (not
            scene.has_value())
        [[unlikely]] {
            return scene.error();
        }

        Array<image::ImageAsset> images{};
        for (const mesh::ImageRef &ref : scene->m_images) {
            mem::SharedBuffer bytes{};
            if (ref.m_is_file) {
                Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(dir / ref.m_rel_path);
                if (not
                    mapped.has_value())
                [[unlikely]] {
                    return mapped.error();
                }
                bytes = *mapped;
            } else {
                bytes = ref.m_bytes;
            }
            if (not bytes.isValid()) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
            Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                bytes.getBufferData(), ref.m_ext, image::ImageDecodeDesc{}, image::ImageUsage::color);
            if (not
                decoded.has_value())
            [[unlikely]] {
                return decoded.error();
            }
            images.push_back(*decoded);
        }

        Expected<TrianglePass::UploadedScene> uploaded = m_triangle_pass->uploadScene(*scene, images);
        if (not
            uploaded.has_value())
        [[unlikely]] {
            return uploaded.error();
        }

        m_scene = std::move(*scene);
        m_images = std::move(images);
        m_uploaded_scene = std::move(*uploaded);
        m_has_scene = true;
        PPR_LOG(Editor, info, "scene loaded", {
            {"meshes", m_scene.m_meshes.size()},
            {"images", m_images.size()},
            {"prims", m_uploaded_scene.m_prims.size()},
        });
        return default_value_v;
    }

    std::error_code ApplicationEditor::unloadScene() {
        std::error_code first_err{};
        if (m_has_scene
            and
        m_triangle_pass)
        {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->releaseScene(m_uploaded_scene));
        }
        m_uploaded_scene = TrianglePass::UploadedScene{};
        m_images.clear();
        m_scene = mesh::SceneAsset{};
        m_has_scene = false;
        if (m_triangle_pass) {
            m_triangle_pass->clearInstances();
        }
        return first_err;
    }

    std::error_code ApplicationEditor::submitSceneInstances_() {
        m_triangle_pass->clearInstances();
        std::size_t prim_cursor = 0u;
        for (const mesh::SceneInstance &instance : m_scene.m_instances) {
            const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
            const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
            if (mesh_index >= m_scene.m_meshes.size()
                or
            node_index >= m_scene.m_nodes.size())
            [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
            const mesh::StaticMeshAsset &mesh_asset = m_scene.m_meshes[mesh_index];
            const float4x4 &world = m_scene.m_nodes[node_index].m_world;
            for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim : mesh_asset.m_prims) {
                if (prim_cursor >= m_uploaded_scene.m_prims.size()) [[unlikely]] {
                    return make_error_code(std::errc::invalid_argument);
                }
                const TrianglePass::UploadedPrimitive &uploaded = m_uploaded_scene.m_prims[prim_cursor++];
                MaterialHandle material = uploaded.m_material;
                if (instance.m_materialOverride != mesh::kInvalidMaterial) {
                    const std::size_t override_index = static_cast<std::size_t>(*instance.m_materialOverride);
                    if (override_index >= m_uploaded_scene.m_materials.size()) [[unlikely]] {
                        return make_error_code(std::errc::invalid_argument);
                    }
                    material = m_uploaded_scene.m_materials[override_index];
                }
                PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->submitInstance(uploaded.m_bag, material, world));
            }
        }
        return default_value_v;
    }

    std::error_code ApplicationEditor::shutdown() {
        std::error_code first_err{};

        // Release scene GPU handles while the pass caches are still alive,
        // before the §2.4 pass shutdown below.
        if (m_has_scene) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, unloadScene());
        }

        // Detach detector first so no callback fires during teardown.
        m_device_disconnected_handle.reset();

        if (m_camera_controller) {
            m_camera_controller->resetInputState();
        }

        if (m_input_background_latch) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_input_background_latch->shutdown(m_main_input_context->m_context));
            m_input_background_latch.reset();
        }

        if (m_ui_service) {
            ServicesStore &app_services = getServices();
            if (not app_services.erase(*m_ui_service)) {
                PPR_LOG(Editor, warning, "ui service was not registered");
            }

            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_ui_service->shutdown());
            m_ui_service.reset();
        }

        if (m_triangle_pass) {
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_triangle_pass->shutdown());
            m_triangle_pass.reset();
        }

        safe_ptr<Window> main_window{};
        if (m_main_input_context) {
            main_window = m_main_input_context->m_window;

            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, m_main_input_context->shutdown());

            m_main_input_context->m_context.clearInputListeners();
            m_main_input_context.reset();
        }

        if (m_main_viewport) {
            PPR_ASSERT(main_window == &m_main_viewport->getWindow());
            m_main_viewport.reset();
        }

        if (main_window) {
            // Drop blur subscription alongside detector detach.
            main_window->m_when_focused.reset();

            IWindowService &window_service = *getPlatform().getWindowService();
            std::ignore = window_service.setMainWindow(nullptr);
            PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, window_service.destroyWindow(std::move(main_window)));
        }

        if (m_player) {
            m_player->getListener().clearInputMappings();
            m_player->clearFrameMessages();
        }

        m_camera_input_mapping.reset();
        m_camera_controller.reset();
        m_camera.reset();

        PPR_RETAIN_ERROR_ON_FAIL(Editor, first_err, Application::shutdown());
        return first_err;
    }

    void ApplicationEditor::onMainWindowFocused_([[maybe_unused]] const Window &window, const bool focused) {
        setBackgroundPriority(not focused);

        if (not focused) {
            // Session reset (Routing latch) is separate from motion reset (controller).
            if (m_input_background_latch) {
                m_input_background_latch->resetInputState();
            }

            if (m_camera_controller) {
                m_camera_controller->resetInputState();
            }
        }
    }

    std::error_code ApplicationEditor::update(TimeSpan dt) {
        if (not m_main_viewport) [[unlikely]] {
            return make_error_code(std::errc::not_connected);
        }

        PPR_RETURN_ERROR_ON_FAIL(Editor, Application::update(dt));

        m_main_viewport->updateFromWindow();

        m_camera->updateModel(dt, *m_camera_controller, m_main_viewport->getViewport());

        const safe_ptr window_service{getPlatform().getWindowService()};
        window_service->renameWindow(m_main_viewport->getWindow(),
            std::format("{} - CPU = {:.2f} ms", getName(), time::seconds(dt) * 1000.0));

        PPR_RETURN_ERROR_ON_FAIL(Editor, m_triangle_pass->update(dt, m_camera->getSnapshot()));

        if (m_has_scene) {
            PPR_RETURN_ERROR_ON_FAIL(Editor, submitSceneInstances_());
        }

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
