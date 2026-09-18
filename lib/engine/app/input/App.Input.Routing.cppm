module;

export module engine.app:input.routing;

import :input.key;
import :input.listener;

import engine.core;
import std;

export namespace pP {
    // Routing-owned background-tap latch: plain held counts + background
    // origins per drag button. Mutate only via the member functions below;
    // the detector actuator and reset paths go through them.
    // Lifetime: the detector actuator holds a non-owning view, so detach the
    // tap (detachBackgroundTap) before destroying the state.
    class InputBackgroundLatch {
        InputListener m_detector_listener{};

        safe_ptr<const InputListener> m_foreground_listener{};

    public:
        const safe_ptr<InputListener> m_background_listener{};

        const int m_foreground_priority{};
        const int m_background_priority{}; // background priority; call site static_cast<int>(EInputListenerPriority::player)
        const int m_detector_priority{}; // detector priority, must be < m_priority; call site static_cast<int>(EInputListenerPriority::detector)

    private:
        u32 m_left_held_count{0u};
        u32 m_middle_held_count{0u};

        bool m_left_background_origin{false};
        bool m_middle_background_origin{false};

    public:
        InputBackgroundLatch(
            int foreground_priority,
            safe_ptr<InputListener> background_listener,
            int background_priority,
            int detector_priority) noexcept;

        ~InputBackgroundLatch() noexcept;

        [[nodiscard]] std::error_code validate() const noexcept;

        [[nodiscard]] std::error_code initialize(InputContext &context, safe_ptr<const InputListener> foreground_listener);
        [[nodiscard]] std::error_code shutdown(InputContext &context);

        [[nodiscard]] static constexpr bool isBackgroundDragButton(const EMouseButton button) noexcept {
            return button == EMouseButton::left or
                   button == EMouseButton::middle;
        }

        [[nodiscard]] bool isEngaged() const noexcept;

        void resetInputState() noexcept;

        void notifyPress(EMouseButton button) noexcept;

        void notifyRelease(EMouseButton button) noexcept;
    };
}
