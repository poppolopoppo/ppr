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
#include <knownfolders.h>
#include <shlobj.h>

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
        };

        explicit Win32Exception(const Win32LastError last_error) noexcept
            : std::runtime_error(last_error.message()),
              m_last_error(last_error) {
        };

        [[nodiscard]] Win32LastError getLastError() const noexcept { return m_last_error; }
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

    [[nodiscard]] std::allocation_result<void *> pageAlloc(const std::size_t size, const bool commit, const PageProtection allowed) {
        const std::size_t aligned_size = alignForward(size, static_cast<std::size_t>(page_granularity));
        const ::DWORD allocation_type = MEM_RESERVE | (commit ? MEM_COMMIT : 0);
        void *const mapped_ptr = ::VirtualAlloc(
            nullptr, size,
            allocation_type,
            pageProtectionFlags_(allowed));
        if (!mapped_ptr) [[unlikely]] {
            throw Win32Exception();
        }

        return std::allocation_result(mapped_ptr, aligned_size);
    }

    void pageCommit(void *const ptr, const std::size_t size, const PageProtection allowed) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::VirtualAlloc(
                ptr, size,
                MEM_COMMIT,
                pageProtectionFlags_(allowed)) == nullptr) [[unlikely]] {
            throw Win32Exception();
        }
    }

    void pageDecommit(void *const ptr, const std::size_t size) {
        PPR_ASSERT(ptr != nullptr);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % page_size == 0u);
        PPR_ASSERT(size % page_size == 0u);

        if (::VirtualFree(ptr, size, MEM_DECOMMIT) == FALSE) {
            throw Win32Exception();
        }
    }

    void pageProtect(void *const ptr, const std::size_t size, const PageProtection allowed) {
        ::DWORD old_protect;
        if (::VirtualProtect(ptr, size, pageProtectionFlags_(allowed), &old_protect) == FALSE) {
            throw Win32Exception();
        }
    }

    void pageOfferToOS(void *const ptr, const std::size_t size) {
        if (::OfferVirtualMemory(ptr, size, VmOfferPriorityNormal) != ERROR_SUCCESS) {
            throw Win32Exception();
        }
    }

    [[nodiscard]] bool pageReclaimFromOS(void *const ptr, const std::size_t size) {
        switch (::ReclaimVirtualMemory(ptr, size)) {
            case ERROR_SUCCESS:
            case ERROR_BUSY:
                return true;;
            default:
                return false;
        }
    }

    void pageFree(void *const ptr, [[maybe_unused]] const std::size_t size) {
#if PPR_ENABLE_ASSERTIONS
        //  https://msdn.microsoft.com/en-us/library/windows/desktop/aa366902(v=vs.85).aspx
        ::MEMORY_BASIC_INFORMATION info;
        if (PPR_ENSURE(::VirtualQuery(ptr, &info, sizeof(info)))) {
            PPR_ASSERT(info.BaseAddress == ptr && "Trying to free memory with an invalid pointer");
            PPR_ASSERT((info.State & (MEM_COMMIT|MEM_RESERVE)) && "Trying to free unreserved memory");
            PPR_ASSERT(info.RegionSize == size && "Trying to free with unmatching region size");
        }
#endif

        if (!::VirtualFree(ptr, 0u, MEM_RELEASE)) {
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
        return ansi.size();
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
#ifndef NDEBUG
        ::OutputDebugStringA(ansi_msg);
#else
        (void) ansi_msg;
#endif
    }

    void outputDebug(const native::char_t *wide_msg) noexcept {
#ifndef NDEBUG
        ::OutputDebugStringW(wide_msg);
#else
        (void) wide_msg;
#endif
    }

    [[nodiscard]] bool isDebuggerPresent() noexcept {
#ifndef NDEBUG
        return ::IsDebuggerPresent();
#else
        return false;
#endif
    }

    void breakpoint() noexcept {
#ifndef NDEBUG
        __debugbreak();
#endif
    }

    void breakpointIfDebugging() noexcept {
#ifndef NDEBUG
        if (::IsDebuggerPresent())

            __debugbreak();
#endif
    }
}
