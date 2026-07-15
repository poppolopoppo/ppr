module;
#include "pP/Macros.h"
module engine.app;

import :input.key;

namespace pP {
    // ------------------------------------------------------------------
    // input value modulate
    // ------------------------------------------------------------------

    InputValue InputValue::modulate(const float value) const noexcept {
        return std::visit(
            overloaded(
                [=](const InputDigital input) constexpr -> InputValue {
                    return InputAxis1D{
                        .m_absolute = input ? value : 0.0f,
                        .m_relative = input ? value : 0.0f,
                    };
                },
                [=](const InputAxis1D input) constexpr -> InputValue {
                    return InputAxis1D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                },
                [=](const InputAxis2D &input) constexpr -> InputValue {
                    return InputAxis2D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                },
                [=](const InputAxis3D &input) constexpr -> InputValue {
                    return InputAxis3D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                }),
            *this);
    }

    InputValue InputValue::modulate(const float2 &value) const noexcept {
        return std::visit(
            overloaded(
                [&](const InputDigital input) constexpr -> InputValue {
                    return InputAxis2D{
                        .m_absolute = input ? value : float2(0.0f),
                        .m_relative = input ? value : float2(0.0f),
                    };
                },
                [&](const InputAxis1D input) constexpr -> InputValue {
                    return InputAxis2D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                },
                [&](const InputAxis2D &input) constexpr -> InputValue {
                    return InputAxis2D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                },
                [&](const InputAxis3D &input) constexpr -> InputValue {
                    return InputAxis2D{
                        .m_absolute = input.m_absolute.xz * value,
                        .m_relative = input.m_relative.xz * value,
                    };
                }),
            *this);
    }

    InputValue InputValue::modulate(const float3 &value) const noexcept {
        return std::visit(
            overloaded(
                [&](const InputDigital input) constexpr -> InputValue {
                    return InputAxis3D{
                        .m_absolute = input ? value : float3(0.0f),
                        .m_relative = input ? value : float3(0.0f),
                    };
                },
                [&](const InputAxis1D input) constexpr -> InputValue {
                    return InputAxis3D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                },
                [&](const InputAxis2D &input) constexpr -> InputValue {
                    return InputAxis3D{
                        .m_absolute = float3(input.m_absolute, 0.0f).xzy * value,
                        .m_relative = float3(input.m_relative, 0.0f).xzy * value,
                    };
                },
                [&](const InputAxis3D &input) constexpr -> InputValue {
                    return InputAxis3D{
                        .m_absolute = input.m_absolute * value,
                        .m_relative = input.m_relative * value,
                    };
                }),
            *this);
    }

    hash_t hashValue(const InputValue &value) noexcept {
        return std::visit(
            [](const auto &inner_value) noexcept -> hash_t {
                return hashValue(inner_value);
            },
            value);
    }

    opaque::Value opaqueValue(const InputValue &value) noexcept {
        return std::visit(
            overloaded(
                [](const InputDigital digital) constexpr noexcept -> opaque::Value {
                    return *digital;
                },
                [](const InputAxis1D axis1d) constexpr noexcept -> opaque::Value {
                    return opaque::Dict{
                        {"absolute", axis1d.m_absolute},
                        {"relative", axis1d.m_relative}
                    };
                },
                [](const InputAxis2D &axis2d) constexpr noexcept -> opaque::Value {
                    return opaque::Dict{
                        {"absolute", opaqueValue(axis2d.m_absolute)},
                        {"relative", opaqueValue(axis2d.m_relative)}
                    };
                },
                [](const InputAxis3D &axis3d) constexpr noexcept -> opaque::Value {
                    return opaque::Dict{
                        {"absolute", opaqueValue(axis3d.m_absolute)},
                        {"relative", opaqueValue(axis3d.m_relative)}
                    };
                }),
            value);
    }

    // ------------------------------------------------------------------
    // input key
    // ------------------------------------------------------------------

    bool InputKey::isKeyboard() const noexcept {
        return std::visit(
            overloaded(
                [](EKeyboardKey) constexpr noexcept -> bool {
                    return true;
                },
                [](auto) constexpr noexcept -> bool {
                    return false;
                }), m_code);
    }

    bool InputKey::isGamepad() const noexcept {
        return std::visit(
            overloaded(
                [](EGamepadAxis) constexpr noexcept -> bool {
                    return true;
                },
                [](EGamepadButton) constexpr noexcept -> bool {
                    return true;
                },
                [](auto) constexpr noexcept -> bool {
                    return false;
                }), m_code);
    }

