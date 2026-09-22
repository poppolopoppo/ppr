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

            // quit the application in 3 seconds to debug shutdown for agents
#if 0
            getTimerManager().schedule(*m_started_at + std::chrono::seconds(3u), [](TimePoint) noexcept -> std::error_code {
                return make_error_code(std::errc::timed_out);
            });
#endif

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

            PPR_RETURN_ERROR_ON_FAIL(Demo, super_t::shutdown());

            return default_value_v;
        }
    };
}

int main(const int argc, char *argv[]) {
    demo::TurboLarbin app("ppr", std::span(&argv[0], pP::checked_cast<std::size_t>(argc)));
    const std::error_code err = app.run();
    return err.value();
}
