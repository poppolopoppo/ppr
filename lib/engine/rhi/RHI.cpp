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
            [[nodiscard]] const char *name() const noexcept override { return "slang-rhi"; }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<Slang::Result>(ev)) {
                    case SLANG_OK: return "indicates success";
                    case SLANG_FAIL: return "generic failure code - meaning a serious error occurred and the call couldn't complete";
                    case SLANG_E_NOT_IMPLEMENTED: return "functionality is not implemented";
                    case SLANG_E_NO_INTERFACE: return "interface not be found";
                    case SLANG_E_ABORT: return "operation was aborted (did not correctly complete)";
                    case SLANG_E_INVALID_HANDLE: return "indicates that a handle passed in as parameter to a method is invalid";
                    case SLANG_E_INVALID_ARG: return "indicates that an argument passed in as parameter to a method is invalid";
                    case SLANG_E_OUT_OF_MEMORY: return "operation could not complete - ran out of memory";
                    case SLANG_E_BUFFER_TOO_SMALL: return "supplied buffer is too small to be able to complete";
                    case SLANG_E_UNINITIALIZED: return "used to identify a Result that has yet to be initialized";
                    case SLANG_E_PENDING: return
                                "returned from an async method meaning the output is invalid (thus an error), but a result for the request is pending, and will be returned on a subsequent call with the async handle.";
                    case SLANG_E_CANNOT_OPEN: return "indicates a file/resource could not be opened";
                    case SLANG_E_NOT_FOUND: return "indicates that a file/resource could not be found";
                    case SLANG_E_INTERNAL_FAIL: return "an unhandled internal failure (typically from unhandled exception)";
                    case SLANG_E_NOT_AVAILABLE: return "could not complete because some underlying feature (hardware or software) was not available";
                    case SLANG_E_TIME_OUT: return "could not complete because the operation times out";

                    default: return std::format("unknown slang-rhi result ({})", ev);
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                return slangRhiErrorCondition_(*this, ev);
            }

            [[nodiscard]] static std::error_condition slangRhiErrorCondition_(const std::error_category &category, const int ev) noexcept {
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

        constexpr SlangRhiErrorCategory g_slang_rhi_error_category{};
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
                rhi::DebugMessageType type,
                rhi::DebugMessageSource source,
                const char *message) override {
                const std::source_location loc = std::source_location::current();
                auto &category = details::log::RHI();

                std::string_view source_name{};
                switch (source) {
                        using enum rhi::DebugMessageSource;
                    case Layer:
                        source_name = "layer";
                        break;
                    case Driver:
                        source_name = "driver";
                        break;
                    case Slang:
                        source_name = "slang";
                        break;
                }

                auto level = Log::ELevel::info;
                switch (type) {
                        using enum rhi::DebugMessageType;
                    case Info:
                        level = Log::ELevel::info;
                        break;
                    case Warning:
                        level = Log::ELevel::warning;
                        break;
                    case Error:
                        level = Log::ELevel::error;
                        break;
                }

                Log::logRaw({category, level, loc}, message, {{"source", source_name}});
            }
        };

        [[nodiscard]] string_literal getDeviceTypeName_(const rhi::DeviceType device_type) noexcept {
            switch (device_type) {
                    using enum rhi::DeviceType;
                case Default:
                    return "default";
                case D3D11:
                    return "d3d11";
                case D3D12:
                    return "d3d12";
                case Vulkan:
                    return "vulkan";
                case Metal:
                    return "metal";
                case CPU:
                    return "cpu";
                case CUDA:
                    return "cuda";
                case WGPU:
                    return "wgpu";
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
                case D3D12:
                    // P3: bindless .Handle programs require SM6.6 descriptor
                    // heaps, which FXC/DXBC cannot compile (X3004). D3D12 goes
                    // DXIL; D3D11 stays DXBC (no bindless there). Gate 4 reviews
                    // any fallout on pre-existing shaders.
                    return SLANG_DXIL;
                case D3D11:
                    return SLANG_DXBC;
                case Vulkan:
                case WGPU:
                    return SLANG_SPIRV;
                case Metal:
                    return SLANG_METAL;
                case CPU:
                    return SLANG_SHADER_HOST_CALLABLE;
                case CUDA:
                    return SLANG_CUDA_OBJECT_CODE;
            }
            std::unreachable();
        }
    }

    // ------------------------------------------------------------------
    // projection free functions
    // ------------------------------------------------------------------

    [[nodiscard]] float4x4 rhi::getOrthoMatrix(
        const float width,
        const float height) noexcept {
        return float4x4::orthoD3D(0.0f, width, 0.0f, height, 0.0f, 1.0f);
    }

    [[nodiscard]] float4x4 rhi::getPerspectiveMatrix(
        const float fov,
        const float aspect,
        const float near_,
        const float far_) noexcept {
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

            [[nodiscard]] rhi::IRHI &getInstance() const noexcept override {
                return *slang_rhi::getRHI();
            }

            [[nodiscard]] rhi::IDevice &getDevice() const noexcept override {
                return *m_device;
            }

            [[nodiscard]] std::error_code initialize(
                const rhi::DeviceType device_type,
                IShaderService &shader_service) override {
                if (m_device) {
                    PPR_LOG(RHI, warning, "RHI already initialized");
                    return rhi::errc::ok;
                }

                rhi::IRHI *const p_instance = slang_rhi::getRHI();
                if (not p_instance) {
                    PPR_LOG(RHI, error, "failed to get RHI instance");
                    return rhi::errc::no_interface;
                }

#if PPR_ENABLE_DEBUG
                PPR_LOG(RHI, info, "enabled Slang RHI debug layers", {
                    {"coreValidation", true},
                    {"GPUAssistedValidation", true}
                });

                PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->setDebugLayerOptions({
                    .required = true,
                    .coreValidation = true,
                    .GPUAssistedValidation = true
                }));

                p_instance->enableDebugLayers();
#endif

                constexpr slang_rhi::Feature required_features[] = {
                    slang_rhi::Feature::Surface,
                    slang_rhi::Feature::Rasterization,
                };

                rhi::DeviceDesc desc{};
                desc.deviceType = device_type;
                desc.slang.slangGlobalSession = shader_service.getGlobalSession();

                desc.requiredFeatures = required_features;
                desc.requiredFeatureCount = safe_narrowing(std::size(required_features));

                // P2 bindless budget (§4): create-time only. Feature::Bindless
                // stays OUT of requiredFeatures (would fail weak hardware);
                // pass caches check hasFeature(Bindless) at init instead.
                desc.bindless.textureCount = rhi::kBindlessTextureBudget;
                desc.bindless.combinedTextureSamplerCount = rhi::kBindlessCombinedBudget;
                desc.bindless.samplerCount = rhi::kBindlessSamplerBudget;
                desc.bindless.bufferCount = rhi::kBindlessBufferBudget;

#if PPR_ENABLE_DEBUG
                desc.enableValidation = true;
#endif
#if PPR_ENABLE_LOGGING
                desc.debugCallback = SlangRhiDebugCallback::get();
#endif

                rhi::ComPtr<rhi::IDevice> device;
                PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->createDevice(desc, device.writeRef()));

                if (not
                    device->hasFeature(rhi::Feature::Bindless))
                {
                    PPR_LOG(RHI, warning, "device lacks bindless support; GPU caches will fail at init", {
                        {"device_type", getDeviceTypeName_(device_type)}
                    });
                }

                const SlangCompileTarget compile_target = toSlangCompileTarget_(device->getDeviceType());

                PPR_RETURN_ERROR_ON_FAIL(RHI, shader_service.setTargetFormat(compile_target));

                m_device = std::move(device);
                PPR_LOG(RHI, info, "RHI device created successfully", {
                    {"device_type", getDeviceTypeName_(device_type)}
                });
                return make_error_code(SLANG_OK);
            }

            [[nodiscard]] std::error_code shutdown() override {
                if (m_device) {
                    PPR_LOG(RHI, info, "RHI service shut down");
                    m_device.setNull();
                }
                return default_value_v;
            }

            [[nodiscard]] rhi::Result createRenderPipeline(
                const rhi::RenderPipelineDesc &desc,
                rhi::IRenderPipeline **outPipeline) override {
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
