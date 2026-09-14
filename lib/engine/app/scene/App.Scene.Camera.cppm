module;

export module engine.app:scene.camera;

import engine.core;
import engine.math;
import std;

export namespace pP {
    class ICameraController;

    enum class ECameraProjection : bool {
        orthographic = false,
        perspective,
    };

    using CameraJitterSamples = TransformView<float2>;

    // ------------------------------------------------------------------
    // CameraModel — camera pose and projection parameters
    // ------------------------------------------------------------------

    struct CameraModel {
        Quaternion m_basis{identity_v};

        float3 m_origin{zero_v};

        float m_fov{pi_over_3_v<>};

        float m_z_near{0.01f};
        float m_z_far{10000.0f};

        ECameraProjection m_camera_mode{ECameraProjection::perspective};

        // true when camera initialized/teleported
        bool m_has_camera_cut{false};
    };

    struct CameraSnapshot : CameraModel {
        using CameraModel::operator=;

        float4x4 m_projection{identity_v};
        float4x4 m_invert_projection{identity_v};

        float4x4 m_view{identity_v};
        float4x4 m_invert_view{identity_v};

        float4x4 m_view_projection{identity_v};
        float4x4 m_invert_view_projection{identity_v};

        float4x4 m_jittered_projection{identity_v};
        float4x4 m_jittered_view_projection{identity_v};
        float4x4 m_invert_jittered_view_projection{identity_v};

        Frustum m_frustum{};
        RayFrustum m_ray_frustum{};

        float3 m_right{math::axis_x};
        float3 m_up{math::axis_y};
        float3 m_forward{math::axis_z};

        float2 m_jitter{zero_v};

        float2 m_viewport_size{zero_v};
        float m_aspect_ratio{1.0f};

        std::size_t m_revision{0};
    };

    // ------------------------------------------------------------------
    // Camera — camera state and view/projection transforms
    // ------------------------------------------------------------------

    struct Viewport;

    /// Note: inherits safe_object — debug-mode ref-count changes layout.
    class Camera : public safe_object {
    public:
        explicit Camera(ECameraProjection projection = ECameraProjection::perspective) noexcept;

        [[nodiscard]] const CameraSnapshot &getSnapshot() const noexcept { return m_actual_state; }
        [[nodiscard]] const std::optional<CameraSnapshot> &getPreviousSnapshot() const noexcept { return m_previous_state; }

        [[nodiscard]] bool hasCameraCut() const noexcept { return m_actual_state.m_has_camera_cut; }

        [[nodiscard]] std::size_t getRevision() const noexcept { return m_actual_state.m_revision; }

        [[nodiscard]] ECameraProjection getCameraMode() const noexcept { return m_actual_state.m_camera_mode; }
        void setCameraMode(ECameraProjection projection) noexcept;

        [[nodiscard]] const float3 &getOrigin() const noexcept { return m_actual_state.m_origin; }
        [[nodiscard]] const Quaternion &getBasis() const noexcept { return m_actual_state.m_basis; }

        [[nodiscard]] const float3 &getUp() const noexcept { return m_actual_state.m_up; }
        [[nodiscard]] const float3 &getForward() const noexcept { return m_actual_state.m_forward; }
        [[nodiscard]] const float3 &getRight() const noexcept { return m_actual_state.m_right; }

        [[nodiscard]] float getFov() const noexcept { return m_actual_state.m_fov; }
        [[nodiscard]] float getFarZ() const noexcept { return m_actual_state.m_z_far; }
        [[nodiscard]] float getNearZ() const noexcept { return m_actual_state.m_z_near; }

        [[nodiscard]] const float4x4 &getProjection() const noexcept { return m_actual_state.m_projection; }
        [[nodiscard]] const float4x4 &getView() const noexcept { return m_actual_state.m_view; }
        [[nodiscard]] const float4x4 &getViewProjection() const noexcept { return m_actual_state.m_view_projection; }
        [[nodiscard]] const float4x4 &getInvertProjection() const noexcept { return m_actual_state.m_invert_projection; }
        [[nodiscard]] const float4x4 &getInvertView() const noexcept { return m_actual_state.m_invert_view; }
        [[nodiscard]] const float4x4 &getInvertViewProjection() const noexcept { return m_actual_state.m_invert_view_projection; }

        [[nodiscard]] const Frustum &getFrustum() const noexcept { return m_actual_state.m_frustum; }
        [[nodiscard]] const RayFrustum &getRayFrustum() const noexcept { return m_actual_state.m_ray_frustum; }

        [[nodiscard]] const float3 &getAngularVelocity() const noexcept { return m_angular_velocity; }
        [[nodiscard]] const float3 &getTranslationalVelocity() const noexcept { return m_translational_velocity; }
        [[nodiscard]] float3 getCameraVelocity() const noexcept { return m_angular_velocity + m_translational_velocity; }

        [[nodiscard]] const float2 &getJitter() const noexcept { return m_actual_state.m_jitter; }

        // Non-owning view: caller backing storage must outlive updateModel.
        void setJitterSamples(std::optional<CameraJitterSamples> pixel_offsets) noexcept;

        [[nodiscard]] const float4x4 &getJitteredProjection() const noexcept { return m_actual_state.m_jittered_projection; }
        [[nodiscard]] const float4x4 &getJitteredViewProjection() const noexcept { return m_actual_state.m_jittered_view_projection; }
        [[nodiscard]] const float4x4 &getInvertJitteredViewProjection() const noexcept { return m_actual_state.m_invert_jittered_view_projection; }

        void signalCameraCutNextFrame() noexcept {
            m_has_camera_cut_next_frame = true;
        }

        void updateModel(TimeSpan dt, const CameraModel &new_model, const Viewport &viewport) noexcept;

        void updateModel(TimeSpan dt, ICameraController &controller, const Viewport &viewport) noexcept;

    private:
        CameraSnapshot m_actual_state{};
        std::optional<CameraSnapshot> m_previous_state;
        std::optional<CameraJitterSamples> m_jittered_pixel_offsets;

        float3 m_angular_velocity{zero_v};
        float3 m_translational_velocity{zero_v};

        ECameraProjection m_camera_mode{ECameraProjection::perspective};

        bool m_has_camera_cut_next_frame{false};
    };
}
