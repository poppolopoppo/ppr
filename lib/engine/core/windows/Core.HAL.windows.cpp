module;

// WIN32_LEAN_AND_MEAN excludes rarely-used services from windows headers.
#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif

// The below excludes some other unused services from the windows headers -- see windows.h for details.
#define NOGDICAPMASKS			// CC_*, LC_*, PC_*, CP_*, TC_*, RC_
//#define NOVIRTUALKEYCODES		// VK_*
//#define NOWINMESSAGES			// WM_*, EM_*, LB_*, CB_*
//#define NOWINSTYLES			// WS_*, CS_*, ES_*, LBS_*, SBS_*, CBS_*
//#define NOSYSMETRICS			// SM_*
//#define NOMENUS				// MF_*
//#define NOICONS				// IDI_*
//#define NOKEYSTATES			// MK_*
//#define NOSYSCOMMANDS			// SC_*
//#define NORASTEROPS			// Binary and Tertiary raster ops
//#define NOSHOWWINDOW			// SW_*
//#define OEMRESOURCE			// OEM Resource values
#define NOATOM					// Atom Manager routines
//#define NOCLIPBOARD			// Clipboard routines
//#define NOCOLOR				// Screen colors
//#define NOCTLMGR				// Control and Dialog routines
#define NODRAWTEXT				// DrawText() and DT_*
//#define NOGDI					// All GDI #defines and routines
#define NOKERNwindows minimalEL	// All KERNEL #defines and routines
//#define NOUSER				// All USER #defines and routines
//#define NONLS					// All NLS #defines and routines
//#define NOMB					// MB_* and MessageBox()
#define NOMEMMGR				// GMEM_*, LMEM_*, GHND, LHND, associated routines
#define NOMETAFILE				// typedef METAFILEPICT
//#define NOMINMAX				// Macros min(a,b) and max(a,b)
//#define NOMSG					// typedef MSG and associated routines
#define NOOPENFILE				// OpenFile(), OemToAnsi, AnsiToOem, and OF_*
#define NOSCROLL				// SB_* and scrolling routines
#define NOSERVICE				// All Service Controller routines, SERVICE_ equates, etc.
#define NOSOUND					// Sound driver routines
//#define NOTEXTMETRIC			// typedef TEXTMETRIC and associated routines
//#define NOWH					// SetWindowsHook and WH_*
//#define NOWINOFFSETS			// GWL_*, GCL_*, associated routines
#define NOCOMM					// COMM driver routines
#define NOKANJI					// Kanji support stuff.
#define NOHELP					// Help engine interface.

#ifdef NDEBUG
#define NOPROFILER				// Profiler interface.
#endif
#define NODEFERWINDOWPOS		// DeferWindowPos routines
#define NOMCX					// Modem Configuration Extensions
#define NOCRYPT
#define NOTAPE
#define NOIMAGE
#define NOPROXYSTUB
#define NORPC

#include <Windows.h>
#include <Memoryapi.h>

#include <crtdbg.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <werapi.h>

// clean the mess after windows.h:#undef CreateDirectory
#undef CreateProcess
#undef CreateSemaphore
#undef CreateWindow
#undef MemoryBarrier
#undef MoveFile
#undef RegisterClass
#undef RemoveDirectory
#undef Yield
#undef small
#undef min
#undef max

#include "pP/Macros.h"

// for VirtualAlloc2() and MapViewOfFile3()
#pragma comment(lib, "mincore.lib")

module engine.core;

import :assert;
import :hal;
import :memory;

import std;

namespace pP::hal {
    [[nodiscard]] std::string_view platformName() noexcept {
        return "windows";
    }

    // ------------------------------------------------------------------
    // win32 exception
    // ------------------------------------------------------------------

    struct Win32LastError {
        ::DWORD m_errno{0};

        Win32LastError() noexcept
            : m_errno(::GetLastError()) {
        }

        explicit Win32LastError(const errno_t errno) noexcept
            : m_errno(errno) {
        }

        [[nodiscard]] std::size_t format(char *const buffer, const std::size_t capacity) const noexcept {
            PPR_ASSERT(buffer && capacity > 0);
            if (!buffer || capacity == 0) [[unlikely]] {
                return 0u;
            }

            ::DWORD len = ::FormatMessageA(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, m_errno,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buffer, checked_cast<DWORD>(capacity) - 1/* reserve null char */, nullptr);

            if (len <= 0) [[unlikely]] {
                constexpr char fallback[] = "unknown win32 error";
                if (std::size(fallback) < capacity) {
                    std::memcpy(buffer, fallback, sizeof(fallback));
                    len = static_cast<::DWORD>(std::size(fallback) - 1/* \0 */);
                }
            }

            PPR_ASSERT(len < capacity);
            buffer[len] = '\0'; // Ensure null-termination
            return checked_cast<std::size_t>(len);
        }

