module;
#include "pP/Macros.h"
module engine.app;

import imgui;
import imgui_internal;

import :input.device;
import :input.key;
import :input.listener;
import :service.input;
import :service.ui;
import :service.window;
import :ui.imgui;
import :window.handle;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(UI, info, none)

    namespace {
        constexpr string_literal kImGuiShader = R"(
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
float2 g_scale;
        float2 g_offset;
        Texture2D g_fontTexture : register(t0, space0);
        SamplerState g_fontSampler : register(s0, space0);
[shader("vertex")]
PsInput vertexMain(VsInput input) {
    PsInput output;
    output.pos = float4(input.pos * g_scale + g_offset, 0.0, 1.0);
    output.uv = input.uv;
    output.col = input.col;
    return output;
}
[shader("fragment")]
float4 fragmentMain(PsInput input) : SV_Target {
    return g_fontTexture.Sample(g_fontSampler, input.uv) * input.col;
}
)";

        void imGuiDebugPrintf(const char *format, const char *buffer) {
            PPR_ASSERT(std::string_view(format) == "%s");
            PPR_ASSERT(buffer != nullptr);
            PPR_LOG_RAW(UI, debug, buffer);
        }

        [[nodiscard]] float2 framebufferScaleFor(const int2 &logical, const int2 &framebuffer) noexcept {
            float2 scale{1.0f};
            if (logical.x > 0) {
                scale.x = static_cast<float>(framebuffer.x) / static_cast<float>(logical.x);
            }
            if (logical.y > 0) {
                scale.y = static_cast<float>(framebuffer.y) / static_cast<float>(logical.y);
            }
            if (not(scale.x > 0.0f)) {
                scale.x = 1.0f;
            }
            if (not(scale.y > 0.0f)) {
                scale.y = 1.0f;
            }
            return scale;
        }

        class ImGuiService final : public IUIService {
            ImGuiContext *m_imgui_context{nullptr};

            /// input handling:
            safe_ptr<InputContext> m_input_context{};

            InputListener m_input_listener{};
            InputMapping m_input_mapping{"ImGuiInputs"};

            InputAction m_input_any_digital{"ImGuiAnyDigital", EInputValueType::digital};

            InputAction m_input_mouse_cursor{"ImGuiMouseCursor", EInputValueType::axis_2d};
            InputAction m_input_mouse_wheel{"ImGuiMouseWheel", EInputValueType::axis_1d};

            InputAction m_input_gamepad_stick{"ImGuiGamepadStick", EInputValueType::axis_2d};
            InputAction m_input_gamepad_trigger{"ImGuiGamepadTrigger", EInputValueType::axis_1d};

            /// render resources:
            rhi::ComPtr<rhi::ISampler> m_font_sampler{};
            rhi::ComPtr<rhi::ITexture> m_font_texture{};
            rhi::ComPtr<rhi::ITextureView> m_font_texture_view{};

            rhi::ComPtr<rhi::IShaderProgram> m_shader_program{};
            rhi::ComPtr<rhi::IInputLayout> m_vertex_layout{};

            struct FrameResources {
                rhi::ComPtr<rhi::IBuffer> m_index_buffer{};
                rhi::ComPtr<rhi::IBuffer> m_vertex_buffer{};
            };

            std::array<FrameResources, 2u> m_frame_resources{};
            std::size_t m_frame_revision{};

            rhi::ComPtr<rhi::IRenderPipeline> m_render_pipeline{};
            std::optional<RenderPipelineKey> m_render_pipeline_key{};

            std::error_code createInvariantRenderState_(rhi::IDevice &device);

            std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device);

            std::error_code createFontTexture_(rhi::IDevice &device, rhi::ICommandQueue &device_queue);

            std::error_code createRenderPipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature);

            std::error_code uploadDrawData_(rhi::IDevice &device, const ImDrawData &draw_data, FrameResources &resources);

            static EInputMessageResponse onInputCharacter_(hal::native::char_t codepoint) noexcept;

            static void onInputAnyDigital_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputMouseCursor_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputMouseWheel_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputGamepadStick_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputGamepadTrigger_(const InputActionEvent &event, const InputKey &trigger) noexcept;

        public:
            ImGuiService() noexcept {
                // TODO: track allocations with a custom scope
                ImGui::SetAllocatorFunctions(
                    [](const std::size_t sz, [[maybe_unused]] void *user_data) -> void * {
                        return std::malloc(sz);
                    },
                    [](void *ptr, [[maybe_unused]] void *user_data) -> void {
                        std::free(ptr);
                    },
                    nullptr
                );

                m_input_any_digital.setStarted(&onInputAnyDigital_);
                m_input_any_digital.setTriggered(&onInputAnyDigital_);
                m_input_any_digital.setCompleted(&onInputAnyDigital_);

                m_input_mouse_cursor.setTriggered(&onInputMouseCursor_);
                m_input_mouse_wheel.setTriggered(&onInputMouseWheel_);

                m_input_gamepad_stick.setTriggered(&onInputGamepadStick_);
                m_input_gamepad_trigger.setTriggered(&onInputGamepadTrigger_);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_any_digital), InputKey::any_digital);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_stick), InputKey::gamepad_left_2d);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_stick), InputKey::gamepad_right_2d);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_trigger), InputKey::gamepad_left_trigger_axis);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_trigger), InputKey::gamepad_right_trigger_axis);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_mouse_cursor), InputKey::mouse_2d);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_mouse_wheel), InputKey::mouse_wheel_axis_x,
                    []([[maybe_unused]] const TimeSpan dt, InputValue &output) noexcept {
                        output = InputValue(std::get<InputAxis1D>(output), InputAxis1D{});
                    });
                m_input_mapping.mapInputKey(safe_ptr(&m_input_mouse_wheel), InputKey::mouse_wheel_axis_y,
                    []([[maybe_unused]] const TimeSpan dt, InputValue &output) noexcept {
                        output = InputValue(InputAxis1D{}, std::get<InputAxis1D>(output));
                    });

                m_input_listener.addInputMapping(&m_input_mapping, 0);
                m_input_listener.setCharacterInputCallback(&onInputCharacter_);
            }

            ~ImGuiService() noexcept override {
                PPR_ASSERT(m_imgui_context == nullptr);

                m_input_listener.clearInputMappings();
                m_input_mapping.clearKeymap();
            }

            [[nodiscard]] void *getContext() const noexcept override {
                return m_imgui_context;
            }

            std::error_code initialize(
                WindowInputContext &window_input_context,
                IRhiService &rhi_service,
                IShaderService &shader_service,
                const int input_listener_priority) override {
                PPR_ASSERT(m_imgui_context == nullptr);

                m_frame_revision = 0u;
                m_imgui_context = ImGui::CreateContext();
                if (not m_imgui_context) {
                    PPR_LOG(UI, error, "failed to create ImGui context");
                    return std::make_error_code(std::errc::not_supported);
                }

                // Route all ImGui recoverable errors through the engine logger.
                // ConfigErrorRecoveryEnableAssert is OFF to prevent ImGui from calling
                // assert() (which fires the CRT dialog). All errors go through the
                // ErrorCallback below, which logs via PPR_LOG_RAW.
                m_imgui_context->ErrorCallback = [](ImGuiContext *, void *, const char *msg) {
                    PPR_LOG_RAW(UI, error, msg);
                };
                m_imgui_context->ErrorCallbackUserData = nullptr;

                ImGui::SetCurrentContext(m_imgui_context);

                ImGuiIO &io = ImGui::GetIO();
                io.ConfigErrorRecovery = true;
                io.ConfigErrorRecoveryEnableAssert = false;
                io.ConfigErrorRecoveryEnableDebugLog = true;
                io.ConfigErrorRecoveryEnableTooltip = static_cast<bool>(PPR_ENABLE_DEBUG);

                io.BackendPlatformName = "pP_IUIService";
                io.BackendRendererName = "pP_SlangRHI";
                io.BackendPlatformUserData = this;

                io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
                io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleFonts;
                io.ConfigFlags |= ImGuiConfigFlags_DpiEnableScaleViewports;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
                io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

                // need to update display size before first call to ImGui::NewFrame()
                io.DisplaySize = ImVec2{
                    static_cast<float>(window_input_context.m_window->m_framebuffer_size.x),
                    static_cast<float>(window_input_context.m_window->m_framebuffer_size.y)
                };

                io.DisplayFramebufferScale = ImVec2{
                    window_input_context.m_window->m_content_scale.x,
                    window_input_context.m_window->m_content_scale.y,
                };

                // attach our input listener to the window:
                m_input_context = &window_input_context.m_context;
                m_input_context->addInputListener(&m_input_listener, input_listener_priority);

                // prepare rendering:
                rhi::IDevice &device = rhi_service.getDevice();
                PPR_RETURN_ERROR_ON_FAIL(UI, createInvariantRenderState_(device));
                PPR_RETURN_ERROR_ON_FAIL(UI, createShaderProgram_(shader_service, device));

                rhi::ComPtr<rhi::ICommandQueue> device_queue{};
                PPR_RETURN_ERROR_ON_FAIL(UI, device.getQueue(rhi::QueueType::Graphics, device_queue.writeRef()));
                PPR_RETURN_ERROR_ON_FAIL(UI, createFontTexture_(device, *device_queue));

                PPR_LOG(UI, info, "UI service initialized", {});
                return default_value_v;
            }

            std::error_code shutdown() override {
                PPR_LOG(UI, info, "UI service shut down");

                if (m_input_context) {
                    m_input_context->removeInputListener(m_input_listener);
                    m_input_context.reset();
                }

                m_render_pipeline.setNull();

                m_font_texture_view.setNull();
                m_font_texture.setNull();
                m_font_sampler.setNull();

                m_vertex_layout.setNull();
                m_shader_program.setNull();

                for (FrameResources &resources: m_frame_resources) {
                    resources.m_index_buffer.setNull();
                    resources.m_vertex_buffer.setNull();
                }

                if (m_imgui_context) {
                    // Detach the backend before destroying the context:
                    // ImGui asserts when Backend*UserData is still set at
                    // DestroyContext ("Forgot to shutdown Platform backend?").
                    ImGui::SetCurrentContext(m_imgui_context);
                    ImGuiIO &io = ImGui::GetIO();
                    io.BackendPlatformName = nullptr;
                    io.BackendRendererName = nullptr;
                    io.BackendPlatformUserData = nullptr;
                    io.BackendRendererUserData = nullptr;
                    io.BackendFlags = ImGuiBackendFlags_None;
                    io.Fonts->SetTexID(nullptr);
                    ImGui::SetCurrentContext(nullptr);
                    ImGui::DestroyContext(m_imgui_context);
                    m_imgui_context = nullptr;
                }

                return default_value_v;
            }

            std::error_code update(const TimeSpan dt, const WindowViewport &viewport) override {
                if (not m_imgui_context) {
                    return std::make_error_code(std::errc::not_connected);
                }
                ImGui::SetCurrentContext(m_imgui_context);
                ImGuiIO &io = ImGui::GetIO();

                io.DeltaTime = static_cast<float>(time::seconds(dt));

                const int2 &display_size = viewport.getViewport().getClientRect().m_extent;
                io.DisplaySize = ImVec2{
                    static_cast<float>(display_size.x),
                    static_cast<float>(display_size.y)
                };

                const float2 &content_scale = viewport.getWindow().m_content_scale;
                io.DisplayFramebufferScale = ImVec2{content_scale.x, content_scale.y};

                ++m_frame_revision;
                ImGui::NewFrame();
                return default_value_v;
            }

            std::error_code render(const DrawContext &draw_context) override {
                if (not m_imgui_context) {
                    return std::make_error_code(std::errc::not_connected);
                }
                ImGui::SetCurrentContext(m_imgui_context);

                ImGui::Render();

                auto *draw_data = ImGui::GetDrawData();
                if (not draw_data or not draw_data->Valid) {
                    return default_value_v;
                }

                if (not m_render_pipeline_key.has_value() or
                    *m_render_pipeline_key != draw_context.m_render_pipeline_key) {
                    PPR_RETURN_ERROR_ON_FAIL(UI, createRenderPipeline_(draw_context.m_device, draw_context.m_render_pipeline_key));
                    m_render_pipeline_key = draw_context.m_render_pipeline_key;
                }

                rhi::ShaderCursor shader_cursor{};
                if (rhi::IShaderObject *const shader_object = draw_context.m_pass.bindPipeline(m_render_pipeline.get()); PPR_ENSURE(shader_object)) {
                    shader_cursor = rhi::ShaderCursor(shader_object);
                } else {
                    return make_error_code(std::errc::broken_pipe);
                }

                const ImGuiIO &io = ImGui::GetIO();
                const float2 imgui_scale{2.0f / io.DisplaySize.x, -2.0f / io.DisplaySize.y};
                const float2 imgui_offset{-1.0f, 1.0f};
                PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_scale"].setData(&imgui_scale, sizeof(float2)));
                PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_offset"].setData(&imgui_offset, sizeof(float2)));
                PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_fontSampler"].setBinding(m_font_sampler));
                PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_fontTexture"].setBinding(m_font_texture_view));

                FrameResources &resources = m_frame_resources[m_frame_revision % 2u];
                PPR_RETURN_ON_FAIL(UI, uploadDrawData_(draw_context.m_device, *draw_data, resources));

                rhi::RenderState render_state{};
                render_state.viewports[0] = draw_context.m_viewport;
                render_state.viewportCount = 1u;
                render_state.vertexBuffers[0] = rhi::BufferOffsetPair(resources.m_vertex_buffer.get(), 0u);
                render_state.vertexBufferCount = 1u;
                render_state.indexBuffer = rhi::BufferOffsetPair(resources.m_index_buffer.get(), 0u);
                render_state.indexFormat = sizeof(ImDrawIdx) == 2u ? rhi::IndexFormat::Uint16 : rhi::IndexFormat::Uint32;
                render_state.scissorRectCount = 1u;

                const float4 clip_offset = imVec(draw_data->DisplayPos).xyxy;
                const float4 clip_scale = imVec(io.DisplayFramebufferScale).xyxy;
                const float2 framebuffer_extent{draw_context.m_viewport.extentX, draw_context.m_viewport.extentY};

                u32 global_vtx_offset = 0;
                u32 global_idx_offset = 0;
                for (int n = 0; n < draw_data->CmdListsCount; n++) {
                    const auto *cmdList = draw_data->CmdLists[n];

                    for (int i = 0; i < cmdList->CmdBuffer.Size; i++) {
                        const auto *cmd = &cmdList->CmdBuffer[i];

                        if (cmd->UserCallback) {
                            cmd->UserCallback(cmdList, cmd);
                            continue;
                        }

                        const float4 clip_rect = (imVec(cmd->ClipRect) - clip_offset) * clip_scale;
                        const uint4 scissor_rect = roundToUInt(float4(
                            max(clip_rect.xy, float2(0)),
                            min(clip_rect.zw, float2(framebuffer_extent))));
                        if (scissor_rect.z <= scissor_rect.x or scissor_rect.w <= scissor_rect.y) {
                            continue;
                        }

                        render_state.scissorRects[0] = rhi::ScissorRect{
                            scissor_rect.x, scissor_rect.y,
                            scissor_rect.z, scissor_rect.w
                        };

                        draw_context.m_pass.setRenderState(render_state);

                        rhi::DrawArguments draw_arguments{};
                        draw_arguments.vertexCount = cmd->ElemCount;
                        draw_arguments.instanceCount = 1u;
                        draw_arguments.startIndexLocation = cmd->IdxOffset + global_idx_offset;
                        draw_arguments.startVertexLocation = static_cast<i32>(cmd->VtxOffset + global_vtx_offset);
                        draw_arguments.startInstanceLocation = 0;

                        draw_context.m_pass.drawIndexed(draw_arguments);
                    }

                    global_vtx_offset += cmdList->VtxBuffer.Size;
                    global_idx_offset += cmdList->IdxBuffer.Size;
                }

                return default_value_v;
            }
        };

        [[nodiscard]] ImGuiKey keyboardKeyToImGuiKey(EKeyboardKey key) noexcept;

        [[nodiscard]] ImGuiKey gamepadButtonToImGuiKey(EGamepadButton button) noexcept;

        [[nodiscard]] int mouseButtonToImGui(EMouseButton button) noexcept;

        std::error_code ImGuiService::createInvariantRenderState_(rhi::IDevice &device) {
            constexpr rhi::InputElementDesc elements[] = {
                {"POSITION", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, pos), 0},
                {"TEXCOORD", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, uv), 0},
                {"COLOR", 0, rhi::Format::RGBA8Unorm, PPR_OFFSETOF(ImDrawVert, col), 0},
            };

            PPR_RETURN_ERROR_ON_FAIL(UI, device.createInputLayout(
                safe_narrowing(sizeof(ImDrawVert)),
                elements,
                3u,
                m_vertex_layout.writeRef()));

            rhi::SamplerDesc sampler_desc{};
            sampler_desc.minFilter = rhi::TextureFilteringMode::Linear;
            sampler_desc.magFilter = rhi::TextureFilteringMode::Linear;
            sampler_desc.mipFilter = rhi::TextureFilteringMode::Linear;
            sampler_desc.addressU = rhi::TextureAddressingMode::ClampToEdge;
            sampler_desc.addressV = rhi::TextureAddressingMode::ClampToEdge;
            sampler_desc.addressW = rhi::TextureAddressingMode::ClampToEdge;
            sampler_desc.maxAnisotropy = 1;

            PPR_RETURN_ERROR_ON_FAIL(UI, device.createSampler(sampler_desc, m_font_sampler.writeRef()));

            return default_value_v;
        }

        std::error_code ImGuiService::createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device) {
            shader::ComPtr<shader::IModule> shader_module{};
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_service.loadModuleFromSource(
                "imgui",
                "imgui.slang",
                kImGuiShader,
                shader_module.writeRef()));

            shader::ComPtr<shader::IEntryPoint> vertex_ep;
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

            shader::ComPtr<shader::IEntryPoint> fragment_ep;
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

            shader::IComponentType *entryPoints[] = {vertex_ep.get(), fragment_ep.get()};

            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = shader_module;
            program_desc.slangEntryPoints = entryPoints;
            program_desc.slangEntryPointCount = std::size(entryPoints);

            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

            return default_value_v;
        }

        std::error_code ImGuiService::createFontTexture_(rhi::IDevice &device, rhi::ICommandQueue &device_queue) {
            ImGuiIO &io = ImGui::GetIO();

            u8 *pixels = nullptr;
            int width = 0, height = 0, bpp = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);

            rhi::TextureDesc texture_desc{};
            texture_desc.type = rhi::TextureType::Texture2D;
            texture_desc.size = {static_cast<u32>(width), static_cast<u32>(height), 1};
            texture_desc.arrayLength = 1;
            texture_desc.mipCount = 1;
            texture_desc.format = rhi::Format::RGBA8Unorm;
            texture_desc.memoryType = rhi::MemoryType::DeviceLocal;
            texture_desc.usage = enumCombine(rhi::TextureUsage::ShaderResource, rhi::TextureUsage::CopyDestination);
            texture_desc.defaultState = rhi::ResourceState::CopyDestination;
            texture_desc.label = "imgui_font_atlas";

            rhi::ComPtr<rhi::ITexture> texture;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.createTexture(texture_desc, nullptr, texture.writeRef()));

            const u64 pixelDataSize = static_cast<u64>(width) * height * 4;
            rhi::BufferDesc staging_desc{};
            staging_desc.size = pixelDataSize;
            staging_desc.usage = rhi::BufferUsage::CopySource;
            staging_desc.memoryType = rhi::MemoryType::Upload;
            staging_desc.defaultState = rhi::ResourceState::General;
            staging_desc.label = "imgui_font_staging";

            rhi::ComPtr<rhi::IBuffer> staging_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.createBuffer(staging_desc, pixels, staging_buffer.writeRef()));

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.createCommandEncoder(encoder.writeRef()));

            rhi::Offset3D dst_offset{0, 0, 0};
            encoder->copyBufferToTexture(
                texture.get(), 0, 0, dst_offset,
                staging_buffer.get(), 0, pixelDataSize,
                static_cast<u32>(pixelDataSize / height),
                {static_cast<u32>(width), static_cast<u32>(height), 1});

            encoder->setTextureState(texture.get(), rhi::SubresourceRange{
                .layer = 0,
                .layerCount = 1,
                .mip = 0,
                .mipCount = 1
            }, rhi::ResourceState::ShaderResource);

            rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, encoder->finish(cmd_buffer.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.submit(cmd_buffer.get()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.waitOnHost());

            staging_buffer.setNull();

            rhi::ComPtr<rhi::ITextureView> view;
            PPR_RETURN_ERROR_ON_FAIL(UI, texture->getDefaultView(view.writeRef()));

            m_font_texture = std::move(texture);
            m_font_texture_view = std::move(view);

            io.Fonts->SetTexID(m_font_texture_view.get());

            PPR_LOG(UI, info, "font texture created", {
                {"width", width},
                {"height", height},
                });
            return default_value_v;
        }

        std::error_code ImGuiService::createRenderPipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature) {
            if (signature.m_color_formats.size() != 1u ||
                signature.m_depth_stencil_format.has_value() ||
                signature.m_sample_count != 1u) {
                PPR_LOG(UI, error, "unsupported render pipeline signature", {
                    {"color_format_count", signature.m_color_formats.size()},
                    {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                    {"sample_count", signature.m_sample_count},
                    });
                return make_error_code(std::errc::operation_not_supported);
            }

            rhi::ColorTargetDesc color_target{};
            color_target.format = signature.m_color_formats.front();
            color_target.enableBlend = true;
            color_target.color.srcFactor = rhi::BlendFactor::SrcAlpha;
            color_target.color.dstFactor = rhi::BlendFactor::InvSrcAlpha;
            color_target.color.op = rhi::BlendOp::Add;
            color_target.alpha.srcFactor = rhi::BlendFactor::One;
            color_target.alpha.dstFactor = rhi::BlendFactor::InvSrcAlpha;
            color_target.alpha.op = rhi::BlendOp::Add;
            color_target.writeMask = rhi::RenderTargetWriteMask::All;

            rhi::RenderPipelineDesc pipeline_desc{};
            pipeline_desc.program = m_shader_program;
            pipeline_desc.inputLayout = m_vertex_layout;
            pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            pipeline_desc.targets = &color_target;
            pipeline_desc.targetCount = 1u;
            pipeline_desc.rasterizer.cullMode = rhi::CullMode::None;
            pipeline_desc.rasterizer.scissorEnable = true;
            pipeline_desc.depthStencil.depthTestEnable = false;
            pipeline_desc.depthStencil.depthWriteEnable = false;
            pipeline_desc.label = "render_imgui";
            pipeline_desc.multisample.sampleCount = signature.m_sample_count;

            PPR_RETURN_ERROR_ON_FAIL(UI, device.createRenderPipeline(pipeline_desc, m_render_pipeline.writeRef()));
            return default_value_v;
        }

        std::error_code ImGuiService::uploadDrawData_(rhi::IDevice &device, const ImDrawData &draw_data, FrameResources &resources) {
            const u32 total_vtx_count = draw_data.TotalVtxCount;
            const u32 total_idx_count = draw_data.TotalIdxCount;
            if (total_vtx_count == 0 || total_idx_count == 0) {
                return default_value_v;
            }

            if (not resources.m_vertex_buffer or
                resources.m_vertex_buffer->getDesc().size < static_cast<u64>(total_vtx_count) * sizeof(ImDrawVert)) {
                const u32 new_capacity = alignForward(total_vtx_count, 8192_u32);
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawVert);

                rhi::BufferDesc vb_desc{};
                vb_desc.size = size_bytes;
                vb_desc.usage = enumCombine(rhi::BufferUsage::VertexBuffer, rhi::BufferUsage::CopySource);
                vb_desc.memoryType = rhi::MemoryType::Upload;
                vb_desc.defaultState = rhi::ResourceState::General;
                vb_desc.label = "imgui_vertex_buffer";

                PPR_RETURN_ERROR_ON_FAIL(UI, device.createBuffer(
                    vb_desc, nullptr, resources.m_vertex_buffer.writeRef()));
            }

            if (not resources.m_index_buffer or
                resources.m_index_buffer->getDesc().size < static_cast<u64>(total_idx_count) * sizeof(ImDrawIdx)) {
                const u32 new_capacity = alignForward(total_idx_count, 16384_u32);
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawIdx);

                rhi::BufferDesc ib_desc{};
                ib_desc.size = size_bytes;
                ib_desc.usage = enumCombine(rhi::BufferUsage::IndexBuffer, rhi::BufferUsage::CopySource);
                ib_desc.memoryType = rhi::MemoryType::Upload;
                ib_desc.defaultState = rhi::ResourceState::General;
                ib_desc.label = "imgui_index_buffer";

                PPR_RETURN_ERROR_ON_FAIL(UI, device.createBuffer(
                    ib_desc, nullptr, resources.m_index_buffer.writeRef()));
            }

            void *mapped_vtx = nullptr;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.mapBuffer(resources.m_vertex_buffer.get(), rhi::CpuAccessMode::Write, &mapped_vtx));

            void *mapped_idx = nullptr;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.mapBuffer(resources.m_index_buffer.get(), rhi::CpuAccessMode::Write, &mapped_idx));

            auto *vtx_dst = static_cast<ImDrawVert *>(mapped_vtx);
            auto *idx_dst = static_cast<ImDrawIdx *>(mapped_idx);

            for (int n = 0; n < draw_data.CmdListsCount; n++) {
                const ImDrawList *cmd_list = draw_data.CmdLists[n];

                std::memcpy(vtx_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
                std::memcpy(idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

                vtx_dst += cmd_list->VtxBuffer.Size;
                idx_dst += cmd_list->IdxBuffer.Size;
            }

            device.unmapBuffer(resources.m_vertex_buffer.get());
            device.unmapBuffer(resources.m_index_buffer.get());

            return default_value_v;
        }

        EInputMessageResponse ImGuiService::onInputCharacter_(const hal::native::char_t codepoint) noexcept {
            // Ctrl+key produces a control character (e.g. Ctrl+A -> 0x01), not text.
            if (ImGui::GetIO().KeyCtrl) {
                return EInputMessageResponse::unhandled;
            }
            if (ImGuiIO &io = ImGui::GetIO(); io.WantTextInput) {
                char8_t utf8_input[8]{};
                const std::size_t utf8_len = hal::native::utf8(
                    {&codepoint, 1u},
                    utf8_input, std::size(utf8_input) - 1u/*\0*/);

                if (utf8_len < std::size(utf8_input)) {
                    utf8_input[utf8_len] = u8'\0';
                    io.AddInputCharactersUTF8(reinterpret_cast<const char *>(&utf8_input[0]));
                    return EInputMessageResponse::consumed;
                }
            }

            return EInputMessageResponse::unhandled;
        }

        void ImGuiService::onInputAnyDigital_(const InputActionEvent &event, const InputKey &trigger) noexcept {
            std::visit(overloaded(
                [&](const EKeyboardKey keyboard_key) noexcept {
                    if (const ImGuiKey imgui_key = keyboardKeyToImGuiKey(keyboard_key); imgui_key != ImGuiKey_None) {
                        const bool is_key_down = event.getDigitalValue();

                        ImGuiIO &io = ImGui::GetIO();
                        io .AddKeyEvent(imgui_key, is_key_down);

                        // handle modifier keys:
                        switch (keyboard_key) {
                            case EKeyboardKey::left_control:
                            case EKeyboardKey::right_control:
                                io.AddKeyEvent(ImGuiMod_Ctrl, is_key_down);
                                break;
                            case EKeyboardKey::left_shift:
                            case EKeyboardKey::right_shift:
                                io.AddKeyEvent(ImGuiMod_Shift, is_key_down);
                                break;
                            case EKeyboardKey::left_alt:
                            case EKeyboardKey::right_alt:
                                io.AddKeyEvent(ImGuiMod_Alt, is_key_down);
                                break;
                            case EKeyboardKey::left_super:
                            case EKeyboardKey::right_super:
                                io.AddKeyEvent(ImGuiMod_Super, is_key_down);
                                break;
                            default: break;
                        }
                    }
                },
                [&](const EGamepadButton gamepad_button) noexcept {
                    if (const ImGuiKey imgui_key = gamepadButtonToImGuiKey(gamepad_button); imgui_key != ImGuiKey_None) {
                        ImGui::GetIO().AddKeyEvent(imgui_key, event.getDigitalValue());
                    }
                },
                [&](const EMouseButton mouse_button) noexcept {
                    if (const ImGuiMouseButton imgui_button = mouseButtonToImGui(mouse_button); imgui_button >= 0) {
                        ImGui::GetIO().AddMouseButtonEvent(imgui_button, event.getDigitalValue());
                    }
                },
                [](auto) noexcept {
                    std::unreachable();
                }
            ), trigger.m_code);
        }

        void ImGuiService::onInputMouseCursor_(const InputActionEvent &event, const InputKey &) noexcept {
            const float2 client_pos = event.getAxis2DValue().m_absolute;
            ImGui::GetIO().AddMousePosEvent(client_pos.x, client_pos.y);
        }

        void ImGuiService::onInputMouseWheel_(const InputActionEvent &event, const InputKey &) noexcept {
            const float2 wheel_delta = event.getAxis2DValue().m_relative;
            ImGui::GetIO().AddMouseWheelEvent(wheel_delta.x, wheel_delta.y);
        }

        void ImGuiService::onInputGamepadStick_(const InputActionEvent &event, const InputKey &trigger) noexcept {
            int stick_index = none_v;
            if (trigger == InputKey::gamepad_left_2d) {
                stick_index = 0;
            } else if (trigger == InputKey::gamepad_right_2d) {
                stick_index = 1;
            } else {
                return;
            }

            const float2 stick_value = event.getAxis2DValue().m_absolute;
            constexpr ImGuiKey stick_axes[][4] = {
                {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown},
                {ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown},
            };

            // left
            if (stick_value.x < 0) {
                ImGui::GetIO().AddKeyAnalogEvent(stick_axes[stick_index][0], true, -stick_value.x);
            }
            // right
            if (stick_value.x > 0) {
                ImGui::GetIO().AddKeyAnalogEvent(stick_axes[stick_index][1], true, stick_value.x);
            }
            // down
            if (stick_value.y < 0) {
                ImGui::GetIO().AddKeyAnalogEvent(stick_axes[stick_index][2], true, -stick_value.y);
            }
            // up
            if (stick_value.y > 0) {
                ImGui::GetIO().AddKeyAnalogEvent(stick_axes[stick_index][3], true, stick_value.y);
            }
        }

        void ImGuiService::onInputGamepadTrigger_(const InputActionEvent &event, const InputKey &trigger) noexcept {
            const float trigger_value = event.getAxis1DValue().m_absolute;

            if (trigger == InputKey::gamepad_left_trigger_axis) {
                ImGui::GetIO().AddKeyAnalogEvent(ImGuiKey_GamepadL2, true, trigger_value);
            }

            if (trigger == InputKey::gamepad_right_trigger_axis) {
                ImGui::GetIO().AddKeyAnalogEvent(ImGuiKey_GamepadR2, true, trigger_value);
            }
        }

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
                case EKeyboardKey::numpad_multiply: return ImGuiKey_KeypadMultiply;
                case EKeyboardKey::numpad_add: return ImGuiKey_KeypadAdd;
                case EKeyboardKey::numpad_subtract: return ImGuiKey_KeypadSubtract;
                case EKeyboardKey::numpad_decimal: return ImGuiKey_KeypadDecimal;
                case EKeyboardKey::numpad_divide: return ImGuiKey_KeypadDivide;
                case EKeyboardKey::numpad_enter: return ImGuiKey_KeypadEnter;
                case EKeyboardKey::numpad_equal: return ImGuiKey_KeypadEqual;
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

        [[nodiscard]] ImGuiKey gamepadButtonToImGuiKey(const EGamepadButton button) noexcept {
            switch (button) {
                case EGamepadButton::start: return ImGuiKey_GamepadStart;
                case EGamepadButton::back: return ImGuiKey_GamepadBack;
                case EGamepadButton::X: return ImGuiKey_GamepadFaceLeft;
                case EGamepadButton::B: return ImGuiKey_GamepadFaceRight;
                case EGamepadButton::Y: return ImGuiKey_GamepadFaceUp;
                case EGamepadButton::A: return ImGuiKey_GamepadFaceDown;
                case EGamepadButton::dpad_left: return ImGuiKey_GamepadDpadLeft;
                case EGamepadButton::dpad_right: return ImGuiKey_GamepadDpadRight;
                case EGamepadButton::dpad_up: return ImGuiKey_GamepadDpadUp;
                case EGamepadButton::dpad_down: return ImGuiKey_GamepadDpadDown;
                case EGamepadButton::left_shoulder: return ImGuiKey_GamepadL1;
                case EGamepadButton::right_shoulder: return ImGuiKey_GamepadR1;
                case EGamepadButton::left_thumb: return ImGuiKey_GamepadL3;
                case EGamepadButton::right_thumb: return ImGuiKey_GamepadR3;
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
    }

    namespace ui {
        std::unique_ptr<IUIService> createImGuiService() {
            return std::make_unique<ImGuiService>();
        }
    }
}
