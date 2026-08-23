module;

#include "pP/Macros.h"

export module engine.app:viewport.camera;

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
import :viewport.filtered_analog;

export namespace pP {
    enum class ECameraProjection : u8 {
        Perspective = 0,
        Orthographic,
    };

    // ------------------------------------------------------------------
    // CameraModel — camera pose and projection parameters
    // ------------------------------------------------------------------

    struct CameraModel {
        float3 position{zero_v};
        float3 right{float3{1.0f, 0.0f, 0.0f}};
        float3 up{float3{0.0f, 1.0f, 0.0f}};
        float3 forward{float3{0.0f, 0.0f, 1.0f}};
        float fov{std::numbers::pi_v<float> / 3.0f};
        float zNear{0.01f};
        float zFar{10000.0f};
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

    // ------------------------------------------------------------------
    // Frustum — view frustum planes, corners, and bounding box
    // ------------------------------------------------------------------

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
        static_assert(std::to_underlying(EFrustumPlane::Bottom) + 1 == 6,
                      "EFrustumPlane count must match m_planes array size");

        float4 m_planes[6];
        float3 m_corners[8];
        float3 m_bbox_min{zero_v};
        float3 m_bbox_max{zero_v};
        rhi::EProjectionConvention m_convention{rhi::EProjectionConvention::D3D};
    };

    // ------------------------------------------------------------------
    // Camera — camera state and view/projection transforms
    // ------------------------------------------------------------------

    /// Note: inherits safe_object — debug-mode ref-count changes layout.
    class Camera : public safe_object {
    private:
        struct State {
            Frustum frustum{};
            float4x4 projection{float4x4::identity()};
            float4x4 view{float4x4::identity()};
            float4x4 invertProjection{float4x4::identity()};
            float4x4 invertView{float4x4::identity()};
            float4x4 invertViewProjection{float4x4::identity()};
            CameraModel model{};
        };

    public:
        Camera() noexcept = default;

        [[nodiscard]] ECameraProjection mode() const noexcept { return m_mode; }
        void setMode(ECameraProjection value) noexcept;

        [[nodiscard]] const State &currentState() const noexcept { return m_current; }
        [[nodiscard]] const State &previousState() const noexcept { return m_previous; }

        [[nodiscard]] const Frustum &frustum() const noexcept { return m_current.frustum; }
        [[nodiscard]] const float4x4 &projection() const noexcept { return m_current.projection; }
        [[nodiscard]] const float4x4 &view() const noexcept { return m_current.view; }
        [[nodiscard]] const float4x4 &viewProjection() const noexcept { return m_viewProjection; }
        [[nodiscard]] const float4x4 &invertProjection() const noexcept { return m_current.invertProjection; }
        [[nodiscard]] const float4x4 &invertView() const noexcept { return m_current.invertView; }
        [[nodiscard]] const float4x4 &invertViewProjection() const noexcept { return m_current.invertViewProjection; }

        [[nodiscard]] const float3 &up() const noexcept { return m_current.model.up; }
        [[nodiscard]] const float3 &forward() const noexcept { return m_current.model.forward; }
        [[nodiscard]] const float3 &right() const noexcept { return m_current.model.right; }
        [[nodiscard]] const float3 &position() const noexcept { return m_current.model.position; }

        [[nodiscard]] float zFar() const noexcept { return m_current.model.zFar; }
        [[nodiscard]] float zNear() const noexcept { return m_current.model.zNear; }

        void updateModel(const CameraModel &model, const int2 &viewportSize) noexcept;
        void update(TimeSpan dt) noexcept;

        void setView(const float4x4 &view) noexcept;
        void setProjection(const float4x4 &projection) noexcept;
        void setViewportSize(const int2 &size) noexcept;
        void setPosition(const float3 &position) noexcept;
        [[nodiscard]] const float2 &viewportSize() const noexcept { return m_viewportSize; }
        [[nodiscard]] float3 velocity() const noexcept { return m_velocity; }
        [[nodiscard]] u64 cameraVersion() const noexcept { return m_camera_version; }
        [[nodiscard]] [[deprecated("use invertViewProjection")]] const float4x4 &inverseViewProjection() const noexcept { return m_current.invertViewProjection; }

        [[nodiscard]] float3 worldToClip(const float3 &world) const noexcept;
        [[nodiscard]] float3 clipToWorld(const float4 &clip) const noexcept;
        [[nodiscard]] float3 clientToWorld(const float2 &client, float z_ndc = 0.0f) const noexcept;
        [[nodiscard]] float3 screenToWorld(const float2 &screen, const int2 &framebuffer_size, float z_ndc = 0.0f) const noexcept;
        [[nodiscard]] float2 worldToScreen(const float3 &world, const int2 &framebuffer_size) const noexcept;
        [[nodiscard]] std::pair<float3, float3> clientToWorldRay(const float2 &client) const noexcept;

