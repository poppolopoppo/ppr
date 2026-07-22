module;

#include "pP/Macros.h"

#include <slang.h>
#include <slang-com-ptr.h>

module engine.shader;

import std;
import engine.core;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(Shader, info, none)

    using ::slang::IBlob;
    using ::slang::IComponentType;
    using ::slang::IEntryPoint;
    using ::slang::IGlobalSession;
    using ::slang::IModule;
    using ::slang::ISession;

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

        void diagnoseIfNeeded(slang::IBlob *diagnostic_blob) {
            if (diagnostic_blob) {
                const std::string_view message{
                    static_cast<const char *>(diagnostic_blob->getBufferPointer()),
                    safe_narrowing(diagnostic_blob->getBufferSize())
                };
                PPR_LOG_RAW(Shader, error, message);
            }
        }
    }

    IModule *ModuleHandle::get() const noexcept {
        return m_state ? m_state->m_module.get() : nullptr;
    }

    bool ModuleHandle::wasReloaded() noexcept {
        return m_state ? m_state->m_reloaded.exchange(false) : false;
    }

    // ------------------------------------------------------------------
    // internal compile result type
    // ------------------------------------------------------------------

    struct CompileResult {
        std::shared_ptr<ModuleState> target;
        shader::ComPtr<IModule> module;
        std::error_code ec;
    };

    // ------------------------------------------------------------------
    // ShaderService — singleton implementing IShaderService
    // ------------------------------------------------------------------

    class ShaderService final : public IShaderService {
        shader::ComPtr<slang::IGlobalSession> m_global_session;
        shader::ComPtr<slang::ISession> m_session;

        // Compile thread
        std::jthread m_compile_thread;
        std::mutex m_job_mutex;
        std::condition_variable m_job_cv;
        std::deque<std::shared_ptr<CompileResult>> m_job_queue;
        std::atomic<bool> m_shutdown{false};

        // Thread-safe result storage (producer: compile thread, consumer: poll)
        std::mutex m_result_mutex;
        std::deque<CompileResult> m_results;

        // Active modules: canonical path → ModuleState
        std::unordered_map<std::string, std::shared_ptr<ModuleState>> m_modules_by_path;

        // Directory watchers: directory path → DirectoryWatcher
        std::unordered_map<std::string, std::unique_ptr<DirectoryWatcher>> m_watchers_by_dir;

    public:
        ShaderService() noexcept = default;

        ~ShaderService() noexcept override {
            m_shutdown = true;
            m_job_cv.notify_all();
        }

        // ------------------------------------------------------------------
        // IShaderService
        // ------------------------------------------------------------------

        std::error_code initialize() override {
            if (m_global_session) {
                PPR_LOG(Shader, warning, "Shader service already initialized");
                return make_error_code(shader::errc::ok);
            }

            shader::ComPtr<slang::IGlobalSession> session;
            const auto create_result = ::slang::createGlobalSession(session.writeRef());
            if (SLANG_FAILED(create_result)) {
                PPR_LOG(Shader, error, "failed to create Slang global session");
                return shader::make_error_code(create_result);
            }
            m_global_session = std::move(session);

            // Create a default session for compilation
            shader::ComPtr<slang::ISession> default_session;
            const auto session_result = m_global_session->createSession({}, default_session.writeRef());
            if (SLANG_FAILED(session_result)) {
                PPR_LOG(Shader, error, "failed to create Slang compilation session");
                return shader::make_error_code(session_result);
            }
            m_session = std::move(default_session);

            // Start the compile thread
            m_compile_thread = std::jthread([this] { compileThread_(); });

            PPR_LOG(Shader, info, "Shader service initialized");
            return make_error_code(shader::errc::ok);
        }

        slang::IGlobalSession *getGlobalSession() const noexcept override {
            return m_global_session.get();
        }

        std::expected<ModuleHandle, std::error_code> loadModuleFromFile(
            const std::filesystem::path &path,
            const string_literal module_name) override {
            // Map the file and copy source
            auto mapped = io::mapFile(path);
            if (not mapped) {
                PPR_LOG(Shader, error, "failed to map shader file", {{"path", path.string().c_str()}});
                return std::unexpected(mapped.error());
            }

            const std::string source(mapped->c_str(), mapped->size());

            // Compile synchronously for the initial load
            shader::Diagnose diagnostics;
            shader::ComPtr<IModule> module;
            *module.writeRef() = m_session->loadModuleFromSourceString(
                module_name.data(),
                path.generic_string().c_str(),
                source.c_str(),
                diagnostics.writeRef());

            if (not module) {
                PPR_LOG(Shader, error, "failed to compile shader", {{"path", path.string().c_str()}});
                return std::unexpected(shader::make_error_code(shader::errc::invalid_arg));
            }

            // Create the module state, storing the source for hot-reload
            auto state = std::make_shared<ModuleState>();
            state->m_module = std::move(module);
            state->m_source_path = path.generic_string();
            state->m_module_name = module_name.data();
            state->m_source = source;

            // Register for file watching
            const auto canonical_path = std::filesystem::weakly_canonical(path);
            const auto dir = canonical_path.parent_path().generic_string();
            const auto full_path = canonical_path.generic_string();

            if (m_watchers_by_dir.find(dir) == m_watchers_by_dir.end()) {
                auto watcher = std::make_unique<DirectoryWatcher>(std::filesystem::path(dir));
                m_watchers_by_dir.emplace(dir, std::move(watcher));
            }

            m_modules_by_path.emplace(full_path, state);

            ModuleHandle handle;
            ModuleStateAccess::setState(handle, std::move(state));
            return handle;
        }

        std::expected<ModuleHandle, std::error_code> loadModuleFromSource(
            const string_literal module_name,
            const char *path,
            const char *source) override {
            shader::Diagnose diagnostics;
            shader::ComPtr<IModule> module;
            *module.writeRef() = m_session->loadModuleFromSourceString(
                module_name.data(),
                path,
                source,
                diagnostics.writeRef());

            if (not module) {
                PPR_LOG(Shader, error, "failed to compile shader module from source", {
                    {"name", module_name.data()}
                });
                return std::unexpected(shader::make_error_code(shader::errc::invalid_arg));
            }

            auto state = std::make_shared<ModuleState>();
            state->m_module = std::move(module);
            state->m_source_path = path;
            state->m_module_name = module_name.data();
            state->m_source = source;

            ModuleHandle handle;
            ModuleStateAccess::setState(handle, std::move(state));
            return handle;
        }

        std::future<std::error_code> reloadModule(ModuleHandle &handle) override {
            if (not ModuleStateAccess::getState(handle)) {
                std::promise<std::error_code> p;
                auto future = p.get_future();
                p.set_value(shader::make_error_code(shader::errc::invalid_arg));
                return future;
            }

            auto promise = std::make_shared<std::promise<std::error_code>>();
            auto future = promise->get_future();

            auto job = std::make_shared<CompileResult>();
            job->target = ModuleStateAccess::getState(handle);

            {
                std::lock_guard lock(m_job_mutex);
                m_job_queue.push_back(std::move(job));
            }
            m_job_cv.notify_one();

            return future;
        }

        void poll() override {
            // 1. Poll file watchers for changes
            for (auto &[dir_path, watcher] : m_watchers_by_dir) {
                std::error_code ec;
                watcher->poll(ec);
                if (ec && ec != std::errc::result_out_of_range) {
                    continue;
                }

                for (const auto &change : watcher->changes()) {
                    const auto full_path = (std::filesystem::path(dir_path) / change.m_filename).generic_string();
                    const auto it = m_modules_by_path.find(full_path);
                    if (it == m_modules_by_path.end()) {
                        continue;
                    }

                    auto &state = it->second;

                    // Re-map the changed file
                    auto mapped = io::mapFile(std::filesystem::path(full_path));
                    if (not mapped) {
                        continue;
                    }

                    // Update stored source for subsequent explicit reloads
                    state->m_source.assign(mapped->c_str(), mapped->size());

                    auto job = std::make_shared<CompileResult>();
                    job->target = state;

                    {
                        std::lock_guard lock(m_job_mutex);
                        m_job_queue.push_back(std::move(job));
                    }
                    m_job_cv.notify_one();
                }
            }

            // 2. Drain results (compile thread → main thread)
            std::deque<CompileResult> pending;
            {
                std::lock_guard lock(m_result_mutex);
                pending.swap(m_results);
            }

            for (auto &r : pending) {
                if (r.ec) {
                    PPR_LOG(Shader, error, "shader recompilation failed", {
                        {"path", r.target ? r.target->m_source_path.c_str() : "unknown"}
                    });
                } else if (r.target) {
                    r.target->m_module = std::move(r.module);
                    r.target->m_reloaded.store(true, std::memory_order_release);
                    PPR_LOG(Shader, info, "shader recompiled", {
                        {"path", r.target->m_source_path.c_str()}
                    });
                }
            }
        }

    private:
        // ------------------------------------------------------------------
        // compile thread: picks up jobs, compiles, sends results back
        // ------------------------------------------------------------------

        void compileThread_() {
            while (not m_shutdown) {
                std::shared_ptr<CompileResult> job;
                {
                    std::unique_lock lock(m_job_mutex);
                    m_job_cv.wait(lock, [this] { return m_shutdown || not m_job_queue.empty(); });
                    if (m_shutdown) {
                        break;
                    }
                    job = std::move(m_job_queue.front());
                    m_job_queue.pop_front();
                }

                auto result = compileModule_(job);

                if (not m_shutdown) {
                    std::lock_guard lock(m_result_mutex);
                    m_results.push_back(std::move(result));
                }
            }
        }

        [[nodiscard]] CompileResult compileModule_(const std::shared_ptr<CompileResult> &job) {
            CompileResult result;
            result.target = job->target;

            shader::Diagnose diagnostics;
            shader::ComPtr<IModule> module;
            *module.writeRef() = m_session->loadModuleFromSourceString(
                job->target->m_module_name.c_str(),
                job->target->m_source_path.c_str(),
                job->target->m_source.c_str(),
                diagnostics.writeRef());

            if (not module) {
                result.ec = shader::make_error_code(shader::errc::invalid_arg);
            } else {
                result.module = std::move(module);
            }

            return result;
        }
    };

    safe_ptr<IShaderService> IShaderService::get() noexcept {
        static ShaderService g_instance{};
        return safe_ptr<IShaderService>(&g_instance);
    }
}
