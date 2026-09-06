module;
#include "pP/UnitTest.h"

export module engine.tests.app:imgui_dpi;

import engine.app;
import engine.core;
import engine.math;
import std;

export namespace pP::tests {
    namespace ImguiDpi {
        [[nodiscard]] constexpr float2 framebufferScaleFor(const int2 logical, const int2 framebuffer) noexcept {
            float2 scale{1.0f};
            if (logical.x > 0) {
                scale.x = static_cast<float>(framebuffer.x) / static_cast<float>(logical.x);
            }
            if (logical.y > 0) {
                scale.y = static_cast<float>(framebuffer.y) / static_cast<float>(logical.y);
            }
            if (not (scale.x > 0.0f)) {
                scale.x = 1.0f;
            }
            if (not (scale.y > 0.0f)) {
                scale.y = 1.0f;
            }
            return scale;
        }

        PPR_UNIT_TEST(scale_2_client_to_framebuffer_mapping) {
            const int2 logical{800, 600};
            const int2 framebuffer{1600, 1200};

            const float2 scale = framebufferScaleFor(logical, framebuffer);
            PPR_TEST_ASSERT(scale.x == 2.0f && scale.y == 2.0f);

            const float2 display_size{static_cast<float>(logical.x), static_cast<float>(logical.y)};
            PPR_TEST_ASSERT(display_size.x * scale.x == static_cast<float>(framebuffer.x));
            PPR_TEST_ASSERT(display_size.y * scale.y == static_cast<float>(framebuffer.y));

            const float2 client{10.0f, 20.0f};
            const float2 fb_pos{client.x * scale.x, client.y * scale.y};
            PPR_TEST_ASSERT(fb_pos.x == 20.0f && fb_pos.y == 40.0f);
        };

        PPR_UNIT_TEST(degenerate_logical_falls_back_to_1) {
            const float2 scale = framebufferScaleFor(int2{0, 0}, int2{1600, 1200});
            PPR_TEST_ASSERT(scale.x == 1.0f && scale.y == 1.0f);
        };
    }

    PPR_UNIT_TEST(imgui_dpi) {
        _.recurse({
            ImguiDpi::scale_2_client_to_framebuffer_mapping,
            ImguiDpi::degenerate_logical_falls_back_to_1,
        });
    };
}
