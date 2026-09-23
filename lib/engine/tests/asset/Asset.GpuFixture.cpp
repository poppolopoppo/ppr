module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.app;
import engine.rhi;
import engine.shader;
import std;

namespace pP::tests::detail::SharedGpu {
    // One headless rendering application shared by every GPU leaf in a group.
    // Plain Application never creates a window; Renderer::renderToTexture and
    // device readback are surface-free, so the window/input services are dead
    // weight here. Each leaf still owns a fresh TrianglePass, so pass caches
    // never leak state between tests.
    constexpr ApplicationDomain kSharedDomain{
        .m_is_headless = true,
        .m_is_interactive = false,
        .m_needs_presence = false,
        .m_needs_rendering = true,
        .m_needs_user_interface = false,
    };

    struct SharedTestApp : Application {
        SharedTestApp()
            : Application(kSharedDomain, "AssetSharedGpu", std::span<const char *const>{}) {
        }

        [[nodiscard]] std::error_code boot() { return Application::initialize(); }
        [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
    };

    struct Holder {
        std::mutex m_mutex{};
        std::unique_ptr<SharedTestApp> m_app{};
        unsigned m_refs = 0u;
    };

    [[nodiscard]] Holder &holder() {
        static Holder s_holder{};
        return s_holder;
    }

    [[nodiscard]] std::error_code acquire() {
        Holder &state = holder();
        const std::lock_guard lock{state.m_mutex};
        if (state.m_app == nullptr) {
            auto app = std::make_unique<SharedTestApp>();
            if (const std::error_code err = app->boot()) {
                return err;
            }
            state.m_app = std::move(app);
        }
        ++state.m_refs;
        return default_value_v;
    }

    [[nodiscard]] std::error_code release() {
        Holder &state = holder();
        const std::lock_guard lock{state.m_mutex};
        if (state.m_app == nullptr) {
            return default_value_v;
        }
        PPR_ASSERT(state.m_refs > 0u);
        if (--state.m_refs == 0u) {
            const std::error_code err = state.m_app->teardown();
            state.m_app.reset();
            return err;
        }
        return default_value_v;
    }

    [[nodiscard]] safe_ptr<IRhiService> rhiService() {
        Holder &state = holder();
        const std::lock_guard lock{state.m_mutex};
        if (state.m_app == nullptr) {
            return {};
        }
        return state.m_app->getServices().get<IRhiService>();
    }

    [[nodiscard]] safe_ptr<IShaderService> shaderService() {
        Holder &state = holder();
        const std::lock_guard lock{state.m_mutex};
        if (state.m_app == nullptr) {
            return {};
        }
        return state.m_app->getServices().get<IShaderService>();
    }

    [[nodiscard]] Renderer *renderer() {
        Holder &state = holder();
        const std::lock_guard lock{state.m_mutex};
        if (state.m_app == nullptr) {
            return nullptr;
        }
        return &state.m_app->getRenderer();
    }
} // namespace pP::tests::detail::SharedGpu
