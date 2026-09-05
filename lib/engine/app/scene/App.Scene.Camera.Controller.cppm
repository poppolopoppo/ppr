module;

export module engine.app:scene.camera.controller;

import std;
import engine.core;
import engine.math;

import :input.filtered_analog;
import :scene.camera;
import :service.input;

export namespace pP {
    struct InputAction;
    class InputMapping;

    // ------------------------------------------------------------------
    // ICameraController — camera controller interface
    // ------------------------------------------------------------------

    class ICameraController : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~ICameraController() noexcept = default;

        virtual void provideInputActionKeyMappings(InputMapping &out_mapping) const = 0;

        virtual void updateCameraModel(TimeSpan dt, CameraModel &model) noexcept = 0;
    };

    // ------------------------------------------------------------------
    // DummyCameraController — noop place holder
    // ------------------------------------------------------------------

    class DummyCameraController final : public ICameraController {
    public:
        DummyCameraController() noexcept = default;

        void provideInputActionKeyMappings(InputMapping &) const override {
        }

        void updateCameraModel(TimeSpan, CameraModel &) noexcept override {
        }
    };

    // ------------------------------------------------------------------
    // BasicCameraController — abstract with common controls for all others
    // ------------------------------------------------------------------

    namespace details {
        class BasicCameraController : public ICameraController {
        public:
            const std::unique_ptr<InputAction> m_translate_action{};
            const std::unique_ptr<InputAction> m_rotate_action{};
            const std::unique_ptr<InputAction> m_speed_action{};
            const std::unique_ptr<InputAction> m_fov_action{};
            const std::unique_ptr<InputAction> m_look_action{};

            FilteredAnalog<Quaternion> m_rotation_analog{identity_v, 0.15f};
            FilteredAnalog<float3> m_position_analog{zero_v, 0.15f};
            FilteredAnalog<float> m_fov_analog{pi_over_3_v<>, 0.8f};
            FilteredAnalog<float> m_speed_analog{1.0f, 0.8f};

            float2 m_fov_min_max{pi_v<float> / 15.0f, 5.0f * pi_v<float> / 7.0f};
            float2 m_speed_multiplier_min_max{0.1f, 50.0f};

            float2 m_mouse_sensitivity{0.1f};
            float2 m_gamepad_sensitivity{0.3f, 0.1f};

            float m_forward_speed{1.0f};
            float m_strafe_speed{1.0f};
            float m_upward_speed{1.0f};

            float m_heading_speed{10.0f};
            float m_pitch_speed{10.0f};

            [[nodiscard]] bool hasTeleported() const noexcept { return m_has_teleported; }

            [[nodiscard]] float3 getPosition() const noexcept { return m_position_analog.filtered(); }
            [[nodiscard]] Quaternion getRotation() const noexcept { return m_rotation_analog.filtered(); }
            [[nodiscard]] float getFov() const noexcept { return m_fov_analog.filtered(); }
            [[nodiscard]] float getSpeedMultiplier() const noexcept { return m_speed_analog.filtered(); }

            [[nodiscard]] float3 getTranslateSpeed() const noexcept { return float3(m_strafe_speed, m_upward_speed, m_forward_speed); }
            [[nodiscard]] float2 getRotateSpeed() const noexcept { return float2(m_heading_speed, m_pitch_speed); }

            [[nodiscard]] float getPositionInertia() const noexcept { return m_position_analog.sensitivity(); }
            void setPositionInertia(const float value) noexcept { m_position_analog.setSensitivity(value); }

            [[nodiscard]] float getRotationInertia() const noexcept { return m_rotation_analog.sensitivity(); }
            void setRotationInertia(const float value) noexcept { m_rotation_analog.setSensitivity(value); }

            void updateCameraModel(TimeSpan dt, CameraModel &model) noexcept override;

            void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;

        protected:
            BasicCameraController();

            virtual void updateCameraPose_(TimeSpan dt, CameraModel &model) noexcept;

            void translateCamera_(const float3 &delta) noexcept { m_delta_position += delta; }

            void rotateCamera_(float heading, float pitch, float roll = zero_v) noexcept;

            void rotateCamera_(const Quaternion &delta) noexcept { m_delta_rotation *= delta; }

            Quaternion m_delta_rotation{identity_v};
            float3 m_delta_position{zero_v};

            bool m_has_teleported{false};
            bool m_has_mouse_look{false};
        };
    }

    // ------------------------------------------------------------------
    // FreeCameraController — free-flight camera controller
    // ------------------------------------------------------------------

    class FreeCameraController final : public details::BasicCameraController {
    public:
        FreeCameraController() = default;

        void lookAt(const float3 &eye, float heading, float pitch, bool has_teleported = false) noexcept;

        void lookAt(const float3 &eye, const float3 &target, const float3 &up, bool has_teleported = false) noexcept;

        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
    };

    // ------------------------------------------------------------------
    // PanCameraController — pan parallel to a 3d plane
    // ------------------------------------------------------------------

    class PanCameraController final : public details::BasicCameraController {
    public:
        PanCameraController() = default;

        [[nodiscard]] Quaternion getParallelBasis() const noexcept { return m_rotation_analog.filtered(); }

        void setParallelPlane(const float3 &plane_normal, const float3 &plane_up, bool has_teleported = false) noexcept;

        void setParallelPlane(const Quaternion &plane_basis, bool has_teleported = false) noexcept;

        void translate(const float3 &eye, bool has_teleported = false) noexcept;

        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
    };

    // ------------------------------------------------------------------
    // OrbitCameraController — orbit around a point
    // ------------------------------------------------------------------

    class OrbitCameraController final : public details::BasicCameraController {
    public:
        OrbitCameraController() = default;

        [[nodiscard]] float3 getOrbitTarget() const noexcept { return m_target_analog.filtered(); }

        void setOrbitTarget(const float3 &target, bool has_teleported = false) noexcept;

        void setOrbitRadius(float radius, bool has_teleported = false) noexcept;

        void lookAt(const float3 &eye, const float3 &target, bool has_teleported = false) noexcept;

        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;

    protected:
        void updateCameraPose_(TimeSpan dt, CameraModel &model) noexcept override;

        FilteredAnalog<float3> m_target_analog{zero_v, 0.15f};
        FilteredAnalog<float> m_radius_analog{1.0f, 0.15f};
    };
}
