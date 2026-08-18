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

export namespace pP {
    class Camera {
    public:
        Camera() noexcept = default;

        void setView(const float4x4 &view) noexcept;
        void setProjection(const float4x4 &projection) noexcept;
        void setViewportSize(const int2 &size) noexcept;
        void setPosition(const float3 &position) noexcept;
        void update(TimeSpan dt) noexcept;

        [[nodiscard]] const float4x4 &view() const noexcept;
        [[nodiscard]] const float4x4 &projection() const noexcept;
        [[nodiscard]] const float4x4 &viewProjection() const noexcept;
        [[nodiscard]] const float4x4 &inverseViewProjection() const noexcept;
        [[nodiscard]] const float3 &position() const noexcept;
        [[nodiscard]] float3 velocity() const noexcept;
        [[nodiscard]] const float2 &viewportSize() const noexcept;
        [[nodiscard]] u64 cameraVersion() const noexcept;

    private:
        friend class FreeCameraController;

        void recomputeViewProjection() noexcept;

        float4x4 m_view{
            float4{1.0f, 0.0f, 0.0f, 0.0f},
            float4{0.0f, 1.0f, 0.0f, 0.0f},
            float4{0.0f, 0.0f, 1.0f, 0.0f},
            float4{0.0f, 0.0f, 0.0f, 1.0f}};
        float4x4 m_projection{m_view};
        float4x4 m_view_projection{m_view};
        float4x4 m_inverse_view_projection{m_view};
        float3 m_position{zero_v};
        float3 m_prev_position{zero_v};
        float3 m_velocity{zero_v};
        float2 m_viewport_size{zero_v};
        u64 m_camera_version{0};
    };

    class ICameraController {
    public:
        virtual ~ICameraController() = default;

        virtual void activate(IInputService &input, Camera &camera) = 0;
        virtual void deactivate() noexcept = 0;
        virtual void update(TimeSpan dt) noexcept = 0;
        virtual void setDeviceType(rhi::DeviceType device_type) noexcept = 0;
    };

    class FreeCameraController final : public ICameraController {
    public:
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        virtual ~FreeCameraController() noexcept override = default;

    private:
        safe_ptr<IInputService> m_input{};
        Camera *m_camera{nullptr};
        InputListener m_listener{};
        float3 m_eye{0.0f, 0.0f, 5.0f};
        float3 m_up{0.0f, 1.0f, 0.0f};
        float m_yaw{0.0f};
        float m_pitch{0.0f};
        float m_move_speed{5.0f};
        bool m_forward{false};
        bool m_back{false};
        bool m_left{false};
        bool m_right{false};
        bool m_up_key{false};
        bool m_down_key{false};
        float2 m_look_delta{zero_v};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};

        void onMessage_(const InputMessage &msg) noexcept;
    };

    class PanCameraController final : public ICameraController {
    public:
        void activate(IInputService &input, Camera &camera) override;
        void deactivate() noexcept override;
        void update(TimeSpan dt) noexcept override;
        void setDeviceType(rhi::DeviceType device_type) noexcept override;
        virtual ~PanCameraController() noexcept override = default;

    private:
        safe_ptr<IInputService> m_input{};
        Camera *m_camera{nullptr};
        InputListener m_listener{};
        float3 m_eye{0.0f, 0.0f, 10.0f};
        float3 m_up{0.0f, 1.0f, 0.0f};
        float m_yaw{0.0f};
        float m_pitch{0.0f};
        float m_pan_speed{5.0f};
        float m_zoom{1.0f};
        rhi::DeviceType m_device_type{rhi::DeviceType::Default};
        bool m_forward{false};
        bool m_back{false};
        bool m_left{false};
        bool m_right{false};
        float2 m_look_delta{zero_v};

        void onMessage_(const InputMessage &msg) noexcept;
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
        rhi::DeviceType m_device_type{rhi::DeviceType::D3D12};
        float m_fov{};
        float m_near{0.1f};
        float m_far{1000.0f};
    };
}
