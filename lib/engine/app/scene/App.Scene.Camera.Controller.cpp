module;
#include "pP/Macros.h"
module engine.app;

import :scene.camera.controller;
import :input.action;
import :input.key;

import engine.core;
import engine.math;
import std;

namespace pP {
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

        m_delta_rotation = Quaternion::identity();
        m_delta_position = float3{zero_v};
        m_translate_impulse = float3{zero_v};
        m_rotate_impulse = float2{zero_v};
        m_speed_impulse = zero_v;
        m_fov_impulse = zero_v;
        m_has_mouse_look = false;
    }

    void details::BasicCameraController::rotateCamera_(const float heading, const float pitch, const float roll) noexcept {
        rotateCamera_(Quaternion::rotateXYZ(pitch, heading, roll));
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

        m_delta_rotation = Quaternion::identity();
        m_delta_position = float3{zero_v};
        m_translate_impulse = float3{zero_v};
        m_rotate_impulse = float2{zero_v};
        m_speed_impulse = zero_v;
        m_fov_impulse = zero_v;
        m_has_teleported = false;
    }

    void details::BasicCameraController::updateCameraPose_(const TimeSpan dt, CameraModel &model) noexcept {
        if (not m_has_teleported and dot2(m_delta_rotation) > 0.0f) {
            m_rotation_analog.setRaw(m_delta_rotation * m_rotation_analog.raw());
        }
        m_rotation_analog.update(dt);

        if (not m_has_teleported and dot2(m_delta_position) > 0.0f) {
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

    void PanCameraController::setParallelPlane(const float3 &plane_normal, const float3 &plane_up, const bool has_teleported) noexcept {
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

        // translation, only parallel/orthogonal to the plane:

        const auto move_modifier = [this](const float3 &delta) -> InputModifierEvent {
            return [self{safe_ptr(this)}, delta](const TimeSpan, InputValue &output) noexcept {
                output = output.modulate(delta * self->getTranslateSpeed());
            };
        };

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::w, move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::s, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::a, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::d, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::up_arrow, move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::down_arrow, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::left_arrow, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::right_arrow, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_up, move_modifier(math::up));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_down, move_modifier(math::down));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_left, move_modifier(math::left));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_dpad_right, move_modifier(math::right));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::q, move_modifier(math::backward));
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::e, move_modifier(math::forward));

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::mouse_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // mouse pointer moves left/right & up/down
                const InputAxis2D raw = std::get<InputAxis2D>(output);
                const float2 sens = self->m_mouse_sensitivity;
                const InputAxis2D scaled{
                    .m_absolute = float2{raw.m_absolute.x * sens.x, raw.m_absolute.y * sens.y},
                    .m_relative = float2{raw.m_relative.x * sens.x, raw.m_relative.y * sens.y},
                };
                output = InputValue(scaled, InputAxis1D{}). // transforms {x,y} vector to {x,y,0}
                        modulate(self->getTranslateSpeed());
            });
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::mouse_wheel_axis_y,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // mouse wheel moves forward/backward
                output = InputValue(InputAxis1D{}, std::get<InputAxis1D>(output)). // transforms {x} vector to {0,x}
                        modulate(self->getTranslateSpeed()); // transforms {0,x} vector to {0,0,x}
            });

        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_left_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // left stick moves left/right & up/down
                output = InputValue(std::get<InputAxis2D>(output), InputAxis1D{}). // transforms {x,y} vector to {x,y,0}
                        modulate(float3(self->m_gamepad_sensitivity.x) * self->getTranslateSpeed());
            });
        out_mapping.mapInputKey(SharedInputAction(m_translate_action.get()), InputKey::gamepad_right_2d,
            [self{safe_ptr(this)}](const TimeSpan, InputValue &output) noexcept {
                // right stick moves only forward/backward
                output = InputValue(InputAxis1D{}, std::get<InputAxis2D>(output)). // transforms {x,y} vector to {0,x,y}
                        modulate(float3(self->m_gamepad_sensitivity.y) * self->getTranslateSpeed() * float3(0, 0, 1)); // transforms {0,x,y} to {0,0,y}
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
            m_rotation_analog.setRaw(m_delta_rotation * m_rotation_analog.raw());

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
