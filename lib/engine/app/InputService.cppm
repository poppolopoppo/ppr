module;
#include "pP/Macros.h"
export module engine.app:input_service;
import engine.core;
import engine.platform;
import std;

namespace pP::detail {
    class InputWindowListener;
}

export namespace pP {

    enum class InputPriority : u32 {
        Console  = 100,
        UI       = 200,
        Gameplay = 300,
        Debug    = 400,
    };

    struct Action {
        std::string name{};
        InputKey key{};
        bool isPressed = false;
        bool justPressed = false;
        float value = 0.0f;
    };

    class ActionMap {
        std::vector<Action> m_actions{};
        HashMap<InputKey, Action*> m_keyToAction{};
    public:
        void bind(InputKey key, std::string_view name);
        void unbind(InputKey key);
        void unbind(std::string_view name);
        [[nodiscard]] const Action* find(std::string_view name) const noexcept;
        [[nodiscard]] Action* find(std::string_view name) noexcept;
        [[nodiscard]] Action* findKey(InputKey key) noexcept;
        void clear();

        auto begin() noexcept { return m_actions.begin(); }
        auto end() noexcept { return m_actions.end(); }
        auto begin() const noexcept { return m_actions.begin(); }
        auto end() const noexcept { return m_actions.end(); }

        void onKeyEvent(InputKey key, bool pressed);
    };

    class InputService {
        std::unique_ptr<detail::InputWindowListener> m_listener{};
        HashMap<InputPriority, ActionMap> m_layers{};
        InputPriority m_lowestActive{};

    public:
        InputService();
        ~InputService() noexcept;

        InputService(const InputService&) = delete;
        InputService& operator=(const InputService&) = delete;

        void attachToWindow(IWindow& window);
        void detachWindow();
        void update();

        void onRawKey(int glfwKey, bool pressed);
        void onRawMouseButton(int glfwButton, bool pressed);

        ActionMap& layer(InputPriority priority);
        [[nodiscard]] const ActionMap& layer(InputPriority priority) const;

        void blockLowerPriority(InputPriority priority) noexcept;
        void unblockLowerPriority() noexcept;
        [[nodiscard]] InputPriority blockedBelow() const noexcept;
        [[nodiscard]] bool isBlocked(InputPriority priority) const noexcept;

    private:
        void onKeyEvent(InputKey key, bool pressed);
    };

} // namespace pP
