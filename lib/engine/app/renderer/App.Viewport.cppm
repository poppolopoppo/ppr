module;

#include "pP/Macros.h"

export module engine.app:viewport;

import engine.core;
import engine.math;
import engine.rhi;
import std;

export namespace pP {
    struct ViewportConfig {
        int2 framebuffer_size{0};
    };

    struct ViewportEntry {
        rhi::ComPtr<rhi::IRenderPipeline> pipeline;
        rhi::Viewport viewport{};
        rhi::ScissorRect scissor{};
        std23::function_ref<std::error_code(rhi::IRenderPassEncoder &)> draw;
    };
}