    bool InputKey::isMouse() const noexcept {
        return std::visit(
            overloaded(
                [](EMouseAxis) constexpr noexcept -> bool {
                    return true;
                },
                [](EMouseButton) constexpr noexcept -> bool {
                    return true;
                },
                [](auto) constexpr noexcept -> bool {
                    return false;
                }), m_code);
    }

    hash_t hashValue(const InputKey &value) noexcept {
        return std::visit(
            overloaded(
                [](const std::monostate) constexpr noexcept -> hash_t {
                    return hash_t(zero_v);
                },
                [](auto input_code) constexpr noexcept -> hash_t {
                    return hashValue(enumOrd(input_code));
                }),
            value.m_code);
    }

    opaque::Value opaqueValue(const InputKey &value) noexcept {
        return value.m_name;
    }

    std::optional<InputKey> InputKey::from(const EGamepadAxis value) noexcept {
        switch (value) {
            case EGamepadAxis::left_stick: return gamepad_left_2d;
            case EGamepadAxis::right_stick: return gamepad_right_2d;
            case EGamepadAxis::left_trigger: return gamepad_left_trigger_axis;
            case EGamepadAxis::right_trigger: return gamepad_right_trigger_axis;

            case EGamepadAxis::stick2: [[fallthrough]];
            case EGamepadAxis::stick3: [[fallthrough]];
            case EGamepadAxis::trigger2: [[fallthrough]];
            case EGamepadAxis::trigger4: break;;
        }
        return std::nullopt;
    }

    std::optional<InputKey> InputKey::from(const EGamepadButton value) noexcept {
        switch (value) {
            case EGamepadButton::button0: return gamepad_face_button_bottom;
            case EGamepadButton::button1: return gamepad_face_button_right;
            case EGamepadButton::button2: return gamepad_face_button_left;
            case EGamepadButton::button3: return gamepad_face_button_top;

            case EGamepadButton::dpad_up: return gamepad_dpad_up;
            case EGamepadButton::dpad_left: return gamepad_dpad_left;
            case EGamepadButton::dpad_right: return gamepad_dpad_right;
            case EGamepadButton::dpad_down: return gamepad_dpad_down;


            case EGamepadButton::left_thumb: return gamepad_left_thumbstick;
            case EGamepadButton::right_thumb: return gamepad_right_thumbstick;

            case EGamepadButton::start: return gamepad_start;
            case EGamepadButton::back: return gamepad_back;

            case EGamepadButton::left_shoulder: return gamepad_left_shoulder;
            case EGamepadButton::right_shoulder: return gamepad_right_shoulder;
        }
        std::unreachable();
    }

    std::optional<InputKey> InputKey::from(const EMouseAxis value) noexcept {
        switch (value) {
            case EMouseAxis::pointer: return mouse_2d;
            case EMouseAxis::scroll_wheel_y: return mouse_wheel_axis_y;
            case EMouseAxis::scroll_wheel_x: return mouse_wheel_axis_x;
        }
        std::unreachable();
    }

    std::optional<InputKey> InputKey::from(const EMouseButton value) noexcept {
        switch (value) {
            case EMouseButton::left: return left_mouse_button;
            case EMouseButton::right: return right_mouse_button;
            case EMouseButton::middle: return middle_mouse_button;
            case EMouseButton::thumb0: return thumb_mouse_button;
            case EMouseButton::thumb1: return thumb_mouse_button2;
        }
        std::unreachable();
    }

