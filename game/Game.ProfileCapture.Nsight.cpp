module;

#include "pP/Macros.h"

#if PPR_HAS_NSIGHT_GRAPHICS_SDK
// Official NVIDIA Nsight Graphics SDK 0.9.2 headers (Apache-2.0). The
// profiling preset supplies the include path; no vendor ABI is guessed here.
#include <NGFX_GPUTrace_Common.h>
#include <NGFX_GPUTrace_Vulkan.h>
#endif

module game.profile_capture;

import engine.core;

import std;

namespace pP::game {
    PPR_DEFINE_LOG_CATEGORY(ProfileCaptureNsight, info, none)

#if PPR_HAS_NSIGHT_GRAPHICS_SDK
    // ------------------------------------------------------------------
    // vendor adapter
    // ------------------------------------------------------------------

    namespace {
        /// Vendor failures reach the driver as ordinary error codes with an
        /// actionable message; no vendor enum or header crosses the interface.
        class NsightResultCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "ppr.nsight-graphics";
            }

            [[nodiscard]] std::string message(const int condition) const override {
                switch (condition) {
                    case NGFX_Result_Success: return "success";
                    case NGFX_Result_NotImplemented: return "Nsight Graphics SDK: operation not implemented";
                    case NGFX_Result_LibNotFound: return "Nsight Graphics SDK: activity library not found";
                    case NGFX_Result_InvalidLib: return "Nsight Graphics SDK: invalid activity library";
                    case NGFX_Result_DifferentActivityInjected:
                        return "Nsight Graphics SDK: a different activity is already injected";
                    case NGFX_Result_InvalidParameter: return "Nsight Graphics SDK: invalid parameter";
                    case NGFX_Result_InvalidState:
                        return "Nsight Graphics SDK: GPU Trace is not injected or not initialized";
                    case NGFX_Result_Timeout: return "Nsight Graphics SDK: operation timed out";
                    case NGFX_Result_InsufficientBuffer: return "Nsight Graphics SDK: output buffer too small";
                    default: return std::format("Nsight Graphics SDK: unspecified error (result {})", condition);
                }
            }
        };

        [[nodiscard]] std::error_code nsightError(const NGFX_Result result) {
            static const NsightResultCategory category{};
            if (result == NGFX_Result_Success) {
                return {};
            }
            return std::error_code{static_cast<int>(result), category};
        }

        class NsightGraphicsProfileCaptureSdk final : public IProfileCaptureSdk {
        public:
            [[nodiscard]] bool available() const noexcept override {
                return true;
            }

            std::error_code initialize() override {
                bool injected = false;
                if (const auto result = nsightError(NGFX_GPUTrace_IsInjected(&injected))) {
                    PPR_LOG(
                        ProfileCaptureNsight, error, "Nsight GPU Trace injection query failed",
                        {{"error", result.message()}});
                    return result;
                }
                if (not injected) {
                    PPR_LOG(ProfileCaptureNsight, error, "Nsight GPU Trace is not injected into this process");
                    return std::make_error_code(std::errc::state_not_recoverable);
                }

                NGFX_GPUTrace_InitializeActivity_Vulkan_Params params{};
                params.version = NGFX_GPUTrace_InitializeActivity_Vulkan_Params_VER;
                if (const auto result = nsightError(NGFX_GPUTrace_InitializeActivity_Vulkan(&params))) {
                    PPR_LOG(
                        ProfileCaptureNsight, error, "Nsight GPU Trace Vulkan initialization failed",
                        {{"error", result.message()}});
                    return result;
                }

                m_initialized = true;
                PPR_LOG(ProfileCaptureNsight, info, "Nsight GPU Trace Vulkan activity initialized");
                return {};
            }

            std::error_code activate(void *const native_queue) override {
                if (native_queue == nullptr) {
                    return std::make_error_code(std::errc::invalid_argument);
                }

                NGFX_GPUTrace_ActivateTrace_Vulkan_Params params{};
                params.version = NGFX_GPUTrace_ActivateTrace_Vulkan_Params_VER;
                params.queue = static_cast<VkQueue>(native_queue);
                if (const auto result = nsightError(NGFX_GPUTrace_ActivateTrace_Vulkan(&params))) {
                    PPR_LOG(
                        ProfileCaptureNsight, error, "Nsight GPU Trace activation failed",
                        {{"error", result.message()}});
                    return result;
                }

                PPR_LOG(ProfileCaptureNsight, info, "Nsight GPU Trace activated on the device queue");
                return {};
            }

            std::error_code startTrace() override {
                NGFX_GPUTrace_StartTrace_Vulkan_Params params{};
                params.version = NGFX_GPUTrace_StartTrace_Vulkan_Params_VER;
                if (const auto result = nsightError(NGFX_GPUTrace_StartTrace_Vulkan(&params))) {
                    PPR_LOG(
                        ProfileCaptureNsight, error, "Nsight GPU Trace start failed", {{"error", result.message()}});
                    return result;
                }

                PPR_LOG(ProfileCaptureNsight, info, "Nsight GPU Trace start accepted");
                return {};
            }

            std::error_code poll(ProfileCaptureSdkStatus &out_status) override {
                out_status = {};

                bool injected = false;
                if (const auto result = nsightError(NGFX_GPUTrace_IsInjected(&injected))) {
                    return result;
                }
                out_status.m_injected = injected;

                bool initialized = false;
                if (const auto result = nsightError(NGFX_GPUTrace_IsInitialized(&initialized))) {
                    return result;
                }
                out_status.m_initialized = initialized or m_initialized;

                NGFX_GPUTrace_GetStatus_Params status_params{};
                status_params.version = NGFX_GPUTrace_GetStatus_Params_VER;
                const NGFX_Result status_result = NGFX_GPUTrace_GetStatus(&status_params);
                if (status_result == NGFX_Result_Success) {
                    out_status.m_status = static_cast<u32>(status_params.status);
                    out_status.m_tracing = status_params.status == NGFX_GPUTrace_Status_Tracing;
                    out_status.m_draining = status_params.status == NGFX_GPUTrace_Status_Draining;
                    out_status.m_errored = status_params.status == NGFX_GPUTrace_Status_Errored;
                    out_status.m_active = status_params.status == NGFX_GPUTrace_Status_Active or
                                          out_status.m_tracing or
                                          out_status.m_draining;
                } else if (status_result != NGFX_Result_InvalidState) {
                    return nsightError(status_result);
                }

                NGFX_ArtifactFileCount_Params count_params{};
                count_params.version = NGFX_ArtifactFileCount_Params_VER;
                const NGFX_Result count_result = NGFX_GPUTrace_GetTraceFileCount(&count_params);
                if (count_result == NGFX_Result_Success) {
                    out_status.m_trace_files = count_params.count;
                } else if (count_result != NGFX_Result_InvalidState) {
                    return nsightError(count_result);
                }

                if (out_status.m_trace_files > 0u) {
                    std::array<char, 4096> path{};
                    NGFX_ArtifactFilePath_Params path_params{};
                    path_params.version = NGFX_ArtifactFilePath_Params_VER;
                    path_params.artifactIndex = out_status.m_trace_files - 1u;
                    path_params.filePath = path.data();
                    path_params.filePathCapacity = static_cast<u32>(path.size());
                    const NGFX_Result path_result = NGFX_GPUTrace_GetTraceFilePath(&path_params);
                    if (path_result == NGFX_Result_Success) {
                        out_status.m_trace_path = std::string{path.data()};
                    } else if (path_result != NGFX_Result_InsufficientBuffer) {
                        return nsightError(path_result);
                    }
                }

                return {};
            }

        private:
            bool m_initialized{};
        };
    } // namespace
#endif

    std::unique_ptr<IProfileCaptureSdk> createProfileCaptureSdk() {
#if PPR_HAS_NSIGHT_GRAPHICS_SDK
        return std::make_unique<NsightGraphicsProfileCaptureSdk>();
#else
        // Ordinary builds and gameplay never initialize or call a vendor SDK.
        return nullptr;
#endif
    }
} // namespace pP::game
