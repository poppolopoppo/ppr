module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-com-helper.h>
#include <slang-rhi.h>
#include <slang-rhi/shader-cursor.h>

export module engine.rhi;

import engine.core;
import engine.math;
import engine.shader;
import std;

namespace slang_rhi {
    using namespace rhi;
}

export namespace pP::rhi {
    using errc = shader::errc;

    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(::slang_rhi::Result result) noexcept;

    [[nodiscard]] std::error_code make_error_code(errc error_code) noexcept;

    // shorter alias, because Slang::Result is just an int32 :/
    [[nodiscard]] std::error_code result(const Slang::Result result) noexcept {
        return make_error_code(result);
    }

    using Slang::ComPtr;

    using slang_rhi::IRHI;

    using slang_rhi::IAdapter;
    using slang_rhi::IBuffer;
    using slang_rhi::ICommandBuffer;
    using slang_rhi::ICommandEncoder;
    using slang_rhi::ICommandQueue;
    using slang_rhi::IComputePipeline;
    using slang_rhi::IComputePassEncoder;
    using slang_rhi::IDebugCallback;
    using slang_rhi::IDevice;
    using slang_rhi::IFence;
    using slang_rhi::IHeap;
    using slang_rhi::IInputLayout;
    using slang_rhi::IRenderPipeline;
    using slang_rhi::IShaderObject;
    using slang_rhi::IShaderProgram;
    using slang_rhi::IShaderTable;
    using slang_rhi::ISurface;
    using slang_rhi::ITexture;
    using slang_rhi::ITextureView;
    using slang_rhi::IRenderPassEncoder;

    using slang_rhi::BufferDesc;
    using slang_rhi::BufferOffsetPair;
    using slang_rhi::BufferRange;
    using slang_rhi::BufferUsage;
    using slang_rhi::ColorClearValue;
    using slang_rhi::ColorTargetDesc;
    using slang_rhi::ComputePipelineDesc;
    using slang_rhi::DeviceAddress;
    using slang_rhi::DeviceDesc;
    using slang_rhi::DeviceInfo;
    using slang_rhi::DeviceLimits;
    using slang_rhi::DeviceType;

    enum class EProjectionConvention {
        D3D,
        VK,
    };

    [[nodiscard]] constexpr EProjectionConvention projectionConventionFromDeviceType(
        const DeviceType type) noexcept {
        switch (type) {
            case DeviceType::D3D11:
            case DeviceType::D3D12:
            case DeviceType::Default:
                return EProjectionConvention::D3D;
            case DeviceType::Vulkan:
            case DeviceType::Metal:
            case DeviceType::WGPU:
                return EProjectionConvention::VK;
            default:
                return EProjectionConvention::D3D;
        }
    }

    [[nodiscard]] float4x4 getOrthoMatrix(
        const DeviceType type,
        const float width,
        const float height) noexcept;

    [[nodiscard]] float4x4 getPerspectiveMatrix(
        const DeviceType type,
        const float fov,
        const float aspect,
        const float near_,
        const float far_) noexcept;

    using slang_rhi::DrawArguments;
    using slang_rhi::Format;
    using slang_rhi::FormatInfo;
    using slang_rhi::FormatKind;
    using slang_rhi::FormatSupport;
    using slang_rhi::IndexFormat;
    using slang_rhi::InputElementDesc;
    using slang_rhi::InputLayoutDesc;
    using slang_rhi::LoadOp;
    using slang_rhi::LinkingStyle;
    using slang_rhi::MemoryType;
    using slang_rhi::PrimitiveTopology;
    using slang_rhi::QueueType;
    using slang_rhi::RenderPassDesc;
    using slang_rhi::RenderPassColorAttachment;
    using slang_rhi::RenderPipelineDesc;
    using slang_rhi::RenderState;
    using slang_rhi::ResourceState;
    using slang_rhi::AspectBlendDesc;
    using slang_rhi::Binding;
    using slang_rhi::BlendFactor;
    using slang_rhi::BlendOp;
    using slang_rhi::CpuAccessMode;
    using slang_rhi::CullMode;
    using slang_rhi::Extent3D;
    using slang_rhi::ISampler;
    using slang_rhi::Offset3D;
    using slang_rhi::RenderTargetWriteMask;
    using slang_rhi::Result;
    using slang_rhi::SamplerDesc;
    using slang_rhi::ScissorRect;
    using slang_rhi::ShaderOffset;
    using slang_rhi::SubresourceLayout;
    using slang_rhi::SubresourceRange;
    using slang_rhi::TextureAddressingMode;
    using slang_rhi::TextureDesc;
    using slang_rhi::TextureFilteringMode;
    using slang_rhi::TextureType;
    using slang_rhi::TextureUsage;
    using slang_rhi::ShaderProgramDesc;
    using slang_rhi::StoreOp;
    using slang_rhi::StructType;
    using slang_rhi::SubresourceData;
    using slang_rhi::SurfaceConfig;
    using slang_rhi::SurfaceInfo;
    using slang_rhi::Viewport;
    using slang_rhi::ShaderCursor;
    using slang_rhi::WindowHandle;
}

export namespace pP {
    // ------------------------------------------------------------------
    // hasFailed() for rhi::Result — ADL target from pP namespace
    // ------------------------------------------------------------------

    [[nodiscard]] constexpr bool hasFailed(const rhi::Result status) noexcept {
        return SLANG_FAILED(status);
    }
}

export namespace pP {
    class IRhiService : public IService {
    public:
        [[nodiscard]] static safe_ptr<IRhiService> get() noexcept;

        [[nodiscard]] virtual std::error_code initialize(
            rhi::DeviceType device_type,
            slang::IGlobalSession *global_session) = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        [[nodiscard]] virtual rhi::IRHI &getInstance() const noexcept = 0;

        [[nodiscard]] virtual rhi::IDevice &getDevice() const noexcept = 0;

        [[nodiscard]] virtual rhi::Result createRenderPipeline(
            const rhi::RenderPipelineDesc &desc,
            rhi::IRenderPipeline **outPipeline) = 0;
    };
}
