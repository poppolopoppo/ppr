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

namespace pP::rhi {
    PPR_DEFINE_LOG_CATEGORY(RHI, info, none)

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
                case SLANG_E_NOT_FOUND: return "indicates a file/resource could not be found";
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

    static constexpr SlangRhiErrorCategory g_slang_rhi_error_category{};

    [[nodiscard]] const std::error_category &error_category() noexcept {
        return g_slang_rhi_error_category;
    }

    [[nodiscard]] std::error_code make_error_code(const ::slang_rhi::Result result) noexcept {
        return SLANG_SUCCEEDED(result) ? std::error_code{} : std::error_code{result, g_slang_rhi_error_category};
    }

    [[nodiscard]] std::error_code make_error_code(const errc error_code) noexcept {
        return make_error_code(static_cast<::slang_rhi::Result>(error_code));
    }

    // ------------------------------------------------------------------
    // pass debug messages to our own logger
    // ------------------------------------------------------------------

#if PPR_ENABLE_LOGGING
    class SlangRhiDebugCallback : public IDebugCallback {
    protected:
        ~SlangRhiDebugCallback() = default;

    public:
        [[nodiscard]] static IDebugCallback *get() noexcept {
            static SlangRhiDebugCallback g_instance{};
            return &g_instance;
        }

        void handleMessage(
            slang_rhi::DebugMessageType type,
            slang_rhi::DebugMessageSource source,
            const char *message) override {
            const std::source_location loc = std::source_location::current();
            auto &category = details::log::RHI();

            std::string_view source_name{};
            switch (source) {
                case slang_rhi::DebugMessageSource::Layer:
                    source_name = "layer";
                    break;
                case slang_rhi::DebugMessageSource::Driver:
                    source_name = "driver";
                    break;
                case slang_rhi::DebugMessageSource::Slang:
                    source_name = "slang";
                    break;
            }

            auto level = Log::ELevel::info;
            switch (type) {
                case slang_rhi::DebugMessageType::Info:
                    level = Log::ELevel::info;
                    break;
                case slang_rhi::DebugMessageType::Warning:
                    level = Log::ELevel::warning;
                    break;
                case slang_rhi::DebugMessageType::Error:
                    level = Log::ELevel::error;
                    break;
            }

            Log::logRaw(
                {category, level, loc},
                message,
                {{"source", source_name}});
        }
    };

    [[nodiscard]] static string_literal getDeviceTypeName_(const DeviceType device_type) noexcept {
        switch (device_type) {
            case DeviceType::Default:
                return "default";
            case DeviceType::D3D11:
                return "d3d11";
            case DeviceType::D3D12:
                return "d3d12";
            case DeviceType::Vulkan:
                return "vulkan";
            case DeviceType::Metal:
                return "metal";
            case DeviceType::CPU:
                return "cpu";
            case DeviceType::CUDA:
                return "cuda";
            case DeviceType::WGPU:
                return "wgpu";
        }
        std::unreachable();
    }
#endif

    [[nodiscard]] SlangCompileTarget toSlangCompileTarget_(const DeviceType device_type) noexcept {
        switch (device_type) {
            case DeviceType::D3D11:
            case DeviceType::D3D12:
                return SLANG_DXBC;
            case DeviceType::Vulkan:
            case DeviceType::WGPU:
                return SLANG_SPIRV;
            case DeviceType::Metal:
                return SLANG_METAL;
            case DeviceType::CPU:
                return SLANG_SHADER_HOST_CALLABLE;
            case DeviceType::CUDA:
                return SLANG_CUDA_OBJECT_CODE;
        }
        std::unreachable();
    }

    // ------------------------------------------------------------------
    // projection free functions
    // ------------------------------------------------------------------

