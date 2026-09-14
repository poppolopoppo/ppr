#include "pP/Macros.h"

import engine.core;
import engine.math;
import engine.rhi;
import engine.app;
import imgui_internal;
import std;

namespace demo {
    using namespace pP;
    PPR_DEFINE_LOG_CATEGORY(Demo, info, none);

    class TurboLarbin : public ApplicationEditor {
    public:
        using super_t = ApplicationEditor;
        using super_t::super_t;

    protected:
        std::error_code initialize() override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::initialize());

            m_started_at = time::now();

            return default_value_v;
        }

        std::error_code update(const TimeSpan dt) override {
            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::update(dt));

#if PPR_ENABLE_DEBUG
            if (const auto ui = getServices().get<IUIService>(); ui.isValid()) {
                ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ui->getContext()));
                static bool g_show_demo_window{true};
                ImGui::ShowDemoWindow(&g_show_demo_window);
            }
#endif

            return default_value_v;
        }

        std::error_code shutdown() override {
            m_started_at.reset();

            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::shutdown());

            return default_value_v;
        }

    private:
        std::optional<TimePoint> m_started_at{};
    };
}

int main(const int argc, char *argv[]) {
    demo::TurboLarbin app("ppr", std::span(&argv[0], argc));
    const std::error_code err = app.run();
    return err.value();
}
