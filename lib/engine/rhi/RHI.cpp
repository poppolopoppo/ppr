module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-rhi.h>

module engine.rhi;

import std;
import engine.core;
import engine.math;
import engine.shader;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(RHI, info, none)

    namespace {
        // ------------------------------------------------------------------
        // slang-rhi error codes
        // ------------------------------------------------------------------

        class SlangRhiErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "slang-rhi";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<Slang::Result>(ev)) {
                    case SLANG_OK: return "indicates success";
                    case SLANG_FAIL:
                        return "generic failure code - meaning a serious error occurred and the call couldn't complete";
                    case SLANG_E_NOT_IMPLEMENTED: return "functionality is not implemented";
                    case SLANG_E_NO_INTERFACE: return "interface not be found";
                    case SLANG_E_ABORT: return "operation was aborted (did not correctly complete)";
                    case SLANG_E_INVALID_HANDLE:
                        return "indicates that a handle passed in as parameter to a method is invalid";
                    case SLANG_E_INVALID_ARG:
                        return "indicates that an argument passed in as parameter to a method is invalid";
                    case SLANG_E_OUT_OF_MEMORY: return "operation could not complete - ran out of memory";
                    case SLANG_E_BUFFER_TOO_SMALL: return "supplied buffer is too small to be able to complete";
                    case SLANG_E_UNINITIALIZED: return "used to identify a Result that has yet to be initialized";
                    case SLANG_E_PENDING:
                        return "returned from an async method meaning the output is invalid (thus an error), but a result for the request is pending, and will be returned on a subsequent call with the async handle.";
                    case SLANG_E_CANNOT_OPEN: return "indicates a file/resource could not be opened";
                    case SLANG_E_NOT_FOUND: return "indicates that a file/resource could not be found";
                    case SLANG_E_INTERNAL_FAIL:
                        return "an unhandled internal failure (typically from unhandled exception)";
                    case SLANG_E_NOT_AVAILABLE:
                        return "could not complete because some underlying feature (hardware or software) was not available";
                    case SLANG_E_TIME_OUT: return "could not complete because the operation times out";

                    default: return std::string("unknown slang-rhi result (") + std::to_string(ev) + ")";
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                return slangRhiErrorCondition_(*this, ev);
            }

            [[nodiscard]] static std::error_condition slangRhiErrorCondition_(
                const std::error_category &category, const int ev) noexcept {
                switch (static_cast<Slang::Result>(ev)) {
                    case SLANG_E_INVALID_ARG: return std::errc::invalid_argument;
                    case SLANG_E_OUT_OF_MEMORY: return std::errc::not_enough_memory;
                    case SLANG_E_NOT_FOUND: return std::errc::no_such_file_or_directory;
                    case SLANG_E_TIME_OUT: return std::errc::timed_out;
                    case SLANG_E_NOT_IMPLEMENTED: return std::errc::function_not_supported;
                    case SLANG_E_BUFFER_TOO_SMALL: return std::errc::result_out_of_range;
                    default: return {ev, category};
                }
            }
        };

        static const SlangRhiErrorCategory g_slang_rhi_error_category{};
    }

    [[nodiscard]] const std::error_category &rhi::error_category() noexcept {
        return g_slang_rhi_error_category;
    }

    [[nodiscard]] std::error_code rhi::make_error_code(const Result result) noexcept {
        return SLANG_SUCCEEDED(result) ? std::error_code{} : std::error_code{result, g_slang_rhi_error_category};
    }

    [[nodiscard]] std::error_code rhi::make_error_code(const errc error_code) noexcept {
        return make_error_code(static_cast<Result>(error_code));
    }

    // ------------------------------------------------------------------
    // pass debug messages to our own logger
    // ------------------------------------------------------------------

