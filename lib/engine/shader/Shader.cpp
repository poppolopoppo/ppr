module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>

module engine.shader;

import std;
import engine.core;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Shader, info, none)

    namespace shader {
        class SlangErrorCategory : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override { return "slang"; }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<Result>(ev)) {
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

                    default: return std::format("unknown slang result ({})", ev);
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                return slang_error_condition(*this, ev);
            }

            [[nodiscard]] static std::error_condition slang_error_condition(const std::error_category &category, const int ev) noexcept {
                switch (static_cast<Result>(ev)) {
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

        static constexpr SlangErrorCategory g_slang_error_category{};

        [[nodiscard]] const std::error_category &error_category() noexcept {
            return g_slang_error_category;
        }

        [[nodiscard]] std::error_code make_error_code(const Result result) noexcept {
            return SLANG_SUCCEEDED(result) ? std::error_code{} : std::error_code{result, g_slang_error_category};
        }

        [[nodiscard]] std::error_code make_error_code(const errc error_code) noexcept {
            return make_error_code(static_cast<Result>(error_code));
        }

        void diagnoseIfNeeded(IBlob *diagnostic_blob) {
            if (diagnostic_blob) {
                const std::string_view message{
                    static_cast<const char *>(diagnostic_blob->getBufferPointer()),
                    safe_narrowing(diagnostic_blob->getBufferSize())
                };
                PPR_LOG_RAW(Shader, error, message);
            }
        }

        // ------------------------------------------------------------------
        // ShaderService — singleton implementing IShaderService
        // ------------------------------------------------------------------

        class ShaderService final : public IShaderService {
            ComPtr<IGlobalSession> m_global_session{};
            ComPtr<ISession> m_session{};

        public:
            ShaderService() noexcept = default;

            // ------------------------------------------------------------------
            // IShaderService
            // ------------------------------------------------------------------

            std::error_code initialize() override {
                if (m_global_session) {
                    PPR_LOG(Shader, warning, "Shader service already initialized");
                    return errc::ok;
                }

                PPR_RETURN_ERROR_ON_FAIL(Shader, createGlobalSession(m_global_session.writeRef()));

                // Create a default session for compilation with a portable SPIR-V target
                TargetDesc target_desc{};
                target_desc.format = SLANG_SPIRV;

                SessionDesc session_desc{};
                session_desc.targets = &target_desc;
                session_desc.targetCount = 1;

                PPR_RETURN_ERROR_ON_FAIL(Shader, m_global_session->createSession(session_desc, m_session.writeRef()));

                PPR_LOG(Shader, info, "Shader service initialized");
                return errc::ok;
            }

            std::error_code shutdown() override {
                if (not m_global_session) {
                    return errc::uninitialized;
                }

                m_session.setNull();
                m_global_session.setNull();

                PPR_LOG(Shader, info, "Shader service shutdown");
                return errc::ok;
            }

            IGlobalSession *getGlobalSession() const noexcept override {
                return m_global_session.get();
            }

            std::error_code loadModuleFromFile(
                const std::filesystem::path &path,
                const char *module_name,
                IModule **out_module) override {
                const auto mapped = io::mapFile(path);
                if (not mapped) {
                    return mapped.error();
                }
                return loadModuleFromSource(module_name, path.generic_string().c_str(), mapped->c_str(), out_module);
            }

            std::error_code loadModuleFromSource(
                const char *module_name,
                const char *path,
                const char *source,
                IModule **out_module) override {
                Diagnose diagnostics;
                *out_module = m_session->loadModuleFromSourceString(
                    module_name,
                    path,
                    source,
                    diagnostics.writeRef());

                if (not *out_module) {
                    PPR_LOG(Shader, error, "failed to compile shader module from source", {
                        {"name", module_name},
                        {"path", path}
                    });
                    return make_error_code(errc::invalid_arg);
                }

                return errc::ok;
            }
        };
    }

    safe_ptr<IShaderService> IShaderService::get() noexcept {
        static shader::ShaderService g_instance{};
        return safe_ptr<IShaderService>(&g_instance);
    }
}
