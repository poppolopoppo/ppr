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

    void diagnoseIfNeeded(slang::IBlob *diagnostic_blob);

    struct Diagnose {
        ComPtr<slang::IBlob> m_diagnostics{};

        Diagnose() noexcept = default;

        ~Diagnose() {
            diagnoseIfNeeded(m_diagnostics.get());
        }

        [[nodiscard]] slang::IBlob **writeRef() noexcept {
            return m_diagnostics.writeRef();
        }
    };
}

export namespace pP {
    struct ModuleState {
        shader::ComPtr<slang::IModule> m_module;
        std::atomic<bool> m_reloaded{false};
        std::string m_source_path;
        std::string m_module_name;
        std::string m_source;
    };

    class ModuleHandle {
    public:
        ModuleHandle() noexcept = default;

        [[nodiscard]] slang::IModule *get() const noexcept;

        [[nodiscard]] bool wasReloaded() noexcept;

    private:
        friend struct ModuleStateAccess;
        std::shared_ptr<ModuleState> m_state;
    };

    struct ModuleStateAccess {
        static void setState(ModuleHandle &handle, std::shared_ptr<ModuleState> state) noexcept {
            handle.m_state = std::move(state);
        }

        [[nodiscard]] static std::shared_ptr<ModuleState> &getState(ModuleHandle &handle) noexcept {
            return handle.m_state;
        }
    };

    class IShaderService : public IService {
    public:
        [[nodiscard]] static safe_ptr<IShaderService> get() noexcept;

        [[nodiscard]] virtual std::error_code initialize() = 0;

        [[nodiscard]] virtual slang::IGlobalSession *getGlobalSession() const noexcept = 0;

        [[nodiscard]] virtual std::expected<ModuleHandle, std::error_code>
        loadModuleFromFile(const std::filesystem::path &path, string_literal module_name) = 0;

        [[nodiscard]] virtual std::expected<ModuleHandle, std::error_code>
        loadModuleFromSource(string_literal module_name, const char *path, const char *source) = 0;

        [[nodiscard]] virtual std::future<std::error_code> reloadModule(ModuleHandle &handle) = 0;

        virtual void poll() = 0;
    };
}

export template<>
struct std::is_error_code_enum<pP::shader::errc> : true_type {
};
