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

}
