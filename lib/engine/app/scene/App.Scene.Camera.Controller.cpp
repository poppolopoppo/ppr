module;
#include "pP/Macros.h"
module engine.app;

import :scene.camera.controller;
import :input.action;
import :input.filtered_analog;
import :input.key;

import engine.core;
import engine.math;
import std;

namespace pP {
    namespace {
        [[nodiscard]] bool isIdentityRotation_(const Quaternion &q) noexcept {
            return std::abs(dot(q, Quaternion::identity())) > 1.0f - 1e-6f;
        }

        [[nodiscard]] Quaternion normalizedOrIdentity_(const Quaternion &q) noexcept {
            return dot(q, q) > epsilon_v<float> ? normalize(q) : Quaternion::identity();
        }

        [[nodiscard]] Quaternion clampPitch_(const Quaternion &basis, const float max_pitch) noexcept {
            const float pitch = static_cast<float>(
                std::atan2(-quaternionTransform(basis, math::forward).y, quaternionTransform(basis, math::axis_y).y));
            if (std::abs(pitch) > max_pitch) {
                return normalize(basis * Quaternion::rotateX(std::copysign(max_pitch, pitch) - pitch));
            }
            return basis;
        }

        // Pan translation integrates per-second rates; clamp the frame so a
        // hitch never teleports. Non-positive/non-finite rests instead.
        [[nodiscard]] TimeSpan clampFrameDt_(const TimeSpan dt) noexcept {
            const double seconds = time::seconds(dt);
            if (not std::isfinite(seconds) or seconds <= 0.0) {
                return TimeSpan{};
            }
            const double clamped = std::min(seconds, 0.25);
            return std::chrono::duration_cast<TimeSpan>(std::chrono::duration<double>(clamped));
        }
    } // namespace

    // ------------------------------------------------------------------
    // BasicCameraController — abstract with common controls for all others
    // ------------------------------------------------------------------

    details::BasicCameraController::BasicCameraController() // NOLINT(*-use-equals-default)
        : m_translate_action{std::make_unique<InputAction>("CameraMove", EInputValueType::axis_3d)},
          m_rotate_action{std::make_unique<InputAction>("CameraRotate", EInputValueType::axis_2d)},
          m_speed_action{std::make_unique<InputAction>("CameraSpeed", EInputValueType::axis_1d)},
          m_fov_action{std::make_unique<InputAction>("CameraFov", EInputValueType::axis_1d)},
          m_look_action{std::make_unique<InputAction>("CameraLook", EInputValueType::digital)} {
        m_translate_action->setTriggered([self{safe_ptr(this)}](const InputActionEvent &event, const InputKey &key) noexcept {
            const InputAxis3D &value = event.getAxis3DValue();
            if (key.isMouse()) {
                self->m_translate_impulse += value.m_relative;
            } else {
                self->setTranslateRate_(key, value.m_absolute);
            }
        });
        m_translate_action->setCompleted([self{safe_ptr(this)}](const InputActionEvent &, const InputKey &key) noexcept {
            self->setTranslateRate_(key, float3{zero_v});
        });

        m_rotate_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept {
            if (const auto &[absolute, relative] = event.getAxis2DValue(); key != InputKey::mouse_2d) {
                setRotateRate_(key, absolute);
            } else if (m_has_mouse_look) {
                m_rotate_impulse += relative;
            }
        });
        m_rotate_action->setCompleted([this](const InputActionEvent &, const InputKey &key) noexcept {
            setRotateRate_(key, float2{zero_v});
        });

