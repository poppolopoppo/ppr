module;
#include "pP/Macros.h"
#include "App.Service.UI.imgui.hpp"
export module engine.app:service.ui;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    class IInputService;
    class IWindowService;
    class Window;

    class IUIService : public virtual IService {
    public:
        [[nodiscard]] virtual std::error_code initialize(
            IRhiService &rhi,
            IWindowService &window_service,
            IInputService &input_service,
            const Window &main_window,
            rhi::Format swapchain_format) = 0;

        [[nodiscard]] virtual std::error_code shutdown() noexcept = 0;

        [[nodiscard]] virtual std::error_code newFrame(TimeSpan dt) = 0;

        [[nodiscard]] virtual std::error_code renderOverlay(
            rhi::IRenderPassEncoder &pass,
            const float4x4 &projection) = 0;

        [[nodiscard]] virtual std::error_code onResize(int2 new_size) = 0;

        [[nodiscard]] virtual void *getContext() const noexcept = 0;
    };

    namespace ui {
        [[nodiscard]] std::unique_ptr<IUIService> createImGuiService();
    }
}