    private:
        State m_current{};
        State m_previous{};
        ECameraProjection m_mode{ECameraProjection::Perspective};
        float2 m_viewportSize{zero_v};
        float4x4 m_viewProjection{float4x4::identity()};
        float3 m_velocity{zero_v};
        bool m_pending_cut{false};
        u64 m_camera_version{0};

        void recomputeViewProjection() noexcept;
    };

    // ------------------------------------------------------------------
    // ICameraController — camera controller interface
    // ------------------------------------------------------------------

    class ICameraController : public IInputActionKeyMappingProvider {
    public:
        ~ICameraController() override = default;

        virtual void activate(IInputService &input, Camera &camera) = 0;
        virtual void deactivate() noexcept = 0;
        virtual void updateCamera(TimeSpan dt, CameraModel &model) noexcept = 0;
        virtual void setDeviceType(rhi::DeviceType) noexcept {}
        virtual void teleport(const float3 &eye, float yaw, float pitch) noexcept = 0;
        virtual void teleport(const float3 &eye, const float3 &target) noexcept;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override = 0;
    };

    // ------------------------------------------------------------------
    // FreeCameraController — free-flight camera controller
    // ------------------------------------------------------------------

    class FreeCameraController final : public ICameraController {
    public:
        FreeCameraController();
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void updateCamera(TimeSpan dt, CameraModel &model) noexcept override;
        void teleport(const float3 &eye, float yaw, float pitch) noexcept override;
        void teleport(const float3 &eye, const float3 &target) noexcept override;
        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
        ~FreeCameraController() noexcept override = default;

        void lookAt(const float3 &eye, float heading, float pitch, bool teleport = false) noexcept;
        void lookAt(const float3 &eye, const float3 &target, const float3 &up, bool teleport = false) noexcept;
        void translate(const float3 &delta) noexcept;
        void rotate(const float2 &delta) noexcept;

        [[nodiscard]] float forwardSpeed() const noexcept { return m_forward_speed; }
        void setForwardSpeed(float value) noexcept;
        [[nodiscard]] float strafeSpeed() const noexcept { return m_strafe_speed; }
        void setStrafeSpeed(float value) noexcept;
        [[nodiscard]] float upwardSpeed() const noexcept { return m_upward_speed; }
        void setUpwardSpeed(float value) noexcept;
        [[nodiscard]] float headingSpeed() const noexcept { return m_heading_speed; }
        void setHeadingSpeed(float value) noexcept;
        [[nodiscard]] float pitchSpeed() const noexcept { return m_pitch_speed; }
        void setPitchSpeed(float value) noexcept;
        [[nodiscard]] float2 fovMinMax() const noexcept { return m_fov_min_max; }
        void setFovMinMax(float2 value) noexcept;
        [[nodiscard]] float2 speedMultiplierMinMax() const noexcept { return m_speed_multiplier_range; }
        void setSpeedMultiplierMinMax(float2 value) noexcept;
        [[nodiscard]] float2 mouseSensitivity() const noexcept { return m_mouse_sensitivity; }
        void setMouseSensitivity(float2 value) noexcept;
        [[nodiscard]] float2 gamepadSensitivity() const noexcept { return m_gamepad_sensitivity; }
        void setGamepadSensitivity(float2 value) noexcept;
        [[nodiscard]] float3 position() const noexcept;
        [[nodiscard]] Quaternion rotation() const noexcept;
        [[nodiscard]] float fov() const noexcept;
        [[nodiscard]] float speedMultiplier() const noexcept;
        [[nodiscard]] float positionInertia() const noexcept;
        void setPositionInertia(float value) noexcept;
        [[nodiscard]] float rotationInertia() const noexcept;
        void setRotationInertia(float value) noexcept;
        [[nodiscard]] InputAction &fovInput() const noexcept { return *m_fov_action; }
        [[nodiscard]] InputAction &lookInput() const noexcept { return *m_look_action; }
        [[nodiscard]] InputAction &moveInput() const noexcept { return *m_move_action; }
        [[nodiscard]] InputAction &rotateInput() const noexcept { return *m_rotate_action; }
        [[nodiscard]] InputAction &speedInput() const noexcept { return *m_speed_action; }
        [[nodiscard]] bool isTeleported() const noexcept { return m_b_teleported; }

    private:
        Camera *m_camera{nullptr};