    std::optional<InputKey> InputKey::from(const EKeyboardKey value) noexcept {
        switch (value) {
            case EKeyboardKey::zero: return zero;
            case EKeyboardKey::one: return one;
            case EKeyboardKey::two: return two;
            case EKeyboardKey::three: return three;
            case EKeyboardKey::four: return four;
            case EKeyboardKey::five: return five;
            case EKeyboardKey::six: return six;
            case EKeyboardKey::seven: return seven;
            case EKeyboardKey::eight: return eight;
            case EKeyboardKey::nine: return nine;
            case EKeyboardKey::a: return a;
            case EKeyboardKey::b: return b;
            case EKeyboardKey::c: return c;
            case EKeyboardKey::d: return d;
            case EKeyboardKey::e: return e;
            case EKeyboardKey::f: return f;
            case EKeyboardKey::g: return g;
            case EKeyboardKey::h: return h;
            case EKeyboardKey::i: return i;
            case EKeyboardKey::j: return j;
            case EKeyboardKey::k: return k;
            case EKeyboardKey::l: return l;
            case EKeyboardKey::m: return m;
            case EKeyboardKey::n: return n;
            case EKeyboardKey::o: return o;
            case EKeyboardKey::p: return p;
            case EKeyboardKey::q: return q;
            case EKeyboardKey::r: return r;
            case EKeyboardKey::s: return s;
            case EKeyboardKey::t: return t;
            case EKeyboardKey::u: return u;
            case EKeyboardKey::v: return v;
            case EKeyboardKey::w: return w;
            case EKeyboardKey::x: return x;
            case EKeyboardKey::y: return y;
            case EKeyboardKey::z: return z;
            case EKeyboardKey::comma: return decimal;
            case EKeyboardKey::equals: return equals;
            case EKeyboardKey::plus: return add;
            case EKeyboardKey::minus: return subtract;
            case EKeyboardKey::period: return period;
            case EKeyboardKey::semicolon: return semicolon;
            case EKeyboardKey::underscore: return underscore;
            case EKeyboardKey::ampersand: return ampersand;
            case EKeyboardKey::apostrophe: return apostrophe;
            case EKeyboardKey::asterix: return multiply;
            case EKeyboardKey::caret: return caret;
            case EKeyboardKey::colon: return colon;
            case EKeyboardKey::dollar: return dollar;
            case EKeyboardKey::exclamation: return exclamation;
            case EKeyboardKey::tilde: return tilde;
            case EKeyboardKey::quote: return quote;
            case EKeyboardKey::slash: return divide;
            case EKeyboardKey::backslash: return backslash;
            case EKeyboardKey::left_bracket: return left_bracket;
            case EKeyboardKey::right_bracket: return right_bracket;
            case EKeyboardKey::left_parenthesis: return left_parenthesis;
            case EKeyboardKey::right_parenthesis: return right_parenthesis;
            case EKeyboardKey::numpad0: return numpad0;
            case EKeyboardKey::numpad1: return numpad1;
            case EKeyboardKey::numpad2: return numpad2;
            case EKeyboardKey::numpad3: return numpad3;
            case EKeyboardKey::numpad4: return numpad4;
            case EKeyboardKey::numpad5: return numpad5;
            case EKeyboardKey::numpad6: return numpad6;
            case EKeyboardKey::numpad7: return numpad7;
            case EKeyboardKey::numpad8: return numpad8;
            case EKeyboardKey::numpad9: return numpad9;
            case EKeyboardKey::f1: return f1;
            case EKeyboardKey::f2: return f2;
            case EKeyboardKey::f3: return f3;
            case EKeyboardKey::f4: return f4;
            case EKeyboardKey::f5: return f5;
            case EKeyboardKey::f6: return f6;
            case EKeyboardKey::f7: return f7;
            case EKeyboardKey::f8: return f8;
            case EKeyboardKey::f9: return f9;
            case EKeyboardKey::f10: return f10;
            case EKeyboardKey::f11: return f11;
            case EKeyboardKey::f12: return f12;
            case EKeyboardKey::escape: return escape;
            case EKeyboardKey::space: return space_bar;
            case EKeyboardKey::pause: return pause;
            case EKeyboardKey::print_screen: return print_screen;
            case EKeyboardKey::scroll_lock: return scroll_lock;
            case EKeyboardKey::backspace: return back_space;
            case EKeyboardKey::enter: return enter;
            case EKeyboardKey::tab: return tab;
            case EKeyboardKey::home: return home;
            case EKeyboardKey::end: return end;
            case EKeyboardKey::insert: return insert;
            case EKeyboardKey::delete_: return delete_;
            case EKeyboardKey::page_up: return page_up;
            case EKeyboardKey::page_down: return page_down;
            case EKeyboardKey::left_arrow: return left_arrow;
            case EKeyboardKey::right_arrow: return right_arrow;
            case EKeyboardKey::up_arrow: return up_arrow;
            case EKeyboardKey::down_arrow: return down_arrow;
            case EKeyboardKey::caps_lock: return caps_lock;
            case EKeyboardKey::num_lock: return num_lock;
            case EKeyboardKey::left_alt: return left_alt;
            case EKeyboardKey::right_alt: return right_alt;
            case EKeyboardKey::left_control: return left_control;
            case EKeyboardKey::right_control: return right_control;
            case EKeyboardKey::left_shift: return left_shift;
            case EKeyboardKey::right_shift: return right_shift;
            case EKeyboardKey::left_super: return left_super;
            case EKeyboardKey::right_super: return right_super;
        }
        std::unreachable();
    }

    void InputKey::enumerateGamepadAxes(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(gamepad_left_2d);
        push_back(gamepad_right_2d);
        push_back(gamepad_left_trigger_axis);
        push_back(gamepad_right_trigger_axis);
    }

