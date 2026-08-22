module;

#include "pP/Macros.h"

export module engine.app:camera;

import std;
import engine.core;
import engine.math;
import engine.rhi;

import :service.input;
import :input.listener;
import :input.device;
import :input.key;
import :input.action;
import :input.mapping;

export namespace pP {
    struct CameraModel {
        float3 position{zero_v};
        float yaw{0.0f};
        float pitch{0.0f};
        bool cameraCut{false};
    };

    enum class EFrustumPlane : u8 {
        Near = 0,
        Far,
        Left,
        Right,
        Top,
        Bottom,
    };

    enum class EFrustumCorner : u8 {
        NearLeftTop = 0,
        NearLeftBottom,
        NearRightBottom,
        NearRightTop,
        FarLeftTop,
        FarLeftBottom,
        FarRightBottom,
        FarRightTop,
    };

    enum class EContainmentType : u8 {
        Outside = 0,
        Intersects,
        Inside,
    };

    class Frustum {
    public:
        Frustum() noexcept;
        explicit Frustum(const float4x4 &viewProjection, rhi::EProjectionConvention convention) noexcept;

        void setMatrix(const float4x4 &viewProjection, rhi::EProjectionConvention convention) noexcept;

        void setMatrix(const float4x4 &viewProjection, const float4x4 &inverseViewProjection, rhi::EProjectionConvention convention) noexcept;

        [[nodiscard]] const float4 &plane(EFrustumPlane p) const noexcept { return m_planes[static_cast<u8>(p)]; }
        [[nodiscard]] float3 corner(EFrustumCorner c) const noexcept { return m_corners[static_cast<u8>(c)]; }
        [[nodiscard]] const float3 &boundingMin() const noexcept { return m_bbox_min; }
        [[nodiscard]] const float3 &boundingMax() const noexcept { return m_bbox_max; }

        [[nodiscard]] EContainmentType contains(float3 point) const noexcept;
        [[nodiscard]] bool intersects(const float3 &box_min, const float3 &box_max) const noexcept;

    private:
        float4 m_planes[6];
        float3 m_corners[8];
        float3 m_bbox_min{zero_v};
        float3 m_bbox_max{zero_v};
        rhi::EProjectionConvention m_convention{rhi::EProjectionConvention::D3D};
    };

    class Camera {
    public:
        Camera() noexcept = default;

        void setView(const float4x4 &view) noexcept;
        void setProjection(const float4x4 &projection) noexcept;
        void setViewportSize(const int2 &size) noexcept;
        void setPosition(const float3 &position) noexcept;
        void updateModel(const CameraModel &model) noexcept;
        void update(TimeSpan dt) noexcept;

        [[nodiscard]] const float4x4 &view() const noexcept;
        [[nodiscard]] const float4x4 &projection() const noexcept;
        [[nodiscard]] const float4x4 &viewProjection() const noexcept;
        [[nodiscard]] const float4x4 &inverseViewProjection() const noexcept;
        [[nodiscard]] const float3 &position() const noexcept;
        [[nodiscard]] float3 velocity() const noexcept;
        [[nodiscard]] const float2 &viewportSize() const noexcept;
        [[nodiscard]] u64 cameraVersion() const noexcept;

        [[nodiscard]] float3 worldToClip(const float3 &world) const noexcept;
        [[nodiscard]] float3 clipToWorld(const float4 &clip) const noexcept;
        [[nodiscard]] float3 clientToWorld(const float2 &client, float z_ndc = 0.0f) const noexcept;
        [[nodiscard]] float3 screenToWorld(const float2 &screen, const int2 &framebuffer_size, float z_ndc = 0.0f) const noexcept;
        [[nodiscard]] float2 worldToScreen(const float3 &world, const int2 &framebuffer_size) const noexcept;
        [[nodiscard]] std::pair<float3, float3> clientToWorldRay(const float2 &client) const noexcept;

    private:
        struct State {
            float4x4 view{float4x4::identity()};
            float4x4 projection{view};
            float4x4 viewProjection{view};
            float4x4 inverseViewProjection{view};
            float3 position{zero_v};
            float2 viewportSize{zero_v};
        };