        m_speed_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept {
            setSpeedRate_(key, event.getAxis1DValue().m_absolute);
        });
        m_speed_action->setCompleted([this](const InputActionEvent &, const InputKey &key) noexcept {
            setSpeedRate_(key, 0.0f);
        });

        m_fov_action->setTriggered([this](const InputActionEvent &event, const InputKey &key) noexcept {
            const InputAxis1D value = event.getAxis1DValue();
            if (key.isMouse()) {
                m_fov_impulse += value.m_relative;
            } else {
                setFovRate_(key, value.m_absolute);
            }
        });
        m_fov_action->setCompleted([this](const InputActionEvent &, const InputKey &key) noexcept {
            setFovRate_(key, 0.0f);
        });

        m_look_action->setStarted([this](const InputActionEvent &, const InputKey &) noexcept {
            m_has_mouse_look = true;
        });
        m_look_action->setCompleted([this](const InputActionEvent &, const InputKey &) noexcept {
            m_has_mouse_look = false;
        });
    }

    void details::BasicCameraController::resetInputState() noexcept {
        m_translate_rates.clear();
        m_rotate_rates.clear();
        m_speed_rates.clear();
        m_fov_rates.clear();

        m_delta_heading = Quaternion::identity();
        m_delta_pitch_roll = Quaternion::identity();
        m_delta_position = float3{zero_v};
        m_translate_impulse = float3{zero_v};
        m_rotate_impulse = float2{zero_v};
        m_speed_impulse = zero_v;
        m_fov_impulse = zero_v;
        m_has_mouse_look = false;
    }

    void details::BasicCameraController::rotateCamera_(const float heading, const float pitch, const float roll) noexcept {
        // Heading is a world-Y pre-multiply; pitch/roll are body-local post-multiplies.
        // Exact regrouping into Ry(sum H) * q * Rx(sum P) * Rz(sum R) holds because roll
        // is unused today, keeping every pitch factor adjacent to the raw basis.
        m_delta_heading = Quaternion::rotateY(heading) * m_delta_heading;
        m_delta_pitch_roll = m_delta_pitch_roll * Quaternion::rotateX(pitch) * Quaternion::rotateZ(roll);
    }

    Quaternion details::BasicCameraController::advanceLook_(const std::optional<float> max_pitch) noexcept {
        if (isIdentityRotation_(m_delta_heading) and isIdentityRotation_(m_delta_pitch_roll)) {
            return m_rotation_analog.raw();
        }
        const Quaternion combined = normalizedOrIdentity_(m_delta_heading * m_rotation_analog.raw() * m_delta_pitch_roll);
        if (max_pitch.has_value()) {
            return clampPitch_(combined, *max_pitch);
        }
        return combined;
    }

    void details::BasicCameraController::setTranslateRate_(const InputKey &key, const float3 &rate) noexcept {
        if (dot2(rate) > epsilon_v<float>) {
            m_translate_rates.insert_or_assign(key, rate);
        } else {
            m_translate_rates.erase(key);
        }
    }

    void details::BasicCameraController::setRotateRate_(const InputKey &key, const float2 &rate) noexcept {
        if (dot2(rate) > epsilon_v<float>) {
            m_rotate_rates.insert_or_assign(key, rate);
        } else {
            m_rotate_rates.erase(key);
        }
    }

    void details::BasicCameraController::setSpeedRate_(const InputKey &key, const float rate) noexcept {
        if (rate != 0.0f) {
            m_speed_rates.insert_or_assign(key, rate);
        } else {
            m_speed_rates.erase(key);
        }
    }

    void details::BasicCameraController::setFovRate_(const InputKey &key, const float rate) noexcept {
        if (rate != 0.0f) {
            m_fov_rates.insert_or_assign(key, rate);
        } else {
            m_fov_rates.erase(key);
        }
    }

    void details::BasicCameraController::updateCameraModel(const TimeSpan dt, CameraModel &model) noexcept {
        const auto elapsed_seconds = static_cast<float>(time::seconds(dt));

        float3 translate_rate{zero_v};
        for (const float3 &rate: m_translate_rates.values()) {
            translate_rate += rate;
        }
        m_delta_position += float3(elapsed_seconds) * translate_rate + m_translate_impulse;

        float2 rotate_rate{zero_v};
        for (const float2 &rate: m_rotate_rates.values()) {
            rotate_rate += rate;
        }
        const float2 rotation = float2(elapsed_seconds) * rotate_rate + m_rotate_impulse;
        rotateCamera_(rotation.x, rotation.y);

        float speed_rate{zero_v};
        for (const float rate: m_speed_rates.values()) {
            speed_rate += rate;
        }
        m_speed_analog.addClamp(speed_rate * elapsed_seconds + m_speed_impulse, m_speed_multiplier_min_max.x, m_speed_multiplier_min_max.y);

        float fov_rate{zero_v};
        for (const float rate: m_fov_rates.values()) {
            fov_rate += rate;
        }
        m_fov_analog.addClamp(fov_rate * elapsed_seconds + m_fov_impulse, m_fov_min_max.x, m_fov_min_max.y);

        m_fov_analog.update(dt);
        m_speed_analog.update(dt);

        updateCameraPose_(dt, model);

        model.m_fov = m_fov_analog.filtered();
        model.m_has_camera_cut = m_has_teleported;

        m_delta_heading = Quaternion::identity();
        m_delta_pitch_roll = Quaternion::identity();
        m_delta_position = float3{zero_v};
        m_translate_impulse = float3{zero_v};
        m_rotate_impulse = float2{zero_v};
        m_speed_impulse = zero_v;
        m_fov_impulse = zero_v;
        m_has_teleported = false;
    }

    void details::BasicCameraController::updateCameraPose_(const TimeSpan dt, CameraModel &model) noexcept {
        if (not m_has_teleported) {
            m_rotation_analog.setRaw(advanceLook_(kMaxPitch));
        }
        m_rotation_analog.update(dt);

        if (not m_has_teleported and dot2(m_delta_position) > 0.0f)
        {
            const float3 local_delta = m_delta_position * float3(m_speed_analog.filtered());
            const float3 world_delta = quaternionTransform(m_rotation_analog.raw(), local_delta);
            m_position_analog.add(world_delta);
        }
        m_position_analog.update(dt);

        model.m_origin = m_position_analog.filtered();
        model.m_basis = normalize(m_rotation_analog.filtered());
    }

    void details::BasicCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        // enable mouse look when RMB down, disable when up:

        out_mapping.mapInputKey(SharedInputAction(m_look_action.get()), InputKey::right_mouse_button);

        // speed:

        const auto speed_modifier = [this](const float delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * std::max(self->m_speed_multiplier_min_max.y - self->m_speed_multiplier_min_max.x, 0.0f));
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_speed_action.get()), InputKey::left_shift, speed_modifier(0.015f));
        out_mapping.mapInputKey(SharedInputAction(m_speed_action.get()), InputKey::left_control, speed_modifier(-0.015f));
        out_mapping.mapInputKey(SharedInputAction(m_speed_action.get()), InputKey::gamepad_left_shoulder, speed_modifier(-0.01f));
        out_mapping.mapInputKey(SharedInputAction(m_speed_action.get()), InputKey::gamepad_right_shoulder, speed_modifier(0.01f));

        // fov:

        const auto fov_modifier = [this](const float delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * std::max(self->m_fov_min_max.y - self->m_fov_min_max.x, 0.0f));
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_fov_action.get()), InputKey::add, fov_modifier(0.08f));
        out_mapping.mapInputKey(SharedInputAction(m_fov_action.get()), InputKey::subtract, fov_modifier(-0.08f));
    }

    // ------------------------------------------------------------------
    // FreeCameraController — free-flight camera controller
    // ------------------------------------------------------------------

    void FreeCameraController::lookAt(const float3 &eye, const float3 &target, const float3 &up, const bool has_teleported) noexcept {
        const float3 forward = safeNormalize<float, 3u>(target - eye, math::forward);
        const float3 right = normalize(cross(up, forward));
        const float3 corrected_up = cross(forward, right);
        const Quaternion rotation(float3x3{right, corrected_up, forward});

        if (has_teleported) {
            m_has_teleported = true;
            m_position_analog.reset(eye);
            m_rotation_analog.reset(rotation);
        } else {
            m_position_analog.setRaw(eye);
            m_rotation_analog.setRaw(rotation);
        }
    }

    void FreeCameraController::lookAt(const float3 &eye, const float heading, const float pitch, const bool has_teleported) noexcept {
        if (has_teleported) {
            m_has_teleported = true;
            m_position_analog.reset(eye);
            m_rotation_analog.reset(Quaternion::rotateXYZ(pitch, heading, 0.0f));
        } else {
            m_position_analog.setRaw(eye);
            m_rotation_analog.setRaw(Quaternion::rotateXYZ(pitch, heading, 0.0f));
        }
    }

    void FreeCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        BasicCameraController::provideInputActionKeyMappings(out_mapping);

        // translation:

        const auto translate_modifier = [this](const float3 &delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * self->getTranslateSpeed());
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::w, translate_modifier(math::forward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::s, translate_modifier(math::backward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::a, translate_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::d, translate_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::up_arrow, translate_modifier(math::forward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::down_arrow, translate_modifier(math::backward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::left_arrow, translate_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::right_arrow, translate_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::page_up, translate_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::page_down, translate_modifier(math::down));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_up, translate_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_down, translate_modifier(math::down));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_left_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(float3(self->m_gamepad_sensitivity.x) * self->getTranslateSpeed());
            });

        // rotation:

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::q, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(-float2{1.0f, 0.0f} * self->getRotateSpeed());
        });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::e, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{1.0f, 0.0f} * self->getRotateSpeed());
        });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::mouse_2d, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(self->m_mouse_sensitivity * self->getRotateSpeed());
        });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::gamepad_right_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(float2{1.0f, -1.0f} * float2(self->m_gamepad_sensitivity.y) * self->getRotateSpeed());
            });

        // fov:

        const auto fov_modifier = [this](const float delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * std::max(self->m_fov_min_max.y - self->m_fov_min_max.x, 0.0f));
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_fov_action.get()), InputKey::mouse_wheel_axis_y, fov_modifier(0.01f));
    }

    // ------------------------------------------------------------------
    // PanCameraController — pan parallel to a 3d plane
    // ------------------------------------------------------------------

    PanCameraController::PanCameraController() {
        m_zoom_action->setTriggered([self{safe_ptr(this)}](const InputActionEvent &event, const InputKey &) noexcept {
            const float wheel_delta = event.getAxis1DValue().m_relative;
            if (std::isfinite(wheel_delta)) {
                self->m_zoom_impulse = std::clamp(
                    self->m_zoom_impulse + std::clamp(wheel_delta, -100.0f, 100.0f), -100.0f, 100.0f);
            }
        });

        // MMB-held grab state; the translate handler below converts
        // mouse_2d deltas only while this is held.
        m_grab_action->setStarted([self{safe_ptr(this)}](const InputActionEvent &, const InputKey &) noexcept {
            self->m_has_grab_pan = true;
        });
        m_grab_action->setCompleted([self{safe_ptr(this)}](const InputActionEvent &, const InputKey &) noexcept {
            self->m_has_grab_pan = false;
        });

        // Pan owns mouse_2d: cursor motion alone never pans (it only
        // records the cursor for anchored zoom); held MMB turns the pixel
        // delta into a 1:1 world-space grab impulse at the current scale.
        // Sign law, derived from the camera basis (screen +x is +right,
        // screen +y-down is -up; the eye moves against the ground's
        // apparent motion so grabbed ground stays under the cursor):
        // local delta = (-dx * scale, +dy * scale, 0).
        m_translate_action->setTriggered(
                [self{safe_ptr(this)}](const InputActionEvent &event, const InputKey &key) noexcept {
                    if (key != InputKey::mouse_2d) {
                        self->setTranslateRate_(key, event.getAxis3DValue().m_absolute);
                        return;
                    }
                    const InputAxis2D cursor = event.getAxis2DValue();
                    self->m_cursor_client = cursor.m_absolute;
                    self->m_has_cursor = true;
                    const bool finite_delta = std::isfinite(cursor.m_relative.x) and std::isfinite(cursor.m_relative.y);
                    if (self->m_has_grab_pan and finite_delta) {
                        const float scale = self->m_ortho_scale;
                        self->m_grab_impulse += float3{-cursor.m_relative.x * scale, cursor.m_relative.y * scale, 0.0f};
                    }
                });
    }

    std::error_code PanCameraController::setOrthoScale(const float scale) noexcept {
        if (not std::isfinite(scale) or scale < 0.001f or scale > 1000.0f) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_ortho_scale = scale;
        m_zoom_impulse = 0.0f;
        return default_value_v;
    }

    float3 PanCameraController::getPanTranslateSpeed() const noexcept {
        const float plane_rate = 600.0f * m_ortho_scale;
        return float3{plane_rate, plane_rate, m_forward_speed};
    }

    std::error_code PanCameraController::setWorldBounds(const float2 &min_bound, const float2 &max_bound) noexcept {
        const bool finite = std::isfinite(min_bound.x) and std::isfinite(min_bound.y) and std::isfinite(max_bound.x) and
                            std::isfinite(max_bound.y);
        const bool ordered = min_bound.x <= max_bound.x and min_bound.y <= max_bound.y;
        if (not finite or not ordered) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        m_world_bounds = WorldBounds{.m_min = min_bound, .m_max = max_bound};
        return default_value_v;
    }

    void PanCameraController::focusOn(const float2 &world_xy) noexcept {
        // Teleport cut keeping depth; bounds apply on the next pose update.
        // A follow target is left alone — the game composes the two.
        const float3 eye = m_position_analog.raw();
        translate(float3{world_xy.x, world_xy.y, eye.z}, true);
    }

    void PanCameraController::resetInputState() noexcept {
        BasicCameraController::resetInputState();
        m_zoom_impulse = 0.0f;
        m_has_grab_pan = false;
        m_grab_impulse = float3{zero_v};
        m_follow_target.reset();
    }

    void PanCameraController::updateCameraModel(const TimeSpan dt, CameraModel &model) noexcept {
        if (m_zoom_impulse != 0.0f) {
            // Wheel zoom is manual input: it cancels follow here because
            // the impulse is consumed below before the pose step runs.
            m_follow_target.reset();
            applyAnchoredZoom_();
        }
        BasicCameraController::updateCameraModel(clampFrameDt_(dt), model);
        model.m_ortho_scale = m_ortho_scale;
    }

    void PanCameraController::updateCameraPose_(const TimeSpan dt, CameraModel &model) noexcept {
        cancelFollowOnManualInput_();
        foldGrabImpulse_();
        easeFollowTarget_(dt);
        BasicCameraController::updateCameraPose_(dt, model);
        clampEyeToWorldBounds_();
        model.m_origin = m_position_analog.filtered();
    }

    void PanCameraController::applyAnchoredZoom_() noexcept {
        const float raw_delta = std::clamp(m_zoom_impulse, -100.0f, 100.0f);
        m_zoom_impulse = 0.0f;

        const float new_scale = std::clamp(m_ortho_scale * std::exp(-0.1f * raw_delta), 0.001f, 1000.0f);
        if (new_scale == m_ortho_scale) {
            return;
        }
        const float old_scale = m_ortho_scale;
        m_ortho_scale = new_scale;

        // Cursor anchor: recenter so the ground point under the cursor is
        // unchanged by the scale step. Without cursor/viewport info this
        // degrades to a plain (center-equivalent) zoom.
        if (not m_has_cursor or not hasValidViewport_()) {
            return;
        }
        const Quaternion basis = normalize(m_rotation_analog.filtered());
        const float3 right = quaternionTransform(basis, math::axis_x);
        const float3 up = quaternionTransform(basis, math::axis_y);
        const float3 forward = quaternionTransform(basis, math::axis_z);
        const bool finite_frame = std::isfinite(right.x + right.y + right.z) and std::isfinite(up.x + up.y + up.z) and
                                  std::isfinite(forward.x + forward.y + forward.z);
        if (not finite_frame or std::abs(forward.z) < 1e-6f) {
            return;
        }

        // Ortho eye is the view corner: world = eye + (cx * s) right +
        // ((H - cy) * s) up, with client pixels measured top-left.
        const float3 eye = m_position_analog.raw();
        const float3 offset_old = right * float3(m_cursor_client.x * old_scale) +
                                  up * float3((m_viewport_size.y - m_cursor_client.y) * old_scale);
        const float3 offset_new = right * float3(m_cursor_client.x * new_scale) +
                                  up * float3((m_viewport_size.y - m_cursor_client.y) * new_scale);
        const float3 origin_old = eye + offset_old;
        const float3 origin_new = eye + offset_new;
        const float t_old = -origin_old.z / forward.z;
        const float t_new = -origin_new.z / forward.z;
        if (not std::isfinite(t_old) or not std::isfinite(t_new)) {
            return;
        }
        const float3 shift = (origin_old + forward * float3(t_old)) - (origin_new + forward * float3(t_new));
        if (not std::isfinite(shift.x + shift.y + shift.z)) {
            return;
        }

        // Snap (not glide): scale jumps instantly, so the anchor must too.
        m_position_analog.reset(eye + shift);
        clampEyeToWorldBounds_();
    }

    void PanCameraController::cancelFollowOnManualInput_() noexcept {
        // Zoom cancels at impulse-consumption time (see updateCameraModel);
        // rates and the grab impulse are still live here.
        const bool manual = not m_translate_rates.empty() or dot2(m_grab_impulse) > epsilon_v<float>;
        if (manual) {
            m_follow_target.reset();
        }
    }

    void PanCameraController::foldGrabImpulse_() noexcept {
        // The 1:1 grab bypasses the speed multiplier: pre-divide so the
        // shared rate path multiplies it back exactly.
        const float speed = m_speed_analog.filtered();
        if (speed > epsilon_v<float>) {
            m_delta_position += m_grab_impulse * float3(1.0f / speed);
        }
        m_grab_impulse = float3{zero_v};
    }

    void PanCameraController::easeFollowTarget_(const TimeSpan dt) noexcept {
        if (not m_follow_target.has_value()) {
            return;
        }
        // Bounded step toward the target at the plane rate law.
        const float max_step = 600.0f * m_ortho_scale * static_cast<float>(time::seconds(dt));
        const float3 target = *m_follow_target;
        float3 eye = m_position_analog.raw();
        const float dx = target.x - eye.x;
        const float dy = target.y - eye.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= max_step) {
            eye.x = target.x;
            eye.y = target.y;
        } else if (dist > 0.0f) {
            eye.x += dx / dist * max_step;
            eye.y += dy / dist * max_step;
        }
        m_position_analog.setRaw(eye);
    }

    void PanCameraController::clampEyeToWorldBounds_() noexcept {
        if (not m_world_bounds.has_value() or not hasValidViewport_()) {
            return;
        }

        // Keep the visible rect [eye, eye + viewport * scale] inside the
        // world; a larger viewport centers the world instead (clamp wins
        // over zoom anchoring, which runs before this). Depth untouched.
        const float full_x = m_viewport_size.x * m_ortho_scale;
        const float full_y = m_viewport_size.y * m_ortho_scale;
        const float2 bounds_min = m_world_bounds->m_min;
        const float2 bounds_max = m_world_bounds->m_max;
        const float low_x = bounds_min.x;
        const float high_x = bounds_max.x - full_x;
        const float low_y = bounds_min.y;
        const float high_y = bounds_max.y - full_y;

        float3 eye = m_position_analog.raw();
        eye.x = low_x >= high_x ? (bounds_min.x + bounds_max.x) * 0.5f - full_x * 0.5f
                                : std::clamp(eye.x, low_x, high_x);
        eye.y = low_y >= high_y ? (bounds_min.y + bounds_max.y) * 0.5f - full_y * 0.5f
                                : std::clamp(eye.y, low_y, high_y);

        const float3 before = m_position_analog.raw();
        if (eye.x != before.x or eye.y != before.y) {
            m_position_analog.reset(eye);
        }
    }

    bool PanCameraController::hasValidViewport_() const noexcept {
        const bool finite = std::isfinite(m_viewport_size.x) and std::isfinite(m_viewport_size.y);
        const bool positive = m_viewport_size.x > 0.0f and m_viewport_size.y > 0.0f;
        return finite and positive;
    }

    void PanCameraController::setParallelPlane(const float3 &plane_normal, const float3 &plane_up,
                                               const bool has_teleported) noexcept {
        PPR_ASSERT(isNormalized<float, 3u>(plane_normal));
        PPR_ASSERT(isNormalized<float, 3u>(plane_up));

        const float3 forward = -plane_normal; // faces the plane
        const float3 right = normalize(cross(plane_up, forward));
        const float3 corrected_up = normalize(cross(forward, right));

        setParallelPlane(Quaternion(float3x3{right, corrected_up, forward}), has_teleported);
    }

    void PanCameraController::setParallelPlane(const Quaternion &plane_basis, const bool has_teleported) noexcept {
        PPR_ASSERT(isNormalized(plane_basis));

        if (has_teleported) {
            m_has_teleported = has_teleported;
            m_rotation_analog.reset(plane_basis);
        } else {
            m_rotation_analog.setRaw(plane_basis);
        }
    }

    void PanCameraController::translate(const float3 &eye, const bool has_teleported) noexcept {
        if (has_teleported) {
            m_has_teleported = has_teleported;
            m_position_analog.reset(eye);
        } else {
            m_position_analog.setRaw(eye);
        }
    }

    void PanCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        BasicCameraController::provideInputActionKeyMappings(out_mapping);
        out_mapping.mapInputKey(SharedInputAction(m_zoom_action.get()), InputKey::mouse_wheel_axis_y);
        // NOTE: RMB arms the shared look flag (Basic mapping above) but Pan
        // binds no rotation path — fixed-plane view, no yaw/pitch by design.
        // MMB-held grab-pan replaces raw cursor panning (see the ctor).
        out_mapping.mapInputKey(SharedInputAction(m_grab_action.get()), InputKey::middle_mouse_button);

        // translation, only parallel/orthogonal to the plane:

        const auto move_modifier = [this](const float3 &delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * self->getPanTranslateSpeed());
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::w, move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::s, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::a, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::d, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::up_arrow,
                                move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::down_arrow, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::left_arrow, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::right_arrow, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_up, move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_down, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_left, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_right, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::q, move_modifier(math::backward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::e, move_modifier(math::forward));

        // Cursor motion alone never pans: the raw axis_2d reaches the grab
        // handler (cursor record + MMB-gated impulse) unmodified.
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::mouse_2d);
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_left_2d,
                                [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                                    // left stick moves left/right & up/down under the same law
                                    output = InputValue(std::get<InputAxis2D>(output), InputAxis1D{})
                                                     . // transforms {x,y} vector to {x,y,0}
                                             modulate(float3(self->m_gamepad_sensitivity.x) *
                                                      self->getPanTranslateSpeed());
                                });
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_right_2d,
                                [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                                    // right stick moves only forward/backward
                                    output =
                                            InputValue(InputAxis1D{}, std::get<InputAxis2D>(output))
                                                    . // transforms {x,y} vector to {0,x,y}
                                            modulate(float3(self->m_gamepad_sensitivity.y) * self->getTranslateSpeed() *
                                                     float3(0, 0, 1)); // transforms {0,x,y} to {0,0,y}
                                });
    }

    // ------------------------------------------------------------------
    // OrbitCameraController — orbit around a point
    // ------------------------------------------------------------------
    namespace {
        [[nodiscard]] float clampOrbitRadius_(const float radius) noexcept {
            return std::max(radius, epsilon_v<float>);
        }

        [[nodiscard]] float3 safeOrbitRight_(const float3 &forward) noexcept {
            const float3 reference_up = std::abs(dot(forward, math::up)) < 1.0f - epsilon_v<float>
                                            ? math::up
                                            : math::right;
            return safeNormalize<float, 3u>(cross(reference_up, forward), math::right);
        }

        [[nodiscard]] float3 orbitOrigin_(const Quaternion &basis, const float3 &target, const float radius) noexcept {
            return target - quaternionTransform(basis, math::forward) * float3(radius);
        }
    }

    void OrbitCameraController::lookAt(const float3 &eye, const float3 &target, const bool has_teleported) noexcept {
        const float3 forward = safeNormalize<float, 3u>(target - eye, math::forward);
        const float3 right = safeOrbitRight_(forward);
        const float3 corrected_up = safeNormalize<float, 3u>(cross(forward, right), math::up);

        const Quaternion rotation(float3x3{right, corrected_up, forward});
        const float radius = clampOrbitRadius_(distance(target, eye));

        if (has_teleported) {
            m_has_teleported = true;
            m_rotation_analog.reset(rotation);
            m_target_analog.reset(target);
            m_radius_analog.reset(radius);
            m_position_analog.reset(eye);
        } else {
            m_rotation_analog.setRaw(rotation);
            m_target_analog.setRaw(target);
            m_radius_analog.setRaw(radius);
            m_position_analog.setRaw(eye);
        }
    }

    void OrbitCameraController::setOrbitTarget(const float3 &target, const bool has_teleported) noexcept {
        m_has_teleported |= has_teleported;

        const Quaternion basis = normalize(m_rotation_analog.filtered());
        const float radius = clampOrbitRadius_(m_radius_analog.filtered());
        const float3 eye = orbitOrigin_(basis, target, radius);

        if (has_teleported) {
            m_target_analog.reset(target);
            m_position_analog.reset(eye);
        } else {
            m_target_analog.setRaw(target);
            m_position_analog.setRaw(eye);
        }
    }

    void OrbitCameraController::setOrbitRadius(const float radius, const bool has_teleported) noexcept {
        PPR_ASSERT(radius > 0);

        const float clamped_radius = clampOrbitRadius_(radius);
        const Quaternion basis = normalize(m_rotation_analog.filtered());
        const float3 target = m_target_analog.filtered();
        const float3 eye = orbitOrigin_(basis, target, clamped_radius);

        if (has_teleported) {
            m_has_teleported = true;
            m_radius_analog.reset(clamped_radius);
            m_position_analog.reset(eye);
        } else {
            m_radius_analog.setRaw(clamped_radius);
            m_position_analog.setRaw(eye);
        }
    }

    void OrbitCameraController::updateCameraPose_(const TimeSpan dt, CameraModel &model) noexcept {
        if (not m_has_teleported) {
            m_rotation_analog.setRaw(advanceLook_(std::nullopt));

            if (dot(m_delta_position, m_delta_position) > 0.0f) {
                const float3 local_delta = m_delta_position * float3(m_speed_analog.filtered());
                m_radius_analog.addClamp(-local_delta.z, epsilon_v<float>, std::numeric_limits<float>::max());
            }
        }

        m_rotation_analog.update(dt);
        m_target_analog.update(dt);
        m_radius_analog.update(dt);

        const Quaternion basis = normalize(m_rotation_analog.filtered());
        const float3 target = m_target_analog.filtered();
        const float radius = clampOrbitRadius_(m_radius_analog.filtered());
        const float3 origin = orbitOrigin_(basis, target, radius);

        m_position_analog.reset(origin);

        model.m_basis = basis;
        model.m_origin = origin;
    }

    void OrbitCameraController::provideInputActionKeyMappings(InputMapping &out_mapping) const noexcept {
        BasicCameraController::provideInputActionKeyMappings(out_mapping);

        // translation, only forward/backward:

        const auto move_modifier = [this](const float3 &delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * self->getTranslateSpeed());
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::w, move_modifier(math::forward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::s, move_modifier(math::backward));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::up_arrow, move_modifier(math::forward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::down_arrow, move_modifier(math::backward));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_up, move_modifier(math::forward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_down, move_modifier(math::backward));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::mouse_wheel_axis_y,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // mouse wheel moves forward/backward
                output = InputValue(InputAxis1D{}, std::get<InputAxis1D>(output)). // transforms {x} vector to {0,x}
                        modulate(self->getTranslateSpeed()); // transforms {0,x} vector to {0,0,x}
            });

        // rotation:

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::a, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(-float2{1.0f, 0.0f} * self->getRotateSpeed());
        });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::d, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(float2{1.0f, 0.0f} * self->getRotateSpeed());
        });

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::left_arrow,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(-float2{1.0f, 0.0f} * self->getRotateSpeed());
            });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::right_arrow,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(float2{1.0f, 0.0f} * self->getRotateSpeed());
            });

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::gamepad_dpad_left,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(-float2{1.0f, 0.0f} * self->getRotateSpeed());
            });
        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::gamepad_dpad_right,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(float2{1.0f, 0.0f} * self->getRotateSpeed());
            });

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::gamepad_left_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(float2{1.0f, -1.0f} * float2(self->m_gamepad_sensitivity.x) * self->getRotateSpeed());
            });

        out_mapping.mapInputKey(SharedInputAction(m_rotate_action.get()), InputKey::mouse_2d, [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
            output = output.modulate(self->m_mouse_sensitivity * self->getRotateSpeed());
        });

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_right_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // right stick moves only forward/backward
                output = InputValue(InputAxis1D{}, std::get<InputAxis2D>(output)). // transforms {x,y} vector to {0,x,y}
                        modulate(float3(self->m_gamepad_sensitivity.y) * self->getTranslateSpeed() * float3(0, 0, 1)); // transforms {0,x,y} to {0,0,y}
            });
    }
}
