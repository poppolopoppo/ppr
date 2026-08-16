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
        // Draw callback receives the entry's viewport/scissor and must set the
        // complete RenderState itself, because RenderPassEncoder::setRenderState
        // replaces the whole state on every call (a buffers-only call would wipe
        // the viewport/scissor set by the caller).
        std23::function_ref<std::error_code(rhi::IRenderPassEncoder &, const rhi::Viewport &, const rhi::ScissorRect &)> draw;
    };
}