        [[nodiscard]] std::string message() const {
            char buffer[512];
            const std::size_t len = format(buffer, std::size(buffer));
            return {buffer, len};
        }
    };

    class [[nodiscard]] Win32Exception : public std::runtime_error {
        Win32LastError m_last_error;

    public:
        Win32Exception() noexcept
            : Win32Exception(Win32LastError{}) {
        }

        explicit Win32Exception(const Win32LastError last_error) noexcept
            : std::runtime_error(last_error.message()),
              m_last_error(last_error) {
        }

        [[nodiscard]] Win32LastError getLastError() const noexcept {
            return m_last_error;
        }
    };

    // ------------------------------------------------------------------
    // operating-system
    // ------------------------------------------------------------------

    [[nodiscard]] std::string_view userName() {
        static const std::string g_username = []() -> std::string {
            wchar_t buffer[256];
            DWORD size = std::size(buffer);
            if (::GetUserNameW(buffer, &size)) {
                return native::ansi(native::string_view(buffer, size - 1));
            }
            return "unknown_user";
        }();
        return g_username;
    }

    // ------------------------------------------------------------------
    // file-system
    // ------------------------------------------------------------------

    [[nodiscard]] const std::filesystem::directory_entry &homeDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            wchar_t buffer[MAX_PATH];
            const DWORD len = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(buffer)
                );
            }

            // Fallback: use HOMEDRIVE + HOMEPATH
            wchar_t drive[MAX_PATH], path[MAX_PATH];
            const DWORD dlen = ::GetEnvironmentVariableW(L"HOMEDRIVE", drive, MAX_PATH);
            const DWORD plen = ::GetEnvironmentVariableW(L"HOMEPATH", path, MAX_PATH);

            if (dlen > 0 && plen > 0) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(std::wstring(drive) + std::wstring(path))
                );
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &systemDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            wchar_t buffer[MAX_PATH];
            const UINT len = ::GetSystemDirectoryW(buffer, MAX_PATH);

            if (len > 0 && len < MAX_PATH) {
                return std::filesystem::directory_entry(
                    std::filesystem::path(buffer)
                );
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            PWSTR path = nullptr;

            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
                const std::filesystem::path p = path;
                ::CoTaskMemFree(path);
                return std::filesystem::directory_entry(p);
            }

            return {};
        }();
        return g_directory;
    }

    [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir() {
        static const auto g_directory = []() -> std::filesystem::directory_entry {
            PWSTR path = nullptr;

            if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
                const std::filesystem::path p = path;
                ::CoTaskMemFree(path);
                return std::filesystem::directory_entry(p);
            }

            return {};
        }();
        return g_directory;
    }

    // ------------------------------------------------------------------
    // memory pages
    // ------------------------------------------------------------------

    const std::size_t page_size = []() noexcept -> std::size_t {
        SYSTEM_INFO sys_info;
        ::GetSystemInfo(&sys_info);
        return checked_cast<std::size_t>(sys_info.dwPageSize);
    }();

    const std::align_val_t page_granularity = []() noexcept -> std::align_val_t {
        SYSTEM_INFO sys_info;
        ::GetSystemInfo(&sys_info);
        return std::align_val_t{checked_cast<std::size_t>(sys_info.dwAllocationGranularity)};
    }();

    [[nodiscard]] PPR_FORCE_INLINE static constexpr ::DWORD pageProtectionFlags_(const PageProtection protect) noexcept {
        if (protect.read) [[likely]] {
            if (protect.write) [[likely]] {
                return protect.execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            }
            return protect.execute ? PAGE_EXECUTE_READ : PAGE_READONLY;
        }
        if (protect.write) {
            return protect.execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
        }
        return protect.execute ? PAGE_EXECUTE : PAGE_NOACCESS;
    }