#if PPR_ENABLE_LOGGING
    namespace {
        class SlangRhiDebugCallback : public rhi::IDebugCallback {
        protected:
            ~SlangRhiDebugCallback() = default;

        public:
            [[nodiscard]] static IDebugCallback *get() noexcept {
                static SlangRhiDebugCallback g_instance{};
                return &g_instance;
            }

            void handleMessage(
                rhi::DebugMessageType type, rhi::DebugMessageSource source, const char *message) override {
                const std::source_location loc = std::source_location::current();
                auto &category = details::log::RHI();

                std::string_view source_name{};
                switch (source) {
                    using enum rhi::DebugMessageSource;
                    case Layer: source_name = "layer"; break;
                    case Driver: source_name = "driver"; break;
                    case Slang: source_name = "slang"; break;
                }

                auto level = Log::ELevel::info;
                switch (type) {
                    using enum rhi::DebugMessageType;
                    case Info: level = Log::ELevel::info; break;
                    case Warning: level = Log::ELevel::warning; break;
                    case Error: level = Log::ELevel::error; break;
                }

                Log::logRaw({category, level, loc}, trim(std::string_view(message)), {{"source", source_name}});
            }
        };

        [[nodiscard]] string_literal getDeviceTypeName_(const rhi::DeviceType device_type) noexcept {
            switch (device_type) {
                using enum rhi::DeviceType;
                case Default: return "default";
                case D3D11: return "d3d11";
                case D3D12: return "d3d12";
                case Vulkan: return "vulkan";
                case Metal: return "metal";
                case CPU: return "cpu";
                case CUDA: return "cuda";
                case WGPU: return "wgpu";
            }
            std::unreachable();
        }
    }