    [[nodiscard]] float4x4 getOrthoMatrix(
        const DeviceType type,
        const float width,
        const float height) noexcept {
        const auto conv = projectionConventionFromDeviceType(type);
        switch (conv) {
            case EProjectionConvention::D3D:
                return float4x4::orthoD3D(0.0f, width, height, 0.0f, -1.0f, 1.0f);
            case EProjectionConvention::VK:
                return float4x4::orthoVK(0.0f, width, 0.0f, height, -1.0f, 1.0f);
            default:
                return float4x4::orthoD3D(0.0f, width, height, 0.0f, -1.0f, 1.0f);
        }
    }

    [[nodiscard]] float4x4 getPerspectiveMatrix(
        const DeviceType type,
        const float fov,
        const float aspect,
        const float near_,
        const float far_) noexcept {
        const float yfov = fov;
        const float xfov = 2.0f * std::atan(std::tan(fov * 0.5f) * aspect);
        const auto conv = projectionConventionFromDeviceType(type);
        switch (conv) {
            case EProjectionConvention::D3D:
                return float4x4::perspectiveD3D(xfov, yfov, near_, far_);
            case EProjectionConvention::VK:
                return float4x4::perspectiveVK(xfov, yfov, near_, far_);
            default:
                return float4x4::perspectiveD3D(xfov, yfov, near_, far_);
        }
    }

    // ------------------------------------------------------------------
    // slang RHI service
    // ------------------------------------------------------------------

    class SlangRhiService : public IRhiService {
    public:
        ComPtr<IDevice> m_device{};

        [[nodiscard]] IRHI &getInstance() const noexcept override {
            return *slang_rhi::getRHI();
        }

        [[nodiscard]] IDevice &getDevice() const noexcept override {
            return *m_device;
        }

        [[nodiscard]] std::error_code initialize(
            const DeviceType device_type,
            slang::IGlobalSession *global_session) override {
            if (m_device) {
                PPR_LOG(RHI, warning, "RHI already initialized");
                return errc::ok;
            }

            IRHI *const p_instance = slang_rhi::getRHI();
            if (not p_instance) {
                PPR_LOG(RHI, error, "failed to get RHI instance");
                return errc::no_interface;
            }

#if PPR_ENABLE_DEBUG
            PPR_RETURN_ERROR_ON_FAIL(
                RHI,
                p_instance->setDebugLayerOptions({
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

            DeviceDesc desc{};
            desc.deviceType = device_type;
            desc.slang.slangGlobalSession = global_session;

            desc.requiredFeatures = required_features;
            desc.requiredFeatureCount = safe_narrowing(std::size(required_features));

#if PPR_ENABLE_DEBUG
            desc.enableValidation = true;
#endif
#if PPR_ENABLE_LOGGING
            desc.debugCallback = SlangRhiDebugCallback::get();
#endif

            ComPtr<IDevice> device;
            PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->createDevice(desc, device.writeRef()));

            m_device = std::move(device);

            const std::error_code target_ec =
                IShaderService::get()->setTargetFormat(toSlangCompileTarget_(m_device->getDeviceType()));
            if (target_ec) {
                PPR_LOG(RHI, error, "failed to configure shader compilation target", {
                    {"message", target_ec.message()}
                });
                return target_ec;
            }

            PPR_LOG(RHI, info, "RHI device created successfully", {
                    {"device_type", getDeviceTypeName_(device_type)}
            });
            return make_error_code(SLANG_OK);
        }

        [[nodiscard]] std::error_code shutdown() override {
            PPR_LOG(RHI, info, "RHI service shut down");

            m_device.setNull();

            PPR_RETURN_ERROR_ON_FAIL(RHI, ::slang_rhi::destroyRHI());
            return errc::ok;
        }

        [[nodiscard]] rhi::Result createRenderPipeline(
            const rhi::RenderPipelineDesc &desc,
            rhi::IRenderPipeline **outPipeline) override {
            return m_device->createRenderPipeline(desc, outPipeline);
        }
    };
}

namespace pP {
    /*static*/
    safe_ptr<IRhiService> IRhiService::get() noexcept {
        static rhi::SlangRhiService g_instance{};
        return safe_ptr<IRhiService>(&g_instance);
    }
}
