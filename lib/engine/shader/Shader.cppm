module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>

export module engine.shader;

import engine.core;
import std;

export namespace pP::shader {
    enum class errc : int {
        ok = SLANG_OK,
        unknown_error = SLANG_FAIL,
        not_implemented = SLANG_E_NOT_IMPLEMENTED,
        no_interface = SLANG_E_NO_INTERFACE,
        abort = SLANG_E_ABORT,
        invalid_handle = SLANG_E_INVALID_HANDLE,
        invalid_arg = SLANG_E_INVALID_ARG,
        out_of_memory = SLANG_E_OUT_OF_MEMORY,
        buffer_too_small = SLANG_E_BUFFER_TOO_SMALL,
        uninitialized = SLANG_E_UNINITIALIZED,
        pending = SLANG_E_PENDING,
        cannot_open = SLANG_E_CANNOT_OPEN,
        not_found = SLANG_E_NOT_FOUND,
        internal_fail = SLANG_E_INTERNAL_FAIL,
        not_available = SLANG_E_NOT_AVAILABLE,
        time_out = SLANG_E_TIME_OUT,
    };

    [[nodiscard]] const std::error_category &error_category() noexcept;

    [[nodiscard]] std::error_code make_error_code(Slang::Result result) noexcept;

    [[nodiscard]] std::error_code make_error_code(errc error_code) noexcept;

    [[nodiscard]] std::error_code result(const Slang::Result result) noexcept {
        return make_error_code(result);
    }

    using Slang::ComPtr;
    using Slang::Result;

    using slang::IBlob;
    using slang::IComponentType;
    using slang::IEntryPoint;
    using slang::IGlobalSession;
    using slang::IModule;
    using slang::ISession;

    using namespace Slang;
    using namespace slang;

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

    using SharedModule = ComPtr<IModule>;
}

export namespace pP {
    class IShaderService : public IService {
    public:
        [[nodiscard]] static safe_ptr<IShaderService> get() noexcept;

        [[nodiscard]] virtual std::error_code initialize() = 0;

        [[nodiscard]] virtual std::error_code shutdown() = 0;

        /// Selects the code generation target (e.g. DXBC for D3D12, SPIR-V for Vulkan).
        /// Must be called before any module is loaded; calling it after returns invalid_arg.
        [[nodiscard]] virtual std::error_code setTargetFormat(SlangCompileTarget target_format) = 0;

        [[nodiscard]] virtual slang::IGlobalSession *getGlobalSession() const noexcept = 0;

        [[nodiscard]] virtual std::error_code
        loadModuleFromFile(const std::filesystem::path &path, const char *module_name, shader::IModule **out_module) = 0;

        [[nodiscard]] virtual std::error_code
        loadModuleFromSource(const char *module_name, const char *path, string_literal source, shader::IModule **out_module) = 0;
    };
}

export template<>
struct std::is_error_code_enum<pP::shader::errc> : true_type { // NOLINT(*-dcl58-cpp)
};
