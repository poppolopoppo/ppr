module;

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-rhi.h>

export module engine.rhi;

import std;

namespace rhi_fwd {
    using namespace rhi;
}

export namespace rhi {
    using namespace rhi_fwd;
    using Slang::ComPtr;

    using rhi_fwd::IAdapter;
    using rhi_fwd::IDevice;
    using rhi_fwd::IBuffer;
    using rhi_fwd::ICommandBuffer;
    using rhi_fwd::ICommandEncoder;
    using rhi_fwd::ICommandQueue;
    using rhi_fwd::IComputePipeline;
    using rhi_fwd::IComputePassEncoder;
    using rhi_fwd::IDebugCallback;
    using rhi_fwd::IFence;
    using rhi_fwd::IHeap;
    using rhi_fwd::IInputLayout;
    using rhi_fwd::ISurface;
    using rhi_fwd::IShaderProgram;
    using rhi_fwd::IShaderObject;
    using rhi_fwd::BufferDesc;
    using rhi_fwd::TextureDesc;
    using rhi_fwd::DeviceDesc;
    using rhi_fwd::ShaderObjectContainerType;
    using rhi_fwd::WindowHandle;
    using rhi_fwd::Result;

    // Thin wrapper exposing basic Initialization
    [[nodiscard]] inline bool initialize() {
        ComPtr<slang::IGlobalSession> global_session;
        if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, global_session.writeRef()))) {
            std::cerr << "Failed to create Slang Global Session!\n";
            return false;
        }

        std::cout << "engine.rhi initialized successfully!\n";
        return true;
    }

    // ------------------------------------------------------------------
    // Concrete Buffer wrapper
    // ------------------------------------------------------------------

    class Buffer {
        ComPtr<IBuffer> m_handle;
        BufferDesc m_desc{};

        Buffer(ComPtr<IBuffer> handle, BufferDesc desc) noexcept
            : m_handle(std::move(handle)), m_desc(desc) {}

    public:
        Buffer(Buffer&&) = default;
        Buffer& operator=(Buffer&&) = default;

        [[nodiscard]] static std::expected<Buffer, Result> create(IDevice* device, const BufferDesc& desc) {
            ComPtr<IBuffer> handle;
            Result result = device->createBuffer(desc, nullptr, handle.writeRef());
            if (SLANG_FAILED(result)) {
                return std::unexpected(result);
            }
            return Buffer(std::move(handle), desc);
        }

        IBuffer* handle() const noexcept { return m_handle.get(); }
        const BufferDesc& desc() const noexcept { return m_desc; }
    };

    // ------------------------------------------------------------------
    // Concrete Texture wrapper
    // ------------------------------------------------------------------

    class Texture {
        ComPtr<ITexture> m_handle;
        TextureDesc m_desc{};

        Texture(ComPtr<ITexture> handle, TextureDesc desc) noexcept
            : m_handle(std::move(handle)), m_desc(desc) {}

    public:
        Texture(Texture&&) = default;
        Texture& operator=(Texture&&) = default;

        [[nodiscard]] static std::expected<Texture, Result> create(IDevice* device, const TextureDesc& desc) {
            ComPtr<ITexture> handle;
            Result result = device->createTexture(desc, nullptr, handle.writeRef());
            if (SLANG_FAILED(result)) {
                return std::unexpected(result);
            }
            return Texture(std::move(handle), desc);
        }

        ITexture* handle() const noexcept { return m_handle.get(); }
        const TextureDesc& desc() const noexcept { return m_desc; }
    };

    // ------------------------------------------------------------------
    // Concrete Shader object wrapper (compute + graphics)
    // ------------------------------------------------------------------

    class Shader {
        ComPtr<IShaderObject> m_handle;

        explicit Shader(ComPtr<IShaderObject> handle) noexcept
            : m_handle(std::move(handle)) {}

    public:
        Shader(Shader&&) = default;
        Shader& operator=(Shader&&) = default;

        [[nodiscard]] static std::expected<Shader, Result> create(IDevice* device, IShaderProgram* program) {
            ComPtr<IShaderObject> handle;
            Result result = device->createRootShaderObject(program, handle.writeRef());
            if (SLANG_FAILED(result)) {
                return std::unexpected(result);
            }
            return Shader(std::move(handle));
        }

        IShaderObject* handle() const noexcept { return m_handle.get(); }
    };

    // ------------------------------------------------------------------
    // Static RHI creation helper
    // ------------------------------------------------------------------

    [[nodiscard]] inline std::expected<ComPtr<IDevice>, Result> createDevice(DeviceDesc desc = {}) {
        auto* rhi_instance = getRHI();
        if (!rhi_instance) {
            return std::unexpected(Result(-1));
        }
        ComPtr<IDevice> device;
        Result result = rhi_instance->createDevice(desc, device.writeRef());
        if (SLANG_FAILED(result)) {
            return std::unexpected(result);
        }
        return device;
    }

    // ------------------------------------------------------------------
    // Surface (swapchain) creation helper
    // ------------------------------------------------------------------

    [[nodiscard]] inline std::expected<ComPtr<ISurface>, Result> createSurface(
        IDevice* device,
        WindowHandle windowHandle)
    {
        ComPtr<ISurface> surface;
        Result result = device->createSurface(windowHandle, surface.writeRef());
        if (SLANG_FAILED(result)) {
            return std::unexpected(result);
        }
        return surface;
    }

}
