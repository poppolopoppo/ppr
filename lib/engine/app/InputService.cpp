module;
#include "pP/Macros.h"
module engine.app;
import :input_service;
import engine.core;
import engine.platform;
import std;

namespace pP::detail {

    class InputWindowListener {
        InputService& m_service;
        IWindow* m_window{};
    public:
        explicit InputWindowListener(InputService& service) noexcept
            : m_service(service) {}

        void attach(IWindow& window) { m_window = &window; }
        void detach() { m_window = nullptr; }

        void onKey(int glfwKey, bool pressed) {
            InputKey key = fromGlfwKey(glfwKey);
            m_service.onKeyEvent(key, pressed);
        }

        void onMouseButton(int glfwButton, bool pressed) {
            InputKey key = fromGlfwMouseButton(glfwButton);
            m_service.onKeyEvent(key, pressed);
        }

        void onCursorPos(double x, double y) {
            (void)x;
            (void)y;
        }
    };

} // namespace pP::detail

namespace pP {

InputService::InputService()
    : m_listener(std::make_unique<detail::InputWindowListener>(*this)) {}

InputService::~InputService() noexcept = default;

void InputService::attachToWindow(IWindow& window) {
    m_listener->attach(window);
}

void InputService::detachWindow() {
    m_listener->detach();
}

void InputService::onRawKey(int glfwKey, bool pressed) {
    m_listener->onKey(glfwKey, pressed);
}

void InputService::onRawMouseButton(int glfwButton, bool pressed) {
    m_listener->onMouseButton(glfwButton, pressed);
}

ActionMap& InputService::layer(InputPriority priority) {
    return m_layers[priority];
}

const ActionMap& InputService::layer(InputPriority priority) const {
    return m_layers[priority];
}

void InputService::blockLowerPriority(InputPriority priority) noexcept {
    m_lowestActive = priority;
}

void InputService::unblockLowerPriority() noexcept {
    m_lowestActive = InputPriority{};
}

InputPriority InputService::blockedBelow() const noexcept {
    return m_lowestActive;
}

bool InputService::isBlocked(InputPriority priority) const noexcept {
    if (m_lowestActive == InputPriority{}) return false;
    return priority < m_lowestActive;
}

void InputService::update() {
    for (auto& [prio, map] : m_layers) {
        (void)prio;
        for (auto& action : map) {
            action.justPressed = false;
            action.value = 0.0f;
        }
    }
}

void InputService::onKeyEvent(InputKey key, bool pressed) {
    for (auto& [prio, map] : m_layers) {
        if (isBlocked(prio)) continue;
        auto* action = map.findKey(key);
        if (action) {
            if (pressed && !action->isPressed) {
                action->justPressed = true;
            }
            action->isPressed = pressed;
            action->value = pressed ? 1.0f : 0.0f;
            return;
        }
    }
}

void ActionMap::bind(InputKey key, std::string_view name) {
    auto& action = m_actions.emplace_back();
    action.name = name;
    action.key = key;
    m_keyToAction[key] = &action;
}

void ActionMap::unbind(InputKey key) {
    m_keyToAction.erase(key);
    std::erase_if(m_actions, [key](const Action& a) { return a.key == key; });
}

void ActionMap::unbind(std::string_view name) {
    InputKey key{};
    for (auto& a : m_actions) {
        if (a.name == name) {
            key = a.key;
            break;
        }
    }
    if (key != keys::unknown) {
        m_keyToAction.erase(key);
    }
    std::erase_if(m_actions, [name](const Action& a) { return a.name == name; });
}

const Action* ActionMap::find(std::string_view name) const noexcept {
    for (auto& a : m_actions) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

Action* ActionMap::find(std::string_view name) noexcept {
    for (auto& a : m_actions) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

Action* ActionMap::findKey(InputKey key) noexcept {
    auto it = m_keyToAction.find(key);
    if (it != m_keyToAction.end()) {
        return it->second;
    }
    return nullptr;
}

void ActionMap::clear() {
    m_actions.clear();
    m_keyToAction.clear();
}

void ActionMap::onKeyEvent(InputKey key, bool pressed) {
    auto* action = findKey(key);
    if (action) {
        if (pressed && !action->isPressed) {
            action->justPressed = true;
        }
        action->isPressed = pressed;
        action->value = pressed ? 1.0f : 0.0f;
    }
}

} // namespace pP
