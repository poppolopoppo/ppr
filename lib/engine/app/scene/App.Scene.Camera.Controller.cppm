module;

export module engine.app:scene.camera.controller;

import std;
import engine.core;
import engine.math;

import :input.filtered_analog;
import :input.key;
import :scene.camera;
import :service.input;

export namespace pP {
    struct InputAction;
    class InputMapping;

    class ICameraController : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~ICameraController() noexcept = default;

        virtual void provideInputActionKeyMappings(InputMapping &out_mapping) const = 0;

        virtual void updateCameraModel(TimeSpan dt, CameraModel &model) noexcept = 0;

        virtual void resetInputState() noexcept {
        }
    };

    class DummyCameraController final : public ICameraController {
    public:
        DummyCameraController() noexcept = default;

        void provideInputActionKeyMappings(InputMapping &) const override {
        }

        void updateCameraModel(TimeSpan, CameraModel &) noexcept override {
        }
    };

    namespace details {
        class BasicCameraController : public ICameraController {
        public:
            const std::unique_ptr<InputAction> m_translate_action{};
            const std::unique_ptr<InputAction> m_rotate_action{};
            const std::unique_ptr<InputAction> m_speed_action{};
            const std::unique_ptr<InputAction> m_fov_action{};
            const std::unique_ptr<InputAction> m_look_action{};

            // Convergence rates (s^-1) for the frame-invariant FilteredAnalog
            // contract (alpha = 1 - exp(-lambda * dt)). Position/rotation use
            // a ~125ms time constant: responsive yet smooth at 60Hz
            // (alpha ~= 0.12). FOV keeps the slower 0.8 rate; speed uses 2.5.
            FilteredAnalog<Quaternion> m_rotation_analog{identity_v, 8.0f};
            FilteredAnalog<float3> m_position_analog{zero_v, 8.0f};
            FilteredAnalog<float> m_fov_analog{pi_over_3_v<>, 0.8f};
            FilteredAnalog<float> m_speed_analog{1.0f, 2.5f};

            float2 m_fov_min_max{pi_v<float> / 9.0f, pi_v<float> / 2.0f};
            float2 m_speed_multiplier_min_max{0.1f, 20.0f};

            float2 m_mouse_sensitivity{0.0025f, 0.0025f};
            float2 m_gamepad_sensitivity{0.6f, 0.5f};

            float m_forward_speed{3.0f};
            float m_strafe_speed{3.0f};
            float m_upward_speed{3.0f};

            float m_heading_speed{1.8f};
            float m_pitch_speed{1.2f};

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

            void resetInputState() noexcept override;

            void updateCameraModel(TimeSpan dt, CameraModel &model) noexcept override;

            void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;

        protected:
            BasicCameraController();

            virtual void updateCameraPose_(TimeSpan dt, CameraModel &model) noexcept;

            void setTranslateRate_(const InputKey &key, const float3 &rate) noexcept;

            void setRotateRate_(const InputKey &key, const float2 &rate) noexcept;

            void setSpeedRate_(const InputKey &key, float rate) noexcept;

            void setFovRate_(const InputKey &key, float rate) noexcept;

            void translateCamera_(const float3 &delta) noexcept { m_delta_position += delta; }

            void rotateCamera_(float heading, float pitch, float roll = zero_v) noexcept;

            void rotateCamera_(const Quaternion &delta) noexcept { m_delta_rotation *= delta; }

            Quaternion m_delta_rotation{identity_v};
            float3 m_delta_position{zero_v};

            FlatMap<InputKey, float3> m_translate_rates{};
            FlatMap<InputKey, float2> m_rotate_rates{};
            FlatMap<InputKey, float> m_speed_rates{};
            FlatMap<InputKey, float> m_fov_rates{};

            float3 m_translate_impulse{zero_v};
            float2 m_rotate_impulse{zero_v};
            float m_speed_impulse{zero_v};
            float m_fov_impulse{zero_v};

            bool m_has_teleported{false};
            bool m_has_mouse_look{false};
        };
    }

    class FreeCameraController final : public details::BasicCameraController {
    public:
        FreeCameraController() = default;

        void lookAt(const float3 &eye, float heading, float pitch, bool has_teleported = false) noexcept;

        void lookAt(const float3 &eye, const float3 &target, const float3 &up, bool has_teleported = false) noexcept;

        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
    };

    class PanCameraController final : public details::BasicCameraController {
    public:
        PanCameraController() = default;

        [[nodiscard]] Quaternion getParallelBasis() const noexcept { return m_rotation_analog.filtered(); }

        void setParallelPlane(const float3 &plane_normal, const float3 &plane_up, bool has_teleported = false) noexcept;

        void setParallelPlane(const Quaternion &plane_basis, bool has_teleported = false) noexcept;

        void translate(const float3 &eye, bool has_teleported = false) noexcept;

        void provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept override;
    };

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

        FilteredAnalog<float3> m_target_analog{zero_v, 8.0f};
        FilteredAnalog<float> m_radius_analog{1.0f, 8.0f};
    };
}