#endif

    namespace {
        [[nodiscard]] SlangCompileTarget toSlangCompileTarget_(const rhi::DeviceType device_type) noexcept {
            switch (device_type) {
                using enum rhi::DeviceType;
                case Default:
                case D3D12: return SLANG_DXIL;
                case D3D11: return SLANG_DXBC;
                case Vulkan:
                case WGPU: return SLANG_SPIRV;
                case Metal: return SLANG_METAL;
                case CPU: return SLANG_SHADER_HOST_CALLABLE;
                case CUDA: return SLANG_CUDA_OBJECT_CODE;
            }
            std::unreachable();
        }
    }

    // ------------------------------------------------------------------
    // adapter selection
    // ------------------------------------------------------------------

    namespace {
        // slang-rhi's public AdapterInfo exposes no per-adapter extension
        // list, so unusable ICD bridges are identified by name. The Microsoft
        // Direct3D12 (DZN) bridge advertises itself as a Vulkan adapter but
        // lacks VK_KHR_shader_non_semantic_info, which the required feature
        // set and bindless budgets need, so vkCreateDevice fails on it. On
        // Ubuntu WSL the loader sorts it ahead of llvmpipe, so relying on
        // slang-rhi's first-listed default adapter selects the failing one.
        [[nodiscard]] bool isUsableAdapterName_(const std::string_view name) noexcept {
            return not name.contains("DZN") and not name.contains("Direct3D12");
        }

        // Preference order among usable adapters: hardware first, software
        // (llvmpipe/lavapipe) as the fallback.
        [[nodiscard]] int adapterSelectionRank_(const slang_rhi::AdapterInfo &info) noexcept {
            switch (info.adapterType) {
                using enum slang_rhi::AdapterType;
                case Discrete: return 0;
                case Integrated: return 1;
                case Software: return 2;
                case Unknown: return 3;
            }
            std::unreachable();
        }

// Both callers are info-level PPR_LOG sites: the definition must gate on the
// log threshold, not on the coarse PPR_ENABLE_LOGGING switch. At a WARNING
// threshold the caller-side gate erases info-level calls with their argument
// expressions, and this helper would otherwise compile unused on Release.
#if PPR_LOG_LEVEL_THRESHOLD <= PPR_LOG_LEVEL_INFO
        [[nodiscard]] string_literal getAdapterTypeName_(const slang_rhi::AdapterType type) noexcept {
            switch (type) {
                using enum slang_rhi::AdapterType;
                case Discrete: return "discrete";
                case Integrated: return "integrated";
                case Software: return "software";
                case Unknown: return "unknown";
            }
            std::unreachable();
        }
#endif

        // Only the Vulkan backend is probed for a usable adapter: the DZN
        // bridge that breaks device creation is a Vulkan ICD, and Vulkan is
        // the sole backend behind DeviceType::Default on Linux. Explicit
        // non-Vulkan types, and Default elsewhere (whose slang-rhi resolution
        // falls back across APIs), keep slang-rhi's own adapter selection.
        [[nodiscard]] std::optional<rhi::DeviceType> resolveProbeDeviceType_(
            const rhi::DeviceType device_type) noexcept {
            if (device_type == rhi::DeviceType::Vulkan) {
                return rhi::DeviceType::Vulkan;
            }
#if defined(__linux__)
            if (device_type == rhi::DeviceType::Default) {
                return rhi::DeviceType::Vulkan;
            }
#endif
            return std::nullopt;
        }

        struct AdapterSelection {
            rhi::IAdapter *m_selected{nullptr};
            u32 m_candidate_count{0};
            u32 m_rejected_count{0};
        };

        // Enumerate the resolved backend's adapters, log each candidate, and
        // return the best usable one. Leaves m_selected null when the backend
        // exposes no adapters, so the caller can fall back to slang-rhi's own
        // resolution.
        [[nodiscard]] AdapterSelection selectConformantAdapter_(
            rhi::IRHI &instance, const rhi::DeviceType device_type) {
            AdapterSelection selection{};
            int best_rank = -1;

            for (u32 index = 0;; ++index) {
                rhi::IAdapter *const adapter = instance.getAdapter(device_type, index);
                if (not adapter) {
                    break;
                }

                const slang_rhi::AdapterInfo &info = adapter->getInfo();
                const std::string_view name{info.name};
                const bool usable = isUsableAdapterName_(name);
                ++selection.m_candidate_count;

                PPR_LOG(
                    RHI, info, "RHI adapter candidate",
                    {
                        {"index", index},
                        {"name", name},
                        {"type", getAdapterTypeName_(info.adapterType)},
                        {"usable", usable},
                    });

                if (not usable) {
                    ++selection.m_rejected_count;
                    continue;
                }

                const int rank = adapterSelectionRank_(info);
                if (not selection.m_selected or rank < best_rank) {
                    selection.m_selected = adapter;
                    best_rank = rank;
                }
            }

            return selection;
        }
    }

    // ------------------------------------------------------------------
    // projection free functions
    // ------------------------------------------------------------------

    [[nodiscard]] float4x4 rhi::getOrthoMatrix(const float width, const float height) noexcept {
        return float4x4::orthoD3D(0.0f, width, 0.0f, height, 0.0f, 1.0f);
    }

    [[nodiscard]] float4x4 rhi::getPerspectiveMatrix(
        const float fov, const float aspect, const float near_, const float far_) noexcept {
        const float x_fov = 2.0f * std::atan(std::tan(fov * 0.5f) * aspect);
        return float4x4::perspectiveD3D(x_fov, fov, near_, far_);
    }

    // ------------------------------------------------------------------
    // slang RHI service
    // ------------------------------------------------------------------

    namespace {
        class SlangRhiService final : public IRhiService {
        public:
            rhi::ComPtr<rhi::IDevice> m_device{};
            // Ownership of the slang-rhi singleton. getRHI() hands us a
            // process-lifetime instance; destroyRHI() frees it. We may
            // destroy it exactly once — that single successful-initialise
            // owns it. A secondary shutdown()/getRHI() cycle (unit tests boot
            // and tear down GPU subsystems repeatedly) must NOT destroy it
            // again: the freed instance is poison and the next getRHI()/init
            // would hand the poisoned object back for member access.
            bool m_instance_owned{false};
            // Default off; set explicitly at the application boundary before initialize().
            rhi::DebugLayerOptions m_debug_options{
                .coreValidation = PPR_ENABLE_DEBUG,
#if PPR_ENABLE_SANITIZER_ADDRESS
                .GPUAssistedValidation = true,
#else
                .GPUAssistedValidation = false,
#endif
            };

            [[nodiscard]] rhi::IRHI &getInstance() const noexcept override {
                return *slang_rhi::getRHI();
            }

            [[nodiscard]] rhi::IDevice &getDevice() const noexcept override {
                return *m_device;
            }

            void setCoreValidation(const bool enabled) noexcept override {
                m_debug_options.coreValidation = enabled;
            }

            [[nodiscard]] bool isCoreValidationEnabled() const noexcept override {
                return m_debug_options.coreValidation;
            }

            void setGpuAssistedValidation(const bool enabled) noexcept override {
                m_debug_options.GPUAssistedValidation = enabled;
            }

            [[nodiscard]] bool isGpuAssistedValidationEnabled() const noexcept override {
                return m_debug_options.GPUAssistedValidation;
            }

            [[nodiscard]] std::error_code initialize(
                const rhi::DeviceType device_type, IShaderService &shader_service) override {
                const hal::ProfileScope scope{"RHI.Initialize"};
                if (m_device) {
                    PPR_LOG(RHI, warning, "RHI already initialized");
                    return rhi::errc::ok;
                }

                rhi::IRHI *const p_instance = slang_rhi::getRHI();
                if (not p_instance) {
                    PPR_LOG(RHI, error, "failed to get RHI instance");
                    return rhi::errc::no_interface;
                }

                rhi::ComPtr<rhi::IDevice> device{};
                bool initialized = false;
                PPR_DEFER {
                    if (not initialized) {
                        device.setNull();
                        const ::slang_rhi::Result result = ::slang_rhi::destroyRHI();
                        const std::error_code cleanup_error = make_error_code(result);
                        if (cleanup_error) {
                            PPR_LOG(
                                RHI, error, "RHI initialization cleanup failed",
                                {{"category", cleanup_error.category().name()}, {"cause", cleanup_error.message()}});
                        }
                        m_instance_owned = false;
                    }
                };

                constexpr slang_rhi::Feature required_features[] = {
                    slang_rhi::Feature::Surface,
                    slang_rhi::Feature::Rasterization,
                };

                rhi::DeviceDesc desc{};
                desc.deviceType = device_type;
                desc.slang.slangGlobalSession = shader_service.getGlobalSession();

                // PPR does not launch CUDA kernels from graphics command buffers.
                // VK_NVX_binary_import fails device creation on some NVIDIA drivers.
                desc.enableCUDALaunchFromGfx = false;

                desc.requiredFeatures = required_features;
                desc.requiredFeatureCount = safe_narrowing(std::size(required_features));

                // Bindless budget: create-time only. Feature::Bindless
                // stays OUT of requiredFeatures (would fail weak hardware);
                // pass caches check hasFeature(Bindless) at init instead.
                desc.bindless.textureCount = rhi::kBindlessTextureBudget;
                desc.bindless.combinedTextureSamplerCount = rhi::kBindlessCombinedBudget;
                desc.bindless.samplerCount = rhi::kBindlessSamplerBudget;
                desc.bindless.bufferCount = rhi::kBindlessBufferBudget;

                AdapterSelection selection{};
                if (const std::optional<rhi::DeviceType> probe_type = resolveProbeDeviceType_(device_type);
                    probe_type) {
                    selection = selectConformantAdapter_(*p_instance, *probe_type);

                    if (selection.m_candidate_count == 0) {
                        PPR_LOG(RHI, warning, "no RHI adapters enumerated; deferring to slang-rhi selection");
                    }
                }

                if (selection.m_selected) {
                    desc.adapter = selection.m_selected;
                    PPR_LOG(
                        RHI, info, "selected RHI adapter",
                        {
                            {"name", std::string_view{selection.m_selected->getInfo().name}},
                            {"type", getAdapterTypeName_(selection.m_selected->getInfo().adapterType)},
                        });
                }

                if (selection.m_candidate_count > 0 and not selection.m_selected) {
                    // Every enumerated adapter is a non-conformant bridge
                    // (e.g. DZN). Fail with a named cause instead of letting
                    // slang-rhi pick the first adapter and surface an opaque
                    // vkCreateDevice extension error.
                    PPR_LOG(
                        RHI, error, "no usable RHI adapter; set VK_ICD_FILENAMES to a conformant ICD",
                        {{"device_type", getDeviceTypeName_(device_type)}, {"rejected", selection.m_rejected_count}});
                    PPR_RETURN_ERROR_ON_FAIL(RHI, rhi::errc::not_available);
                }

#if PPR_ENABLE_LOGGING
                if (m_debug_options.isDebugLayersEnabled()) {
                    // Effective opt-in state: off unless the application boundary
                    // set it before initialize() (--gpu-validation).
                    PPR_LOG(
                        RHI, info, "enabled Slang RHI debug and validation layers",
                        {{"coreValidation", m_debug_options.coreValidation},
                            { "GPUAssistedValidation",
                                m_debug_options.GPUAssistedValidation }});

                    PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->setDebugLayerOptions(m_debug_options));

                    p_instance->enableDebugLayers();

                    desc.enableValidation = true;
                    desc.debugCallback = SlangRhiDebugCallback::get();
                }
#endif

                {
                    const hal::ProfileScope scope_create{"RHI.CreateDevice"};
                    PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->createDevice(desc, device.writeRef()));
                }

                if (not device->hasFeature(rhi::Feature::Bindless)) {
                    PPR_LOG(
                        RHI, warning, "device lacks bindless support; GPU caches will fail at init",
                        {{"device_type", getDeviceTypeName_(device_type)}});
                }

                const SlangCompileTarget compile_target = toSlangCompileTarget_(device->getDeviceType());

                PPR_RETURN_ERROR_ON_FAIL(RHI, shader_service.setTargetFormat(compile_target));

                m_device = std::move(device);
                m_instance_owned = true;
                initialized = true;
                PPR_LOG(
                    RHI, info, "RHI device created successfully",
                    {
                        {"device_type", getDeviceTypeName_(device_type)},
                        {"compile_target", static_cast<int>(compile_target)},
                        {"bindless_textures", rhi::kBindlessTextureBudget},
                        {"bindless_combined", rhi::kBindlessCombinedBudget},
                        {"bindless_samplers", rhi::kBindlessSamplerBudget},
                        {"bindless_buffers", rhi::kBindlessBufferBudget},
                    });
                return make_error_code(SLANG_OK);
            }

            [[nodiscard]] std::error_code shutdown() override {
                if (m_device) {
                    m_device.setNull();
                }
                if (m_instance_owned) {
                    const ::slang_rhi::Result result = ::slang_rhi::destroyRHI();
                    m_instance_owned = false;
                    PPR_LOG(RHI, info, "RHI instance teardown", {{"result", static_cast<int>(result)}});
                    PPR_RETURN_ERROR_ON_FAIL(RHI, make_error_code(result));
                }

                PPR_LOG(RHI, info, "RHI service shut down");
                return default_value_v;
            }

            [[nodiscard]] rhi::Result createRenderPipeline(
                const rhi::RenderPipelineDesc &desc, rhi::IRenderPipeline **outPipeline) override {
                return m_device->createRenderPipeline(desc, outPipeline);
            }
        };
    }

    /*static*/
    safe_ptr<IRhiService> IRhiService::get() noexcept {
        static SlangRhiService g_instance{};
        return safe_ptr<IRhiService>(&g_instance);
    }
}
