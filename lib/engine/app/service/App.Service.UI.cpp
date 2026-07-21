module;

#include "pP/Macros.h"
#include "App.Service.UI.imgui.hpp"
#include <imgui_internal.h>

module engine.app;

import :service.ui;
import :service.input;
import :service.window;
import :window.handle;
import std;
import engine.core;
import engine.math;
import engine.rhi;

namespace pP::ui {
    PPR_DEFINE_LOG_CATEGORY(UI, info, none)

#if PPR_ENABLE_ASSERTIONS
    void imGuiAssertFailure(const char *message, const std::source_location &site) {
        Assertion::onFailure(Assertion::require, message, site);
    }
#endif

#if PPR_ENABLE_LOGGING
    void imGuiDebugPrintf(const char *format, const char *buffer) {
        PPR_ASSERT(std::string_view(format) == "%s");
        PPR_ASSERT(buffer != nullptr);
        PPR_LOG_RAW(UI, debug, buffer);
    }
#endif

    void setupImGuiErrorCallback(ImGuiContext *ctx) noexcept {
        ctx->ErrorCallback = [](ImGuiContext *, void *, const char *msg) {
            PPR_LOG_RAW(UI, error, msg);
        };
        ctx->ErrorCallbackUserData = nullptr;
    }

    namespace {
        constexpr std::string_view kImGuiShader = R"(
struct VsInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
    float4 col : COLOR0;
};
struct PsInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};
struct ProjConstants {
    float4x4 m_matrix;
};
[[vk::binding(0, 0)]] ConstantBuffer<ProjConstants> g_proj;
[[vk::binding(1, 0)]] Texture2D g_fontTexture;
[[vk::binding(2, 0)]] SamplerState g_fontSampler;
[shader("vertex")]
PsInput vertexMain(VsInput input) {
    PsInput output;
    output.pos = mul(g_proj.m_matrix, float4(input.pos, 0.0, 1.0));
    output.uv = input.uv;
    output.col = input.col;
    return output;
}
[shader("fragment")]
float4 fragmentMain(PsInput input) : SV_Target {
    return g_fontTexture.Sample(g_fontSampler, input.uv) * input.col;
}
)";

        struct FrameResources {
            rhi::ComPtr<rhi::IBuffer> m_vertex_buffer;
            rhi::ComPtr<rhi::IBuffer> m_index_buffer;
            u32 m_vertex_buffer_capacity{0};
            u32 m_index_buffer_capacity{0};
        };

        [[nodiscard]] ImGuiKey keyboardKeyToImGuiKey(const EKeyboardKey key) noexcept {
            switch (key) {
                case EKeyboardKey::tab: return ImGuiKey_Tab;
                case EKeyboardKey::left_arrow: return ImGuiKey_LeftArrow;
                case EKeyboardKey::right_arrow: return ImGuiKey_RightArrow;
                case EKeyboardKey::up_arrow: return ImGuiKey_UpArrow;
                case EKeyboardKey::down_arrow: return ImGuiKey_DownArrow;
                case EKeyboardKey::page_up: return ImGuiKey_PageUp;
                case EKeyboardKey::page_down: return ImGuiKey_PageDown;
                case EKeyboardKey::home: return ImGuiKey_Home;
                case EKeyboardKey::end: return ImGuiKey_End;
                case EKeyboardKey::insert: return ImGuiKey_Insert;
                case EKeyboardKey::delete_: return ImGuiKey_Delete;
                case EKeyboardKey::backspace: return ImGuiKey_Backspace;
                case EKeyboardKey::space: return ImGuiKey_Space;
                case EKeyboardKey::enter: return ImGuiKey_Enter;
                case EKeyboardKey::escape: return ImGuiKey_Escape;
                case EKeyboardKey::left_control: return ImGuiKey_LeftCtrl;
                case EKeyboardKey::left_shift: return ImGuiKey_LeftShift;
                case EKeyboardKey::left_alt: return ImGuiKey_LeftAlt;
                case EKeyboardKey::left_super: return ImGuiKey_LeftSuper;
                case EKeyboardKey::right_control: return ImGuiKey_RightCtrl;
                case EKeyboardKey::right_shift: return ImGuiKey_RightShift;
                case EKeyboardKey::right_alt: return ImGuiKey_RightAlt;
                case EKeyboardKey::right_super: return ImGuiKey_RightSuper;
                case EKeyboardKey::num_lock: return ImGuiKey_NumLock;
                case EKeyboardKey::caps_lock: return ImGuiKey_CapsLock;
                case EKeyboardKey::scroll_lock: return ImGuiKey_ScrollLock;
                case EKeyboardKey::pause: return ImGuiKey_Pause;
                case EKeyboardKey::print_screen: return ImGuiKey_PrintScreen;
                case EKeyboardKey::f1: return ImGuiKey_F1;
                case EKeyboardKey::f2: return ImGuiKey_F2;
                case EKeyboardKey::f3: return ImGuiKey_F3;
                case EKeyboardKey::f4: return ImGuiKey_F4;
                case EKeyboardKey::f5: return ImGuiKey_F5;
                case EKeyboardKey::f6: return ImGuiKey_F6;
                case EKeyboardKey::f7: return ImGuiKey_F7;
                case EKeyboardKey::f8: return ImGuiKey_F8;
                case EKeyboardKey::f9: return ImGuiKey_F9;
                case EKeyboardKey::f10: return ImGuiKey_F10;
                case EKeyboardKey::f11: return ImGuiKey_F11;
                case EKeyboardKey::f12: return ImGuiKey_F12;
                case EKeyboardKey::zero: return ImGuiKey_0;
                case EKeyboardKey::one: return ImGuiKey_1;
                case EKeyboardKey::two: return ImGuiKey_2;
                case EKeyboardKey::three: return ImGuiKey_3;
                case EKeyboardKey::four: return ImGuiKey_4;
                case EKeyboardKey::five: return ImGuiKey_5;
                case EKeyboardKey::six: return ImGuiKey_6;
                case EKeyboardKey::seven: return ImGuiKey_7;
                case EKeyboardKey::eight: return ImGuiKey_8;
                case EKeyboardKey::nine: return ImGuiKey_9;
                case EKeyboardKey::a: return ImGuiKey_A;
                case EKeyboardKey::b: return ImGuiKey_B;
                case EKeyboardKey::c: return ImGuiKey_C;
                case EKeyboardKey::d: return ImGuiKey_D;
                case EKeyboardKey::e: return ImGuiKey_E;
                case EKeyboardKey::f: return ImGuiKey_F;
                case EKeyboardKey::g: return ImGuiKey_G;
                case EKeyboardKey::h: return ImGuiKey_H;
                case EKeyboardKey::i: return ImGuiKey_I;
                case EKeyboardKey::j: return ImGuiKey_J;
                case EKeyboardKey::k: return ImGuiKey_K;
                case EKeyboardKey::l: return ImGuiKey_L;
                case EKeyboardKey::m: return ImGuiKey_M;
                case EKeyboardKey::n: return ImGuiKey_N;
                case EKeyboardKey::o: return ImGuiKey_O;
                case EKeyboardKey::p: return ImGuiKey_P;
                case EKeyboardKey::q: return ImGuiKey_Q;
                case EKeyboardKey::r: return ImGuiKey_R;
                case EKeyboardKey::s: return ImGuiKey_S;
                case EKeyboardKey::t: return ImGuiKey_T;
                case EKeyboardKey::u: return ImGuiKey_U;
                case EKeyboardKey::v: return ImGuiKey_V;
                case EKeyboardKey::w: return ImGuiKey_W;
                case EKeyboardKey::x: return ImGuiKey_X;
                case EKeyboardKey::y: return ImGuiKey_Y;
                case EKeyboardKey::z: return ImGuiKey_Z;
                case EKeyboardKey::numpad0: return ImGuiKey_Keypad0;
                case EKeyboardKey::numpad1: return ImGuiKey_Keypad1;
                case EKeyboardKey::numpad2: return ImGuiKey_Keypad2;
                case EKeyboardKey::numpad3: return ImGuiKey_Keypad3;
                case EKeyboardKey::numpad4: return ImGuiKey_Keypad4;
                case EKeyboardKey::numpad5: return ImGuiKey_Keypad5;
                case EKeyboardKey::numpad6: return ImGuiKey_Keypad6;
                case EKeyboardKey::numpad7: return ImGuiKey_Keypad7;
                case EKeyboardKey::numpad8: return ImGuiKey_Keypad8;
                case EKeyboardKey::numpad9: return ImGuiKey_Keypad9;
                case EKeyboardKey::period: return ImGuiKey_Period;
                case EKeyboardKey::comma: return ImGuiKey_Comma;
                case EKeyboardKey::semicolon: return ImGuiKey_Semicolon;
                case EKeyboardKey::slash: return ImGuiKey_Slash;
                case EKeyboardKey::backslash: return ImGuiKey_Backslash;
                case EKeyboardKey::equals: return ImGuiKey_Equal;
                case EKeyboardKey::minus: return ImGuiKey_Minus;
                case EKeyboardKey::left_bracket: return ImGuiKey_LeftBracket;
                case EKeyboardKey::right_bracket: return ImGuiKey_RightBracket;
                case EKeyboardKey::apostrophe: return ImGuiKey_Apostrophe;
                case EKeyboardKey::tilde: return ImGuiKey_GraveAccent;
                default: return ImGuiKey_None;
            }
        }

        [[nodiscard]] int mouseButtonToImGui(const EMouseButton button) noexcept {
            switch (button) {
                case EMouseButton::left: return 0;
                case EMouseButton::right: return 1;
                case EMouseButton::middle: return 2;
                case EMouseButton::thumb0: return 3;
                case EMouseButton::thumb1: return 4;
                default: return -1;
            }
        }

        constexpr std::array<EMouseButton, 3> kPolledMouseButtons{
            EMouseButton::left, EMouseButton::right, EMouseButton::middle
        };
    }

    class ImGuiService final : public IUIService {
    public:
        ImGuiService() noexcept = default;

        ~ImGuiService() noexcept override {
            if (m_imgui_context) {
                shutdown();
            }
        }

        std::error_code initialize(
            IRhiService &rhi,
            IWindowService &window_service,
            IInputService &input_service,
            const Window &main_window,
            const rhi::Format swapchain_format) override {
            m_window_service = safe_ptr(&window_service);
            m_input_service = safe_ptr(&input_service);
            m_main_window = safe_ptr(&main_window);
            m_device = &rhi.getDevice();
            m_swapchain_format = swapchain_format;

            rhi::IDevice &device = *m_device;

            // Create ImGui context
            m_imgui_context = ImGui::CreateContext();
            if (not m_imgui_context) {
                PPR_LOG(UI, error, "failed to create ImGui context");
                return std::make_error_code(std::errc::not_supported);
            }
            ImGui::SetCurrentContext(m_imgui_context);

            // Register runtime error callback for ImGui recoverable errors
            setupImGuiErrorCallback(m_imgui_context);

            ImGuiIO &io = ImGui::GetIO();
            io.ConfigErrorRecovery = true;
            io.ConfigErrorRecoveryEnableAssert = true;
            io.ConfigErrorRecoveryEnableDebugLog = true;
            io.ConfigErrorRecoveryEnableTooltip = false;

            io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
            io.BackendPlatformName = "pP_IUIService";
            io.BackendRendererName = "pP_SlangRHI";

            // Get graphics queue
            PPR_RETURN_ON_FAIL(UI, rhi::result(m_device->getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            m_framebuffer_size = main_window.m_framebuffer_size;

            // Compile ImGui shaders
            {
                rhi::ComPtr<shader::ISession> slangSession = device.getSlangSession();
                if (not slangSession) {
                    PPR_LOG(UI, error, "failed to get Slang session from device");
                    return rhi::errc::not_available;
                }

                shader::Diagnose diagnostics;
                const shader::ComPtr<shader::IModule> module(
                    slangSession->loadModuleFromSourceString(
                        "imgui",
                        "imgui.slang",
                        kImGuiShader.data(),
                        diagnostics.writeRef()));

                if (not module) {
                    return shader::errc::cannot_open;
                }

                shader::ComPtr<shader::IEntryPoint> vertex_ep;
                PPR_RETURN_ERROR_ON_FAIL(UI, shader::result(module->findEntryPointByName("vertexMain", vertex_ep.writeRef())));

                shader::ComPtr<shader::IEntryPoint> fragment_ep;
                PPR_RETURN_ERROR_ON_FAIL(UI, shader::result(module->findEntryPointByName("fragmentMain", fragment_ep.writeRef())));

                shader::IComponentType *entryPoints[] = {vertex_ep, fragment_ep};

                rhi::ShaderProgramDesc program_desc{};
                program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
                program_desc.slangGlobalScope = module;
                program_desc.slangEntryPoints = entryPoints;
                program_desc.slangEntryPointCount = std::size(entryPoints);

                PPR_RETURN_ERROR_ON_FAIL(UI, shader::result(device.createShaderProgram(program_desc, m_program.writeRef(), diagnostics.writeRef())));
            }

            // Create input layout
            {
                rhi::InputElementDesc elements[] = {
                    {"POSITION", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, pos), 0},
                    {"TEXCOORD", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, uv), 0},
                    {"COLOR", 0, rhi::Format::RGBA8Unorm, PPR_OFFSETOF(ImDrawVert, col), 0},
                };

                PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createInputLayout(
                    safe_narrowing(sizeof(ImDrawVert)),
                    elements,
                    3u,
                    m_input_layout.writeRef())));
            }

            // Create font sampler
            {
                rhi::SamplerDesc sampler_desc{};
                sampler_desc.minFilter = rhi::TextureFilteringMode::Linear;
                sampler_desc.magFilter = rhi::TextureFilteringMode::Linear;
                sampler_desc.mipFilter = rhi::TextureFilteringMode::Linear;
                sampler_desc.addressU = rhi::TextureAddressingMode::ClampToEdge;
                sampler_desc.addressV = rhi::TextureAddressingMode::ClampToEdge;
                sampler_desc.addressW = rhi::TextureAddressingMode::ClampToEdge;
                sampler_desc.maxAnisotropy = 1;

                PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createSampler(sampler_desc, m_font_sampler.writeRef())));
            }

            // Create font texture
            {
                const std::error_code ft_err = createFontTexture_();
                if (ft_err) {
                    return ft_err;
                }
            }

            // Create render pipeline
            {
                rhi::ColorTargetDesc color_target{};
                color_target.format = m_swapchain_format;
                color_target.enableBlend = true;
                color_target.color.srcFactor = rhi::BlendFactor::SrcAlpha;
                color_target.color.dstFactor = rhi::BlendFactor::InvSrcAlpha;
                color_target.color.op = rhi::BlendOp::Add;
                // Alpha channel must follow the same One/InvSrcAlpha convention every official
                // ImGui backend uses (confirmed against imgui_impl_vulkan.cpp/imgui_impl_dx12.cpp):
                // srcAlpha=One, dstAlpha=InvSrcAlpha. The previous InvSrcAlpha/Zero pairing left
                // the alpha channel of any render target ImGui draws into with the wrong coverage,
                // which only shows up once you composite that target elsewhere (editor viewports,
                // render-to-texture panels, etc.) rather than a plain opaque swapchain.
                color_target.alpha.srcFactor = rhi::BlendFactor::One;
                color_target.alpha.dstFactor = rhi::BlendFactor::InvSrcAlpha;
                color_target.alpha.op = rhi::BlendOp::Add;
                color_target.writeMask = rhi::RenderTargetWriteMask::All;

                rhi::RenderPipelineDesc pipeline_desc{};
                pipeline_desc.program = m_program.get();
                pipeline_desc.inputLayout = m_input_layout.get();
                pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
                pipeline_desc.targets = &color_target;
                pipeline_desc.targetCount = 1u;
                pipeline_desc.rasterizer.cullMode = rhi::CullMode::None;
                pipeline_desc.rasterizer.scissorEnable = true;
                pipeline_desc.depthStencil.depthTestEnable = false;
                pipeline_desc.depthStencil.depthWriteEnable = false;
                pipeline_desc.label = "imgui pipeline";

                PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createRenderPipeline(pipeline_desc, m_pipeline.writeRef())));
            }

            PPR_LOG(UI, info, "UI service initialized", {
                    {"width", m_framebuffer_size.x},
                    {"height", m_framebuffer_size.y},
            });

            return {};
        }

        std::error_code newFrame(const TimeSpan dt) override {
            ImGui::SetCurrentContext(m_imgui_context);
            ImGuiIO &io = ImGui::GetIO();

            // Display size and framebuffer scale. DisplaySize is already given to us in
            // framebuffer pixels (see m_framebuffer_size below), so the scale factor between
            // "logical" and "physical" coordinates is 1:1 here — set it explicitly rather than
            // relying on ImGui's default so renderOverlay()'s scissor-rect math (which now also
            // multiplies by this factor) stays correct if a logical/physical split is introduced
            // later.
            io.DisplaySize = ImVec2(
                static_cast<float>(m_framebuffer_size.x),
                static_cast<float>(m_framebuffer_size.y));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            io.DeltaTime = static_cast<float>(
                std::chrono::duration<double>(dt).count());

            // Input: poll from IInputService
            if (m_input_service.isValid()) [[likely]] {
                pollInputState_();
            }

            ImGui::NewFrame();

            static bool g_show_demo_window{true};
            ImGui::ShowDemoWindow(&g_show_demo_window);

            return default_value_v;
        }

        std::error_code renderOverlay(rhi::IRenderPassEncoder &pass) override {
            ImGui::SetCurrentContext(m_imgui_context);
            ImGuiIO &io = ImGui::GetIO();

            ImGui::Render();
            auto *drawData = ImGui::GetDrawData();
            if (not drawData or not drawData->Valid) {
                return default_value_v;
            }

            const float fb_width = io.DisplaySize.x * io.DisplayFramebufferScale.x;
            const float fb_height = io.DisplaySize.y * io.DisplayFramebufferScale.y;
            if (fb_width <= 0 || fb_height <= 0) {
                return default_value_v;
            }

            // Upload vertex/index data for this frame
            auto *const fr = &m_frame_resources[m_current_frame];
            PPR_RETURN_ON_FAIL(UI, uploadDrawData_(drawData, fr));

            // Bind pipeline
            auto *const root_obj = pass.bindPipeline(m_pipeline.get());
            if (not root_obj) {
                return default_value_v;
            }

            // Set up bindings via ShaderCursor (auto-resolves binding range indices)
            rhi::ShaderCursor root_cursor(root_obj);

            // Set projection matrix (orthographic)
            {
                const float L = drawData->DisplayPos.x;
                const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
                const float T = drawData->DisplayPos.y;
                const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;

                const float4x4 orthographic = float4x4::orthoD3D(L, R, B, T, -1.0f, 1.0f);
                RHI_RETURN_ERROR_ON_FAIL(UI, root_cursor["g_proj"].setData(orthographic.data(), sizeof(orthographic)));
            }

            // Bind font texture and sampler
            {
                auto *textureView = m_fontTextureView.get();
                auto *sampler = m_font_sampler.get();
                if (textureView) {
                    RHI_RETURN_ERROR_ON_FAIL(UI, root_cursor["g_fontTexture"].setBinding(rhi::Binding(textureView)));
                }
                if (sampler) {
                    RHI_RETURN_ERROR_ON_FAIL(UI, root_cursor["g_fontSampler"].setBinding(rhi::Binding(sampler)));
                }
            }

            // Render state fields that stay constant across every draw call in this pass —
            // hoisted out of the loop below so only the scissor rect is rebuilt per command.
            rhi::RenderState render_state{};
            render_state.viewports[0] = rhi::Viewport::fromSize(fb_width, fb_height);
            render_state.viewportCount = 1;
            render_state.vertexBuffers[0] = rhi::BufferOffsetPair(fr->m_vertex_buffer.get(), 0);
            render_state.vertexBufferCount = 1;
            render_state.indexBuffer = rhi::BufferOffsetPair(fr->m_index_buffer.get(), 0);
            render_state.indexFormat = sizeof(ImDrawIdx) == 2
                                    ? rhi::IndexFormat::Uint16
                                    : rhi::IndexFormat::Uint32;
            render_state.scissorRectCount = 1;

            // clip_off/clip_scale convert an ImDrawCmd's ClipRect (display coordinates) into
            // framebuffer pixels, matching every official ImGui backend's RenderDrawData.
            const ImVec2 clip_off = drawData->DisplayPos;
            const ImVec2 clip_scale = io.DisplayFramebufferScale;

            // Render all draw lists. Vertex/index data for every ImDrawList was concatenated
            // back-to-back into one shared buffer pair by uploadDrawData_ below, so each draw
            // must add the running base offset of all prior lists on top of the command's own
            // list-local VtxOffset/IdxOffset — otherwise every list after the first reads from
            // the wrong place in the buffer. The two accumulators below previously were computed
            // and never actually applied to the draw arguments.
            u32 globalVtxOffset = 0;
            u32 globalIdxOffset = 0;
            for (int n = 0; n < drawData->CmdListsCount; n++) {
                const auto *cmdList = drawData->CmdLists[n];

                for (int i = 0; i < cmdList->CmdBuffer.Size; i++) {
                    const auto *cmd = &cmdList->CmdBuffer[i];

                    if (cmd->UserCallback) {
                        cmd->UserCallback(cmdList, cmd);
                        continue;
                    }

                    // Clip rect in framebuffer pixels, clamped to the framebuffer bounds. The
                    // clamp to >= 0 is required, not cosmetic: ClipRect can legitimately extend
                    // past the top/left edge of the display (a window dragged partially
                    // off-screen, a scrolled child region, etc.), and casting a negative float
                    // straight to uint32_t wraps around to a huge value, corrupting the scissor
                    // rect instead of just clipping it away.
                    ImVec2 clip_min{
                        (cmd->ClipRect.x - clip_off.x) * clip_scale.x,
                        (cmd->ClipRect.y - clip_off.y) * clip_scale.y
                    };
                    ImVec2 clip_max{
                        (cmd->ClipRect.z - clip_off.x) * clip_scale.x,
                        (cmd->ClipRect.w - clip_off.y) * clip_scale.y
                    };
                    clip_min.x = std::max(clip_min.x, 0.0f);
                    clip_min.y = std::max(clip_min.y, 0.0f);
                    clip_max.x = std::min(clip_max.x, fb_width);
                    clip_max.y = std::min(clip_max.y, fb_height);

                    if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
                        continue;
                    }

                    render_state.scissorRects[0] = rhi::ScissorRect{
                        static_cast<u32>(clip_min.x),
                        static_cast<u32>(clip_min.y),
                        static_cast<u32>(clip_max.x),
                        static_cast<u32>(clip_max.y)
                    };
                    pass.setRenderState(render_state);

                    rhi::DrawArguments draw_arguments{};
                    draw_arguments.vertexCount = cmd->ElemCount;
                    draw_arguments.instanceCount = 1;
                    draw_arguments.startIndexLocation = cmd->IdxOffset + globalIdxOffset;
                    draw_arguments.startVertexLocation = static_cast<i32>(cmd->VtxOffset + globalVtxOffset);
                    draw_arguments.startInstanceLocation = 0;
                    pass.drawIndexed(draw_arguments);
                }

                globalVtxOffset += cmdList->VtxBuffer.Size;
                globalIdxOffset += cmdList->IdxBuffer.Size;
            }

            // Advance to next frame resource
            m_current_frame = (m_current_frame + 1) % kFrameCount;
            return default_value_v;
        }

        std::error_code shutdown() noexcept override {
            if (m_queue) {
                m_queue->waitOnHost();
            }

            for (auto &fr: m_frame_resources) {
                fr.m_vertex_buffer.setNull();
                fr.m_index_buffer.setNull();
                fr.m_vertex_buffer_capacity = 0;
                fr.m_index_buffer_capacity = 0;
            }

            m_fontTexture.setNull();
            m_fontTextureView.setNull();
            m_font_sampler.setNull();
            m_pipeline.setNull();
            m_input_layout.setNull();
            m_program.setNull();

            m_queue.setNull();

            if (m_imgui_context) {
                ImGui::DestroyContext(m_imgui_context);
                m_imgui_context = nullptr;
            }

            m_input_service = nullptr;
            m_window_service = nullptr;
            m_main_window = nullptr;

            PPR_LOG(UI, info, "UI service shut down");
            return default_value_v;
        }

        std::error_code onResize(const int2 new_size) override {
            if (new_size.x <= 0 || new_size.y <= 0) {
                return default_value_v;
            }

            m_framebuffer_size = new_size;
            PPR_LOG(UI, info, "UI surface resize", {
                    {"width", new_size.x},
                    {"height", new_size.y},
            });
            return default_value_v;
        }

        [[nodiscard]] void *getContext() const noexcept override {
            return m_imgui_context;
        }

    private:
        static constexpr u32 kFrameCount = 2;
        static constexpr u32 kInitialBufferSize = 16384;

        rhi::Format m_swapchain_format{rhi::Format::Undefined};
        ImGuiContext *m_imgui_context{nullptr};
        rhi::ComPtr<rhi::IDevice> m_device;
        rhi::ComPtr<rhi::ICommandQueue> m_queue;
        rhi::ComPtr<rhi::IRenderPipeline> m_pipeline;
        rhi::ComPtr<rhi::IInputLayout> m_input_layout;
        rhi::ComPtr<rhi::IShaderProgram> m_program;
        rhi::ComPtr<rhi::ITexture> m_fontTexture;
        rhi::ComPtr<rhi::ITextureView> m_fontTextureView;
        rhi::ComPtr<rhi::ISampler> m_font_sampler;
        int2 m_framebuffer_size{};
        u32 m_current_frame{0};
        FrameResources m_frame_resources[kFrameCount]{};
        safe_ptr<IInputService> m_input_service;
        safe_ptr<IWindowService> m_window_service;
        safe_ptr<const Window> m_main_window;

        [[nodiscard]] std::error_code createFontTexture_() {
            ImGuiIO &io = ImGui::GetIO();

            u8 *pixels = nullptr;
            int width = 0, height = 0, bpp = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);

            rhi::IDevice &device = *m_device;

            // Create font texture
            rhi::TextureDesc texture_desc{};
            texture_desc.type = rhi::TextureType::Texture2D;
            texture_desc.size = {static_cast<u32>(width), static_cast<u32>(height), 1};
            texture_desc.arrayLength = 1;
            texture_desc.mipCount = 1;
            texture_desc.format = rhi::Format::RGBA8Unorm;
            texture_desc.memoryType = rhi::MemoryType::DeviceLocal;
            texture_desc.usage = enumCombine(rhi::TextureUsage::ShaderResource, rhi::TextureUsage::CopyDestination);
            texture_desc.defaultState = rhi::ResourceState::CopyDestination;
            texture_desc.label = "imgui font atlas";

            rhi::ComPtr<rhi::ITexture> texture;
            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createTexture(texture_desc, nullptr, texture.writeRef())));

            // Create staging buffer for upload
            const u64 pixelDataSize = static_cast<u64>(width) * height * 4;
            rhi::BufferDesc staging_desc{};
            staging_desc.size = pixelDataSize;
            staging_desc.usage = rhi::BufferUsage::CopySource;
            staging_desc.memoryType = rhi::MemoryType::Upload;
            staging_desc.defaultState = rhi::ResourceState::General;
            staging_desc.label = "imgui font staging";

            rhi::ComPtr<rhi::IBuffer> staging_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createBuffer(staging_desc, pixels, staging_buffer.writeRef())));

            // Copy staging buffer to texture via a temporary encoder
            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(m_queue->createCommandEncoder(encoder.writeRef())));

            rhi::Offset3D dst_offset{0, 0, 0};
            encoder->copyBufferToTexture(
                texture.get(), 0, 0, dst_offset,
                staging_buffer.get(), 0, pixelDataSize,
                static_cast<u32>(pixelDataSize / height), // row pitch
                {static_cast<u32>(width), static_cast<u32>(height), 1});

            encoder->setTextureState(texture.get(), rhi::ResourceState::ShaderResource);

            rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(encoder->finish(cmd_buffer.writeRef())));

            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(m_queue->submit(cmd_buffer.get())));

            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(m_queue->waitOnHost()));

            staging_buffer.setNull();

            // Create texture view
            rhi::ComPtr<rhi::ITextureView> view;
            PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(texture->getDefaultView(view.writeRef())));

            m_fontTexture = std::move(texture);
            m_fontTextureView = std::move(view);

            // Store texture ID for ImGui
            io.Fonts->SetTexID(m_fontTextureView.get());

            PPR_LOG(UI, info, "font texture created", {
                {"width", width},
                {"height", height},
            });

            return default_value_v;
        }

        void pollInputState_() {
            ImGuiIO &io = ImGui::GetIO();
            const KeyboardState &kbd = m_input_service->getKeyboard();
            const MouseState &mouse = m_input_service->getMouse();

            // Process mouse position
            {
                const float2 cursor{
                    mouse.m_cursor_pos.m_raw.m_absolute.x,
                    mouse.m_cursor_pos.m_raw.m_absolute.y,
                };
                if (cursor.x >= 0 && cursor.y >= 0) {
                    io.AddMousePosEvent(cursor.x, cursor.y);
                }
            }

            // Process mouse wheel
            {
                const float wheel_y = mouse.m_wheel_y.m_raw.m_relative;
                const float wheel_x = mouse.m_wheel_x.m_raw.m_relative;
                if (wheel_y != 0.0f || wheel_x != 0.0f) {
                    io.AddMouseWheelEvent(wheel_x, wheel_y);
                }
            }

            // Process mouse buttons. The previous version built this list inline as
            // { std::pair{EMouseButton::left, 0}, {EMouseButton::right, 1}, {EMouseButton::middle, 2} }
            // — only the first element spells out std::pair explicitly, so the range-for's
            // std::initializer_list<std::pair<EMouseButton,int>> gets deduced from that one
            // element while the bare-brace elements list-initialize against it. That's not
            // guaranteed by the standard the way a same-typed list is, so it's worth not
            // depending on across compilers/toolchain updates. kPolledMouseButtons plus the
            // mouseButtonToImGui() switch above (previously unused) gives the same mapping
            // without relying on that.
            for (const EMouseButton button: kPolledMouseButtons) {
                const int imgui_button = mouseButtonToImGui(button);
                if (mouse.m_buttons.isPressed(button)) {
                    io.AddMouseButtonEvent(imgui_button, true);
                } else if (mouse.m_buttons.isUp(button)) {
                    io.AddMouseButtonEvent(imgui_button, false);
                }
            }

            // Process keyboard keys
            const auto feed_key = [&](const EKeyboardKey pprKey) {
                const bool down = kbd.m_keys.isDown(pprKey);
                const bool pressed = kbd.m_keys.isPressed(pprKey);
                const bool released = kbd.m_keys.isUp(pprKey);
                if (pressed || released) {
                    const ImGuiKey imguiKey = keyboardKeyToImGuiKey(pprKey);
                    if (imguiKey != ImGuiKey_None) {
                        io.AddKeyEvent(imguiKey, down);
                    }
                }
            };

            // Feed all keyboard keys
            for (u8 k = 0; k < static_cast<u8>(EKeyboardKey::right_super) + 1; k++) {
                feed_key(static_cast<EKeyboardKey>(k));
            }

            // Process text input (characters queued by input system)
            for (const auto &ch: kbd.m_characters) {
                if (ch >= 32 && ch < 0xFFFE) {
                    io.AddInputCharacter(ch);
                }
            }

            // Modifier state
            io.AddKeyEvent(ImGuiMod_Ctrl,
                           kbd.m_keys.isDown(EKeyboardKey::left_control) ||
                           kbd.m_keys.isDown(EKeyboardKey::right_control));
            io.AddKeyEvent(ImGuiMod_Shift,
                           kbd.m_keys.isDown(EKeyboardKey::left_shift) ||
                           kbd.m_keys.isDown(EKeyboardKey::right_shift));
            io.AddKeyEvent(ImGuiMod_Alt,
                           kbd.m_keys.isDown(EKeyboardKey::left_alt) ||
                           kbd.m_keys.isDown(EKeyboardKey::right_alt));
            io.AddKeyEvent(ImGuiMod_Super,
                           kbd.m_keys.isDown(EKeyboardKey::left_super) ||
                           kbd.m_keys.isDown(EKeyboardKey::right_super));
        }

        [[nodiscard]] std::error_code uploadDrawData_(const ImDrawData *drawData, FrameResources *fr) {
            const u32 total_vtx_count = drawData->TotalVtxCount;
            const u32 total_idx_count = drawData->TotalIdxCount;

            if (total_vtx_count == 0 || total_idx_count == 0) {
                return default_value_v;
            }

            rhi::IDevice &device = *m_device;

            // Grow vertex buffer if needed
            if (not fr->m_vertex_buffer || fr->m_vertex_buffer_capacity < total_vtx_count) {
                const u32 new_capacity = total_vtx_count + 8192;
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawVert);

                rhi::BufferDesc vb_desc{};
                vb_desc.size = size_bytes;
                vb_desc.usage = enumCombine(rhi::BufferUsage::VertexBuffer, rhi::BufferUsage::CopySource);
                vb_desc.memoryType = rhi::MemoryType::Upload;
                vb_desc.defaultState = rhi::ResourceState::General;
                vb_desc.label = "imgui vertex buffer";

                PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createBuffer(vb_desc, nullptr, fr->m_vertex_buffer.writeRef())));
                fr->m_vertex_buffer_capacity = new_capacity;
            }

            // Grow index buffer if needed
            if (not fr->m_index_buffer || fr->m_index_buffer_capacity < total_idx_count) {
                const u32 new_capacity = total_idx_count + 16384;
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawIdx);

                rhi::BufferDesc ib_desc{};
                ib_desc.size = size_bytes;
                ib_desc.usage = enumCombine(rhi::BufferUsage::IndexBuffer, rhi::BufferUsage::CopySource);
                ib_desc.memoryType = rhi::MemoryType::Upload;
                ib_desc.defaultState = rhi::ResourceState::General;
                ib_desc.label = "imgui index buffer";

                PPR_RETURN_ERROR_ON_FAIL(UI, rhi::result(device.createBuffer(ib_desc, nullptr, fr->m_index_buffer.writeRef())));
                fr->m_index_buffer_capacity = new_capacity;
            }

            // Map and copy vertex data
            {
                void *mapped_vtx = nullptr;
                if (hasFailed(device.mapBuffer(fr->m_vertex_buffer.get(), rhi::CpuAccessMode::Write, &mapped_vtx))) {
                    PPR_LOG(UI, error, "failed to map vertex buffer");
                    return default_value_v;
                }

                void *mapped_idx = nullptr;
                if (hasFailed(device.mapBuffer(fr->m_index_buffer.get(), rhi::CpuAccessMode::Write, &mapped_idx))) {
                    PPR_LOG(UI, error, "failed to map index buffer");
                    device.unmapBuffer(fr->m_vertex_buffer.get());
                    return default_value_v;
                }

                auto *vtx_dst = static_cast<ImDrawVert *>(mapped_vtx);
                auto *idx_dst = static_cast<ImDrawIdx *>(mapped_idx);

                for (int n = 0; n < drawData->CmdListsCount; n++) {
                    const auto *cmd_list = drawData->CmdLists[n];

                    memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
                    memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

                    vtx_dst += cmd_list->VtxBuffer.Size;
                    idx_dst += cmd_list->IdxBuffer.Size;
                }

                device.unmapBuffer(fr->m_vertex_buffer.get());
                device.unmapBuffer(fr->m_index_buffer.get());
            }

            return default_value_v;
        }
    };

    std::unique_ptr<IUIService> createImGuiService() {
        return std::make_unique<ImGuiService>();
    }
}