        State m_current{};
        State m_previous{};
        float3 m_velocity{zero_v};
        bool m_pending_cut{false};
        u64 m_camera_version{0};

        void recomputeViewProjection() noexcept;
    };

    class ICameraController : public IInputActionKeyMappingProvider {
    public:
        ~ICameraController() override = default;

        virtual void activate(IInputService &input, Camera &camera) = 0;
        virtual void deactivate() noexcept = 0;
        virtual void update(TimeSpan dt) noexcept = 0;
        virtual void setDeviceType(rhi::DeviceType device_type) noexcept = 0;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override = 0;
    };

    class FreeCameraController final : public ICameraController {
    public:
        FreeCameraController() noexcept;
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
        ~FreeCameraController() noexcept override = default;

    private:
        Camera *m_camera{nullptr};
        float3 m_eye{0.0f, 0.0f, 5.0f};
        float3 m_up{0.0f, 1.0f, 0.0f};
        float m_yaw{0.0f};
        float m_pitch{0.0f};
        float m_move_speed{5.0f};
        float m_rotate_speed{1.0f};
        float2 m_mouse_sensitivity{0.01f, 0.01f};
        float2 m_gamepad_sensitivity{1.0f, 0.02f};
        float m_fov{60.0f * std::numbers::pi_v<float> / 180.0f};
        float m_fov_min{std::numbers::pi_v<float> / 15.0f};
        float m_fov_max{5.0f * std::numbers::pi_v<float> / 7.0f};
        float m_position_inertia{0.15f};
        float m_rotation_inertia{0.15f};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};

        std::unique_ptr<InputAction> m_move_action{};
        std::unique_ptr<InputAction> m_rotate_action{};
        std::unique_ptr<InputAction> m_speed_action{};
        std::unique_ptr<InputAction> m_fov_action{};
        std::unique_ptr<InputAction> m_look_action{};

        bool m_look_active{false};

        float3 m_move_sum{zero_v};

        float2 m_rotate_sum{zero_v};
        float2 m_rotate_accum{zero_v};
        float3 m_gamepad_move_last{zero_v};
        float2 m_gamepad_rotate_last{zero_v};
        float m_speed_sum{0.0f};
        float m_fov_accum{0.0f};

        HashMap<InputKey, float3> m_move_contrib{};
        HashMap<InputKey, float2> m_rotate_contrib{};
        HashMap<InputKey, float> m_speed_contrib{};
        HashMap<InputKey, float> m_fov_contrib{};

