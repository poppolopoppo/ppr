module;
#include "pP/Macros.h"
#include <GLFW/glfw3.h>
module engine.core;
import :input_key;

namespace pP {

InputKey fromGlfwKey(int glfwKey) noexcept {
    using namespace keys;
    switch (glfwKey) {
        case GLFW_KEY_A: return a; case GLFW_KEY_B: return b;
        case GLFW_KEY_C: return c; case GLFW_KEY_D: return d;
        case GLFW_KEY_E: return e; case GLFW_KEY_F: return f;
        case GLFW_KEY_G: return g; case GLFW_KEY_H: return h;
        case GLFW_KEY_I: return i; case GLFW_KEY_J: return j;
        case GLFW_KEY_K: return k; case GLFW_KEY_L: return l;
        case GLFW_KEY_M: return m; case GLFW_KEY_N: return n;
        case GLFW_KEY_O: return o; case GLFW_KEY_P: return p;
        case GLFW_KEY_Q: return q; case GLFW_KEY_R: return r;
        case GLFW_KEY_S: return s; case GLFW_KEY_T: return t;
        case GLFW_KEY_U: return u; case GLFW_KEY_V: return v;
        case GLFW_KEY_W: return w; case GLFW_KEY_X: return x;
        case GLFW_KEY_Y: return y; case GLFW_KEY_Z: return z;
        case GLFW_KEY_0: return num0; case GLFW_KEY_1: return num1;
        case GLFW_KEY_2: return num2; case GLFW_KEY_3: return num3;
        case GLFW_KEY_4: return num4; case GLFW_KEY_5: return num5;
        case GLFW_KEY_6: return num6; case GLFW_KEY_7: return num7;
        case GLFW_KEY_8: return num8; case GLFW_KEY_9: return num9;
        case GLFW_KEY_SPACE: return space;
        case GLFW_KEY_ENTER: return enter;
        case GLFW_KEY_ESCAPE: return escape;
        case GLFW_KEY_TAB: return tab;
        case GLFW_KEY_BACKSPACE: return backspace;
        case GLFW_KEY_LEFT_SHIFT: return leftShift;
        case GLFW_KEY_RIGHT_SHIFT: return rightShift;
        case GLFW_KEY_LEFT_CONTROL: return leftCtrl;
        case GLFW_KEY_RIGHT_CONTROL: return rightCtrl;
        case GLFW_KEY_LEFT_ALT: return leftAlt;
        case GLFW_KEY_RIGHT_ALT: return rightAlt;
        default: return unknown;
    }
}

InputKey fromGlfwMouseButton(int glfwButton) noexcept {
    switch (glfwButton) {
        case GLFW_MOUSE_BUTTON_LEFT: return keys::mouseLeft;
        case GLFW_MOUSE_BUTTON_RIGHT: return keys::mouseRight;
        case GLFW_MOUSE_BUTTON_MIDDLE: return keys::mouseMiddle;
        default: return keys::unknown;
    }
}

} // namespace pP
