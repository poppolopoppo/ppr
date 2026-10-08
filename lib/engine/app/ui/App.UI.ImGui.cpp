module;
#include "pP/Macros.h"
module engine.app;

import imgui;
import imgui_internal;

import :input.device;
import :input.key;
import :input.listener;
import :input.routing;
import :renderer.frame_fence;
import :renderer.types;
import :service.input;
import :service.ui;
import :service.window;
import :ui.imgui;
import :window.handle;

import engine.core;
import engine.math;
import engine.rhi;
import engine.shader;
import std;

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

        [[maybe_unused]] void imGuiDebugPrintf(const char *format, const char *buffer) {
            PPR_ASSERT(std::string_view(format) == "%s");
            PPR_ASSERT(buffer != nullptr);
            PPR_LOG_RAW(UI, debug, buffer);
        }

        [[nodiscard, maybe_unused]] float2 framebufferScaleFor(const int2 &logical, const int2 &framebuffer) noexcept {
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

        class ImGuiService final : public IUIService, public IFenceAwareUiService {
            ImGuiContext *m_imgui_context{nullptr};

            /// input handling:
            InputListener m_input_listener{};
            safe_ptr<InputContext> m_input_context{};
            bool m_pending_deselect{false};
            InputMapping m_input_mapping{"ImGuiInputs"};

            InputAction m_input_any_keyboard_key{"ImGuiAnyKeyboardKey", EInputValueType::digital};
            InputAction m_input_any_mouse_button{"ImGuiAnyMouseButton", EInputValueType::digital};
            InputAction m_input_any_gamepad_button{"ImGuiAnyGamepadButton", EInputValueType::digital};

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

            // Two upload slots for two frame credits: ZERO slack by
            // construction. Slots are selected by per-slot completion
            // values, never by CPU parity — when both submissions are still
            // GPU-pending the frame waits its slot out instead of
            // overwriting early. Growth of either buffer waits for the
            // youngest tracked use (R6).
            static constexpr u32 kUiFrameSlots{2u};
            std::array<FrameResources, kUiFrameSlots> m_frame_resources{};
            std::array<FenceCompletionValue, kUiFrameSlots> m_slot_values{};
            u32 m_resource_cursor{0u};
            u32 m_last_used_slot{0u};
            std::size_t m_frame_revision{};

            // Per-slot ROOT shader objects carrying the font texture/sampler
            // bindings plus the resolved g_scale/g_offset cursor. A frame
            // binds and writes only its selected slot's root — no
            // persistent-root mutation across frames. Replaced
            // pipeline/roots retire through m_deferred_releases.
            std::array<rhi::ComPtr<rhi::IShaderObject>, kUiFrameSlots> m_slot_roots{};
            DeferredReleaseQueue m_deferred_releases{};
            FenceCompletionValue m_last_submitted_value{kFenceValueNone};

            // Font atlas generation: rebuilt when the display framebuffer
            // scale changes (DPI), installed after the upload's own
            // global-idle wait, then rebound into every slot root. The
            // previous atlas drops only after that wait, so no submission
            // ever samples a destroyed texture.
            //
            // P2 ownership split: the CLIENT stages owned pixels + a version
            // into the detached data (m_font_scale/staged_* are main-side
            // only); the encode lane installs them and publishes
            // m_font_installed_version (atomic: the client re-stages while
            // its staged version is unacknowledged, so a dropped frame
            // cannot lose a DPI rebuild). The TexID is frozen at init and
            // never rewritten afterwards, so the lane performs zero live
            // IO/context writes.
            float2 m_font_scale{0.0f, 0.0f};
            u64 m_font_staged_version{0u};
            std::atomic<u64> m_font_installed_version{0u};
            u64 m_font_texid_init{0u};
            u64 m_font_rebuild_count{0u};
            u64 m_growth_wait_count{0u};
            u64 m_rejected_callback_count{0u};

            rhi::ComPtr<rhi::IRenderPipeline> m_render_pipeline{};
            std::optional<RenderPipelineKey> m_render_pipeline_key{};

            std::error_code createInvariantRenderState_(rhi::IDevice &device);

            std::error_code createShaderProgram_(IShaderService &shader_service, rhi::IDevice &device);

            std::error_code createFontTexture_(rhi::IDevice &device, rhi::ICommandQueue &device_queue);

            std::error_code createRenderPipeline_(rhi::IDevice &device, const RenderPipelineSignature &signature);

            // Builds one font atlas (upload + queue wait) without installing
            // it; shared by creation and DPI rebuilds.
            std::error_code uploadFontAtlas_(
                rhi::IDevice &device, rhi::ICommandQueue &device_queue, rhi::ComPtr<rhi::ITexture> &out_texture,
                rhi::ComPtr<rhi::ITextureView> &out_view);

            // P2 detach/encode overrides are defined inline below (single
            // in-class definition each); the lane-confined helpers they
            // call are declared here.
            [[nodiscard]] u64 installedFontVersion() const noexcept override {
                return m_font_installed_version.load(std::memory_order_acquire);
            }

            // Lane-side font install from client-staged pixels (never
            // touches live Fonts/context state; TexID stays frozen).
            std::error_code installDetachedFontAtlas_(
                rhi::IDevice &device, rhi::ICommandQueue &device_queue, const DetachedUiDrawData &detached);

            std::error_code uploadDetachedDrawData_(
                rhi::IDevice &device, const DetachedUiDrawData &detached, FrameResources &resources,
                FenceCompletionValue completed, FenceWaitHook &wait_for_fence);

            std::error_code encodeDetachedUiFrame_(const DetachedUiDrawData &detached, const DrawContext &draw_context);

            void notifyFrameSubmitted_(FenceCompletionValue submit_value) noexcept {
                m_last_submitted_value = submit_value;
                m_slot_values[m_last_used_slot] = submit_value;
            }

            // IFenceAwareUiService: fence value of the frame's accepted
            // SUBMIT, reported by the frame owner. Never called on submit
            // failure (the slot keeps its older, conservative value).
            void notifyFrameSubmitted(const FenceCompletionValue submit_value) noexcept override {
                notifyFrameSubmitted_(submit_value);
            }

            void reclaimCompleted_(FenceCompletionValue completed) noexcept;

            void drainPendingDeselect_();

            static EInputMessageResponse onInputCharacter_(hal::native::char_t codepoint) noexcept;

            static void onInputAnyDigital_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputMouseCursor_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputMouseWheel_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputGamepadStick_(const InputActionEvent &event, const InputKey &trigger) noexcept;

            static void onInputGamepadTrigger_(const InputActionEvent &event, const InputKey &trigger) noexcept;

        public:
            ImGuiService() noexcept {
                ImGui::SetAllocatorFunctions(
                    [](const std::size_t sz, [[maybe_unused]] void *user_data) -> void * { return std::malloc(sz); },
                    [](void *ptr, [[maybe_unused]] void *user_data) -> void { std::free(ptr); }, nullptr);

                m_input_any_keyboard_key.setStarted(&onInputAnyDigital_);
                m_input_any_keyboard_key.setTriggered(&onInputAnyDigital_);
                m_input_any_keyboard_key.setCompleted(&onInputAnyDigital_);

                m_input_any_gamepad_button.setStarted(&onInputAnyDigital_);
                m_input_any_gamepad_button.setTriggered(&onInputAnyDigital_);
                m_input_any_gamepad_button.setCompleted(&onInputAnyDigital_);

                m_input_any_mouse_button.setStarted(&onInputAnyDigital_);
                m_input_any_mouse_button.setTriggered(&onInputAnyDigital_);
                m_input_any_mouse_button.setCompleted(&onInputAnyDigital_);

                m_input_mouse_cursor.setTriggered(&onInputMouseCursor_);
                m_input_mouse_wheel.setTriggered(&onInputMouseWheel_);

                m_input_gamepad_stick.setTriggered(&onInputGamepadStick_);
                m_input_gamepad_trigger.setTriggered(&onInputGamepadTrigger_);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_any_keyboard_key), InputKey::any_keyboard_key);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_any_gamepad_button), InputKey::any_gamepad_button);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_any_mouse_button), InputKey::any_mouse_button);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_stick), InputKey::gamepad_left_2d);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_stick), InputKey::gamepad_right_2d);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_trigger), InputKey::gamepad_left_trigger_axis);
                m_input_mapping.mapInputKey(safe_ptr(&m_input_gamepad_trigger), InputKey::gamepad_right_trigger_axis);

                m_input_mapping.mapInputKey(safe_ptr(&m_input_mouse_cursor), InputKey::mouse_2d);
                m_input_mapping.mapInputKey(
                    safe_ptr(&m_input_mouse_wheel), InputKey::mouse_wheel_axis_x,
                    []([[maybe_unused]] const TimeSpan dt, InputValue &output) noexcept {
                        output = InputValue(std::get<InputAxis1D>(output), InputAxis1D{});
                    });
                m_input_mapping.mapInputKey(
                    safe_ptr(&m_input_mouse_wheel), InputKey::mouse_wheel_axis_y,
                    []([[maybe_unused]] const TimeSpan dt, InputValue &output) noexcept {
                        output = InputValue(InputAxis1D{}, std::get<InputAxis1D>(output));
                    });

                m_input_listener.addInputMapping(&m_input_mapping, 0);
                m_input_listener.setCharacterInputCallback(&onInputCharacter_);
                m_input_listener.setRawKeyCallback(
                    [this]([[maybe_unused]] const TimeSpan dt, const InputMessage &message) {
                        if (const EMouseButton *const button = std::get_if<EMouseButton>(&message.m_key.m_code);
                            button not_eq nullptr and InputBackgroundLatch::isBackgroundDragButton(*button)) {
                            if (message.m_event == EInputMessageEvent::pressed or
                                message.m_event == EInputMessageEvent::repeat) [[likely]] {
                                if (not hasMouseCaptureUnlessPopupClose()) [[likely]] {
                                    m_pending_deselect = true;
                                }
                            }
                        }
                        return EInputMessageResponse::unhandled;
                    });
            }

            ~ImGuiService() noexcept override {
                PPR_ASSERT(m_imgui_context == nullptr);

                m_input_listener.clearInputMappings();
                m_input_mapping.clearKeymap();
            }

            [[nodiscard]] bool hasMouseCaptureUnlessPopupClose() const noexcept override {
                return m_input_any_mouse_button.hasConsumeInput();
            }

            [[nodiscard]] std::error_code clearWindowFocus() override {
                if (not m_imgui_context) {
                    return std::make_error_code(std::errc::not_connected);
                }
                ImGui::SetCurrentContext(m_imgui_context);
                ImGui::SetWindowFocus(nullptr);
                return default_value_v;
            }

            [[nodiscard]] void *getContext() const noexcept override {
                return m_imgui_context;
            }

            safe_ptr<const InputListener> getInputListener() const noexcept override {
                return safe_ptr{&m_input_listener};
            }

            [[nodiscard]] std::error_code initialize(
                WindowInputContext &window_input_context, IRhiService &rhi_service, IShaderService &shader_service,
                const int input_listener_priority) override {
                const hal::ProfileScope scope{"App.UI.Init"};
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

                m_input_context = &window_input_context.m_context;
                m_input_context->addInputListener(&m_input_listener, input_listener_priority);

                rhi::IDevice &device = rhi_service.getDevice();
                PPR_RETURN_ERROR_ON_FAIL(UI, createInvariantRenderState_(device));
                PPR_RETURN_ERROR_ON_FAIL(UI, createShaderProgram_(shader_service, device));

                rhi::ComPtr<rhi::ICommandQueue> device_queue{};
                PPR_RETURN_ERROR_ON_FAIL(UI, device.getQueue(rhi::QueueType::Graphics, device_queue.writeRef()));
                PPR_RETURN_ERROR_ON_FAIL(UI, createFontTexture_(device, *device_queue));

                PPR_LOG(UI, info, "UI service initialized", {});
                return default_value_v;
            }

            // Frontend/backend shutdown split (client-owned frontend vs
            // lane-confined GPU): the Detached half runs ONLY on the
            // client lane (main) before any lane GPU release —
            // input-listener removal plus consume-gating reset so no
            // client callback fires while the worker releases GPU state.
            // shutdownGpuResourcesOnOwnerLane runs ONLY on the GPU owner
            // lane (worker when a lane owns the renderer, else main-inline)
            // after quiescence is proven — pipelines, roots, buffers, font
            // atlas, layout, and program release. The Context half runs
            // ONLY on main after the lane GPU release has been acknowledged
            // — backend detach plus live ImGui context destruction.
            // Quiescence is permission to release, never permission to
            // migrate frontend destruction to the worker. All halves are
            // idempotent.
            [[nodiscard]] std::error_code shutdownFrontendDetachedOnMain() {
                if (m_input_context) {
                    m_input_context->removeInputListener(m_input_listener);
                    m_input_context.reset();
                }
                m_pending_deselect = false;
                m_input_any_keyboard_key.setConsumeInput(false);
                m_input_any_mouse_button.setConsumeInput(false);
                m_input_mouse_cursor.setConsumeInput(false);
                m_input_mouse_wheel.setConsumeInput(false);
                m_input_any_gamepad_button.setConsumeInput(false);
                m_input_gamepad_stick.setConsumeInput(false);
                m_input_gamepad_trigger.setConsumeInput(false);
                return default_value_v;
            }

            [[nodiscard]] std::error_code shutdownGpuResourcesOnOwnerLane() {
                m_render_pipeline.setNull();
                m_render_pipeline_key.reset();
                // Precondition (established by the Editor): GPU completion
                // was already proven idle, so no pending frame can read the
                // pipelines, roots, or buffers released here. Deferred
                // pipeline/roots drain unconditionally — after confirmed
                // loss the objects are dead either way. Slot values reset
                // with the fence epoch they named.
                std::ignore = m_deferred_releases.drainAll();
                m_last_submitted_value = kFenceValueNone;
                m_slot_values.fill(kFenceValueNone);
                m_resource_cursor = 0u;
                m_last_used_slot = 0u;
                for (rhi::ComPtr<rhi::IShaderObject> &root: m_slot_roots) {
                    root.setNull();
                }

                m_font_texture_view.setNull();
                m_font_texture.setNull();
                m_font_sampler.setNull();

                m_vertex_layout.setNull();
                m_shader_program.setNull();

                for (FrameResources &resources: m_frame_resources) {
                    resources.m_index_buffer.setNull();
                    resources.m_vertex_buffer.setNull();
                }
                return default_value_v;
            }

            [[nodiscard]] std::error_code shutdownFrontendOnMain() {
                if (m_input_context) {
                    m_input_context->removeInputListener(m_input_listener);
                    m_input_context.reset();
                }
                m_pending_deselect = false;

                // Reset consume gating to non-consuming (defaults to player).
                m_input_any_keyboard_key.setConsumeInput(false);
                m_input_any_mouse_button.setConsumeInput(false);
                m_input_mouse_cursor.setConsumeInput(false);
                m_input_mouse_wheel.setConsumeInput(false);
                m_input_any_gamepad_button.setConsumeInput(false);
                m_input_gamepad_stick.setConsumeInput(false);
                m_input_gamepad_trigger.setConsumeInput(false);

                if (m_imgui_context) {
                    // Detach backend before destroying context. Client lane
                    // only: the live context/IO is client-owned and never
                    // destroyed on the worker.
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

            [[nodiscard]] std::error_code shutdown() override {
                PPR_LOG(UI, info, "UI service shut down");

                // Inline/reference path (no lane owns the renderer): all
                // halves run here on main — frontend detach first so no
                // callback fires during GPU release, context destroy last.
                // The lane path never calls this: the Editor runs the GPU
                // half on the worker and both frontend halves on main via
                // the free handoffs below.
                PPR_RETURN_ERROR_ON_FAIL(UI, shutdownFrontendDetachedOnMain());
                PPR_RETURN_ERROR_ON_FAIL(UI, shutdownGpuResourcesOnOwnerLane());
                PPR_RETURN_ERROR_ON_FAIL(UI, shutdownFrontendContextOnMain());
                return default_value_v;
            }

            // Context destruction without input detach (runs after the lane
            // GPU release has been acknowledged): backend detach + live
            // context destroy on the client lane.
            [[nodiscard]] std::error_code shutdownFrontendContextOnMain() {
                if (m_imgui_context) {
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

            // Two-lane affinity enforcement (S2): production (update /
            // buildDetachedUiFrame) and encoding (renderDetachedUiFrame /
            // render-detached) are each single-owner — a role never runs
            // on two threads at once nor migrates threads. Produce/encode
            // overlap across roles is allowed by design (their field sets
            // are disjoint; the shared font version is atomic), but each
            // role's confinement is CHECKED at every entry, not assumed.
            std::mutex m_affinity_mutex{};
            std::thread::id m_produce_thread{};
            std::thread::id m_encode_thread{};
            bool m_produce_seen{false};
            bool m_encode_seen{false};
            std::atomic<int> m_produce_active{0};
            std::atomic<int> m_encode_active{0};

            struct AffinityScope {
                ImGuiService &m_owner;
                bool m_is_encode;

                explicit AffinityScope(ImGuiService &owner, bool is_encode) noexcept
                    : m_owner(owner), m_is_encode(is_encode) {
                    const std::thread::id here = std::this_thread::get_id();
                    {
                        const std::lock_guard guard{m_owner.m_affinity_mutex};
                        if (m_is_encode) {
                            PPR_ASSERT(not m_owner.m_encode_seen or m_owner.m_encode_thread == here);
                            m_owner.m_encode_seen = true;
                            m_owner.m_encode_thread = here;
                        } else {
                            PPR_ASSERT(not m_owner.m_produce_seen or m_owner.m_produce_thread == here);
                            m_owner.m_produce_seen = true;
                            m_owner.m_produce_thread = here;
                        }
                    }
                    if (m_is_encode) {
                        PPR_ASSERT(m_owner.m_encode_active.exchange(1, std::memory_order_acq_rel) == 0);
                    } else {
                        // Produce may nest (render → buildDetachedUiFrame);
                        // only a concurrent re-entry invalidates.
                        m_owner.m_produce_active.fetch_add(1, std::memory_order_acq_rel);
                    }
                }

                ~AffinityScope() {
                    if (m_is_encode) {
                        m_owner.m_encode_active.store(0, std::memory_order_release);
                    } else {
                        m_owner.m_produce_active.fetch_sub(1, std::memory_order_acq_rel);
                    }
                }
            };

            [[nodiscard]] std::error_code update(const TimeSpan dt, const WindowViewport &viewport) override {
                const hal::ProfileScope scope{"App.UI.Update"};
                const AffinityScope affinity{*this, false};
                if (not m_imgui_context) {
                    return std::make_error_code(std::errc::not_connected);
                }
                ImGui::SetCurrentContext(m_imgui_context);
                ImGuiIO &io = ImGui::GetIO();

                io.DeltaTime = static_cast<float>(time::seconds(dt));

                const int2 &display_size = viewport.getViewport().getClientRect().m_extent;
                io.DisplaySize = ImVec2{static_cast<float>(display_size.x), static_cast<float>(display_size.y)};

                const float2 &content_scale = viewport.getWindow().m_content_scale;
                io.DisplayFramebufferScale = ImVec2{content_scale.x, content_scale.y};

                ++m_frame_revision;
                ImGui::NewFrame();

                // Focus gate: consume flags from last frame's widgets.
                m_input_any_keyboard_key.setConsumeInput(io.WantCaptureKeyboard);
                m_input_any_mouse_button.setConsumeInput(io.WantCaptureMouseUnlessPopupClose);
                m_input_mouse_cursor.setConsumeInput(io.WantCaptureMouseUnlessPopupClose);
                m_input_mouse_wheel.setConsumeInput(io.WantCaptureMouseUnlessPopupClose);

                const bool gamepad_nav_capture = io.NavActive &&
                                                 (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
                m_input_any_gamepad_button.setConsumeInput(gamepad_nav_capture);
                m_input_gamepad_stick.setConsumeInput(gamepad_nav_capture);
                m_input_gamepad_trigger.setConsumeInput(gamepad_nav_capture);
                drainPendingDeselect_();
                return default_value_v;
            }

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) override {
                // Inline reference path: the same build + encode the worker
                // runs, on one lane. No duplicated stage logic.
                const AffinityScope affinity{*this, false};
                DetachedUiDrawData detached{};
                PPR_RETURN_ERROR_ON_FAIL(UI, buildDetachedUiFrame(detached));
                return encodeDetachedUiFrame_(detached, draw_context);
            }

            [[nodiscard]] std::error_code buildDetachedUiFrame(DetachedUiDrawData &out) override {
                // CLIENT lane only: completes the ImGui frame and publishes
                // owned, imgui-free draw data. The worker never re-enters
                // this function nor reads anything it reads here.
                const AffinityScope affinity{*this, false};
                static_assert(
                    sizeof(DetachedUiVertex) == sizeof(ImDrawVert) and
                    PPR_OFFSETOF(DetachedUiVertex, m_position) == PPR_OFFSETOF(ImDrawVert, pos) and
                    PPR_OFFSETOF(DetachedUiVertex, m_uv) == PPR_OFFSETOF(ImDrawVert, uv) and
                    PPR_OFFSETOF(DetachedUiVertex, m_color) == PPR_OFFSETOF(ImDrawVert, col));
                if (not m_imgui_context) {
                    return std::make_error_code(std::errc::not_connected);
                }
                out = DetachedUiDrawData{};
                ImGui::SetCurrentContext(m_imgui_context);
                ImGui::Render();

                ImDrawData *const draw_data = ImGui::GetDrawData();
                if (not draw_data or not draw_data->Valid) {
                    return default_value_v;
                }

                const ImGuiIO &io = ImGui::GetIO();
                const ImGuiPlatformIO &platform_io = ImGui::GetPlatformIO();
                out.m_display_size = float2{io.DisplaySize.x, io.DisplaySize.y};
                out.m_framebuffer_scale = float2{io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y};
                out.m_framebuffer_extent = float2{
                    out.m_display_size.x * out.m_framebuffer_scale.x,
                    out.m_display_size.y * out.m_framebuffer_scale.y,
                };
                // Pinned 1.92: viewport origin lives on the draw data (not
                // the IO), and texture identity goes through GetTexID()
                // (the TextureId field was removed) — both read here on the
                // client only, then baked into the detached copy. Explicit
                // component access throughout (no swizzles: ImVec2 and
                // mango float2 alike expose only .x/.y).
                const float2 origin{draw_data->DisplayPos.x, draw_data->DisplayPos.y};
                const float2 fb_scale = out.m_framebuffer_scale;
                const float4 clip_offset{origin.x, origin.y, origin.x, origin.y};
                const float4 clip_scale{fb_scale.x, fb_scale.y, fb_scale.x, fb_scale.y};

                out.m_vertices.reserve(checked_cast<std::size_t>(draw_data->TotalVtxCount));
                out.m_indices.reserve(checked_cast<std::size_t>(draw_data->TotalIdxCount));

                u32 vtx_base = 0u;
                u32 idx_base = 0u;
                for (int list = 0; list < draw_data->CmdListsCount; ++list) {
                    const ImDrawList *const cmd_list = draw_data->CmdLists[list];
                    // Layout-identical bulk copy (see the static_assert
                    // above): memcpy is the blessed type-punning here, never
                    // element reads through the imgui type.
                    const std::size_t list_vtx = checked_cast<std::size_t>(cmd_list->VtxBuffer.Size);
                    const std::size_t vtx_write_base = out.m_vertices.size();
                    out.m_vertices.resize(vtx_write_base + list_vtx);
                    std::memcpy(
                        out.m_vertices.data() + vtx_write_base, cmd_list->VtxBuffer.Data,
                        list_vtx * sizeof(ImDrawVert));
                    for (int idx = 0; idx < cmd_list->IdxBuffer.Size; ++idx) {
                        out.m_indices.push_back(static_cast<u32>(cmd_list->IdxBuffer[idx]));
                    }

                    for (int cmd_index = 0; cmd_index < cmd_list->CmdBuffer.Size; ++cmd_index) {
                        const ImDrawCmd &cmd = cmd_list->CmdBuffer[cmd_index];
                        // Callback policy: only the known render-only reset
                        // command crosses the lane (as a flag honored at
                        // encode); any other client-context callback is
                        // REJECTED and counted — never invoked on the render
                        // path, where it could touch client-owned state.
                        if (cmd.UserCallback != nullptr) {
                            if (cmd.UserCallback == platform_io.DrawCallback_ResetRenderState) {
                                DetachedUiCommand reset{};
                                reset.m_reset_render_state = true;
                                out.m_commands.push_back(reset);
                            } else {
                                ++m_rejected_callback_count;
                                PPR_LOG(UI, warning, "rejected unsupported draw-list user callback");
                            }
                            continue;
                        }
                        // Texture identity is frozen at init: every stock
                        // command carries the init-time font view. Anything
                        // else (game textures pushed as ImTextureID) is
                        // rejected explicitly — the lane must never
                        // dereference an unknown identity.
                        if (cmd.GetTexID() != m_font_texid_init) {
                            ++m_rejected_callback_count;
                            PPR_LOG(UI, warning, "rejected draw command with unknown texture identity");
                            continue;
                        }
                        DetachedUiCommand detached{};
                        detached.m_clip_rect = (imVec(cmd.ClipRect) - clip_offset) * clip_scale;
                        detached.m_texture_id = static_cast<u64>(cmd.GetTexID());
                        detached.m_element_count = cmd.ElemCount;
                        detached.m_index_offset = cmd.IdxOffset + idx_base;
                        detached.m_vertex_offset = static_cast<u32>(cmd.VtxOffset + vtx_base);
                        out.m_commands.push_back(detached);
                    }
                    vtx_base += checked_cast<u32>(cmd_list->VtxBuffer.Size);
                    idx_base += checked_cast<u32>(cmd_list->IdxBuffer.Size);
                }
                out.m_has_content = not out.m_commands.empty();

                // Font publication: stage owned pixels only when the atlas
                // content/scale actually changed; an unacknowledged pending
                // generation retransmits the SAME version until the consumer
                // installs it. Empty pixels mean "keep the installed atlas".
                const float2 display_scale{io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y};
                // Component-wise compare (mango float2 equality yields a
                // mask, not bool — same idiom as the pre-split DPI check).
                const bool font_scale_changed =
                    (display_scale.x != m_font_scale.x or display_scale.y != m_font_scale.y);
                const bool font_unacknowledged =
                    (m_font_staged_version != m_font_installed_version.load(std::memory_order_acquire));
                if (font_scale_changed or font_unacknowledged) {
                    u8 *pixels = nullptr;
                    int width = 0, height = 0, bpp = 0;
                    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
                    if (pixels == nullptr or width <= 0 or height <= 0) [[unlikely]] {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const std::size_t pixel_bytes = static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(height) *
                                                    4u;
                    out.m_font_pixels.assign(pixels, pixels + pixel_bytes);
                    out.m_font_extent = int2{width, height};
                    if (font_scale_changed) {
                        // New atlas content/scale: mint the next generation.
                        m_font_scale = display_scale;
                        out.m_font_version = ++m_font_staged_version;
                    } else {
                        // Retransmit the SAME pending generation until it is
                        // acknowledged; never mint a fresh generation for an
                        // unacknowledged retransmit.
                        out.m_font_version = m_font_staged_version;
                    }
                }
                return default_value_v;
            }

            [[nodiscard]] std::error_code renderDetachedUiFrame(
                const DetachedUiDrawData &detached, const DrawContext &draw_context) override {
                // ANY lane (inline reference or worker): consumes ONLY the
                // detached data plus lane-confined GPU state. No live
                // context, no live IO, no GetDrawData(), no frontend fields.
                const AffinityScope affinity{*this, true};
                return encodeDetachedUiFrame_(detached, draw_context);
            }
        };

        std::error_code ImGuiService::encodeDetachedUiFrame_(
            const DetachedUiDrawData &detached, const DrawContext &draw_context) {
            const hal::ProfileScope scope{"App.UI.Render"};
            // Retire replaced pipelines/roots even on frames that encode
            // nothing below: deferred objects must not outlive completion.
            reclaimCompleted_(draw_context.m_completed_fence_value);

            // Lane-side font install from client-staged pixels: no live
            // Fonts/context reads, no TexID rewrite (frozen at init).
            if (detached.m_font_version != 0u and
                detached.m_font_version != m_font_installed_version.load(std::memory_order_acquire)) {
                if (detached.m_font_pixels.empty() or detached.m_font_extent.x <= 0 or detached.m_font_extent.y <= 0)
                    [[unlikely]] {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                rhi::ComPtr<rhi::ICommandQueue> device_queue{};
                PPR_RETURN_ERROR_ON_FAIL(
                    UI, draw_context.m_device.getQueue(rhi::QueueType::Graphics, device_queue.writeRef()));
                PPR_RETURN_ERROR_ON_FAIL(UI, installDetachedFontAtlas_(draw_context.m_device, *device_queue, detached));
            }

            if (not m_render_pipeline_key.has_value() or *m_render_pipeline_key != draw_context.m_render_pipeline_key) {
                PPR_RETURN_ERROR_ON_FAIL(
                    UI, createRenderPipeline_(draw_context.m_device, draw_context.m_render_pipeline_key));
                m_render_pipeline_key = draw_context.m_render_pipeline_key;
            }

            // Completion-selected upload slot (zero slack: 2 slots for 2
            // credits). A stale slot waits instead of overwriting early.
            {
                const std::span<const FenceCompletionValue> values{m_slot_values.data(), m_slot_values.size()};
                const u32 slot = selectRingSlot(draw_context.m_completed_fence_value, values, m_resource_cursor);
                if (not slotFenceComplete(draw_context.m_completed_fence_value, m_slot_values[slot])) [[unlikely]] {
                    // An unbound hook here is a Renderer contract
                    // violation: fail closed rather than overwrite early
                    // or invoke an empty callable.
                    if (not draw_context.m_wait_for_fence) [[unlikely]] {
                        return make_error_code(std::errc::invalid_argument);
                    }
                    PPR_RETURN_ERROR_ON_FAIL(UI, draw_context.m_wait_for_fence(m_slot_values[slot]));
                }
                m_resource_cursor = slot;
                m_last_used_slot = slot;
            }

            rhi::IShaderObject *const slot_root = m_slot_roots[m_resource_cursor].get();
            if (slot_root == nullptr) [[unlikely]] {
                return make_error_code(std::errc::invalid_argument);
            }
            draw_context.m_pass.bindPipeline(m_render_pipeline.get(), slot_root);
            rhi::ShaderCursor shader_cursor{slot_root};

            // Scale/offset derive from the DETACHED display size baked at
            // build time — never live IO.
            const float2 imgui_scale{
                2.0f / detached.m_display_size.x,
                -2.0f / detached.m_display_size.y,
            };
            const float2 imgui_offset{-1.0f, 1.0f};
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_scale"].setData(&imgui_scale, sizeof(float2)));
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_cursor["g_offset"].setData(&imgui_offset, sizeof(float2)));
            // Font bindings ride the per-slot root (fixed at pipeline
            // creation / font rebuild); only the scale/offset above vary
            // per frame, written into a slot no in-flight submission
            // still reads.

            FrameResources &resources = m_frame_resources[m_resource_cursor];
            PPR_RETURN_ON_FAIL(
                UI, uploadDetachedDrawData_(
                        draw_context.m_device, detached, resources, draw_context.m_completed_fence_value,
                        draw_context.m_wait_for_fence));

            rhi::RenderState render_state{};
            render_state.viewports[0] = draw_context.m_viewport;
            render_state.viewportCount = 1u;
            render_state.vertexBuffers[0] = rhi::BufferOffsetPair(resources.m_vertex_buffer.get(), 0u);
            render_state.vertexBufferCount = 1u;
            render_state.indexBuffer = rhi::BufferOffsetPair(resources.m_index_buffer.get(), 0u);
            render_state.indexFormat = sizeof(ImDrawIdx) == 2u ? rhi::IndexFormat::Uint16 : rhi::IndexFormat::Uint32;
            render_state.scissorRectCount = 1u;

            const float2 framebuffer_extent = detached.m_framebuffer_extent;
            for (const DetachedUiCommand &cmd: detached.m_commands) {
                // Known render-only reset, honored at its encoded position.
                if (cmd.m_reset_render_state) {
                    draw_context.m_pass.setRenderState(render_state);
                    continue;
                }
                if (cmd.m_element_count == 0u) {
                    continue;
                }
                const uint4 scissor_rect = roundToUInt<float, 4u>(
                    float4(max(cmd.m_clip_rect.xy, float2(0)), min(cmd.m_clip_rect.zw, float2(framebuffer_extent))));
                if (scissor_rect.z <= scissor_rect.x or scissor_rect.w <= scissor_rect.y) {
                    continue;
                }

                render_state.scissorRects[0] = rhi::ScissorRect{
                    scissor_rect.x, scissor_rect.y, scissor_rect.z, scissor_rect.w};

                draw_context.m_pass.setRenderState(render_state);

                rhi::DrawArguments draw_arguments{};
                draw_arguments.vertexCount = cmd.m_element_count;
                draw_arguments.instanceCount = 1u;
                draw_arguments.startIndexLocation = cmd.m_index_offset;
                draw_arguments.startVertexLocation = cmd.m_vertex_offset;
                draw_arguments.startInstanceLocation = 0;

                draw_context.m_pass.drawIndexed(draw_arguments);
            }

            return default_value_v;
        }

        std::error_code ImGuiService::installDetachedFontAtlas_(
            rhi::IDevice &device, rhi::ICommandQueue &device_queue, const DetachedUiDrawData &detached) {
            const int2 extent = detached.m_font_extent;
            const std::size_t pixel_bytes = detached.m_font_pixels.size();
            const u64 expected = static_cast<u64>(extent.x) * static_cast<u64>(extent.y) * 4u;
            if (extent.x <= 0 or extent.y <= 0 or pixel_bytes != expected) [[unlikely]] {
                return std::make_error_code(std::errc::invalid_argument);
            }

            rhi::TextureDesc texture_desc{};
            texture_desc.type = rhi::TextureType::Texture2D;
            texture_desc.size = {static_cast<u32>(extent.x), static_cast<u32>(extent.y), 1};
            texture_desc.arrayLength = 1;
            texture_desc.mipCount = 1;
            texture_desc.format = rhi::Format::RGBA8Unorm;
            texture_desc.memoryType = rhi::MemoryType::DeviceLocal;
            texture_desc.usage = enumCombine(rhi::TextureUsage::ShaderResource, rhi::TextureUsage::CopyDestination);
            texture_desc.defaultState = rhi::ResourceState::CopyDestination;
            texture_desc.label = "imgui_font_atlas";

            rhi::ComPtr<rhi::ITexture> texture;
            PPR_RETURN_ERROR_ON_FAIL(UI, device.createTexture(texture_desc, nullptr, texture.writeRef()));

            rhi::BufferDesc staging_desc{};
            staging_desc.size = pixel_bytes;
            staging_desc.usage = rhi::BufferUsage::CopySource;
            staging_desc.memoryType = rhi::MemoryType::Upload;
            staging_desc.defaultState = rhi::ResourceState::General;
            staging_desc.label = "imgui_font_staging";

            rhi::ComPtr<rhi::IBuffer> staging_buffer;
            PPR_RETURN_ERROR_ON_FAIL(
                UI, device.createBuffer(staging_desc, detached.m_font_pixels.data(), staging_buffer.writeRef()));

            rhi::ComPtr<rhi::ICommandEncoder> encoder;
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.createCommandEncoder(encoder.writeRef()));

            const u32 row_pitch = static_cast<u32>(pixel_bytes / static_cast<std::size_t>(extent.y));
            rhi::Offset3D dst_offset{0, 0, 0};
            encoder->copyBufferToTexture(
                texture.get(), 0, 0, dst_offset, staging_buffer.get(), 0, pixel_bytes, row_pitch,
                {static_cast<u32>(extent.x), static_cast<u32>(extent.y), 1});

            encoder->setTextureState(
                texture.get(), rhi::SubresourceRange{.layer = 0, .layerCount = 1, .mip = 0, .mipCount = 1},
                rhi::ResourceState::ShaderResource);

            rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, encoder->finish(cmd_buffer.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.submit(cmd_buffer.get()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.waitOnHost());

            staging_buffer.setNull();

            rhi::ComPtr<rhi::ITextureView> view;
            PPR_RETURN_ERROR_ON_FAIL(UI, texture->getDefaultView(view.writeRef()));

            // The upload's own queue wait above already drained prior
            // submissions: install + rebind immediately, drop the old atlas
            // without deferral. TexID stays frozen at its init value, so no
            // live frontend state is written from this lane.
            m_font_texture = std::move(texture);
            m_font_texture_view = std::move(view);
            for (rhi::ComPtr<rhi::IShaderObject> &root: m_slot_roots) {
                if (root) {
                    rhi::ShaderCursor root_cursor{root.get()};
                    PPR_RETURN_ERROR_ON_FAIL(UI, root_cursor["g_fontTexture"].setBinding(m_font_texture_view));
                }
            }
            m_font_installed_version.store(detached.m_font_version, std::memory_order_release);
            ++m_font_rebuild_count;

            PPR_LOG(
                UI, info, "font texture rebuilt from detached pixels",
                {
                    {"width", extent.x},
                    {"height", extent.y},
                });
            return default_value_v;
        }

        std::error_code ImGuiService::uploadDetachedDrawData_(
            rhi::IDevice &device, const DetachedUiDrawData &detached, FrameResources &resources,
            const FenceCompletionValue completed, FenceWaitHook &wait_for_fence) {
            const hal::ProfileScope scope{"App.UI.Upload"};
            const u32 total_vtx_count = checked_cast<u32>(detached.m_vertices.size());
            const u32 total_idx_count = checked_cast<u32>(detached.m_indices.size());
            if (total_vtx_count == 0 || total_idx_count == 0) {
                return default_value_v;
            }

            // Growth replaces the whole buffer, so the youngest submission
            // that may still read it must complete first (R6). The slot was
            // selected by completion above; growth additionally gates on the
            // YOUNGEST tracked use across both slots.
            auto wait_for_youngest = [&]() -> std::error_code {
                const std::span<const FenceCompletionValue> values{m_slot_values.data(), m_slot_values.size()};
                const FenceCompletionValue youngest = maxSlotValue(values);
                if (youngest != kFenceValueNone and youngest > completed) {
                    ++m_growth_wait_count;
                    PPR_RETURN_ERROR_ON_FAIL(UI, wait_for_fence(youngest));
                }
                return default_value_v;
            };

            if (not resources.m_vertex_buffer or
                resources.m_vertex_buffer->getDesc().size < static_cast<u64>(total_vtx_count) * sizeof(ImDrawVert)) {
                PPR_RETURN_ERROR_ON_FAIL(UI, wait_for_youngest());
                const u32 new_capacity = alignForward(total_vtx_count, 8192_u32);
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawVert);

                rhi::BufferDesc vb_desc{};
                vb_desc.size = size_bytes;
                vb_desc.usage = enumCombine(rhi::BufferUsage::VertexBuffer, rhi::BufferUsage::CopySource);
                vb_desc.memoryType = rhi::MemoryType::Upload;
                vb_desc.defaultState = rhi::ResourceState::General;
                vb_desc.label = "imgui_vertex_buffer";

                PPR_RETURN_ERROR_ON_FAIL(
                    UI, device.createBuffer(vb_desc, nullptr, resources.m_vertex_buffer.writeRef()));
            }

            if (not resources.m_index_buffer or
                resources.m_index_buffer->getDesc().size < static_cast<u64>(total_idx_count) * sizeof(ImDrawIdx)) {
                PPR_RETURN_ERROR_ON_FAIL(UI, wait_for_youngest());
                const u32 new_capacity = alignForward(total_idx_count, 16384_u32);
                const u64 size_bytes = static_cast<u64>(new_capacity) * sizeof(ImDrawIdx);

                rhi::BufferDesc ib_desc{};
                ib_desc.size = size_bytes;
                ib_desc.usage = enumCombine(rhi::BufferUsage::IndexBuffer, rhi::BufferUsage::CopySource);
                ib_desc.memoryType = rhi::MemoryType::Upload;
                ib_desc.defaultState = rhi::ResourceState::General;
                ib_desc.label = "imgui_index_buffer";

                PPR_RETURN_ERROR_ON_FAIL(
                    UI, device.createBuffer(ib_desc, nullptr, resources.m_index_buffer.writeRef()));
            }

            void *mapped_vtx = nullptr;
            PPR_RETURN_ERROR_ON_FAIL(
                UI, device.mapBuffer(resources.m_vertex_buffer.get(), rhi::CpuAccessMode::Write, &mapped_vtx));

            void *mapped_idx = nullptr;
            PPR_RETURN_ERROR_ON_FAIL(
                UI, device.mapBuffer(resources.m_index_buffer.get(), rhi::CpuAccessMode::Write, &mapped_idx));

            // Layout-identical copy (see the static_assert at detach): the
            // detached vertices ARE ImDrawVerts without the imgui type.
            std::memcpy(
                mapped_vtx, detached.m_vertices.data(),
                checked_cast<std::size_t>(total_vtx_count) * sizeof(DetachedUiVertex));
            if constexpr (sizeof(ImDrawIdx) == 2u) {
                auto *idx_dst = static_cast<u16 *>(mapped_idx);
                for (const u32 index: detached.m_indices) {
                    *idx_dst++ = static_cast<u16>(index);
                }
            } else {
                std::memcpy(
                    mapped_idx, detached.m_indices.data(), checked_cast<std::size_t>(total_idx_count) * sizeof(u32));
            }

            device.unmapBuffer(resources.m_vertex_buffer.get());
            device.unmapBuffer(resources.m_index_buffer.get());

            return default_value_v;
        }

        [[nodiscard]] ImGuiKey keyboardKeyToImGuiKey(EKeyboardKey key) noexcept;

        [[nodiscard]] ImGuiKey gamepadButtonToImGuiKey(EGamepadButton button) noexcept;

        [[nodiscard]] int mouseButtonToImGui(EMouseButton button) noexcept;

        std::error_code ImGuiService::createInvariantRenderState_(rhi::IDevice &device) {
            constexpr rhi::InputElementDesc elements[] = {
                {"POSITION", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, pos), 0},
                {"TEXCOORD", 0, rhi::Format::RG32Float, PPR_OFFSETOF(ImDrawVert, uv), 0},
                {"COLOR", 0, rhi::Format::RGBA8Unorm, PPR_OFFSETOF(ImDrawVert, col), 0},
            };

            PPR_RETURN_ERROR_ON_FAIL(
                UI,
                device.createInputLayout(safe_narrowing(sizeof(ImDrawVert)), elements, 3u, m_vertex_layout.writeRef()));

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
            shader::SharedModule shader_module{};
            PPR_RETURN_ERROR_ON_FAIL(
                UI,
                shader_service.loadModuleFromSource("imgui", "imgui.slang", kImGuiShader, shader_module.writeRef()));

            shader::ComPtr<shader::IEntryPoint> vertex_ep;
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_module->findEntryPointByName("vertexMain", vertex_ep.writeRef()));

            shader::ComPtr<shader::IEntryPoint> fragment_ep;
            PPR_RETURN_ERROR_ON_FAIL(UI, shader_module->findEntryPointByName("fragmentMain", fragment_ep.writeRef()));

            shader::IComponentType *entryPoints[] = {vertex_ep.get(), fragment_ep.get()};

            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = shader_module.get();
            program_desc.slangEntryPoints = entryPoints;
            program_desc.slangEntryPointCount = std::size(entryPoints);

            shader::Diagnose diagnostics;
            PPR_RETURN_ERROR_ON_FAIL(
                UI, device.createShaderProgram(program_desc, m_shader_program.writeRef(), diagnostics.writeRef()));

            return default_value_v;
        }

        std::error_code ImGuiService::createFontTexture_(rhi::IDevice &device, rhi::ICommandQueue &device_queue) {
            rhi::ComPtr<rhi::ITexture> texture{};
            rhi::ComPtr<rhi::ITextureView> view{};
            PPR_RETURN_ERROR_ON_FAIL(UI, uploadFontAtlas_(device, device_queue, texture, view));

            m_font_texture = std::move(texture);
            m_font_texture_view = std::move(view);
            m_font_scale = float2{ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y};

            // TexID is frozen here, once, on the client lane: every later
            // detached command carries this identity, and the encode lane
            // never rewrites it (no live frontend writes off the client).
            m_font_texid_init = static_cast<u64>(reinterpret_cast<std::uintptr_t>(m_font_texture_view.get()));
            ImGui::GetIO().Fonts->SetTexID(m_font_texture_view.get());
            m_font_staged_version = 1u;
            m_font_installed_version.store(1u, std::memory_order_release);

            PPR_LOG(
                UI, info, "font texture created",
                {
                    {"width", m_font_texture->getDesc().size.width},
                    {"height", m_font_texture->getDesc().size.height},
                });
            return default_value_v;
        }

        std::error_code ImGuiService::uploadFontAtlas_(
            rhi::IDevice &device, rhi::ICommandQueue &device_queue, rhi::ComPtr<rhi::ITexture> &out_texture,
            rhi::ComPtr<rhi::ITextureView> &out_view) {
            ImGuiIO &io = ImGui::GetIO();

            u8 *pixels = nullptr;
            int width = 0, height = 0, bpp = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);

            // NOTE: height feeds an integer division below (staging row pitch); reject a
            // degenerate atlas with an error instead of faulting on division by zero.
            if (pixels == nullptr or width <= 0 or height <= 0) [[unlikely]] {
                PPR_LOG(
                    UI, error, "font atlas has no pixels",
                    {
                        {"width", width},
                        {"height", height},
                    });
                return std::make_error_code(std::errc::invalid_argument);
            }

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

            const u64 pixelDataSize = static_cast<u64>(width) * static_cast<u64>(height) * 4u;
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
                texture.get(), 0, 0, dst_offset, staging_buffer.get(), 0, pixelDataSize,
                static_cast<u32>(pixelDataSize / static_cast<u64>(height)),
                {static_cast<u32>(width), static_cast<u32>(height), 1});

            encoder->setTextureState(
                texture.get(), rhi::SubresourceRange{.layer = 0, .layerCount = 1, .mip = 0, .mipCount = 1},
                rhi::ResourceState::ShaderResource);

            rhi::ComPtr<rhi::ICommandBuffer> cmd_buffer;
            PPR_RETURN_ERROR_ON_FAIL(UI, encoder->finish(cmd_buffer.writeRef()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.submit(cmd_buffer.get()));
            PPR_RETURN_ERROR_ON_FAIL(UI, device_queue.waitOnHost());

            staging_buffer.setNull();

            rhi::ComPtr<rhi::ITextureView> view;
            PPR_RETURN_ERROR_ON_FAIL(UI, texture->getDefaultView(view.writeRef()));

            // The upload's own queue wait above already drained prior
            // submissions, so the atlas is safe to sample on return; the
            // caller installs it (and drops the previous one) directly.
            out_texture = std::move(texture);
            out_view = std::move(view);
            return default_value_v;
        }

        std::error_code ImGuiService::createRenderPipeline_(
            rhi::IDevice &device, const RenderPipelineSignature &signature) {
            // The sample count is a SampleCount by construction — all four
            // enumerators are accepted — so only the color shape and the
            // absent depth attachment are checked.
            if (signature.colorFormats().size() != 1u || signature.m_depth_stencil_format.has_value()) {
                PPR_LOG(
                    UI, error, "unsupported render pipeline signature",
                    {
                        {"color_format_count", signature.colorFormats().size()},
                        {"has_depth_stencil", signature.m_depth_stencil_format.has_value()},
                        {"sample_count", static_cast<u32>(signature.m_sample_count)},
                    });
                return make_error_code(std::errc::invalid_argument);
            }

            rhi::ColorTargetDesc color_target{};
            color_target.format = signature.colorFormats().front();
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
            pipeline_desc.multisample.sampleCount = static_cast<u32>(signature.m_sample_count);

            rhi::ComPtr<rhi::IRenderPipeline> pipeline{};
            PPR_RETURN_ERROR_ON_FAIL(UI, device.createRenderPipeline(pipeline_desc, pipeline.writeRef()));

            // Per-slot roots, each carrying the font bindings plus its own
            // scale/offset cursor. Creation-time bindings are safe: no
            // submission can reference objects that did not exist a moment
            // ago. The previous pipeline/roots (if any) defer with the
            // latest submission that could reference them.
            std::array<rhi::ComPtr<rhi::IShaderObject>, kUiFrameSlots> slot_roots{};
            for (u32 slot = 0u; slot < kUiFrameSlots; ++slot) {
                rhi::ComPtr<rhi::IShaderObject> root{};
                PPR_RETURN_ERROR_ON_FAIL(
                    UI, make_error_code(device.createRootShaderObject(pipeline->getProgram(), root.writeRef())));
                rhi::ShaderCursor cursor{root.get()};
                PPR_RETURN_ERROR_ON_FAIL(UI, cursor["g_fontSampler"].setBinding(m_font_sampler));
                PPR_RETURN_ERROR_ON_FAIL(UI, cursor["g_fontTexture"].setBinding(m_font_texture_view));
                slot_roots[slot] = std::move(root);
            }

            if (m_render_pipeline) {
                rhi::ComPtr<rhi::IRenderPipeline> retired_pipeline = std::move(m_render_pipeline);
                std::array<rhi::ComPtr<rhi::IShaderObject>, kUiFrameSlots> retired_roots = std::move(m_slot_roots);
                const FenceCompletionValue last_use = m_last_submitted_value;
                m_deferred_releases.defer(
                    last_use,
                    [held_pipeline = std::move(retired_pipeline), held_roots = std::move(retired_roots)]() mutable {
                        held_pipeline.setNull();
                        for (rhi::ComPtr<rhi::IShaderObject> &root: held_roots) {
                            root.setNull();
                        }
                    });
            }

            m_render_pipeline = std::move(pipeline);
            m_slot_roots = std::move(slot_roots);
            return default_value_v;
        }

        void ImGuiService::reclaimCompleted_(const FenceCompletionValue completed) noexcept {
            std::ignore = m_deferred_releases.reclaim(completed);
        }

        void ImGuiService::drainPendingDeselect_() {
            if (not m_pending_deselect) {
                return;
            }
            m_pending_deselect = false;
            if (const std::error_code err = clearWindowFocus(); err) {
                PPR_LOG(UI, warning, "drain pending deselect failed");
            }
        }

        EInputMessageResponse ImGuiService::onInputCharacter_(const hal::native::char_t codepoint) noexcept {
            // Ctrl+key produces a control character (e.g. Ctrl+A -> 0x01), not text.
            if (ImGui::GetIO().KeyCtrl) {
                return EInputMessageResponse::unhandled;
            }
            if (ImGuiIO &io = ImGui::GetIO(); io.WantTextInput) {
                char8_t utf8_input[8]{};
                const std::size_t utf8_len = hal::native::utf8(
                    {&codepoint, 1u}, utf8_input, std::size(utf8_input) - 1u /*\0*/);

                if (utf8_len < std::size(utf8_input)) {
                    utf8_input[utf8_len] = u8'\0';
                    io.AddInputCharactersUTF8(reinterpret_cast<const char *>(&utf8_input[0]));
                    return EInputMessageResponse::consumed;
                }
            }

            return EInputMessageResponse::unhandled;
        }

        void ImGuiService::onInputAnyDigital_(const InputActionEvent &event, const InputKey &trigger) noexcept {
            std::visit(
                overloaded(
                    [&](const EKeyboardKey keyboard_key) noexcept {
                        if (const ImGuiKey imgui_key = keyboardKeyToImGuiKey(keyboard_key);
                            imgui_key != ImGuiKey_None) {
                            const bool is_key_down = event.getDigitalValue();

                            ImGuiIO &io = ImGui::GetIO();
                            io.AddKeyEvent(imgui_key, is_key_down);

                            // handle modifier keys:
                            switch (keyboard_key) {
                                case EKeyboardKey::left_control:
                                case EKeyboardKey::right_control: io.AddKeyEvent(ImGuiMod_Ctrl, is_key_down); break;
                                case EKeyboardKey::left_shift:
                                case EKeyboardKey::right_shift: io.AddKeyEvent(ImGuiMod_Shift, is_key_down); break;
                                case EKeyboardKey::left_alt:
                                case EKeyboardKey::right_alt: io.AddKeyEvent(ImGuiMod_Alt, is_key_down); break;
                                case EKeyboardKey::left_super:
                                case EKeyboardKey::right_super: io.AddKeyEvent(ImGuiMod_Super, is_key_down); break;
                                default: break;
                            }
                        }
                    },
                    [&](const EGamepadButton gamepad_button) noexcept {
                        if (const ImGuiKey imgui_key = gamepadButtonToImGuiKey(gamepad_button);
                            imgui_key != ImGuiKey_None) {
                            ImGui::GetIO().AddKeyEvent(imgui_key, event.getDigitalValue());
                        }
                    },
                    [&](const EMouseButton mouse_button) noexcept {
                        if (const ImGuiMouseButton imgui_button = mouseButtonToImGui(mouse_button); imgui_button >= 0) {
                            ImGui::GetIO().AddMouseButtonEvent(imgui_button, event.getDigitalValue());
                        }
                    },
                    [](auto) noexcept { std::unreachable(); }),
                trigger.m_code);
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
                {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickUp,
                    ImGuiKey_GamepadLStickDown},
                {ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, ImGuiKey_GamepadRStickUp,
                    ImGuiKey_GamepadRStickDown},
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

    // Lane/main split handoffs for the Editor teardown (external linkage
    // so the Editor implementation unit can call them without seeing the
    // anonymous concrete type): each casts to the concrete service and runs
    // exactly one half on the caller's lane. The caller owns affinity —
    // Detached + Context halves on main, GPU half on the owner lane after
    // quiescence — and checks acknowledgement for the lane half.
    [[nodiscard]] std::error_code shutdownImGuiFrontendDetachedOnMain(IUIService &ui) {
        auto *impl = dynamic_cast<ImGuiService *>(&ui);
        if (not impl) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return impl->shutdownFrontendDetachedOnMain();
    }

    [[nodiscard]] std::error_code releaseImGuiGpuResourcesOnOwnerLane(IUIService &ui) {
        auto *impl = dynamic_cast<ImGuiService *>(&ui);
        if (not impl) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return impl->shutdownGpuResourcesOnOwnerLane();
    }

    [[nodiscard]] std::error_code shutdownImGuiFrontendContextOnMain(IUIService &ui) {
        auto *impl = dynamic_cast<ImGuiService *>(&ui);
        if (not impl) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        return impl->shutdownFrontendContextOnMain();
    }

    namespace ui {
        std::unique_ptr<IUIService> createImGuiService() {
            return std::make_unique<ImGuiService>();
        }
    }
}