#if WINVER < 0x0A00
    // fallback using VirtualAlloc() to force alignment with repeated allocations
    [[nodiscard]] static PVOID WINAPI alignedVirtualAllocFallback_(
        [[maybe_unused]] HANDLE process,
        PVOID p_base_address,
        SIZE_T size,
        ULONG allocation_type,
        ULONG protection_flags,
        MEM_EXTENDED_PARAMETER *p_params,
        ULONG num_params) {
        // Optimistically try mapping precisely the right amount before falling back to the slow method :
        void *p = ::VirtualAlloc(p_base_address, size, allocation_type, protection_flags);

        std::size_t alignment_v = static_cast<std::size_t>(page_granularity);
        for (ULONG i = 0u; i < num_params; ++i) {
            if (p_params[i].Type == MemExtendedParameterAddressRequirements) {
                const auto *const p_address_requirements = static_cast<const MEM_ADDRESS_REQUIREMENTS *>(p_params[i].Pointer);
                if (p_address_requirements->Alignment) {
                    alignment_v = static_cast<std::size_t>(p_address_requirements->Alignment);
                }
            }
        }

        if (alignForward(p, std::align_val_t{alignment_v}) != p) {
            // Fill "bubbles" (reserve unaligned regions) at the beginning of virtual address space, otherwise there will be always falling back to the slow method
            if (std::bit_cast<std::uintptr_t>(p) < 16 * 1024 * 1024)
                ::VirtualAlloc(p, alignment_v - (std::bit_cast<std::uintptr_t>(p) & (alignment_v - 1u)), MEM_RESERVE, PAGE_NOACCESS);

            do {
                p = ::VirtualAlloc(nullptr, size + alignment_v - static_cast<std::size_t>(page_granularity), MEM_RESERVE, PAGE_NOACCESS);
                if (nullptr == p) // if OOM
                    return nullptr;

                ::VirtualFree(p, 0, MEM_RELEASE); // Unfortunately, WinAPI doesn't support release a part of allocated region, so release a whole region

                p = ::VirtualAlloc(
                    std::bit_cast<void *>(std::bit_cast<std::uintptr_t>(p) + (alignment_v - 1u) & ~(alignment_v - 1u)),
                    size, allocation_type, protection_flags);
            } while (nullptr == p);
        }

        PPR_ASSERT(alignForward(p, std::align_val_t{alignment_v}) == p);
        return p;
    }
#endif

    // try to dynamically load VirtualAlloc2(), and unlock native alignment and support MEM_64K_PAGES (to reduce TLB pressure for whole engine)
    [[nodiscard]] PPR_FORCE_INLINE static void *alignedVirtualAlloc_(
        const std::size_t size,
        const std::align_val_t alignment,
        const ::DWORD allocation_type,
        const ::DWORD protection_flags) {
        using virtual_alloc_2_f = PVOID (WINAPI*)(
            HANDLE, PVOID, SIZE_T, ULONG, ULONG,
            MEM_EXTENDED_PARAMETER *, ULONG);
#if WINVER >= 0x0A00
        static constexpr virtual_alloc_2_f g_virtual_alloc_2 = &::VirtualAlloc2;
#else
        static const auto g_virtual_alloc_2 = []() noexcept -> virtual_alloc_2_f {
            if (const ::HMODULE h_kernel = ::GetModuleHandleW(L"KernelBase.dll")) [[likely]] {
                const auto fn_virtual_alloc_2 = reinterpret_cast<virtual_alloc_2_f>(
                    ::GetProcAddress(h_kernel, "VirtualAlloc2"));

                if (fn_virtual_alloc_2) {
                    return fn_virtual_alloc_2;
                }
            }
            return &alignedVirtualAllocFallback_;
        }();
#endif

        ::MEM_ADDRESS_REQUIREMENTS address_requirements{};
        address_requirements.Alignment = static_cast<std::size_t>(alignment);

        ::MEM_EXTENDED_PARAMETER params{
            .Type = MemExtendedParameterAddressRequirements,
            .Pointer = &address_requirements
        };

        return g_virtual_alloc_2(
            nullptr,
            nullptr,
            size,
            allocation_type | MEM_64K_PAGES,
            protection_flags,
            &params, 1u);
    }

    [[nodiscard]] std::allocation_result<void *> pageAlloc(
        const std::size_t size,
        const bool commit,
        const PageProtection allowed,
        std::align_val_t alignment) {
        PPR_ASSERT((static_cast<std::size_t>(alignment) % static_cast<std::size_t>(page_granularity)) == 0u);
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));

        void *const p_result = alignedVirtualAlloc_(
            aligned_size,
            alignment,
            MEM_RESERVE | (commit ? MEM_COMMIT : 0),
            pageProtectionFlags_(allowed));

        if (!p_result) [[unlikely]] {
            throw std::bad_alloc();
        }

        PPR_ASSERT(alignForward(p_result, alignment) == p_result);

