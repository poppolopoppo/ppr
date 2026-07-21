module;
#include "pP/Macros.h"
export module engine.app:input.device;

import engine.core;
import :service.input;
import :input.key;

export namespace pP {
    // ------------------------------------------------------------------
    // input messages
    // ------------------------------------------------------------------

    enum class EInputMessageEvent : u8 {
        pressed = 0,
        released,
        repeat,
        double_click,
        axis,
    };

    struct InputMessage {
        InputKey m_key;
        InputValue m_value;

        TimeSpan m_delta_time;
        InputDeviceID m_device_id;
        EInputMessageEvent m_event;

        constexpr InputMessage(
            InputKey key,
            InputValue value,
            const TimeSpan delta_time,
            const InputDeviceID device_id,
            const EInputMessageEvent event) noexcept
            : m_key{std::move(key)}, m_value{std::move(value)},
              m_delta_time{delta_time}, m_device_id{device_id}, m_event{event} {
        }

        constexpr ~InputMessage() noexcept = default;

        [[nodiscard]] constexpr bool isPressed() const noexcept {
            return m_event == EInputMessageEvent::pressed;
        }

        [[nodiscard]] constexpr bool isReleased() const noexcept {
            return m_event == EInputMessageEvent::released;
        }

        [[nodiscard]] constexpr bool isRepeat() const noexcept {
            return m_event == EInputMessageEvent::repeat;
        }

        [[nodiscard]] constexpr bool isDoubleClick() const noexcept {
            return m_event == EInputMessageEvent::double_click;
        }

        [[nodiscard]] constexpr bool isAxis() const noexcept {
            return m_event == EInputMessageEvent::axis;
        }

        [[nodiscard]] constexpr InputDigital getDigitalValue() const noexcept {
            return std::get<InputDigital>(m_value);
        }

        [[nodiscard]] constexpr InputAxis1D getAxis1DValue() const noexcept {
            return std::get<InputAxis1D>(m_value);
        }

        [[nodiscard]] constexpr const InputAxis2D &getAxis2DValue() const noexcept {
            return std::get<InputAxis2D>(m_value);
        }

        [[nodiscard]] constexpr const InputAxis3D &getAxis3DValue() const noexcept {
            return std::get<InputAxis3D>(m_value);
        }
    };

    // ------------------------------------------------------------------
    // abstract input device
    // ------------------------------------------------------------------

    class IInputDevice : public safe_object {
    public:
        // ReSharper disable once CppHidingFunction
        virtual ~IInputDevice() noexcept = default;

        [[nodiscard]] virtual const InputDeviceID &getInputDeviceID() const noexcept = 0;

        [[nodiscard]] virtual std::error_code supportedInputKeys(Collector<InputKey> supports_key) const = 0;

        [[nodiscard]] virtual std::error_code postInputMessages(TimeSpan dt, Collector<InputMessage> post_event) = 0;

        virtual void resetInputState() noexcept = 0;
    };

    using SharedInputDevice = safe_ptr<const IInputDevice>;

    // ------------------------------------------------------------------
    // analog axis input state
    // ------------------------------------------------------------------

    template<typename T>
    struct InputAxisState {
        using value_type = T;
        using const_reference = std::conditional_t<
            std::is_trivially_copyable_v<T>,
            const T, const T &>;
        details::input_value<value_type> m_raw{};
        details::input_value<value_type> m_filtered{};

        value_type m_next_raw_absolute{zero_v};

        float m_dead_zone{epsilon_v};
        float m_sensitivity{2.0f};

        [[nodiscard]] constexpr const details::input_value<value_type> &
        get(const bool use_filtered) const noexcept {
            return use_filtered ? m_filtered : m_raw;
        }

        void add(const_reference delta) noexcept {
            m_next_raw_absolute += delta;
        }

        void addClamp(const_reference offset, const_reference vmin, const_reference vmax) noexcept {
            m_next_raw_absolute = clamp(m_next_raw_absolute + offset, vmin, vmax);
        }

        void set(const_reference absolute) noexcept {
            m_next_raw_absolute = absolute;
        }

        void update(const double elapsed_seconds) noexcept {
            m_raw.m_relative = m_next_raw_absolute - m_raw.m_absolute;
            if (dot2(m_raw.m_relative) > dot2(m_dead_zone)) {
                m_raw.m_absolute = m_next_raw_absolute;
            } else {
                m_raw.m_relative = value_type{zero_v};
            }

            const float blend_rate = saturate(static_cast<float>(
                std::pow(elapsed_seconds,
                         1.0 / std::max<float>(m_sensitivity, epsilon_v))));

            const value_type next_filtered = lerp(
                m_filtered.m_absolute,
                m_raw.m_absolute,
                blend_rate);

            m_filtered.m_relative = next_filtered - m_filtered.m_absolute;
            m_filtered.m_absolute = next_filtered;
        }

        void postInputMessages(
            const InputDeviceID &device_id,
            const InputKey &input_key,
            const bool enable_filtered_inputs,
            const TimeSpan dt,
            const Collector<InputMessage> post_event) const noexcept {
            if (const auto &analog_value = get(enable_filtered_inputs);
                dot2(analog_value.m_relative) > 0) {
                InputValue input_value(analog_value);
                PPR_ASSERT(input_value.getType() == input_key.m_value);

                post_event(InputMessage(
                    input_key, std::move(input_value),
                    dt, device_id, EInputMessageEvent::axis
                ));
            }
        }