        std::unique_ptr<InputAction> m_move_action{};
        std::unique_ptr<InputAction> m_rotate_action{};
        std::unique_ptr<InputAction> m_speed_action{};
        std::unique_ptr<InputAction> m_fov_action{};
        std::unique_ptr<InputAction> m_look_action{};

        float3 m_delta_position{zero_v};
        float2 m_delta_rotation{zero_v};
        bool m_b_teleported{false};
        bool m_b_mouse_look{false};

        float m_forward_speed{1.0f};
        float m_strafe_speed{1.0f};
        float m_upward_speed{1.0f};
        float m_heading_speed{10.0f};
        float m_pitch_speed{10.0f};
        float2 m_fov_min_max{std::numbers::pi_v<float> / 15.0f, 5.0f * std::numbers::pi_v<float> / 7.0f};
        float2 m_speed_multiplier_range{0.1f, 50.0f};
        float2 m_mouse_sensitivity{0.05f};
        float2 m_gamepad_sensitivity{0.3f, 0.1f};

        FilteredAnalog<float3> m_position_analog{};
        FilteredAnalog<Quaternion> m_rotation_analog{};
        FilteredAnalog<float> m_fov_analog{};
        FilteredAnalog<float> m_speed_analog{};
    };

    // ------------------------------------------------------------------
    // PanCameraController — pan/zoom camera controller
    // ------------------------------------------------------------------

    class PanCameraController final : public ICameraController {
    public:
        PanCameraController();
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void updateCamera(TimeSpan dt, CameraModel &model) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        void teleport(const float3 &eye, float yaw, float pitch) noexcept override;
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

        float m_speed_sum{0.0f};
        bool m_look_active{false};

        HashMap<InputKey, float> m_speed_contrib{};

        FilteredAnalog<float3> m_position_analog{};
        FilteredAnalog<Quaternion> m_rotation_analog{};
        FilteredAnalog<float> m_zoom_analog{};

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

    // ------------------------------------------------------------------
    // DummyCameraController — no-op camera controller
    // ------------------------------------------------------------------

    class DummyCameraController final : public ICameraController {
    public:
        DummyCameraController() noexcept = default;
        void activate(IInputService &, Camera &camera) noexcept override;
        void deactivate() noexcept override;
        void updateCamera(TimeSpan, CameraModel &) noexcept override;
        void setDeviceType(rhi::DeviceType) noexcept override;
        void teleport(const float3 &eye, float yaw, float pitch) noexcept override;
        void provideInputActionKeyMappings(InputMapping &) const noexcept override;
        ~DummyCameraController() noexcept override = default;

    private:
        Camera *m_camera{nullptr};
    };

    // ------------------------------------------------------------------
    // OrbitCameraController — orbit camera controller
    // ------------------------------------------------------------------

    class OrbitCameraController final : public ICameraController {
    public:
        OrbitCameraController();
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void updateCamera(TimeSpan dt, CameraModel &model) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        void teleport(const float3 &eye, const float3 &target) noexcept override;
        void teleport(const float3 &eye, float yaw, float pitch) noexcept override;
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

        FilteredAnalog<float2> m_rotate_analog{};
        FilteredAnalog<float> m_zoom_analog{};

        void onRotateStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onRotateTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onZoomTriggered_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookStarted_(const InputActionEvent &event, const InputKey &key) noexcept;
        void onLookCompleted_(const InputActionEvent &event, const InputKey &key) noexcept;
    };

    // ------------------------------------------------------------------
    // ICameraService — camera service interface
    // ------------------------------------------------------------------

    class ICameraService : public IService {
    public:
        ~ICameraService() override = default;

        [[nodiscard]] virtual std::error_code initialize(IInputService &input) = 0;
        virtual void setController(std::unique_ptr<ICameraController> &&controller) = 0;
        [[nodiscard]] virtual Camera &camera() noexcept = 0;
        virtual void update(TimeSpan dt) noexcept = 0;

        virtual void setPerspectiveProjection(float fov, float aspect, float near_, float far_) noexcept = 0;
        virtual void setOrthoProjection(float width, float height) noexcept = 0;
        virtual void setDeviceType(rhi::DeviceType type) noexcept = 0;
        virtual void setViewportSize(const int2 &size) noexcept = 0;
        virtual void teleport(const float3 &eye, float yaw, float pitch) noexcept = 0;
        virtual void teleport(const float3 &eye, const float3 &target) noexcept = 0;
    };

    // ------------------------------------------------------------------
    // CameraService — camera service implementation
    // ------------------------------------------------------------------

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
        void teleport(const float3 &eye, float yaw, float pitch) noexcept override;
        void teleport(const float3 &eye, const float3 &target) noexcept override;

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
