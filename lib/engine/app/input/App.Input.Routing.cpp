module;

#include "pP/Macros.h"

module engine.app;

import :input.routing;

import engine.core;
import std;

namespace pP {

    InputBackgroundLatch::InputBackgroundLatch(
        const int foreground_priority,
        safe_ptr<InputListener> background_listener,
        const int background_priority,
        const int detector_priority) noexcept
    : m_background_listener(std::move(background_listener)),
m_foreground_priority(foreground_priority),
m_background_priority(background_priority),
m_detector_priority(detector_priority){
        // Non-owning view: the state owner must detach before destroying it,
        // mirroring the detector-listener ownership contract.
        m_detector_listener.setRawKeyCallback(
            [tap_state = this](TimeSpan, const InputMessage &message) noexcept {
                PPR_ASSERT(tap_state != nullptr);

                if (const EMouseButton *const button = std::get_if<EMouseButton>(&message.m_key.m_code);
                    button != nullptr && isBackgroundDragButton(*button)) [[likely]] {

                    // Latch on pressed only: repeats must not inflate the hold,
                    // so a single release always clears it.
                    if (message.m_event == EInputMessageEvent::pressed) [[likely]] {
                        tap_state->notifyPress(*button);
                    } else if (message.m_event == EInputMessageEvent::released) [[likely]] {
                        tap_state->notifyRelease(*button);
                    }
                }

                return EInputMessageResponse::unhandled;
            });
    }


    InputBackgroundLatch::~InputBackgroundLatch() noexcept {
        m_detector_listener.setRawKeyCallback({});
    }

    std::error_code InputBackgroundLatch::validate() const noexcept {
        if (not m_background_listener) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (m_foreground_priority >= m_detector_priority) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (m_detector_priority >= m_background_priority) {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return default_value_v;
    }


    std::error_code InputBackgroundLatch::initialize(InputContext &context, safe_ptr<const InputListener> foreground_listener) {
        if (not foreground_listener.isValid()) {
            return make_error_code(std::errc::invalid_argument);
        }
        if (m_foreground_listener.isValid()) {
            return make_error_code(std::errc::already_connected);
        }
        if (const std::error_code err = validate()) {
            return err;
        }

        m_foreground_listener = std::move(foreground_listener);

        context.addInputListener(safe_ptr{&m_detector_listener}, m_detector_priority);
        context.addInputListener(m_background_listener, m_background_priority);
        return default_value_v;
    }

    std::error_code InputBackgroundLatch::shutdown(InputContext &context) {
        std::error_code err{};

        if (not context.removeInputListener(m_detector_listener)) {
            err = make_error_code(std::errc::not_connected);
        }

        if (m_background_listener and not context.removeInputListener(*m_background_listener)) {
            err = make_error_code(std::errc::not_connected);
        }

        if (not m_foreground_listener) {
            err = make_error_code(std::errc::not_connected);
        }
        m_foreground_listener.reset();
        return err;
    }

    bool InputBackgroundLatch::isEngaged() const noexcept {
        return (m_left_held_count > 0u and m_left_background_origin) or
               (m_middle_held_count > 0u and m_middle_background_origin);
    }

    void InputBackgroundLatch::resetInputState() noexcept {
        m_left_held_count = 0u;
        m_left_background_origin = false;

        m_middle_held_count = 0u;
        m_middle_background_origin = false;
    }

    void InputBackgroundLatch::notifyPress(const EMouseButton button) noexcept {
        if (button == EMouseButton::left) {
            if (m_left_held_count == 0u) {
                m_left_background_origin = true;
            }

            ++m_left_held_count;
        } else if (button == EMouseButton::middle) {
            if (m_middle_held_count == 0u) {
                m_middle_background_origin = true;
            }

            ++m_middle_held_count;
        }
    }

    void InputBackgroundLatch::notifyRelease(const EMouseButton button) noexcept {
        if (button == EMouseButton::left) {
            if (m_left_held_count == 0u) {
                m_left_background_origin = false;
                return;
            }

            --m_left_held_count;

            if (m_left_held_count == 0u) {
                m_left_background_origin = false;
            }
        } else if (button == EMouseButton::middle) {
            if (m_middle_held_count == 0u) {
                m_middle_background_origin = false;
                return;
            }

            --m_middle_held_count;

            if (m_middle_held_count == 0u) {
                m_middle_background_origin = false;
            }
        }
    }
}
