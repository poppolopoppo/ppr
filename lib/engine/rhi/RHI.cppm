module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-com-helper.h>
#include <slang-rhi.h>
#include <slang-rhi/shader-cursor.h>

export module engine.rhi;

import engine.core;
import std;

namespace slang_rhi {
    using namespace rhi;
}

export namespace pP::shader {
    enum class errc : int {
        //! SLANG_OK indicates success
        ok = SLANG_OK,

        //! SLANG_FAIL is the generic failure code - meaning a serious error occurred and the call
        //! couldn't complete
        unknown_error = SLANG_FAIL,

        //! Functionality is not implemented
        not_implemented = SLANG_E_NOT_IMPLEMENTED,
        //! Interface not be found
        no_interface = SLANG_E_NO_INTERFACE,
        //! Operation was aborted (did not correctly complete)
        abort = SLANG_E_ABORT,

        //! Indicates that a handle passed in as parameter to a method is invalid.
        invalid_handle = SLANG_E_INVALID_HANDLE,
        //! Indicates that an argument passed in as parameter to a method is invalid.
        invalid_arg = SLANG_E_INVALID_ARG,
        //! Operation could not complete - ran out of memory
        out_of_memory = SLANG_E_OUT_OF_MEMORY,

        // Supplied buffer is too small to be able to complete
        buffer_too_small = SLANG_E_BUFFER_TOO_SMALL,
        //! Used to identify a Result that has yet to be initialized.
        //! It defaults to failure such that if used incorrectly will fail, as similar in concept to
        //! using an uninitialized variable.
        uninitialized = SLANG_E_UNINITIALIZED,
        //! Returned from an async method meaning the output is invalid (thus an error), but a result
        //! for the request is pending, and will be returned on a subsequent call with the async handle.
        pending = SLANG_E_PENDING,
        //! Indicates a file/resource could not be opened
        cannot_open = SLANG_E_CANNOT_OPEN,
        //! Indicates a file/resource could not be found
        not_found = SLANG_E_NOT_FOUND,
        //! An unhandled internal failure (typically from unhandled exception)
        internal_fail = SLANG_E_INTERNAL_FAIL,
        //! Could not complete because some underlying feature (hardware or software) was not available
        not_available = SLANG_E_NOT_AVAILABLE,
        //! Could not complete because the operation times out.
        time_out = SLANG_E_TIME_OUT,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(Slang::Result result) noexcept;

    [[nodiscard]] std::error_code make_error_code(errc error_code) noexcept;

    // shorter alias, because Slang::Result is just an int32 :/
    [[nodiscard]] std::error_code result(const Slang::Result result) noexcept {
        return make_error_code(result);
    }

    using Slang::ComPtr;
    using Slang::Result;

    using ::slang::IBindlessResourceMetadata;
    using ::slang::IBlob;
    using ::slang::IByteCodeRunner;
    using ::slang::ICompileRequest;
    using ::slang::IComponentType;
    using ::slang::IEntryPoint;
    using ::slang::IGlobalSession;
    using ::slang::IMetadata;
    using ::slang::IModule;
    using ::slang::ISession;

    void diagnoseIfNeeded(IBlob *diagnostic_blob);

    struct Diagnose {
        ComPtr<IBlob> m_diagnostics{};

        Diagnose() noexcept = default;

        ~Diagnose() {
            diagnoseIfNeeded(m_diagnostics.get());
        }

        [[nodiscard]] IBlob **writeRef() noexcept {
            return m_diagnostics.writeRef();
        }
    };
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

        [[nodiscard]] virtual std::error_code initialize(rhi::DeviceType device_type) = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        [[nodiscard]] virtual rhi::IRHI &getInstance() const noexcept = 0;

        [[nodiscard]] virtual rhi::IDevice &getDevice() const noexcept = 0;
    };
}

export template<>
struct std::is_error_code_enum<pP::rhi::errc> : true_type {
};