#if PPR_ENABLE_ASSERTIONS
        //  https://msdn.microsoft.com/en-us/library/windows/desktop/aa366902(v=vs.85).aspx
        ::MEMORY_BASIC_INFORMATION info;
        if (PPR_ENSURE(::VirtualQuery(p_result, &info, sizeof(info)))) {
            PPR_ASSERT(info.BaseAddress == p_result && "Allocate memory with an invalid pointer");
            PPR_ASSERT((info.State & (MEM_COMMIT|MEM_RESERVE)) && "Allocate unreserved memory");
            PPR_ASSERT(info.RegionSize == aligned_size && "Allocate with unmatching region size");
        }
#endif

        return std::allocation_result(p_result, aligned_size);
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::VirtualAlloc(
                ptr, size,
                MEM_COMMIT,
                pageProtectionFlags_(allowed)) == nullptr) [[unlikely]] {
            throw std::bad_alloc();
        }
    }

    void pageDecommit(void *const ptr, const std::size_t size) noexcept(false) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::VirtualFree(ptr, size, MEM_DECOMMIT) == FALSE) {
            throw std::bad_alloc();
        }
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) {
        ::DWORD old_protect;
        if (::VirtualProtect(ptr, size, pageProtectionFlags_(allowed), &old_protect) == FALSE) {
            throw std::bad_alloc();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) noexcept(false) {
        if (::OfferVirtualMemory(ptr, size, VmOfferPriorityNormal) != ERROR_SUCCESS) {
            throw std::bad_alloc();
        }
    }

    [[nodiscard]] bool pageReclaimFromOS(void *const ptr, const std::size_t size) {
        switch (::ReclaimVirtualMemory(ptr, size)) {
            case ERROR_SUCCESS:
            case ERROR_BUSY:
                return true;
            default:
                return false;
        }
    }

    void pageFree(void *const ptr, [[maybe_unused]] const std::size_t size) {
        if (!::VirtualFree(ptr, 0u, MEM_RELEASE)) {
            throw std::bad_alloc();
        }
    }

    // ------------------------------------------------------------------
    // ring buffer
    // ------------------------------------------------------------------

    /// https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2
    void *ringBufferAlloc(const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        ::HANDLE section = nullptr;
        void *ringBuffer = nullptr;
        void *placeholder1 = nullptr;
        void *placeholder2 = nullptr;
        void *view1 = nullptr;
        void *view2 = nullptr;

        //
        // Reserve a placeholder region where the buffer will be mapped.
        //

        placeholder1 = static_cast<PCHAR>(::VirtualAlloc2(
            nullptr,
            nullptr,
            2u * buffer_size,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
            PAGE_NOACCESS,
            nullptr, 0
        ));

        if (placeholder1 == nullptr) {
            throw Win32Exception();
        }

        //
        // Split the placeholder region into two regions of equal size.
        //

        const BOOL result = ::VirtualFree(
            placeholder1,
            buffer_size,
            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER
        );

        if (result == FALSE) {
            throw Win32Exception();
        }

        placeholder2 = reinterpret_cast<void *>(reinterpret_cast<ULONG_PTR>(placeholder1) + buffer_size);

        //
        // Create a pagefile-backed section for the buffer.
        //

        section = ::CreateFileMapping(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            safe_narrowing{buffer_size}, nullptr
        );

        if (section == nullptr) {
            throw Win32Exception();
        }

        //
        // Map the section into the first placeholder region.
        //

        view1 = ::MapViewOfFile3(
            section,
            nullptr,
            placeholder1,
            0,
            buffer_size,
            MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE,
            nullptr, 0
        );

        if (view1 == nullptr) {
            throw Win32Exception();
        }

        //
        // Ownership transferred, don't free this now.
        //

        placeholder1 = nullptr;

        //
        // Map the section into the second placeholder region.
        //

        view2 = ::MapViewOfFile3(
            section,
            nullptr,
            placeholder2,
            0,
            buffer_size,
            MEM_REPLACE_PLACEHOLDER,
            PAGE_READWRITE,
            nullptr, 0
        );

        if (view2 == nullptr) {
            throw Win32Exception();
        }

        //
        // Success, return both mapped views to the caller.
        //

        ringBuffer = view1;
        // void *const secondaryView = view2;

        ::CloseHandle(section);
        return ringBuffer;
    }

    void ringBufferFree(const void *ring_buffer, const std::size_t buffer_size) noexcept(false) {
        PPR_ASSERT(ring_buffer != nullptr);
        PPR_ASSERT(alignForward(buffer_size, page_granularity) == buffer_size);

        if (not::UnmapViewOfFile(ring_buffer)) {
            throw Win32Exception();
        }

        if (const auto *secondary_view = static_cast<const std::byte *>(ring_buffer) + buffer_size;
            not::UnmapViewOfFile(secondary_view)) {
            throw Win32Exception();
        }
    }

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, char8_t *const p_dst, const std::size_t capacity) noexcept {
        static_assert(sizeof(char8_t) == sizeof(char));
        const std::size_t n_chars = std::min(ansi.size(), capacity);
        memcpy(p_dst, ansi.data(), n_chars * sizeof(char8_t));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(const std::string_view ansi, wchar_t *p_dst, const std::size_t capacity) noexcept {
        const int n_chars = ::MultiByteToWideChar(
            CP_ACP, 0,
            ansi.data(), static_cast<int>(ansi.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])));
        PPR_ASSERT(n_chars == ansi.size() || (n_chars > 0 && n_chars <= capacity));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, wchar_t *p_dst, const std::size_t capacity) noexcept {
        static_assert(sizeof(*LPCCH{}) == sizeof(char8_t));
        const int n_chars = ::MultiByteToWideChar(
            CP_UTF8, 0,
            reinterpret_cast<LPCCH>(utf8.data()), static_cast<int>(utf8.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])));
        PPR_ASSERT(n_chars <= utf8.size() || (n_chars > 0 && n_chars <= capacity));
        return n_chars;
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char8_t *p_dst, const std::size_t capacity) noexcept {
        const int n_bytes = ::WideCharToMultiByte(
            CP_UTF8, 0,
            wide.data(), static_cast<int>(wide.size()),
            reinterpret_cast<LPSTR>(p_dst), static_cast<int>(capacity * sizeof(p_dst[0])),
            nullptr, nullptr);
        PPR_ASSERT(n_bytes >= wide.size() || (n_bytes > 0 && n_bytes <= capacity));
        return static_cast<std::size_t>(n_bytes);
    }

    [[nodiscard]] std::size_t transcode(const std::wstring_view wide, char *const p_dst, const std::size_t capacity) noexcept {
        const int n_bytes = ::WideCharToMultiByte(
            CP_ACP, 0,
            wide.data(), static_cast<int>(wide.size()),
            p_dst, static_cast<int>(capacity * sizeof(p_dst[0])),
            nullptr, nullptr);
        PPR_ASSERT(n_bytes >= wide.size() || (n_bytes > 0 && n_bytes <= capacity));
        return static_cast<std::size_t>(n_bytes);
    }

    [[nodiscard]] std::size_t transcode(const std::u8string_view utf8, char *const p_dst, const std::size_t capacity) noexcept {
        // TODO: use thread-local transient allocator
        const std::wstring wide = toString<wchar_t>(utf8);
        return transcode(wide, p_dst, capacity);
    }

    // ------------------------------------------------------------------
    // debugger
    // ------------------------------------------------------------------

    void outputDebug(const char *ansi_msg) noexcept {
#if PPR_ENABLE_DEBUG
        ::OutputDebugStringA(ansi_msg);
#else
        (void) ansi_msg;
#endif
    }

    void outputDebug(const native::char_t *wide_msg) noexcept {
#if PPR_ENABLE_DEBUG
        ::OutputDebugStringW(wide_msg);
#else
        (void) wide_msg;
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#if PPR_ENABLE_DEBUG
        return ::IsDebuggerPresent();
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#if PPR_ENABLE_DEBUG
        __debugbreak();
#endif
    }

    void breakpointIfDebugging() noexcept {
#if PPR_ENABLE_DEBUG
        if (::IsDebuggerPresent())
            __debugbreak();
#endif
    }

    void disableSystemErrorReporting() noexcept {
        // Redirect abort() / _wassert() output to stderr instead of a dialog
        ::_set_error_mode(_OUT_TO_STDERR);

        // Redirect all three CRT report channels away from dialog boxes
        for (const int channel: {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            ::_CrtSetReportMode(channel, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            ::_CrtSetReportFile(channel, _CRTDBG_FILE_STDERR);
        }

        // Suppress OS-level error dialogs:
        //   SEM_NOGPFAULTERRORBOX  – no crash dialog for access violations / GP faults
        //   SEM_FAILCRITICALERRORS – no "insert disk" / hard-error dialogs
        //   SEM_NOOPENFILEERRORBOX – no missing-DLL popup dialogs
        ::SetErrorMode(SEM_NOGPFAULTERRORBOX |
                       SEM_FAILCRITICALERRORS |
                       SEM_NOOPENFILEERRORBOX);

        // Belt-and-suspenders: tell WER to queue reports silently for this process.
        // Required if any crash path bypasses SetErrorMode (e.g. __fastfail).
        ::WerSetFlags(WER_FAULT_REPORTING_FLAG_QUEUE);
    }

    // ------------------------------------------------------------------
    // spawn process helpers
    // ------------------------------------------------------------------

    namespace process {
        [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false) {
            wchar_t buffer[MAX_PATH];
            const DWORD len = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (len == 0 || len >= MAX_PATH) {
                throw std::runtime_error("Failed to get executable path");
            }
            return std::filesystem::path(buffer, buffer + len);
        }

        [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false) {
            std::wstring cmdline = L"\"" + executable.wstring() + L"\"";
            for (const auto &arg: args) {
                cmdline += L" \"";
                cmdline += toString<wchar_t>(std::string_view(arg));
                cmdline += L'"';
            }

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi{};

            constexpr DWORD creationFlags = CREATE_NO_WINDOW | CREATE_DEFAULT_ERROR_MODE;

            if (!::CreateProcessW(
                executable.c_str(),
                cmdline.data(),
                nullptr, nullptr, FALSE,
                creationFlags, nullptr, nullptr, &si, &pi)) {
                throw std::runtime_error("Failed to create process");
            }

            ::CloseHandle(pi.hThread);
            ::WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exit_code = 0;
            if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
                exit_code = 0;
            }
            ::CloseHandle(pi.hProcess);

            return static_cast<int>(exit_code);
        }
    }
}

