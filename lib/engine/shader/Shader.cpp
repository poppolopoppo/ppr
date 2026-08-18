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
        // MappedFileBlob — owns a MappedFile and exposes it as an ISlangBlob
        // ------------------------------------------------------------------

        class MappedFileBlob final : public IBlob {
            MappedFile m_file{};
            std::atomic<u32> m_ref_count{0};

        public:
            explicit MappedFileBlob(MappedFile file) noexcept
                : m_file(std::move(file)) {
            }

            // ISlangUnknown
            SLANG_NO_THROW SlangResult SLANG_MCALL
            queryInterface(const SlangUUID &uuid, void **out_object) override {
                if (uuid == ISlangUnknown::getTypeGuid() or uuid == ISlangBlob::getTypeGuid()) {
                    addRef();
                    *out_object = static_cast<IBlob *>(this);
                    return SLANG_OK;
                }
                return SLANG_E_NO_INTERFACE;
            }

            SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
                return ++m_ref_count;
            }

            SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
                const u32 count = --m_ref_count;
                if (count == 0) {
                    delete this;
                }
                return count;
            }

            // ISlangBlob
            SLANG_NO_THROW void const *SLANG_MCALL getBufferPointer() override {
                return m_file.c_str();
            }

            SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override {
                return m_file.size();
            }
        };

        // ------------------------------------------------------------------
        // ShaderService — singleton implementing IShaderService
        // ------------------------------------------------------------------

        class ShaderService final : public IShaderService {
            ComPtr<IGlobalSession> m_global_session{};
            ComPtr<ISession> m_session{};
            SlangCompileTarget m_target_format = SLANG_DXBC;
            bool m_modules_loaded = false;

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

                // Create a compilation session for the active backend's target format
                // Set row-major matrix layout for maximum portability across APIs (D3D, Vulkan, OpenGL, Metal)
                // Row-major is the only layout reliably portable across all targets
                TargetDesc target_desc{};
                target_desc.format = m_target_format;

                SessionDesc session_desc{};
                session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
                session_desc.targets = &target_desc;
                session_desc.targetCount = 1;

                PPR_RETURN_ERROR_ON_FAIL(Shader, m_global_session->createSession(session_desc, m_session.writeRef()));

                PPR_LOG(Shader, info, "Shader service initialized", {
                    {"target", static_cast<int>(m_target_format)}
                });
                return errc::ok;
            }

            std::error_code setTargetFormat(const SlangCompileTarget target_format) override {
                if (not m_global_session) {
                    PPR_LOG(Shader, error, "cannot set target format before initialization");
                    return errc::uninitialized;
                }
                if (m_modules_loaded) {
                    PPR_LOG(Shader, error, "cannot change target format after modules have been loaded");
                    return errc::invalid_arg;
                }
                if (target_format == m_target_format) {
                    return errc::ok;
                }

                m_target_format = target_format;
                m_session.setNull();

                // Set row-major matrix layout for maximum portability across APIs (D3D, Vulkan, OpenGL, Metal)
                // Row-major is the only layout reliably portable across all targets
                TargetDesc target_desc{};
                target_desc.format = m_target_format;

                SessionDesc session_desc{};
                session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
                session_desc.targets = &target_desc;
                session_desc.targetCount = 1;

                PPR_RETURN_ERROR_ON_FAIL(Shader, m_global_session->createSession(session_desc, m_session.writeRef()));

                PPR_LOG(Shader, info, "shader target format set", {
                    {"target", static_cast<int>(m_target_format)}
                });
                return errc::ok;
            }

            std::error_code shutdown() override {
                if (not m_global_session) {
                    return errc::uninitialized;
                }

                if (m_modules_loaded) {
                    PPR_LOG(Shader, warning, "shutting down with loaded modules — modules are owned by the session and will be destroyed with it");
                }
                m_modules_loaded = false;
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
                auto mapped = io::mapFile(path);
                if (not mapped) {
                    return mapped.error();
                }

                const auto source_blob = ComPtr<IBlob>{new MappedFileBlob(std::move(*mapped))};
                const std::string path_string = path.generic_string();

                Diagnose diagnostics;
                *out_module = m_session->loadModuleFromSource(
                    module_name,
                    path_string.c_str(),
                    source_blob,
                    diagnostics.writeRef());

                if (not *out_module) {
                    PPR_LOG(Shader, error, "failed to compile shader module from file", {
                        {"name", module_name},
                        {"path", path_string}
                    });
                    return make_error_code(errc::invalid_arg);
                }

                m_modules_loaded = true;
                return errc::ok;
            }

            std::error_code loadModuleFromSource(
                const char *module_name,
                const char *path,
                const string_literal source,
                IModule **out_module) override {
                Diagnose diagnostics;
                *out_module = m_session->loadModuleFromSourceString(
                    module_name,
                    path,
                    source.data(),
                    diagnostics.writeRef());

                if (not *out_module) {
                    PPR_LOG(Shader, error, "failed to compile shader module from source", {
                        {"name", module_name},
                        {"path", path}
                    });
                    return make_error_code(errc::invalid_arg);
                }

                m_modules_loaded = true;
                return errc::ok;
            }
        };
    }

    safe_ptr<IShaderService> IShaderService::get() noexcept {
        static shader::ShaderService g_instance{};
        return safe_ptr<IShaderService>(&g_instance);
    }
}