    void InputKey::enumerateGamepadButtons(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(gamepad_left_thumbstick);
        push_back(gamepad_right_thumbstick);
        push_back(gamepad_face_button_bottom);
        push_back(gamepad_face_button_right);
        push_back(gamepad_face_button_left);
        push_back(gamepad_face_button_top);
        push_back(gamepad_left_shoulder);
        push_back(gamepad_right_shoulder);
        push_back(gamepad_dpad_up);
        push_back(gamepad_dpad_down);
        push_back(gamepad_dpad_right);
        push_back(gamepad_dpad_left);
        push_back(gamepad_start);
        push_back(gamepad_back);
    }

    void InputKey::enumerateMouseAxes(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(mouse_2d);
        push_back(mouse_wheel_axis_x);
        push_back(mouse_wheel_axis_y);
    }

    void InputKey::enumerateMouseButtons(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(left_mouse_button);
        push_back(right_mouse_button);
        push_back(middle_mouse_button);
        push_back(thumb_mouse_button);
        push_back(thumb_mouse_button2);
    }

    void InputKey::enumerateAll(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(mouse_2d);
        push_back(mouse_wheel_axis_x);
        push_back(mouse_wheel_axis_y);
        push_back(left_mouse_button);
        push_back(right_mouse_button);
        push_back(middle_mouse_button);
        push_back(thumb_mouse_button);
        push_back(thumb_mouse_button2);
        push_back(back_space);
        push_back(tab);
        push_back(enter);
        push_back(pause);
        push_back(caps_lock);
        push_back(escape);
        push_back(space_bar);
        push_back(page_up);
        push_back(page_down);
        push_back(end);
        push_back(home);
        push_back(left_arrow);
        push_back(up_arrow);
        push_back(right_arrow);
        push_back(down_arrow);
        push_back(insert);
        push_back(delete_);
        push_back(zero);
        push_back(one);
        push_back(two);
        push_back(three);
        push_back(four);
        push_back(five);
        push_back(six);
        push_back(seven);
        push_back(eight);
        push_back(nine);
        push_back(a);
        push_back(b);
        push_back(c);
        push_back(d);
        push_back(e);
        push_back(f);
        push_back(g);
        push_back(h);
        push_back(i);
        push_back(j);
        push_back(k);
        push_back(l);
        push_back(m);
        push_back(n);
        push_back(o);
        push_back(p);
        push_back(q);
        push_back(r);
        push_back(s);
        push_back(t);
        push_back(u);
        push_back(v);
        push_back(w);
        push_back(x);
        push_back(y);
        push_back(z);
        push_back(numpad0);
        push_back(numpad1);
        push_back(numpad2);
        push_back(numpad3);
        push_back(numpad4);
        push_back(numpad5);
        push_back(numpad6);
        push_back(numpad7);
        push_back(numpad8);
        push_back(numpad9);
        push_back(multiply);
        push_back(add);
        push_back(subtract);
        push_back(decimal);
        push_back(divide);
        push_back(f1);
        push_back(f2);
        push_back(f3);
        push_back(f4);
        push_back(f5);
        push_back(f6);
        push_back(f7);
        push_back(f8);
        push_back(f9);
        push_back(f10);
        push_back(f11);
        push_back(f12);
        push_back(num_lock);
        push_back(print_screen);
        push_back(scroll_lock);
        push_back(left_shift);
        push_back(right_shift);
        push_back(left_control);
        push_back(right_control);
        push_back(left_alt);
        push_back(right_alt);
        push_back(left_super);
        push_back(right_super);
        push_back(semicolon);
        push_back(equals);
        push_back(underscore);

        push_back(period);
        push_back(tilde);
        push_back(left_bracket);
        push_back(backslash);
        push_back(right_bracket);
        push_back(apostrophe);
        push_back(ampersand);
        push_back(caret);
        push_back(colon);
        push_back(dollar);
        push_back(exclamation);
        push_back(left_parenthesis);
        push_back(right_parenthesis);
        push_back(quote);
        push_back(gamepad_left_2d);
        push_back(gamepad_right_2d);
        push_back(gamepad_left_trigger_axis);
        push_back(gamepad_right_trigger_axis);
        push_back(gamepad_left_thumbstick);
        push_back(gamepad_right_thumbstick);
        push_back(gamepad_face_button_bottom);
        push_back(gamepad_face_button_right);
        push_back(gamepad_face_button_left);
        push_back(gamepad_face_button_top);
        push_back(gamepad_left_shoulder);
        push_back(gamepad_right_shoulder);
        push_back(gamepad_dpad_up);
        push_back(gamepad_dpad_down);
        push_back(gamepad_dpad_right);
        push_back(gamepad_dpad_left);
        push_back(gamepad_start);
        push_back(gamepad_back);
    }