namespace pP::hal::io {
    // ------------------------------------------------------------------
    // platform-specific state structures
    // ------------------------------------------------------------------

    struct IoHandleData {
        ::HANDLE m_port{nullptr}; // IOCP handle
    };

    struct FileHandleData {
        ::HANDLE m_file{INVALID_HANDLE_VALUE};
    };

    struct MapHandleData {
        ::HANDLE m_mapping{nullptr};
        void    *m_data{nullptr};
        std::size_t m_size{0};
    };

    // OVERLAPPED-compatible header that carries user_data
    struct OverlappedExt : public ::OVERLAPPED {
        void *m_user_data{nullptr};
    };

    static_assert(sizeof(OverlappedExt) == sizeof(::OVERLAPPED) + sizeof(void *));
    static_assert(sizeof(OverlappedExt) <= overlapped_storage_size_v);

    // IOCP API uses ULONG_PTR as opaque completion key (file association + completion status).
    // HANDLE and ULONG_PTR are both pointer-sized on all MSVC targets — no truncation.
    static_assert(sizeof(::HANDLE) <= sizeof(::ULONG_PTR),
        "IOCP completion key must be wide enough to hold a HANDLE");

    // ------------------------------------------------------------------
    // lifecycle
    // ------------------------------------------------------------------

