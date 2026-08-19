module;

#include "pP/Macros.h"

export module engine.app:ui.imgui;

import std;

export import imgui;

export namespace pP {
    class IUIService;
}

export namespace pP::ui {
    [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();
}
