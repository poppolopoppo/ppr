module;

#include "pP/Macros.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-rhi.h>

module engine.rhi;

import std;
import engine.core;
import engine.math;
import engine.shader;

namespace pP {
    PPR_DEFINE_LOG_CATEGORY(RHI, info, none)

    namespace {
        // ------------------------------------------------------------------
        // slang-rhi error codes
        // ------------------------------------------------------------------

        class SlangRhiErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override {
                return "slang-rhi";
            }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<Slang::Result>(ev)) {
                    case SLANG_OK: return "indicates success";
                    case SLANG_FAIL:
                        return "generic failure code - meaning a serious error occurred and the call couldn't complete";
                    case SLANG_E_NOT_IMPLEMENTED: return "functionality is not implemented";
                    case SLANG_E_NO_INTERFACE: return "interface not be found";
                    case SLANG_E_ABORT: return "operation was aborted (did not correctly complete)";
                    case SLANG_E_INVALID_HANDLE:
                        return "indicates that a handle passed in as parameter to a method is invalid";
                    case SLANG_E_INVALID_ARG:
                        return "indicates that an argument passed in as parameter to a method is invalid";
                    case SLANG_E_OUT_OF_MEMORY: return "operation could not complete - ran out of memory";
                    case SLANG_E_BUFFER_TOO_SMALL: return "supplied buffer is too small to be able to complete";
                    case SLANG_E_UNINITIALIZED: return "used to identify a Result that has yet to be initialized";
                    case SLANG_E_PENDING:
                        return "returned from an async method meaning the output is invalid (thus an error), but a result for the request is pending, and will be returned on a subsequent call with the async handle.";
                    case SLANG_E_CANNOT_OPEN: return "indicates a file/resource could not be opened";
                    case SLANG_E_NOT_FOUND: return "indicates that a file/resource could not be found";
                    case SLANG_E_INTERNAL_FAIL:
                        return "an unhandled internal failure (typically from unhandled exception)";
                    case SLANG_E_NOT_AVAILABLE:
                        return "could not complete because some underlying feature (hardware or software) was not available";
                    case SLANG_E_TIME_OUT: return "could not complete because the operation times out";

                    default: return std::string("unknown slang-rhi result (") + std::to_string(ev) + ")";
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                return slangRhiErrorCondition_(*this, ev);
            }