    IoHandle init() noexcept(false) {
        const ::HANDLE port = ::CreateIoCompletionPort(
            INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (port == nullptr) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: CreateIoCompletionPort failed");
        }

        auto *data = new IoHandleData();
        data->m_port = port;
        return static_cast<IoHandle>(data);
    }

    void deinit(const IoHandle handle) noexcept {
        auto *data = static_cast<IoHandleData *>(handle);
        if (data != nullptr) {
            ::CloseHandle(data->m_port);
            delete data;
        }
    }

    // ------------------------------------------------------------------
    // file operations
    // ------------------------------------------------------------------

    FileHandle openFile(const IoHandle io, const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        const auto *io_data = static_cast<const IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            throw std::invalid_argument("IoPort: invalid IoHandle");
        }

        ::DWORD access = 0;
        ::DWORD disposition = OPEN_EXISTING;
        ::DWORD share = FILE_SHARE_READ;

        if (flags.m_bits & OpenFlags::read) {
            access |= GENERIC_READ;
        }
        if (flags.m_bits & OpenFlags::write) {
            access |= GENERIC_WRITE;
            share = FILE_SHARE_READ; // exclusive write
        }
        if ((flags.m_bits & (OpenFlags::create | OpenFlags::write)) == (OpenFlags::create | OpenFlags::write)) {
            disposition = OPEN_ALWAYS;
        }
        if (flags.m_bits & OpenFlags::truncate) {
            disposition = CREATE_ALWAYS;
        }

        const ::HANDLE file = ::CreateFileW(
            path.c_str(),
            access,
            share,
            nullptr,
            disposition,
            FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: CreateFileW failed");
        }