    void InputKey::enumerateKeyboardKeys(const std23::function_ref<void(const InputKey &key)> push_back) noexcept {
        push_back(back_space);
        push_back(tab);
        push_back(enter);
        push_back(pause);
        push_back(caps_lock);
        push_back(escape);
        push_back(space_bar);
        push_back(page_up);
        push_back(page_down);
        push_back(end);
        push_back(home);
        push_back(left_arrow);
        push_back(up_arrow);
        push_back(right_arrow);
        push_back(down_arrow);
        push_back(insert);
        push_back(delete_);
        push_back(zero);
        push_back(one);
        push_back(two);
        push_back(three);
        push_back(four);
        push_back(five);
        push_back(six);
        push_back(seven);
        push_back(eight);
        push_back(nine);
        push_back(a);
        push_back(b);
        push_back(c);
        push_back(d);
        push_back(e);
        push_back(f);
        push_back(g);
        push_back(h);
        push_back(i);
        push_back(j);
        push_back(k);
        push_back(l);
        push_back(m);
        push_back(n);
        push_back(o);
        push_back(p);
        push_back(q);
        push_back(r);
        push_back(s);
        push_back(t);
        push_back(u);
        push_back(v);
        push_back(w);
        push_back(x);
        push_back(y);
        push_back(z);
        push_back(numpad0);
        push_back(numpad1);
        push_back(numpad2);
        push_back(numpad3);
        push_back(numpad4);
        push_back(numpad5);
        push_back(numpad6);
        push_back(numpad7);
        push_back(numpad8);
        push_back(numpad9);
        push_back(multiply);
        push_back(add);
        push_back(subtract);
        push_back(decimal);
        push_back(divide);
        push_back(f1);
        push_back(f2);
        push_back(f3);
        push_back(f4);
        push_back(f5);
        push_back(f6);
        push_back(f7);
        push_back(f8);
        push_back(f9);
        push_back(f10);
        push_back(f11);
        push_back(f12);
        push_back(num_lock);
        push_back(print_screen);
        push_back(scroll_lock);
        push_back(left_shift);
        push_back(right_shift);
        push_back(left_control);
        push_back(right_control);
        push_back(left_alt);
        push_back(right_alt);
        push_back(left_super);
        push_back(right_super);
        push_back(semicolon);
        push_back(equals);
        push_back(underscore);

        push_back(period);
        push_back(tilde);
        push_back(left_bracket);
        push_back(backslash);
        push_back(right_bracket);
        push_back(apostrophe);
        push_back(ampersand);
        push_back(caret);
        push_back(colon);
        push_back(dollar);
        push_back(exclamation);
        push_back(left_parenthesis);
        push_back(right_parenthesis);
        push_back(quote);
    }