            [[nodiscard]] static std::error_condition slangRhiErrorCondition_(
                const std::error_category &category, const int ev) noexcept {
                switch (static_cast<Slang::Result>(ev)) {
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

        static const SlangRhiErrorCategory g_slang_rhi_error_category{};
    }

    [[nodiscard]] const std::error_category &rhi::error_category() noexcept {
        return g_slang_rhi_error_category;
    }

    [[nodiscard]] std::error_code rhi::make_error_code(const Result result) noexcept {
        return SLANG_SUCCEEDED(result) ? std::error_code{} : std::error_code{result, g_slang_rhi_error_category};
    }

    [[nodiscard]] std::error_code rhi::make_error_code(const errc error_code) noexcept {
        return make_error_code(static_cast<Result>(error_code));
    }

    // ------------------------------------------------------------------
    // pass debug messages to our own logger
    // ------------------------------------------------------------------

#if PPR_ENABLE_LOGGING
    namespace {
        class SlangRhiDebugCallback : public rhi::IDebugCallback {
        protected:
            ~SlangRhiDebugCallback() = default;

        public:
            [[nodiscard]] static IDebugCallback *get() noexcept {
                static SlangRhiDebugCallback g_instance{};
                return &g_instance;
            }

            void handleMessage(
                rhi::DebugMessageType type, rhi::DebugMessageSource source, const char *message) override {
                const std::source_location loc = std::source_location::current();
                auto &category = details::log::RHI();

                std::string_view source_name{};
                switch (source) {
                    using enum rhi::DebugMessageSource;
                    case Layer: source_name = "layer"; break;
                    case Driver: source_name = "driver"; break;
                    case Slang: source_name = "slang"; break;
                }

                auto level = Log::ELevel::info;
                switch (type) {
                    using enum rhi::DebugMessageType;
                    case Info: level = Log::ELevel::info; break;
                    case Warning: level = Log::ELevel::warning; break;
                    case Error: level = Log::ELevel::error; break;
                }

                Log::logRaw({category, level, loc}, trim(std::string_view(message)), {{"source", source_name}});
            }
        };
    }
#endif

    namespace {
        [[nodiscard]] string_literal getDeviceTypeName_(const rhi::DeviceType device_type) noexcept {
            switch (device_type) {
                using enum rhi::DeviceType;
                case Default: return "default";
                case D3D11: return "d3d11";
                case D3D12: return "d3d12";
                case Vulkan: return "vulkan";
                case Metal: return "metal";
                case CPU: return "cpu";
                case CUDA: return "cuda";
                case WGPU: return "wgpu";
            }
            std::unreachable();
        }

        [[nodiscard]] SlangCompileTarget toSlangCompileTarget_(const rhi::DeviceType device_type) noexcept {
            switch (device_type) {
                using enum rhi::DeviceType;
                case Default:
                case D3D12: return SLANG_DXIL;
                case D3D11: return SLANG_DXBC;
                case Vulkan:
                case WGPU: return SLANG_SPIRV;
                case Metal: return SLANG_METAL;
                case CPU: return SLANG_SHADER_HOST_CALLABLE;
                case CUDA: return SLANG_CUDA_OBJECT_CODE;
            }
            std::unreachable();
        }
    }

    // ------------------------------------------------------------------
    // adapter selection
    // ------------------------------------------------------------------

    namespace {
        // slang-rhi's public AdapterInfo exposes no per-adapter extension
        // list, so unusable ICD bridges are identified by name. The Microsoft
        // Direct3D12 (DZN) bridge advertises itself as a Vulkan adapter but
        // lacks VK_KHR_shader_non_semantic_info, which the required feature
        // set and bindless budgets need, so vkCreateDevice fails on it. On
        // Ubuntu WSL the loader sorts it ahead of llvmpipe, so relying on
        // slang-rhi's first-listed default adapter selects the failing one.
        [[nodiscard]] bool isUsableAdapterName_(const std::string_view name) noexcept {
            return not name.contains("DZN") and not name.contains("Direct3D12");
        }

        // Preference order among usable adapters: hardware first, software
        // (llvmpipe/lavapipe) as the fallback.
        [[nodiscard]] int adapterSelectionRank_(const slang_rhi::AdapterInfo &info) noexcept {
            switch (info.adapterType) {
                using enum slang_rhi::AdapterType;
                case Discrete: return 0;
                case Integrated: return 1;
                case Software: return 2;
                case Unknown: return 3;
            }
            std::unreachable();
        }

// Both callers are info-level PPR_LOG sites: the definition must gate on the
// log threshold, not on the coarse PPR_ENABLE_LOGGING switch. At a WARNING
// threshold the caller-side gate erases info-level calls with their argument
// expressions, and this helper would otherwise compile unused on Release.
#if PPR_LOG_LEVEL_THRESHOLD <= PPR_LOG_LEVEL_INFO
        [[nodiscard]] string_literal getAdapterTypeName_(const slang_rhi::AdapterType type) noexcept {
            switch (type) {
                using enum slang_rhi::AdapterType;
                case Discrete: return "discrete";
                case Integrated: return "integrated";
                case Software: return "software";
                case Unknown: return "unknown";
            }
            std::unreachable();
        }
#endif

        // Only the Vulkan backend is probed for a usable adapter: the DZN
        // bridge that breaks device creation is a Vulkan ICD, and Vulkan is
        // the sole backend behind DeviceType::Default on Linux. Explicit
        // non-Vulkan types, and Default elsewhere (whose slang-rhi resolution
        // falls back across APIs), keep slang-rhi's own adapter selection.
        [[nodiscard]] std::optional<rhi::DeviceType> resolveProbeDeviceType_(
            const rhi::DeviceType device_type) noexcept {
            if (device_type == rhi::DeviceType::Vulkan) {
                return rhi::DeviceType::Vulkan;
            }
#if defined(__linux__)
            if (device_type == rhi::DeviceType::Default) {
                return rhi::DeviceType::Vulkan;
            }
#endif
            return std::nullopt;
        }

        struct AdapterSelection {
            rhi::IAdapter *m_selected{nullptr};
            u32 m_candidate_count{0};
            u32 m_rejected_count{0};
        };

        // Enumerate the resolved backend's adapters, log each candidate, and
        // return the best usable one. Leaves m_selected null when the backend
        // exposes no adapters, so the caller can fall back to slang-rhi's own
        // resolution.
        [[nodiscard]] AdapterSelection selectConformantAdapter_(
            rhi::IRHI &instance, const rhi::DeviceType device_type) {
            AdapterSelection selection{};
            int best_rank = -1;

            for (u32 index = 0;; ++index) {
                rhi::IAdapter *const adapter = instance.getAdapter(device_type, index);
                if (not adapter) {
                    break;
                }

                const slang_rhi::AdapterInfo &info = adapter->getInfo();
                const std::string_view name{info.name};
                const bool usable = isUsableAdapterName_(name);
                ++selection.m_candidate_count;

                PPR_LOG(
                    RHI, info, "RHI adapter candidate",
                    {
                        {"index", index},
                        {"name", name},
                        {"type", getAdapterTypeName_(info.adapterType)},
                        {"usable", usable},
                    });

                if (not usable) {
                    ++selection.m_rejected_count;
                    continue;
                }

                const int rank = adapterSelectionRank_(info);
                if (not selection.m_selected or rank < best_rank) {
                    selection.m_selected = adapter;
                    best_rank = rank;
                }
            }

            return selection;
        }
    }

    // ------------------------------------------------------------------
    // persistent shader cache
    // ------------------------------------------------------------------

    namespace {
        /// Version-namespaced compiled-artifact store for slang-rhi entry points.
        ///
        /// Root: `<cwd>/.ppr-shader-cache/<format-version>/<slang-build-tag>/<device-type>/`.
        /// An entry is the lowercase hex of the opaque slang-rhi key plus `.bin`.
        /// The key already covers compiler, target and entry-point identity, so
        /// there is no header, checksum or additional hash. Reset by deleting
        /// the root directory; bump `kFormatVersion` when the layout changes.
        class PersistentShaderCache final : public slang_rhi::IPersistentCache {
        public:
            // Bump when the directory layout or entry encoding changes; stale
            // trees then stay unreachable instead of being misread.
            static constexpr std::string_view kFormatVersion{"1"};
            static constexpr std::size_t kMaxKeyBytes{128};
            static constexpr std::size_t kMaxBuildTagLength{64};
            static constexpr std::uintmax_t kMaxEntryBytes{64u * 1024u * 1024u};
#if defined(_WIN32)
            // MAX_PATH counts the whole path including the terminator.
            static constexpr std::size_t kMaxPathLength{259};
#else
            // PATH_MAX counts the whole path including the terminator.
            static constexpr std::size_t kMaxPathLength{4095};
#endif

            /// Builds the cache, or returns nullptr after one info log when the
            /// root cannot be created. A failed root is never installed on the
            /// device; failures after installation disable the cache silently
            /// (the first one logs once) instead of failing each lookup.
            static PersistentShaderCache *create_(
                const rhi::DeviceType device_type, slang::IGlobalSession *const global_session) {
                std::error_code ec;
                const std::filesystem::path working_directory = std::filesystem::current_path(ec);
                if (ec) {
                    logUnavailable_("working directory unavailable");
                    return nullptr;
                }

                std::filesystem::path root = working_directory / ".ppr-shader-cache" / kFormatVersion;

                if (global_session != nullptr) {
                    if (const char *const build_tag = global_session->getBuildTagString(); build_tag != nullptr) {
                        if (std::string sanitized = sanitizeBuildTag_(build_tag); not sanitized.empty()) {
                            root /= std::move(sanitized);
                        }
                    }
                }

                root /= std::string_view{getDeviceTypeName_(device_type)};

                std::filesystem::create_directories(root, ec);
                if (ec) {
                    logUnavailable_("cache root not creatable");
                    return nullptr;
                }

                return new PersistentShaderCache{std::move(root)};
            }

            ~PersistentShaderCache() = default;

            SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(
                const SlangUUID &uuid, void **const out_object) override {
                if (out_object == nullptr) {
                    return SLANG_E_INVALID_ARG;
                }

                if (uuid != IPersistentCache::getTypeGuid()) {
                    *out_object = nullptr;
                    return SLANG_E_NO_INTERFACE;
                }

                *out_object = static_cast<IPersistentCache *>(this);
                addRef();
                return SLANG_OK;
            }

            SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
                return m_reference_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
            }

            SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
                const uint32_t previous = m_reference_count.fetch_sub(1u, std::memory_order_acq_rel);
                if (previous == 1u) {
                    delete this;
                    return 0u;
                }
                return previous - 1u;
            }

            SLANG_NO_THROW SlangResult SLANG_MCALL writeCache(ISlangBlob *const key, ISlangBlob *const data) override {
                if (not m_available.load(std::memory_order_relaxed)) {
                    return SLANG_FAIL;
                }
                if (key == nullptr or data == nullptr) {
                    return SLANG_E_INVALID_ARG;
                }

                const std::string hex = keyHex_(key);
                const std::filesystem::path destination = entryPath_(hex);
                if (destination.empty()) {
                    return SLANG_OK;
                }

                const char *const buffer = static_cast<const char *>(data->getBufferPointer());
                const std::size_t buffer_size = data->getBufferSize();
                if (buffer_size > 0u and buffer == nullptr) {
                    return SLANG_E_INVALID_ARG;
                }

                // A per-process temporary keeps concurrent writers off each
                // other; the rename below publishes the finished entry. The
                // key hex keeps a crash leftover traceable to its entry.
                const std::filesystem::path temporary = temporaryPath_(
                    hex, m_sequence.fetch_add(1u, std::memory_order_relaxed));
                if (temporary.empty()) {
                    return SLANG_OK;
                }

                {
                    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
                    if (not stream) {
                        std::error_code ignored;
                        std::filesystem::remove(temporary, ignored);
                        disable_("temporary entry not writable");
                        return SLANG_FAIL;
                    }

                    stream.write(buffer, static_cast<std::streamsize>(buffer_size));
                    stream.close();
                    if (not stream) {
                        std::error_code ignored;
                        std::filesystem::remove(temporary, ignored);
                        disable_("temporary entry not writable");
                        return SLANG_FAIL;
                    }
                }

                std::error_code ec;
                std::filesystem::rename(temporary, destination, ec);
                if (not ec) {
                    return SLANG_OK;
                }

                // Windows rename refuses to replace an existing entry. That
                // means a concurrent writer already stored an equivalent
                // artifact, so the temporary is discarded without an error.
                std::error_code exists_ec;
                const bool destination_exists = std::filesystem::exists(destination, exists_ec);
                std::error_code remove_ec;
                std::filesystem::remove(temporary, remove_ec);
                if (destination_exists) {
                    return SLANG_OK;
                }

                disable_("entry rename failed");
                return SLANG_FAIL;
            }

            SLANG_NO_THROW SlangResult SLANG_MCALL queryCache(
                ISlangBlob *const key, ISlangBlob **const out_data) override {
                if (out_data == nullptr) {
                    return SLANG_E_INVALID_ARG;
                }
                *out_data = nullptr;

                if (not m_available.load(std::memory_order_relaxed)) {
                    return SLANG_E_NOT_FOUND;
                }
                if (key == nullptr) {
                    return SLANG_E_INVALID_ARG;
                }

                const std::string hex = keyHex_(key);
                const std::filesystem::path entry = entryPath_(hex);
                if (entry.empty()) {
                    noteMiss_();
                    return SLANG_E_NOT_FOUND;
                }

                // Silent miss path: a cold cache must not emit an error. Only
                // non-throwing size/open/read calls run here. The size check
                // enforces the read cap before any bytes are read.
                std::error_code size_ec;
                const std::uintmax_t entry_size = std::filesystem::file_size(entry, size_ec);
                if (size_ec or entry_size == 0u or entry_size > kMaxEntryBytes) {
                    noteMiss_();
                    return SLANG_E_NOT_FOUND;
                }

                std::vector<std::byte> bytes(static_cast<std::size_t>(entry_size));
                std::ifstream stream{entry, std::ios::binary};
                stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (not stream or stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
                    noteMiss_();
                    return SLANG_E_NOT_FOUND;
                }
                stream.close();

                // slang_createBlob returns one reference. The ComPtr takes that
                // reference over so a failure cannot leak, and detach hands the
                // caller exactly the reference it owns.
                rhi::ComPtr<ISlangBlob> blob{Slang::INIT_ATTACH, ::slang_createBlob(bytes.data(), bytes.size())};
                if (not blob) {
                    return SLANG_E_OUT_OF_MEMORY;
                }

                *out_data = blob.detach();
                noteHit_();
                return SLANG_OK;
            }

        private:
            explicit PersistentShaderCache(std::filesystem::path root) : m_root{std::move(root)} {
            }

            [[nodiscard]] static std::string keyHex_(ISlangBlob *const key) {
                const std::size_t key_size = key->getBufferSize();
                if (key_size == 0u or key_size > kMaxKeyBytes) {
                    return {};
                }

                const auto *const key_bytes = static_cast<const std::byte *>(key->getBufferPointer());
                if (key_bytes == nullptr) {
                    return {};
                }

                constexpr char hex_digits[] = "0123456789abcdef";
                std::string hex{};
                hex.reserve(key_size * 2u);
                for (std::size_t index = 0u; index < key_size; ++index) {
                    const unsigned value = std::to_integer<unsigned>(key_bytes[index]);
                    hex.push_back(hex_digits[value >> 4u]);
                    hex.push_back(hex_digits[value & 0x0fu]);
                }
                return hex;
            }

            [[nodiscard]] std::filesystem::path entryPath_(const std::string &hex) const {
                if (hex.empty()) {
                    return {};
                }

                std::filesystem::path entry = m_root / (hex + ".bin");
                if (entry.native().size() > kMaxPathLength) {
                    return {};
                }
                return entry;
            }

            [[nodiscard]] std::filesystem::path temporaryPath_(const std::string &hex, const u32 sequence) const {
                if (hex.empty()) {
                    return {};
                }

                std::filesystem::path temporary = m_root / std::format("{}.{}.{}.tmp", hex, processId_(), sequence);
                if (temporary.native().size() > kMaxPathLength) {
                    return {};
                }
                return temporary;
            }

            void disable_(const char *const reason) {
                if (m_available.exchange(false, std::memory_order_relaxed)) {
                    logUnavailable_(reason);
                }
            }

            static void logUnavailable_(const char *const reason) {
                PPR_LOG_ONCE(RHI, info, "persistent shader cache disabled", {{"reason", reason}});
            }

            static void noteHit_() {
                PPR_LOG_ONCE(RHI, info, "persistent shader cache hit");
            }

            static void noteMiss_() {
                PPR_LOG_ONCE(RHI, info, "persistent shader cache miss");
            }

            [[nodiscard]] static bool isBuildTagCharacter_(const char character) noexcept {
                if (character >= 'a' and character <= 'z') {
                    return true;
                }
                if (character >= 'A' and character <= 'Z') {
                    return true;
                }
                if (character >= '0' and character <= '9') {
                    return true;
                }
                return character == '.' or character == '_' or character == '-';
            }

            [[nodiscard]] static std::string sanitizeBuildTag_(const char *const build_tag) {
                std::string sanitized{};
                for (const char *cursor = build_tag; cursor != nullptr and *cursor != '\0'; ++cursor) {
                    const char character = *cursor;
                    if (isBuildTagCharacter_(character)) {
                        sanitized.push_back(character);
                    }
                }

                // A single path component must not traverse ('.', '..'),
                // and an unexpectedly long tag is omitted rather than
                // truncated, which could alias two builds.
                if (sanitized == "." or sanitized == ".." or sanitized.size() > kMaxBuildTagLength) {
                    return {};
                }
                return sanitized;
            }

            [[nodiscard]] static int processId_() noexcept {
#if defined(_WIN32)
                return ::_getpid();
#else
                return static_cast<int>(::getpid());
#endif
            }

            std::filesystem::path m_root{};
            std::atomic<u32> m_reference_count{1u};
            std::atomic<u32> m_sequence{0u};
            std::atomic<bool> m_available{true};
        };
    }

    // ------------------------------------------------------------------
    // projection free functions
    // ------------------------------------------------------------------

    [[nodiscard]] float4x4 rhi::getOrthoMatrix(const float width, const float height) noexcept {
        return float4x4::orthoD3D(0.0f, width, 0.0f, height, 0.0f, 1.0f);
    }

    [[nodiscard]] float4x4 rhi::getPerspectiveMatrix(
        const float fov, const float aspect, const float near_, const float far_) noexcept {
        const float x_fov = 2.0f * std::atan(std::tan(fov * 0.5f) * aspect);
        return float4x4::perspectiveD3D(x_fov, fov, near_, far_);
    }

    // ------------------------------------------------------------------
    // slang RHI service
    // ------------------------------------------------------------------

    namespace {
        class SlangRhiService final : public IRhiService {
        public:
            rhi::ComPtr<rhi::IDevice> m_device{};
            // Compiled-artifact store shared with the device for this service
            // lifetime. The device addRefs it; shutdown() releases it only
            // after the device is gone.
            rhi::ComPtr<slang_rhi::IPersistentCache> m_shader_cache{};
            // Ownership of the slang-rhi singleton. getRHI() hands us a
            // process-lifetime instance; destroyRHI() frees it. We may
            // destroy it exactly once — that single successful-initialise
            // owns it. A secondary shutdown()/getRHI() cycle (unit tests boot
            // and tear down GPU subsystems repeatedly) must NOT destroy it
            // again: the freed instance is poison and the next getRHI()/init
            // would hand the poisoned object back for member access.
            bool m_instance_owned{false};
            // Default off; set explicitly at the application boundary before initialize().
            rhi::DebugLayerOptions m_debug_options{
                .coreValidation = PPR_ENABLE_DEBUG,
#if PPR_ENABLE_SANITIZER_ADDRESS
                .GPUAssistedValidation = true,
#else
                .GPUAssistedValidation = false,
#endif
            };

            [[nodiscard]] rhi::IRHI &getInstance() const noexcept override {
                return *slang_rhi::getRHI();
            }

            [[nodiscard]] rhi::IDevice &getDevice() const noexcept override {
                return *m_device;
            }

            void setCoreValidation(const bool enabled) noexcept override {
                m_debug_options.coreValidation = enabled;
            }

            [[nodiscard]] bool isCoreValidationEnabled() const noexcept override {
                return m_debug_options.coreValidation;
            }

            void setGpuAssistedValidation(const bool enabled) noexcept override {
                m_debug_options.GPUAssistedValidation = enabled;
            }

            [[nodiscard]] bool isGpuAssistedValidationEnabled() const noexcept override {
                return m_debug_options.GPUAssistedValidation;
            }

            [[nodiscard]] std::error_code initialize(
                const rhi::DeviceType device_type, IShaderService &shader_service) override {
                const hal::ProfileScope scope{"RHI.Initialize"};
                if (m_device) {
                    PPR_LOG(RHI, warning, "RHI already initialized");
                    return rhi::errc::ok;
                }

                rhi::IRHI *const p_instance = slang_rhi::getRHI();
                if (not p_instance) {
                    PPR_LOG(RHI, error, "failed to get RHI instance");
                    return rhi::errc::no_interface;
                }

                rhi::ComPtr<rhi::IDevice> device{};
                bool initialized = false;
                PPR_DEFER {
                    if (not initialized) {
                        device.setNull();
                        m_shader_cache.setNull();
                        const ::slang_rhi::Result result = ::slang_rhi::destroyRHI();
                        const std::error_code cleanup_error = make_error_code(result);
                        if (cleanup_error) {
                            PPR_LOG(
                                RHI, error, "RHI initialization cleanup failed",
                                {{"category", cleanup_error.category().name()}, {"cause", cleanup_error.message()}});
                        }
                        m_instance_owned = false;
                    }
                };

                constexpr slang_rhi::Feature required_features[] = {
                    slang_rhi::Feature::Surface,
                    slang_rhi::Feature::Rasterization,
                };

                rhi::DeviceDesc desc{};
                desc.deviceType = device_type;
                desc.slang.slangGlobalSession = shader_service.getGlobalSession();

                if (PersistentShaderCache *const shader_cache = PersistentShaderCache::create_(
                        device_type, desc.slang.slangGlobalSession);
                    shader_cache != nullptr) {
                    m_shader_cache.attach(shader_cache);
                    desc.persistentShaderCache = m_shader_cache.get();
                }

                // TEMPORARY: attribute cold scene pipeline creation to compiler and native phases.
                desc.enableCompilationReports = true;

                // PPR does not launch CUDA kernels from graphics command buffers.
                // VK_NVX_binary_import fails device creation on some NVIDIA drivers.
                desc.enableCUDALaunchFromGfx = false;

                desc.requiredFeatures = required_features;
                desc.requiredFeatureCount = safe_narrowing(std::size(required_features));

                // Bindless budget: create-time only. Feature::Bindless
                // stays OUT of requiredFeatures (would fail weak hardware);
                // pass caches check hasFeature(Bindless) at init instead.
                desc.bindless.textureCount = rhi::kBindlessTextureBudget;
                desc.bindless.combinedTextureSamplerCount = rhi::kBindlessCombinedBudget;
                desc.bindless.samplerCount = rhi::kBindlessSamplerBudget;
                desc.bindless.bufferCount = rhi::kBindlessBufferBudget;

                AdapterSelection selection{};
                if (const std::optional<rhi::DeviceType> probe_type = resolveProbeDeviceType_(device_type);
                    probe_type) {
                    selection = selectConformantAdapter_(*p_instance, *probe_type);

                    if (selection.m_candidate_count == 0) {
                        PPR_LOG(RHI, warning, "no RHI adapters enumerated; deferring to slang-rhi selection");
                    }
                }

                if (selection.m_selected) {
                    desc.adapter = selection.m_selected;
                    PPR_LOG(
                        RHI, info, "selected RHI adapter",
                        {
                            {"name", std::string_view{selection.m_selected->getInfo().name}},
                            {"type", getAdapterTypeName_(selection.m_selected->getInfo().adapterType)},
                        });
                }

                if (selection.m_candidate_count > 0 and not selection.m_selected) {
                    // Every enumerated adapter is a non-conformant bridge
                    // (e.g. DZN). Fail with a named cause instead of letting
                    // slang-rhi pick the first adapter and surface an opaque
                    // vkCreateDevice extension error.
                    PPR_LOG(
                        RHI, error, "no usable RHI adapter; set VK_ICD_FILENAMES to a conformant ICD",
                        {{"device_type", getDeviceTypeName_(device_type)}, {"rejected", selection.m_rejected_count}});
                    PPR_RETURN_ERROR_ON_FAIL(RHI, rhi::errc::not_available);
                }

#if PPR_ENABLE_LOGGING
                if (m_debug_options.isDebugLayersEnabled()) {
                    // Effective opt-in state: off unless the application boundary
                    // set it before initialize() (--gpu-validation).
                    PPR_LOG(
                        RHI, info, "enabled Slang RHI debug and validation layers",
                        {{"coreValidation", m_debug_options.coreValidation},
                            { "GPUAssistedValidation",
                                m_debug_options.GPUAssistedValidation }});

                    PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->setDebugLayerOptions(m_debug_options));

                    p_instance->enableDebugLayers();

                    desc.enableValidation = true;
                    desc.debugCallback = SlangRhiDebugCallback::get();
                }
#endif

                {
                    const hal::ProfileScope scope_create{"RHI.CreateDevice"};
                    PPR_RETURN_ERROR_ON_FAIL(RHI, p_instance->createDevice(desc, device.writeRef()));
                }

                if (not device->hasFeature(rhi::Feature::Bindless)) {
                    PPR_LOG(
                        RHI, warning, "device lacks bindless support; GPU caches will fail at init",
                        {{"device_type", getDeviceTypeName_(device_type)}});
                }

                const SlangCompileTarget compile_target = toSlangCompileTarget_(device->getDeviceType());

                PPR_RETURN_ERROR_ON_FAIL(RHI, shader_service.setTargetFormat(compile_target));

                m_device = std::move(device);
                m_instance_owned = true;
                initialized = true;
                PPR_LOG(
                    RHI, info, "RHI device created successfully",
                    {
                        {"device_type", getDeviceTypeName_(device_type)},
                        {"compile_target", static_cast<int>(compile_target)},
                        {"bindless_textures", rhi::kBindlessTextureBudget},
                        {"bindless_combined", rhi::kBindlessCombinedBudget},
                        {"bindless_samplers", rhi::kBindlessSamplerBudget},
                        {"bindless_buffers", rhi::kBindlessBufferBudget},
                    });
                return make_error_code(SLANG_OK);
            }

            [[nodiscard]] std::error_code shutdown() override {
                if (m_device) {
                    m_device.setNull();
                }
                // Order matters: the device holds its own reference, so the
                // service reference is dropped only after the device is gone.
                m_shader_cache.setNull();
                if (m_instance_owned) {
                    const ::slang_rhi::Result result = ::slang_rhi::destroyRHI();
                    m_instance_owned = false;
                    PPR_LOG(RHI, info, "RHI instance teardown", {{"result", static_cast<int>(result)}});
                    PPR_RETURN_ERROR_ON_FAIL(RHI, make_error_code(result));
                }

                PPR_LOG(RHI, info, "RHI service shut down");
                return default_value_v;
            }

            [[nodiscard]] rhi::Result createRenderPipeline(
                const rhi::RenderPipelineDesc &desc, rhi::IRenderPipeline **outPipeline) override {
                return m_device->createRenderPipeline(desc, outPipeline);
            }
        };
    }

    /*static*/
    safe_ptr<IRhiService> IRhiService::get() noexcept {
        static SlangRhiService g_instance{};
        return safe_ptr<IRhiService>(&g_instance);
    }
}
