module;

export module engine.app:input.key;

import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // keyboard
    // ------------------------------------------------------------------

    enum class EKeyboardKey : u8 {
        // TNumeric

        zero = '0',
        one = '1',
        two = '2',
        three = '3',
        four = '4',
        five = '5',
        six = '6',
        seven = '7',
        eight = '8',
        nine = '9',

        // alpha

        a = 'a',
        b = 'b',
        c = 'c',
        d = 'd',
        e = 'e',
        f = 'f',
        g = 'g',
        h = 'h',
        i = 'i',
        j = 'j',
        k = 'k',
        l = 'l',
        m = 'm',
        n = 'n',
        o = 'o',
        p = 'p',
        q = 'q',
        r = 'r',
        s = 's',
        t = 't',
        u = 'u',
        v = 'v',
        w = 'w',
        x = 'x',
        y = 'y',
        z = 'z',

        // punctuation

        comma = ',',
        equals = '=',
        plus = '+',
        minus = '-',
        period = '.',
        semicolon = ';',
        underscore = '_',

        ampersand = '&',
        apostrophe = '\'',
        asterix = '*',
        caret = '^',
        colon = ':',
        dollar = '$',
        exclamation = '!',
        tilde = '~',
        quote = '"',

        slash = '/',
        backslash = '\\',
        left_bracket = '[',
        right_bracket = ']',
        left_parenthesis = '(',
        right_parenthesis = ')',

        // numpad

        numpad0 = 0,
        numpad1,
        numpad2,
        numpad3,
        numpad4,
        numpad5,
        numpad6,
        numpad7,
        numpad8,
        numpad9,

        numpad_multiply,
        numpad_add,
        numpad_subtract,
        numpad_decimal,
        numpad_divide,
        numpad_enter,
        numpad_equal,

        // function

        f1,
        f2,
        f3,
        f4,
        f5,
        f6,
        f7,
        f8,
        f9,
        f10,
        f11,
        f12,

        // specials

        escape = tilde + 1,
        space,

        pause,
        print_screen,
        scroll_lock,

        backspace,
        enter,
        tab,

        home,
        end,
        insert,
        delete_,
        page_up,
        page_down,

        // directions

        left_arrow,
        right_arrow,
        up_arrow,
        down_arrow,

        // specials

        caps_lock,
        num_lock,

        // modifiers

        left_alt,
        right_alt,
        left_control,
        right_control,
        left_shift,
        right_shift,

        left_super, // windows key / apple command
        right_super,
    };

    // ------------------------------------------------------------------
    // gamepad
    // ------------------------------------------------------------------

    enum class EGamepadAxis : u8 {
        stick0 = 0,
        stick1,
        stick2,
        stick3,

        trigger0,
        trigger1,
        trigger2,
        trigger4,

        left_stick = stick0,
        right_stick = stick1,

        LS = left_stick,
        LR = right_stick,

        left_trigger = trigger0,
        right_trigger = trigger1,

        LT = left_trigger,
        RT = right_trigger,
    };

    enum class EGamepadButton : u8 {
        button0 = 0,
        button1,
        button2,
        button3,
        button4,
        button5,
        button6,
        button7,
        button8,
        button9,

        dpad_up,
        dpad_left,
        dpad_right,
        dpad_down,

        A = button0,
        B = button1,
        X = button2,
        Y = button3,

        left_thumb = button4,
        right_thumb = button5,

        LB = left_thumb,
        RB = right_thumb,

        start = button6,
        back = button7,

        left_shoulder = button8,
        right_shoulder = button9,

        LS = left_shoulder,
        RS = right_shoulder,
    };

    // ------------------------------------------------------------------
    // mouse
    // ------------------------------------------------------------------

    enum class EMouseAxis : u8 {
        pointer = 0,
        scroll_wheel_y,
        scroll_wheel_x,
    };

    enum class EMouseButton : u8 {
        button0 = 0, // left (assuming left handed)
        button1, // right
        button2, // middle

        thumb0, // previous
        thumb1, // next

        left = button0,
        right = button1,
        middle = button2,

        back = thumb0,
        next = thumb1,
    };

    // ------------------------------------------------------------------
    // input key -> variant
    // ------------------------------------------------------------------

    enum class EInputValueType : u8 {
        digital = 0,
        axis_1d = 1,
        axis_2d = 2,
        axis_3d = 3,
    };

    using InputKeyCode = std::variant<
        std::monostate,

        EKeyboardKey,

        EGamepadAxis,
        EGamepadButton,

        EMouseAxis,
        EMouseButton
    >;

    struct InputKey final {
        string_literal m_name;
        InputKeyCode m_code;
        EInputValueType m_value;

        constexpr InputKey(
            const string_literal name,
            const InputKeyCode code,
            const EInputValueType value) noexcept
            : m_name(name), m_code(code), m_value(value) {
        }

        constexpr ~InputKey() noexcept = default;

        [[nodiscard]] constexpr bool isAnalog() const noexcept {
            return m_value != EInputValueType::digital;
        }

        [[nodiscard]] constexpr bool isDigital() const noexcept {
            return m_value == EInputValueType::digital;
        }

        [[nodiscard]] constexpr bool isAny() const noexcept {
            return std::get_if<std::monostate>(&m_code) != nullptr;
        }

        [[nodiscard]] bool isKeyboard() const noexcept;

        [[nodiscard]] bool isGamepad() const noexcept;

        [[nodiscard]] bool isMouse() const noexcept;

        [[nodiscard]] friend constexpr bool
        operator==(const InputKey &lhs, const InputKey &rhs) noexcept {
            return lhs.m_code == rhs.m_code;
        }

        [[nodiscard]] friend constexpr std::strong_ordering
        operator<=>(const InputKey &lhs, const InputKey &rhs) noexcept {
            return lhs.m_code <=> rhs.m_code;
        }

        [[nodiscard]] friend hash_t hashValue(const InputKey &value) noexcept;

        [[nodiscard]] friend opaque::Value opaqueValue(const InputKey &value) noexcept;

        static std::error_code enumerateAll(Collector<InputKey> push_back) noexcept;

        static std::error_code enumerateKeyboardKeys(Collector<InputKey> push_back) noexcept;

        static std::error_code enumerateGamepadAxes(Collector<InputKey> push_back) noexcept;

        static std::error_code enumerateGamepadButtons(Collector<InputKey> push_back) noexcept;

        static std::error_code enumerateMouseAxes(Collector<InputKey> push_back) noexcept;

        static std::error_code enumerateMouseButtons(Collector<InputKey> push_back) noexcept;

        [[nodiscard]] static std::optional<InputKey> from(EKeyboardKey value) noexcept;

        [[nodiscard]] static std::optional<InputKey> from(EGamepadAxis value) noexcept;

        [[nodiscard]] static std::optional<InputKey> from(EGamepadButton value) noexcept;

        [[nodiscard]] static std::optional<InputKey> from(EMouseAxis value) noexcept;

        [[nodiscard]] static std::optional<InputKey> from(EMouseButton value) noexcept;

        // static aliases:

        static const InputKey any_key;

        static const InputKey any_axis_1d;
        static const InputKey any_axis_2d;
        static const InputKey any_axis_3d;

        static const InputKey mouse_2d;
        static const InputKey mouse_wheel_axis_x;
        static const InputKey mouse_wheel_axis_y;

        static const InputKey left_mouse_button;
        static const InputKey right_mouse_button;
        static const InputKey middle_mouse_button;
        static const InputKey thumb_mouse_button;
        static const InputKey thumb_mouse_button2;

        static const InputKey back_space;
        static const InputKey tab;
        static const InputKey enter;
        static const InputKey pause;

        static const InputKey caps_lock;
        static const InputKey escape;
        static const InputKey space_bar;
        static const InputKey page_up;
        static const InputKey page_down;
        static const InputKey end;
        static const InputKey home;

        static const InputKey left_arrow;
        static const InputKey up_arrow;
        static const InputKey right_arrow;
        static const InputKey down_arrow;

        static const InputKey insert;
        static const InputKey delete_;

        static const InputKey zero;
        static const InputKey one;
        static const InputKey two;
        static const InputKey three;
        static const InputKey four;
        static const InputKey five;
        static const InputKey six;
        static const InputKey seven;
        static const InputKey eight;
        static const InputKey nine;

        static const InputKey a;
        static const InputKey b;
        static const InputKey c;
        static const InputKey d;
        static const InputKey e;
        static const InputKey f;
        static const InputKey g;
        static const InputKey h;
        static const InputKey i;
        static const InputKey j;
        static const InputKey k;
        static const InputKey l;
        static const InputKey m;
        static const InputKey n;
        static const InputKey o;
        static const InputKey p;
        static const InputKey q;
        static const InputKey r;
        static const InputKey s;
        static const InputKey t;
        static const InputKey u;
        static const InputKey v;
        static const InputKey w;
        static const InputKey x;
        static const InputKey y;
        static const InputKey z;

        static const InputKey numpad0;
        static const InputKey numpad1;
        static const InputKey numpad2;
        static const InputKey numpad3;
        static const InputKey numpad4;
        static const InputKey numpad5;
        static const InputKey numpad6;
        static const InputKey numpad7;
        static const InputKey numpad8;
        static const InputKey numpad9;

        static const InputKey numpad_multiply;
        static const InputKey numpad_add;
        static const InputKey numpad_subtract;
        static const InputKey numpad_decimal;
        static const InputKey numpad_divide;
        static const InputKey numpad_enter;
        static const InputKey numpad_equal;

        static const InputKey multiply;
        static const InputKey add;
        static const InputKey subtract;
        static const InputKey decimal;
        static const InputKey divide;

        static const InputKey f1;
        static const InputKey f2;
        static const InputKey f3;
        static const InputKey f4;
        static const InputKey f5;
        static const InputKey f6;
        static const InputKey f7;
        static const InputKey f8;
        static const InputKey f9;
        static const InputKey f10;
        static const InputKey f11;
        static const InputKey f12;

        static const InputKey num_lock;
        static const InputKey print_screen;
        static const InputKey scroll_lock;

        static const InputKey left_shift;
        static const InputKey right_shift;
        static const InputKey left_control;
        static const InputKey right_control;
        static const InputKey left_alt;
        static const InputKey right_alt;
        static const InputKey left_super;
        static const InputKey right_super;

        static const InputKey semicolon;
        static const InputKey equals;
        static const InputKey underscore;
        static const InputKey hyphen;
        static const InputKey period;
        static const InputKey tilde;
        static const InputKey left_bracket;
        static const InputKey backslash;
        static const InputKey right_bracket;
        static const InputKey apostrophe;

        static const InputKey ampersand;
        static const InputKey caret;
        static const InputKey colon;
        static const InputKey dollar;
        static const InputKey exclamation;
        static const InputKey left_parenthesis;
        static const InputKey right_parenthesis;
        static const InputKey quote;

        // Gamepad keys
        static const InputKey gamepad_left_2d;
        static const InputKey gamepad_right_2d;
        static const InputKey gamepad_left_trigger_axis;
        static const InputKey gamepad_right_trigger_axis;

        static const InputKey gamepad_left_thumbstick;
        static const InputKey gamepad_right_thumbstick;
        static const InputKey gamepad_face_button_bottom;
        static const InputKey gamepad_face_button_right;
        static const InputKey gamepad_face_button_left;
        static const InputKey gamepad_face_button_top;
        static const InputKey gamepad_left_shoulder;
        static const InputKey gamepad_right_shoulder;
        static const InputKey gamepad_dpad_up;
        static const InputKey gamepad_dpad_down;
        static const InputKey gamepad_dpad_right;
        static const InputKey gamepad_dpad_left;
        static const InputKey gamepad_start;
        static const InputKey gamepad_back;
    };

    // ------------------------------------------------------------------
    // input value
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        struct input_value {
            T m_absolute{zero_v};
            T m_relative{zero_v};
        };
    }

    using InputDigital = Numeric<bool, InputKey>;
    using InputAxis1D = details::input_value<float>;
    using InputAxis2D = details::input_value<float2>;
    using InputAxis3D = details::input_value<float3>;

    namespace details {
        using InputValueVariant = std::variant<
            InputDigital,
            InputAxis1D,
            InputAxis2D,
            InputAxis3D>;
    }

    struct [[nodiscard]] InputValue final : details::InputValueVariant {
        using details::InputValueVariant::InputValueVariant;
        using details::InputValueVariant::operator=;

        constexpr ~InputValue() noexcept = default;

        [[nodiscard]] constexpr EInputValueType getType() const noexcept {
            return static_cast<EInputValueType>(index());
        }

        [[nodiscard]] InputValue modulate(float value) const noexcept;

        [[nodiscard]] InputValue modulate(const float2 &value) const noexcept;

        [[nodiscard]] InputValue modulate(const float3 &value) const noexcept;

        [[nodiscard]] friend hash_t hashValue(const InputValue &value) noexcept;

        [[nodiscard]] friend opaque::Value opaqueValue(const InputValue &value) noexcept;
    };
}