        void reset() noexcept {
            reset(zero_v);
        }

        void reset(const_reference init) noexcept {
            m_raw.m_absolute = init;
            m_raw.m_relative = value_type{zero_v};
            m_filtered = m_raw;
            m_next_raw_absolute = m_raw.m_absolute;
        }
    };

    // ------------------------------------------------------------------
    // digital input state
    // ------------------------------------------------------------------

    template<typename ButtonT, mem::details::TAllocator AllocatorT = mem::GPA>
        requires std::is_enum_v<ButtonT> or std::is_integral_v<ButtonT>
    class InputDigitalState {
    public:
        using set_type = FlatSet<ButtonT>;

        InputDigitalState() noexcept
            requires mem::Allocator<AllocatorT>::is_stateless_v
        = default;

        explicit InputDigitalState(AllocatorT &alloc) noexcept
            : m_buttons_queued(stl_allocator(alloc)),
              m_buttons_down(stl_allocator(alloc)),
              m_buttons_pressed(stl_allocator(alloc)),
              m_buttons_up(stl_allocator(alloc)) {
        }

        explicit InputDigitalState(const AllocatorT &alloc) noexcept
            : m_buttons_queued(stl_allocator(alloc)),
              m_buttons_down(stl_allocator(alloc)),
              m_buttons_pressed(stl_allocator(alloc)),
              m_buttons_up(stl_allocator(alloc)) {
        }

        void setPressed(const ButtonT button) {
            m_buttons_queued.insert(button);
        }

        [[nodiscard]] const set_type &getDown() const noexcept {
            return m_buttons_down;
        }

        [[nodiscard]] const set_type &getPressed() const noexcept {
            return m_buttons_pressed;
        }

        [[nodiscard]] const set_type &getUp() const noexcept {
            return m_buttons_up;
        }

        [[nodiscard]] bool anyDown() const noexcept {
            return not m_buttons_down.empty();
        }

        [[nodiscard]] bool isDown(const ButtonT button) const noexcept {
            return m_buttons_down.contains(button);
        }

        [[nodiscard]] bool areDown(const std::initializer_list<ButtonT> buttons) const noexcept {
            for (const ButtonT button: buttons) {
                if (not m_buttons_down.contains(button)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool anyPressed() const noexcept {
            return not m_buttons_pressed.empty();
        }

        [[nodiscard]] bool isPressed(const ButtonT button) const noexcept {
            return m_buttons_pressed.contains(button);
        }

        [[nodiscard]] bool arePressed(const std::initializer_list<ButtonT> buttons) const noexcept {
            for (const ButtonT button: buttons) {
                if (not m_buttons_pressed.contains(button)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool anyUp() const noexcept {
            return not m_buttons_up.empty();
        }

        [[nodiscard]] bool isUp(const ButtonT button) const noexcept {
            return m_buttons_up.contains(button);
        }

        [[nodiscard]] bool areUp(const std::initializer_list<ButtonT> buttons) const noexcept {
            for (const ButtonT button: buttons) {
                if (not m_buttons_up.contains(button)) {
                    return false;
                }
            }
            return true;
        }

        bool update() {
            m_buttons_up.clear();
            m_buttons_down.clear();

            for (const ButtonT event: m_buttons_queued) {
                if (not m_buttons_pressed.contains(event)) {
                    PPR_VERIFY(m_buttons_down.insert(event).second);
                }
            }

            for (const ButtonT event: m_buttons_pressed) {
                if (not m_buttons_queued.contains(event)) {
                    PPR_VERIFY(m_buttons_up.insert(event).second);
                }
            }

            swap(m_buttons_pressed, m_buttons_queued);
            m_buttons_queued.clear();

            return not(m_buttons_up.empty() && m_buttons_down.empty());
        }

        std::error_code postInputMessages(
            const InputDeviceID &device_id,
            const TimeSpan dt,
            const Collector<InputMessage> post_event) const noexcept {
            const auto post_button_event = [&](const ButtonT button, const EInputMessageEvent event) -> std::error_code {
                if (const std::optional<InputKey> input_key = InputKey::from(button);
                    input_key.has_value()) {
                    PPR_ASSERT(EInputValueType::digital == input_key->m_value);

                    if (const std::error_code err = post_event(InputMessage(
                        input_key.value(),
                        InputDigital(EInputMessageEvent::released != event),
                        dt, device_id, event
                    ))) [[unlikely]] {
                        return err;
                    }
                }
                return default_value_v;
            };

            for (const ButtonT button: m_buttons_down) {
                if (const std::error_code err = post_button_event(button, EInputMessageEvent::pressed)) [[unlikely]] {
                    return err;
                }
            }
            for (const ButtonT button: m_buttons_pressed) {
                if (const std::error_code err = post_button_event(button, EInputMessageEvent::repeat)) [[unlikely]] {
                    return err;
                }
            }
            for (const ButtonT button: m_buttons_up) {
                if (const std::error_code err = post_button_event(button, EInputMessageEvent::released)) [[unlikely]] {
                    return err;
                }
            }

            return default_value_v;
        }

        void reset() noexcept {
            m_buttons_queued.clear();

            m_buttons_down.clear();
            m_buttons_pressed.clear();
            m_buttons_up.clear();
        }

    private:
        set_type m_buttons_queued{};

        set_type m_buttons_down{};
        set_type m_buttons_pressed{};
        set_type m_buttons_up{};
    };
}