        // associate with the completion port
        // ULONG_PTR cast: required by IOCP API for completion key — safe because both are pointer-sized
        // (verified by static_assert above).
        if (::CreateIoCompletionPort(file, io_data->m_port,
                                     reinterpret_cast<::ULONG_PTR>(file), 0) == nullptr) [[unlikely]] {
            ::CloseHandle(file);
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: CreateIoCompletionPort (assoc) failed");
        }

        ::SetFileCompletionNotificationModes(file, FILE_SKIP_SET_EVENT_ON_HANDLE);

        auto *file_data = new FileHandleData();
        file_data->m_file = file;
        return static_cast<FileHandle>(file_data);
    }

    void closeFile(const IoHandle io, const FileHandle file) noexcept {
        (void)io;
        auto *data = static_cast<FileHandleData *>(file);
        if (data != nullptr) {
            if (data->m_file != INVALID_HANDLE_VALUE) {
                ::CancelIoEx(data->m_file, nullptr);
                ::CloseHandle(data->m_file);
            }
            delete data;
        }
    }

    // ------------------------------------------------------------------
    // submit & drain
    // ------------------------------------------------------------------

    std::size_t submit(const IoHandle io, const std::span<SubmitEntry> entries) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            return 0u;
        }

        std::size_t submitted = 0u;
        for (auto &entry : entries) {
            const auto *file_data = static_cast<const FileHandleData *>(entry.m_file);
            if (file_data == nullptr || file_data->m_file == INVALID_HANDLE_VALUE) {
                continue;
            }

            PPR_ASSERT(entry.m_overlapped != nullptr);
            auto *overlapped = ::new(entry.m_overlapped) OverlappedExt{};
            overlapped->Offset = static_cast<::DWORD>(entry.m_file_offset & 0xFFFFFFFFu);
            overlapped->OffsetHigh = static_cast<::DWORD>(entry.m_file_offset >> 32u);
            overlapped->m_user_data = entry.m_user_data;

            PPR_ASSERT(entry.m_buffer_size <= static_cast<u64>(std::numeric_limits<::DWORD>::max()));

            ::BOOL result = FALSE;
            if (entry.m_opcode == Opcode::read) {
                result = ::ReadFile(
                    file_data->m_file,
                    entry.m_buffer,
                    static_cast<::DWORD>(entry.m_buffer_size),
                    nullptr,
                    overlapped);
            } else {
                result = ::WriteFile(
                    file_data->m_file,
                    entry.m_buffer,
                    static_cast<::DWORD>(entry.m_buffer_size),
                    nullptr,
                    overlapped);
            }

            if (not result) {
                const ::DWORD err = ::GetLastError();
                if (err != ERROR_IO_PENDING) [[unlikely]] {
                    // operation failed synchronously — enqueue a synthetic completion
                    // ULONG_PTR cast: required by IOCP API for completion key — safe per static_assert above.
                    ::PostQueuedCompletionStatus(
                        io_data->m_port,
                        0,
                        reinterpret_cast<::ULONG_PTR>(file_data->m_file),
                        overlapped);
                }
            }
            ++submitted;
        }

        return submitted;
    }

    static std::size_t drainCompletions_(const IoHandle io, const std::span<CompletionEntry> entries,
                                         const ::DWORD timeout_ms) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data == nullptr) [[unlikely]] {
            return 0u;
        }

        if (entries.empty()) {
            return 0u;
        }

        ::OVERLAPPED_ENTRY ov_entries[64];
        const ::ULONG max_count = static_cast<::ULONG>(
            std::min(entries.size(), static_cast<std::size_t>(64u)));
        ::ULONG count = 0u;

        const ::BOOL result = ::GetQueuedCompletionStatusEx(
            io_data->m_port,
            ov_entries,
            max_count,
            &count,
            timeout_ms,
            FALSE);

        if (not result) [[unlikely]] {
            return 0u;
        }

        for (::ULONG i = 0u; i < count; ++i) {
            auto *ext = static_cast<OverlappedExt *>(ov_entries[i].lpOverlapped);
            CompletionEntry &ce = entries[static_cast<std::size_t>(i)];
            ce.m_user_data = ext != nullptr ? ext->m_user_data : nullptr;
            ce.m_bytes_transferred = ov_entries[i].dwNumberOfBytesTransferred;

            if (ext != nullptr && ov_entries[i].lpCompletionKey != 0u) {
                // ULONG_PTR → HANDLE: reverse of the association cast above — safe per static_assert.
                const ::HANDLE file_handle = reinterpret_cast<::HANDLE>(ov_entries[i].lpCompletionKey);
                ::DWORD dummy;
                if (!::GetOverlappedResult(file_handle, ext, &dummy, FALSE)) {
                    ce.m_error = std::error_code(::GetLastError(), std::system_category());
                }
            } else {
                ce.m_error = {};
            }
        }

        return static_cast<std::size_t>(count);
    }

    std::size_t poll(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        return drainCompletions_(io, entries, 0u);
    }

    std::size_t wait(const IoHandle io, const std::span<CompletionEntry> entries) noexcept {
        return drainCompletions_(io, entries, INFINITE);
    }

    void wake(const IoHandle io) noexcept {
        auto *io_data = static_cast<IoHandleData *>(io);
        if (io_data != nullptr) {
            ::PostQueuedCompletionStatus(io_data->m_port, 0, 0, nullptr);
        }
    }

    void cancelIo(const FileHandle file, void *const overlapped) noexcept {
        const auto *data = static_cast<const FileHandleData *>(file);
        if (data != nullptr && data->m_file != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(data->m_file, static_cast<::OVERLAPPED *>(overlapped));
        }
    }

    // ------------------------------------------------------------------
    // memory-mapped files
    // ------------------------------------------------------------------

    MapHandle mapFile([[maybe_unused]] const IoHandle io, const std::filesystem::path &path, const OpenFlags flags) noexcept(false) {
        ::DWORD desired_access = GENERIC_READ;
        ::DWORD share = FILE_SHARE_READ;
        ::DWORD protection = PAGE_READONLY;
        ::DWORD map_access = FILE_MAP_READ;

        if (flags.m_bits & OpenFlags::write) {
            desired_access = GENERIC_READ | GENERIC_WRITE;
            protection = PAGE_READWRITE;
            map_access = FILE_MAP_READ | FILE_MAP_WRITE;
            share = FILE_SHARE_READ;
        }

        const ::HANDLE file = ::CreateFileW(
            path.c_str(),
            desired_access,
            share,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: mapFile CreateFileW failed");
        }

        PPR_DEFER { ::CloseHandle(file); };

        ::LARGE_INTEGER file_size{};
        if (not::GetFileSizeEx(file, &file_size)) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: mapFile GetFileSizeEx failed");
        }

        if (file_size.QuadPart == 0) [[unlikely]] {
            // empty file — still create a valid mapping
            auto *md = new MapHandleData();
            md->m_data = nullptr;
            md->m_size = 0;
            return static_cast<MapHandle>(md);
        }

        const ::HANDLE mapping = ::CreateFileMappingW(
            file,
            nullptr,
            protection,
            static_cast<::DWORD>(file_size.QuadPart >> 32u),
            static_cast<::DWORD>(file_size.QuadPart & 0xFFFFFFFFu),
            nullptr);

        if (mapping == nullptr) [[unlikely]] {
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: mapFile CreateFileMappingW failed");
        }

        void *const data = ::MapViewOfFile(
            mapping,
            map_access,
            0, 0,
            static_cast<std::size_t>(file_size.QuadPart));

        if (data == nullptr) [[unlikely]] {
            ::CloseHandle(mapping);
            throw std::system_error(
                std::error_code(::GetLastError(), std::system_category()),
                "IoPort: mapFile MapViewOfFile failed");
        }

        auto *md = new MapHandleData();
        md->m_mapping = mapping;
        md->m_data = data;
        md->m_size = static_cast<std::size_t>(file_size.QuadPart);
        return static_cast<MapHandle>(md);
    }

    void unmapFile([[maybe_unused]] const IoHandle io, const MapHandle map) noexcept {
        auto *data = static_cast<MapHandleData *>(map);
        if (data != nullptr) {
            if (data->m_data != nullptr) {
                ::UnmapViewOfFile(data->m_data);
            }
            if (data->m_mapping != nullptr) {
                ::CloseHandle(data->m_mapping);
            }
            delete data;
        }
    }

    void *mapData(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_data : nullptr;
    }

    std::size_t mapSize(const MapHandle map) noexcept {
        const auto *data = static_cast<const MapHandleData *>(map);
        return data != nullptr ? data->m_size : 0u;
    }
}

namespace pP {
    std::mt19937_64 randomNumberGenerator() noexcept {
        std::array<std::uint32_t, 8> seed_data{};
        std::random_device rd;

        for (auto &x: seed_data) {
            x = rd();
        }

        std::seed_seq seq(seed_data.begin(), seed_data.end());
        return std::mt19937_64(seq);
    }
}
