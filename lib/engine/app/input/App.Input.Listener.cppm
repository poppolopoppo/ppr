module;
#include "pP/Macros.h"
export module engine.app:input.listener;

import :input.key;
import :service.input;
import :window.handle;

import engine.core;
import std;

export namespace pP {
    class IInputService;

    struct InputMessage;
    class InputMapping;
    enum class EInputMessageResponse : u8;
    using SharedInputMapping = safe_ptr<const InputMapping>;

    struct InputAction;
    struct InputActionEvent;
    struct InputActionKeyMapping;
    using SharedInputAction = safe_ptr<const InputAction>;

    class Window;

    // ------------------------------------------------------------------
    // input listener
    // ------------------------------------------------------------------

    using InputActionCallback = std::move_only_function<void(const InputActionEvent &event, const InputKey &trigger) const noexcept>;
    using InputCharacterInputCallback = std::move_only_function<EInputMessageResponse(hal::native::char_t codepoint) const>;
    using InputRawKeyCallback = std::move_only_function<EInputMessageResponse(TimeSpan dt, const InputMessage &message)>;

    class InputListener final : public safe_object {
        using InputMappingIndex = Numeric<u32, SharedInputMapping>;
        using KeyMappingIndex = Numeric<u32, InputKey>;

        struct InputBinding {
            InputMappingIndex m_input_mapping{max_v};
            KeyMappingIndex m_key_mapping{max_v};
        };

        void rebuildKeybindings_();

        [[nodiscard]] const InputActionKeyMapping &getKeyMapping_(const InputBinding &binding) const noexcept;

        PrioritySet<SharedInputMapping> m_mappings{};
        FlatMap<SharedInputAction, InputActionEvent> m_action_events{};
        FlatMultiMap<InputKey, InputBinding> m_keybindings{};

        InputActionCallback m_action_callback{};
        InputCharacterInputCallback m_character_input_callback{};
        InputRawKeyCallback m_raw_key_callback{};

        EInputMessageResponse m_listener_mode;

    public:
        InputListener() noexcept;

        [[nodiscard]] constexpr EInputMessageResponse getInputListenerMode() const noexcept {
            return m_listener_mode;
        }

        void setInputListenerMode(const EInputMessageResponse value) noexcept {
            m_listener_mode = value;
        }

        void setActionCallback(InputActionCallback callback) noexcept {
            m_action_callback = std::move(callback);
        }

        void setCharacterInputCallback(InputCharacterInputCallback callback) noexcept {
            m_character_input_callback = std::move(callback);
        }

        void setRawKeyCallback(InputRawKeyCallback callback) noexcept {
            m_raw_key_callback = std::move(callback);
        }

        [[nodiscard]] bool hasInputMapping(const InputMapping &mapping) const noexcept;

        void addInputMapping(SharedInputMapping mapping, int priority);

        bool removeInputMapping(const InputMapping &mapping);

        void clearInputMappings();

        [[nodiscard]] bool isKeyHandledByAction(const InputKey &key) const noexcept;

        [[nodiscard]] std::optional<InputValue> getActionValue(const InputAction &action) const noexcept;

        [[nodiscard]] EInputMessageResponse postCharacterInput(hal::native::char_t codepoint) const;

        [[nodiscard]] EInputMessageResponse postKeyEvent(TimeSpan dt, const InputMessage &message) noexcept;
    };

    using SharedInputListener = safe_ptr<const InputListener>;

    // ------------------------------------------------------------------
    // input context - route input messages to subscribed listeners
    // ------------------------------------------------------------------

    class InputContext final : public safe_object {
        PrioritySet<safe_ptr<InputListener> > m_listeners{};

        const safe_ptr<InputContext> m_parent{};

    public:
        explicit InputContext(safe_ptr<InputContext> parent_context = {}) noexcept;

        // NOTE: implicitly-defined copy/move would evaluate safe_ptr's
        // is_base_of constraints on the still-incomplete InputContext in
        // release builds (C2139); define them out-of-line where it is complete.
        // Assignments stay deleted: m_parent is const, as with the implicit ops.
        InputContext(const InputContext &other) = delete;

        InputContext &operator=(const InputContext &other) = delete;

        InputContext &operator=(InputContext &&other) = delete;

        InputContext(InputContext &&other) noexcept;

        [[nodiscard]] safe_ptr<const InputContext> getParentContext() const noexcept {
            return m_parent;
        }

        // listeners:
        [[nodiscard]] bool hasInputListener(const InputListener &listener) const noexcept;

        void addInputListener(safe_ptr<InputListener> listener, int priority = 0);

        bool removeInputListener(const InputListener &listener);

        void clearInputListeners();

        // input events:
        [[nodiscard]] EInputMessageResponse postCharacterInput(hal::native::char_t codepoint) const;

        [[nodiscard]] EInputMessageResponse postKeyEvent(TimeSpan dt, const InputMessage &message) const;
    };

    using SharedInputContext = safe_ptr<const InputContext>;

    // ------------------------------------------------------------------
    // window input context - setup an input context for a specific window
    // ------------------------------------------------------------------

    class WindowInputContext final {
    public:
        const safe_ptr<IInputService> m_inputs{};
        const safe_ptr<Window> m_window{};

        InputContext m_context;

        WindowInputContext(safe_ptr<IInputService> inputs, safe_ptr<Window> window);

        ~WindowInputContext();

    private:
        void onWindowCharacterInput_(const Window &window, hal::native::char_t codepoint) const;

        void onKeyboardPressed_(const Window &window, EKeyboardKey key, bool pressed) const;

        void onMouseClicked_(const Window &window, EMouseButton button, bool clicked) const;

        void onMouseMoved_(const Window &window, const float2 &client_pos) const;

        void onMouseScrolled_(const Window &window, const float2 &delta) const;
    };
}
