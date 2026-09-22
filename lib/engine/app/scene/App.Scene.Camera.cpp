module;
#include "pP/Macros.h"

module engine.app;

import :scene.camera;
import :scene.camera.controller;
import :window.viewport;

import engine.core;
import engine.math;
import std;

namespace pP {
    Camera::Camera(const ECameraProjection projection) noexcept
        : m_camera_mode(projection) {
    }

    void Camera::setCameraMode(const ECameraProjection projection) noexcept {
        if (m_camera_mode != projection) {
            m_camera_mode = projection;
            m_has_camera_cut_next_frame = true;
        }
    }

    void Camera::setJitterSamples(std::optional<CameraJitterSamples> pixel_offsets) noexcept {
        if (pixel_offsets.has_value() && pixel_offsets->empty()) {
            pixel_offsets.reset();
        }
        m_jittered_pixel_offsets = std::move(pixel_offsets);
        m_has_camera_cut_next_frame = true;
    }

    void Camera::updateModel(const TimeSpan dt, const CameraModel &new_model, const Viewport &viewport) noexcept {
        PPR_ASSERT(not isNan<float, 3u>(new_model.m_origin));
        PPR_ASSERT(not isNan(new_model.m_basis));
        PPR_ASSERT(not isNan(new_model.m_fov));
        PPR_ASSERT(not isNan(new_model.m_z_near));
        PPR_ASSERT(not isNan(new_model.m_z_far));

        PPR_ASSERT(isNormalized(new_model.m_basis));

        PPR_ASSERT(new_model.m_fov >= 0 && new_model.m_fov <= pi_v<float>);
        PPR_ASSERT(new_model.m_z_near < new_model.m_z_far);

        // Degenerate viewport (minimized/zero window): keep prior state, since
        // getAspectRatio() asserts on zero height and the jitter path divides by extent.
        // Velocities stay stale here by design; P2 snapshots them explicitly.
        const PixelRect client_rect = viewport.getClientRect();
        if (client_rect.m_extent.x <= 0 or client_rect.m_extent.y <= 0) [[unlikely]] {
            return;
        }

        const bool has_previous_state = m_previous_state.has_value();
        m_previous_state = m_actual_state;

        m_actual_state = new_model;
        m_actual_state.m_camera_mode = m_camera_mode;

        if (has_previous_state) {
            ++m_actual_state.m_revision;
        } else {
            // first frame is always a camera cut
            m_actual_state.m_has_camera_cut = true;
        }
        // handle manual camera cut request from external client
        if (m_has_camera_cut_next_frame) {
            m_has_camera_cut_next_frame = false;
            m_actual_state.m_has_camera_cut = true;
        }
        // reset camera revision each time a camera cut happened
        if (m_actual_state.m_has_camera_cut) {
            m_actual_state.m_revision = 0;
        }

        m_actual_state.m_viewport_size = toFloat<int, 2u>(client_rect.m_extent);
        m_actual_state.m_aspect_ratio = client_rect.getAspectRatio();

        m_actual_state.m_right = quaternionTransform(new_model.m_basis, math::axis_x);
        m_actual_state.m_up = quaternionTransform(new_model.m_basis, math::axis_y);
        m_actual_state.m_forward = quaternionTransform(new_model.m_basis, math::axis_z);

        m_actual_state.m_view = float4x4::lookat(
            new_model.m_origin + m_actual_state.m_forward,
            new_model.m_origin,
            m_actual_state.m_up);

        switch (new_model.m_camera_mode) {
            case ECameraProjection::perspective:
                m_actual_state.m_projection = rhi::getPerspectiveMatrix(
                    new_model.m_fov,
                    m_actual_state.m_aspect_ratio,
                    new_model.m_z_near,
                    new_model.m_z_far);
                break;
            case ECameraProjection::orthographic:
                m_actual_state.m_projection = rhi::getOrthoMatrix(
                    m_actual_state.m_viewport_size.x,
                    m_actual_state.m_viewport_size.y);
                break;
        }

        PPR_ASSERT(not isNan(m_actual_state.m_view));
        PPR_ASSERT(not isNan(m_actual_state.m_projection));

        m_actual_state.m_view_projection = m_actual_state.m_view * m_actual_state.m_projection;
        m_actual_state.m_invert_view = inverse(m_actual_state.m_view);
        m_actual_state.m_invert_projection = inverse(m_actual_state.m_projection);
        m_actual_state.m_invert_view_projection = inverse(m_actual_state.m_view_projection);

        PPR_ASSERT(not isNan(m_actual_state.m_view_projection));
        PPR_ASSERT(not isNan(m_actual_state.m_invert_view));
        PPR_ASSERT(not isNan(m_actual_state.m_invert_projection));
        PPR_ASSERT(not isNan(m_actual_state.m_invert_view_projection));

        float2 pixel_offset{zero_v};
        if (m_jittered_pixel_offsets.has_value() && not m_jittered_pixel_offsets->empty()) {
            const std::size_t period_index = m_actual_state.m_revision % m_jittered_pixel_offsets->size();
            pixel_offset = m_jittered_pixel_offsets->at(period_index);
        }

        // NDC offset pixel_offset * 2 / size in division form: single componentwise op,
        // bit-identical since s / 2 is exact for binary floating point.
        m_actual_state.m_jitter = pixel_offset / (m_actual_state.m_viewport_size / 2.0f); // convert pixel offset to ndc offset with aspect-ratio
        m_actual_state.m_jittered_projection = m_actual_state.m_projection * makeJitterMatrix(m_actual_state.m_jitter);
        m_actual_state.m_jittered_view_projection = m_actual_state.m_view * m_actual_state.m_jittered_projection;
        m_actual_state.m_invert_jittered_view_projection = inverse(m_actual_state.m_jittered_view_projection);

        PPR_ASSERT(not isNan(m_actual_state.m_jittered_projection));
        PPR_ASSERT(not isNan(m_actual_state.m_jittered_view_projection));
        PPR_ASSERT(not isNan(m_actual_state.m_invert_jittered_view_projection));

        // Frusta pin the unjittered VP remapped to D3D [0,1] depth. RayFrustum
        // consumes side planes only, but shares the same corrected matrix so
        // both paths keep a single depth source (remap mirrors
        // makeZeroToOneFrustum in Math.cppm).
        m_actual_state.m_frustum = makeZeroToOneFrustum(m_actual_state.m_view_projection);
        const float4x4 zero_to_one_vp = m_actual_state.m_view_projection * float4x4{
                                            float4{1.0f, 0.0f, 0.0f, 0.0f},
                                            float4{0.0f, 1.0f, 0.0f, 0.0f},
                                            float4{0.0f, 0.0f, 2.0f, 0.0f},
                                            float4{0.0f, 0.0f, -1.0f, 1.0f},
                                        };
        m_actual_state.m_ray_frustum = RayFrustum(zero_to_one_vp);

        // conditionally computes camera velocity, skipped if:
        // * it's the first frame rendered,
        // * a camera cut happened, eg teleport or mode changed,
        // * signalCameraCutNextFrame() was called.
        const auto dt_seconds = static_cast<float>(time::seconds(dt));
        if (m_actual_state.m_has_camera_cut or dt_seconds < epsilon_v<>) {
            m_angular_velocity = 0.0f;
            m_translational_velocity = 0.0f;
            return;
        }

        const CameraSnapshot &previous_state = *m_previous_state;

        m_translational_velocity = (m_actual_state.m_origin - previous_state.m_origin) / dt_seconds;
        m_angular_velocity = angularVelocity(dt_seconds, previous_state.m_basis, m_actual_state.m_basis);

        PPR_ASSERT(not isNan<float, 3u>(m_angular_velocity));
        PPR_ASSERT(not isNan<float, 3u>(m_translational_velocity));
    }

    void Camera::updateModel(const TimeSpan dt, ICameraController &controller, const Viewport &viewport) noexcept {
        CameraModel new_model = m_actual_state;
        controller.updateCameraModel(dt, new_model);
        updateModel(dt, new_model, viewport);
    }
}