        void onMoveStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onMoveCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onMoveTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookStarted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept;
        void onLookCompleted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept;
        void onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onSpeedStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onSpeedCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onFovAccumulate_(const InputActionEvent &event, const InputKey &key) noexcept;
    };

    class PanCameraController final : public ICameraController {
    public:
        PanCameraController() noexcept;
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
        ~PanCameraController() noexcept override = default;

    private:
        Camera *m_camera{nullptr};
        float3 m_eye{0.0f, 0.0f, 10.0f};
        float3 m_up{0.0f, 1.0f, 0.0f};
        float m_yaw{0.0f};
        float m_pitch{0.0f};
        float m_pan_speed{5.0f};
        float m_rotate_speed{1.0f};
        float m_zoom{1.0f};
        float2 m_mouse_sensitivity{0.01f, 0.01f};
        float2 m_gamepad_sensitivity{1.0f, 0.02f};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};

        std::unique_ptr<InputAction> m_move_action{};
        std::unique_ptr<InputAction> m_rotate_action{};
        std::unique_ptr<InputAction> m_speed_action{};
        std::unique_ptr<InputAction> m_fov_action{};
        std::unique_ptr<InputAction> m_look_action{};

        float3 m_move_sum{zero_v};

        float2 m_rotate_sum{zero_v};
        float2 m_rotate_accum{zero_v};
        float3 m_gamepad_move_last{zero_v};
        float2 m_gamepad_rotate_last{zero_v};
        float m_speed_sum{0.0f};
        float m_zoom_accum{0.0f};
        bool m_look_active{false};

        HashMap<InputKey, float3> m_move_contrib{};
        HashMap<InputKey, float2> m_rotate_contrib{};
        HashMap<InputKey, float> m_speed_contrib{};

        void onMoveStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onMoveCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onMoveTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onSpeedStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onSpeedCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomAccumulate_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookStarted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept;
        void onLookCompleted_(const InputActionEvent &event [[maybe_unused]], const InputKey &key [[maybe_unused]]) noexcept;
    };

    class DummyCameraController final : public ICameraController {
    public:
        DummyCameraController() noexcept = default;
        void activate(IInputService &, Camera &camera) noexcept override;
        void deactivate() noexcept override;
        void update(TimeSpan) noexcept override;
        void setDeviceType(rhi::DeviceType) noexcept override;
        void provideInputActionKeyMappings(InputMapping &) const noexcept override;
        ~DummyCameraController() noexcept override = default;

    private:
        Camera *m_camera{nullptr};
    };

    class OrbitCameraController final : public ICameraController {
    public:
        OrbitCameraController() noexcept;
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
        ~OrbitCameraController() noexcept override = default;

        void setTarget(const float3 &target) noexcept { m_target = target; }
        void setDistance(float distance) noexcept { m_distance = clamp(distance, m_min_distance, m_max_distance); }
        [[nodiscard]] const float3 &target() const noexcept { return m_target; }
        [[nodiscard]] float distance() const noexcept { return m_distance; }

    private:
        Camera *m_camera{nullptr};
        float3 m_target{0.0f, 0.0f, 0.0f};
        float m_distance{10.0f};
        float m_yaw{0.0f};
        float m_pitch{0.0f};
        float m_rotate_speed{1.0f};
        float m_zoom_speed{1.0f};
        float m_min_distance{0.1f};
        float m_max_distance{1000.0f};
        float2 m_mouse_sensitivity{0.01f, 0.01f};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};

        std::unique_ptr<InputAction> m_rotate_action{};
        std::unique_ptr<InputAction> m_zoom_action{};
        std::unique_ptr<InputAction> m_look_action{};

        bool m_look_active{false};

        float2 m_rotate_sum{zero_v};
        float2 m_rotate_accum{zero_v};
        float m_zoom_sum{0.0f};
        float m_zoom_accum{0.0f};

        HashMap<InputKey, float2> m_rotate_contrib{};
        HashMap<InputKey, float> m_zoom_contrib{};

        void onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
    };

    class ICameraService : public IService {
    public:
        virtual ~ICameraService() = default;

        [[nodiscard]] virtual std::error_code initialize(IInputService &input) = 0;
        virtual void setController(std::unique_ptr<ICameraController> &&controller) = 0;
        [[nodiscard]] virtual Camera &camera() noexcept = 0;
        virtual void update(TimeSpan dt) noexcept = 0;

        virtual void setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept = 0;
        virtual void setOrthoProjection(float width, float height) noexcept = 0;
        virtual void setDeviceType(rhi::DeviceType type) noexcept = 0;
        virtual void setViewportSize(const int2 &size) noexcept = 0;
    };

    class CameraService final : public ICameraService {
    public:
        CameraService() noexcept;
        ~CameraService() noexcept override;

        [[nodiscard]] std::error_code initialize(IInputService &input) override;
        void setController(std::unique_ptr<ICameraController> &&controller) override;
        [[nodiscard]] Camera &camera() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void deactivateController() noexcept;

        void setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept override;
        void setOrthoProjection(float width, float height) noexcept override;
        void setDeviceType(rhi::DeviceType type) noexcept override;
        void setViewportSize(const int2 &size) noexcept override;

    private:
        safe_ptr<IInputService> m_input{};
        Camera m_camera{};
        std::unique_ptr<ICameraController> m_controller{};
        InputMapping m_controller_mapping{"CameraController"};
        rhi::DeviceType m_device_type{rhi::DeviceType::D3D12};
        float m_fov{};
        float m_near{0.1f};
        float m_far{1000.0f};
    };
}