    const InputKey InputKey::any_key{"any_key", std::monostate{}, EInputValueType::digital};
    const InputKey InputKey::any_axis_1d{"any_axis_1d", std::monostate{}, EInputValueType::axis_1d};
    const InputKey InputKey::any_axis_2d{"any_axis_2d", std::monostate{}, EInputValueType::axis_2d};
    const InputKey InputKey::any_axis_3d{"any_axis_3d", std::monostate{}, EInputValueType::axis_3d};
    const InputKey InputKey::mouse_2d{"mouse2d", EMouseAxis::pointer, EInputValueType::axis_2d};
    const InputKey InputKey::mouse_wheel_axis_x{"mouse_wheel_axis_x", EMouseAxis::scroll_wheel_x, EInputValueType::axis_1d};
    const InputKey InputKey::mouse_wheel_axis_y{"mouse_wheel_axis_y", EMouseAxis::scroll_wheel_y, EInputValueType::axis_1d};
    const InputKey InputKey::left_mouse_button{"left_mouse_button", EMouseButton::left, EInputValueType::digital};
    const InputKey InputKey::right_mouse_button{"right_mouse_button", EMouseButton::right, EInputValueType::digital};
    const InputKey InputKey::middle_mouse_button{"middle_mouse_button", EMouseButton::middle, EInputValueType::digital};
    const InputKey InputKey::thumb_mouse_button{"thumb_mouse_button", EMouseButton::thumb0, EInputValueType::digital};
    const InputKey InputKey::thumb_mouse_button2{"thumb_mouse_button2", EMouseButton::thumb1, EInputValueType::digital};
    const InputKey InputKey::back_space{"back_space", EKeyboardKey::backspace, EInputValueType::digital};
    const InputKey InputKey::tab{"tab", EKeyboardKey::tab, EInputValueType::digital};
    const InputKey InputKey::enter{"enter", EKeyboardKey::enter, EInputValueType::digital};
    const InputKey InputKey::pause{"pause", EKeyboardKey::pause, EInputValueType::digital};
    const InputKey InputKey::caps_lock{"caps_lock", EKeyboardKey::caps_lock, EInputValueType::digital};
    const InputKey InputKey::escape{"escape", EKeyboardKey::escape, EInputValueType::digital};
    const InputKey InputKey::space_bar{"space_bar", EKeyboardKey::space, EInputValueType::digital};
    const InputKey InputKey::page_up{"page_up", EKeyboardKey::page_up, EInputValueType::digital};
    const InputKey InputKey::page_down{"page_down", EKeyboardKey::page_down, EInputValueType::digital};
    const InputKey InputKey::end{"end", EKeyboardKey::end, EInputValueType::digital};
    const InputKey InputKey::home{"home", EKeyboardKey::home, EInputValueType::digital};
    const InputKey InputKey::left_arrow{"left_arrow", EKeyboardKey::left_arrow, EInputValueType::digital};
    const InputKey InputKey::up_arrow{"up_arrow", EKeyboardKey::up_arrow, EInputValueType::digital};
    const InputKey InputKey::right_arrow{"right_arrow", EKeyboardKey::right_arrow, EInputValueType::digital};
    const InputKey InputKey::down_arrow{"down_arrow", EKeyboardKey::down_arrow, EInputValueType::digital};
    const InputKey InputKey::insert{"insert", EKeyboardKey::insert, EInputValueType::digital};
    const InputKey InputKey::delete_{"delete", EKeyboardKey::delete_, EInputValueType::digital};
    const InputKey InputKey::zero{"0", EKeyboardKey::zero, EInputValueType::digital};
    const InputKey InputKey::one{"1", EKeyboardKey::one, EInputValueType::digital};
    const InputKey InputKey::two{"2", EKeyboardKey::two, EInputValueType::digital};
    const InputKey InputKey::three{"3", EKeyboardKey::three, EInputValueType::digital};
    const InputKey InputKey::four{"4", EKeyboardKey::four, EInputValueType::digital};
    const InputKey InputKey::five{"5", EKeyboardKey::five, EInputValueType::digital};
    const InputKey InputKey::six{"6", EKeyboardKey::six, EInputValueType::digital};
    const InputKey InputKey::seven{"7", EKeyboardKey::seven, EInputValueType::digital};
    const InputKey InputKey::eight{"8", EKeyboardKey::eight, EInputValueType::digital};
    const InputKey InputKey::nine{"9", EKeyboardKey::nine, EInputValueType::digital};
    const InputKey InputKey::a{"a", EKeyboardKey::a, EInputValueType::digital};
    const InputKey InputKey::b{"b", EKeyboardKey::b, EInputValueType::digital};
    const InputKey InputKey::c{"c", EKeyboardKey::c, EInputValueType::digital};
    const InputKey InputKey::d{"d", EKeyboardKey::d, EInputValueType::digital};
    const InputKey InputKey::e{"e", EKeyboardKey::e, EInputValueType::digital};
    const InputKey InputKey::f{"f", EKeyboardKey::f, EInputValueType::digital};
    const InputKey InputKey::g{"g", EKeyboardKey::g, EInputValueType::digital};
    const InputKey InputKey::h{"h", EKeyboardKey::h, EInputValueType::digital};
    const InputKey InputKey::i{"i", EKeyboardKey::i, EInputValueType::digital};
    const InputKey InputKey::j{"j", EKeyboardKey::j, EInputValueType::digital};
    const InputKey InputKey::k{"k", EKeyboardKey::k, EInputValueType::digital};
    const InputKey InputKey::l{"l", EKeyboardKey::l, EInputValueType::digital};
    const InputKey InputKey::m{"m", EKeyboardKey::m, EInputValueType::digital};
    const InputKey InputKey::n{"n", EKeyboardKey::n, EInputValueType::digital};
    const InputKey InputKey::o{"o", EKeyboardKey::o, EInputValueType::digital};
    const InputKey InputKey::p{"p", EKeyboardKey::p, EInputValueType::digital};
    const InputKey InputKey::q{"q", EKeyboardKey::q, EInputValueType::digital};
    const InputKey InputKey::r{"r", EKeyboardKey::r, EInputValueType::digital};
    const InputKey InputKey::s{"s", EKeyboardKey::s, EInputValueType::digital};
    const InputKey InputKey::t{"t", EKeyboardKey::t, EInputValueType::digital};
    const InputKey InputKey::u{"u", EKeyboardKey::u, EInputValueType::digital};
    const InputKey InputKey::v{"v", EKeyboardKey::v, EInputValueType::digital};
    const InputKey InputKey::w{"w", EKeyboardKey::w, EInputValueType::digital};
    const InputKey InputKey::x{"x", EKeyboardKey::x, EInputValueType::digital};
    const InputKey InputKey::y{"y", EKeyboardKey::y, EInputValueType::digital};
    const InputKey InputKey::z{"z", EKeyboardKey::z, EInputValueType::digital};
    const InputKey InputKey::numpad0{"numpad0", EKeyboardKey::numpad0, EInputValueType::digital};
    const InputKey InputKey::numpad1{"numpad1", EKeyboardKey::numpad1, EInputValueType::digital};
    const InputKey InputKey::numpad2{"numpad2", EKeyboardKey::numpad2, EInputValueType::digital};
    const InputKey InputKey::numpad3{"numpad3", EKeyboardKey::numpad3, EInputValueType::digital};
    const InputKey InputKey::numpad4{"numpad4", EKeyboardKey::numpad4, EInputValueType::digital};
    const InputKey InputKey::numpad5{"numpad5", EKeyboardKey::numpad5, EInputValueType::digital};
    const InputKey InputKey::numpad6{"numpad6", EKeyboardKey::numpad6, EInputValueType::digital};
    const InputKey InputKey::numpad7{"numpad7", EKeyboardKey::numpad7, EInputValueType::digital};
    const InputKey InputKey::numpad8{"numpad8", EKeyboardKey::numpad8, EInputValueType::digital};
    const InputKey InputKey::numpad9{"numpad9", EKeyboardKey::numpad9, EInputValueType::digital};
    const InputKey InputKey::multiply{"multiply", EKeyboardKey::asterix, EInputValueType::digital};
    const InputKey InputKey::add{"add", EKeyboardKey::plus, EInputValueType::digital};
    const InputKey InputKey::subtract{"subtract", EKeyboardKey::minus, EInputValueType::digital};
    const InputKey InputKey::decimal{"decimal", EKeyboardKey::comma, EInputValueType::digital};
    const InputKey InputKey::divide{"divide", EKeyboardKey::slash, EInputValueType::digital};
    const InputKey InputKey::f1{"f1", EKeyboardKey::f1, EInputValueType::digital};
    const InputKey InputKey::f2{"f2", EKeyboardKey::f2, EInputValueType::digital};
    const InputKey InputKey::f3{"f3", EKeyboardKey::f3, EInputValueType::digital};
    const InputKey InputKey::f4{"f4", EKeyboardKey::f4, EInputValueType::digital};
    const InputKey InputKey::f5{"f5", EKeyboardKey::f5, EInputValueType::digital};
    const InputKey InputKey::f6{"f6", EKeyboardKey::f6, EInputValueType::digital};
    const InputKey InputKey::f7{"f7", EKeyboardKey::f7, EInputValueType::digital};
    const InputKey InputKey::f8{"f8", EKeyboardKey::f8, EInputValueType::digital};
    const InputKey InputKey::f9{"f9", EKeyboardKey::f9, EInputValueType::digital};
    const InputKey InputKey::f10{"f10", EKeyboardKey::f10, EInputValueType::digital};
    const InputKey InputKey::f11{"f11", EKeyboardKey::f11, EInputValueType::digital};
    const InputKey InputKey::f12{"f12", EKeyboardKey::f12, EInputValueType::digital};
    const InputKey InputKey::num_lock{"num_lock", EKeyboardKey::num_lock, EInputValueType::digital};
    const InputKey InputKey::print_screen{"print_screen", EKeyboardKey::print_screen, EInputValueType::digital};
    const InputKey InputKey::scroll_lock{"scroll_lock", EKeyboardKey::scroll_lock, EInputValueType::digital};
    const InputKey InputKey::left_shift{"left_shift", EKeyboardKey::left_shift, EInputValueType::digital};
    const InputKey InputKey::right_shift{"right_shift", EKeyboardKey::right_shift, EInputValueType::digital};
    const InputKey InputKey::left_control{"left_control", EKeyboardKey::left_control, EInputValueType::digital};
    const InputKey InputKey::right_control{"right_control", EKeyboardKey::right_control, EInputValueType::digital};
    const InputKey InputKey::left_alt{"left_alt", EKeyboardKey::left_alt, EInputValueType::digital};
    const InputKey InputKey::right_alt{"right_alt", EKeyboardKey::right_alt, EInputValueType::digital};
    const InputKey InputKey::left_super{"left_super", EKeyboardKey::left_super, EInputValueType::digital};
    const InputKey InputKey::right_super{"right_super", EKeyboardKey::right_super, EInputValueType::digital};
    const InputKey InputKey::semicolon{"semicolon", EKeyboardKey::semicolon, EInputValueType::digital};
    const InputKey InputKey::equals{"equals", EKeyboardKey::equals, EInputValueType::digital};
    const InputKey InputKey::underscore{"underscore", EKeyboardKey::underscore, EInputValueType::digital};
    const InputKey InputKey::period{"period", EKeyboardKey::period, EInputValueType::digital};
    const InputKey InputKey::tilde{"tilde", EKeyboardKey::tilde, EInputValueType::digital};
    const InputKey InputKey::left_bracket{"left_bracket", EKeyboardKey::left_bracket, EInputValueType::digital};
    const InputKey InputKey::backslash{"backslash", EKeyboardKey::backslash, EInputValueType::digital};
    const InputKey InputKey::right_bracket{"right_bracket", EKeyboardKey::right_bracket, EInputValueType::digital};
    const InputKey InputKey::apostrophe{"apostrophe", EKeyboardKey::apostrophe, EInputValueType::digital};
    const InputKey InputKey::ampersand{"ampersand", EKeyboardKey::ampersand, EInputValueType::digital};
    const InputKey InputKey::caret{"caret", EKeyboardKey::caret, EInputValueType::digital};
    const InputKey InputKey::colon{"colon", EKeyboardKey::colon, EInputValueType::digital};
    const InputKey InputKey::dollar{"dollar", EKeyboardKey::dollar, EInputValueType::digital};
    const InputKey InputKey::exclamation{"exclamation", EKeyboardKey::exclamation, EInputValueType::digital};
    const InputKey InputKey::left_parenthesis{"left_parenthesis", EKeyboardKey::left_parenthesis, EInputValueType::digital};
    const InputKey InputKey::right_parenthesis{"right_parenthesis", EKeyboardKey::right_parenthesis, EInputValueType::digital};
    const InputKey InputKey::quote{"quote", EKeyboardKey::quote, EInputValueType::digital};
    const InputKey InputKey::gamepad_left_2d{"gamepad_left2d", EGamepadAxis::left_stick, EInputValueType::axis_2d};
    const InputKey InputKey::gamepad_right_2d{"gamepad_right2d", EGamepadAxis::right_stick, EInputValueType::axis_2d};
    const InputKey InputKey::gamepad_left_trigger_axis{"gamepad_left_trigger_axis", EGamepadAxis::left_trigger, EInputValueType::axis_1d};
    const InputKey InputKey::gamepad_right_trigger_axis{"gamepad_right_trigger_axis", EGamepadAxis::right_trigger, EInputValueType::axis_1d};
    const InputKey InputKey::gamepad_left_thumbstick{"gamepad_left_thumbstick", EGamepadButton::left_thumb, EInputValueType::digital};
    const InputKey InputKey::gamepad_right_thumbstick{"gamepad_right_thumbstick", EGamepadButton::right_thumb, EInputValueType::digital};
    const InputKey InputKey::gamepad_face_button_bottom{"gamepad_face_button_bottom", EGamepadButton::button0, EInputValueType::digital};
    const InputKey InputKey::gamepad_face_button_right{"gamepad_face_button_right", EGamepadButton::button1, EInputValueType::digital};
    const InputKey InputKey::gamepad_face_button_left{"gamepad_face_button_left", EGamepadButton::button2, EInputValueType::digital};
    const InputKey InputKey::gamepad_face_button_top{"gamepad_face_button_top", EGamepadButton::button3, EInputValueType::digital};
    const InputKey InputKey::gamepad_left_shoulder{"gamepad_left_shoulder", EGamepadButton::left_shoulder, EInputValueType::digital};
    const InputKey InputKey::gamepad_right_shoulder{"gamepad_right_shoulder", EGamepadButton::right_shoulder, EInputValueType::digital};
    const InputKey InputKey::gamepad_dpad_up{"gamepad_d_pad_up", EGamepadButton::dpad_up, EInputValueType::digital};
    const InputKey InputKey::gamepad_dpad_down{"gamepad_d_pad_down", EGamepadButton::dpad_down, EInputValueType::digital};
    const InputKey InputKey::gamepad_dpad_right{"gamepad_d_pad_right", EGamepadButton::dpad_right, EInputValueType::digital};
    const InputKey InputKey::gamepad_dpad_left{"gamepad_d_pad_left", EGamepadButton::dpad_left, EInputValueType::digital};
    const InputKey InputKey::gamepad_start{"gamepad_start", EGamepadButton::start, EInputValueType::digital};
    const InputKey InputKey::gamepad_back{"gamepad_back", EGamepadButton::back, EInputValueType::digital};
}
