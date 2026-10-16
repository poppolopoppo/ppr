module;
#include <typeinfo>

#include "pP/Macros.h"

module engine.core;
import :logger;
import :memory;
import :memory.pointer;
import std;

namespace pP::mem {
    PPR_DEFINE_LOG_CATEGORY(Memory, debug, none)

    [[noreturn]] void throwBadAlloc() {
        PPR_THROW(Memory, std::bad_alloc{});
    }
}

namespace pP {
    // ------------------------------------------------------------------
    // general purpose allocator use stl default allocator
    // ------------------------------------------------------------------

    std::allocation_result<void *> mem::GPA::allocateRaw(
        const std::size_t bytes, const std::align_val_t alignment) noexcept {
        void *const ptr =
            (alignment > max_align_v ? operator new(bytes, alignment, std::nothrow)
                                     : operator new(bytes, std::nothrow));
        PPR_ASSERT(!ptr || alignForward(ptr, alignment) == ptr);
        return {ptr, ptr ? bytes : 0u};
    }

    void mem::GPA::deallocateRaw(void *const ptr, const std::size_t bytes, const std::align_val_t alignment) noexcept {
        if (alignment > max_align_v) {
            operator delete(ptr, bytes, alignment);
        } else {
            operator delete(ptr, bytes);
        }
    }

    // ------------------------------------------------------------------
    // os virtual memory allocator
    // ------------------------------------------------------------------

    std::allocation_result<void *> mem::OS::allocateRaw(const std::size_t bytes, const std::align_val_t alignment) {
        const auto [ptr, reserved] = hal::pageAlloc(bytes);
        PPR_ASSERT(std::bit_cast<std::uintptr_t>(ptr) % static_cast<std::size_t>(alignment) == 0);
        return {ptr, reserved};
    }

    void mem::OS::deallocateRaw(
        void *const ptr, const std::size_t bytes, [[maybe_unused]] const std::align_val_t alignment) {
        hal::pageFree(ptr, bytes);
    }

    // ------------------------------------------------------------------
    // page pool allocators
    // ------------------------------------------------------------------

    // Global-pool ownership policy: both global pools below are explicit
    // process-lifetime owners — intentionally leaked (a static pointer with
    // trivial destruction, so the pool itself is never destroyed) and left for
    // OS process reclamation. TLS LocalCaches may flush at any teardown order,
    // including after main(), so destroying a global pool first would leave a
    // late flush hitting a dead pool. Owned (non-global) pools are unaffected
    // and keep normal destruction.

    auto mem::HugePage::getGlobalPool() noexcept -> PagePool & {
        static PagePool *const g_instance = [] {
            void *const storage = ::operator new(sizeof(PagePool), std::align_val_t{hal::cacheline_size_v});
            return std::construct_at(static_cast<PagePool *>(storage), block_size_v, num_reserved_blocks_v);
        }();
        return *g_instance;
    }

    auto mem::HugePage::getThreadLocalCache() noexcept -> local_block_cache_t & {
        alignas(hal::cacheline_size_v) thread_local local_block_cache_t g_instance_tls{};
        return g_instance_tls;
    }

    auto mem::SmallPage::getGlobalPool() noexcept -> pooling_allocator_t & {
        [[maybe_unused]] static const auto &_{HugePage::getGlobalPool()};
        static pooling_allocator_t *const g_instance = [] {
            void *const storage = ::operator new(sizeof(pooling_allocator_t), std::align_val_t{hal::cacheline_size_v});
            return std::construct_at(static_cast<pooling_allocator_t *>(storage));
        }();
        return *g_instance;
    }

    auto mem::SmallPage::getThreadLocalCache() noexcept -> local_block_cache_t & {
        // Init HugePage TLS per thread so SmallPage TLS teardown never outlives its backing cache.
        (void) HugePage::getThreadLocalCache();
        alignas(hal::cacheline_size_v) thread_local local_block_cache_t g_instance_tls{};
        return g_instance_tls;
    }

    // ------------------------------------------------------------------
    // safe object with reference tracking
    // ------------------------------------------------------------------

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    PPR_DEFINE_LOG_CATEGORY(SafeObject, info, none)
}
namespace std {
    static void formatStackEntryTo(
        const std::stacktrace_entry &entry, pP::opaque::format_context &ctx) noexcept { // NOLINT(*-dcl58-cpp)
        ctx.write(std::to_string(entry));
    }

    pP::opaque::Value opaqueValue(const std::stacktrace_entry &entry) noexcept { // NOLINT(*-dcl58-cpp)
        return pP::opaque::Formatter{std23::nontype<&formatStackEntryTo>, entry};
    }
}
namespace pP {

#endif

#if PPR_ENABLE_DEBUG
    safe_object::~safe_object() noexcept(false) {

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
        {
            const std::unique_lock scope_lock{m_referencer_barrier};

            for (const auto [index, referencer]: std::ranges::enumerate_view(m_references)) {
                const std::stacktrace &callstack = referencer.m_callstack;

                PPR_LOG(
                    SafeObject, warning, "safe_object destroyed with live reference",
                    {
                        {"typename", typeid(*static_cast<const safe_object *>(referencer.m_derived)).name()},
                        {"address", std::bit_cast<std::uintptr_t>(referencer.m_derived)},
                        {"index", index},
                        {"stacktrace", opaqueValue(callstack)},
                    });
            }

            if (not m_references.isEmpty()) {
                PPR_FLUSH_LOG(true);
            }
        }
#endif

        PPR_ASSERT(
            m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
            "safe_object destroyed while safe_ptr references still exist!");
    }

    // Relocation/copying changes object identity: the new instance starts
    // unobserved, and the source must not be observed at the time of the op.
    safe_object::safe_object(safe_object &&other) {
        PPR_ASSERT(
            other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
            "Source of move construction is still observed by a safe_ptr!");
    }

    safe_object &safe_object::operator=(safe_object &&other) {
        if (this != &other) {
            PPR_ASSERT(
                m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Target of move assignment is still observed by a safe_ptr!");
            PPR_ASSERT(
                other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Source of move assignment is still observed by a safe_ptr!");
        }
        return *this;
    }

    safe_object::safe_object(const safe_object &other) {
        PPR_ASSERT(
            other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
            "Source of copy construction is still observed by a safe_ptr!");
    }

    safe_object &safe_object::operator=(const safe_object &other) {
        if (this != &other) {
            PPR_ASSERT(
                m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Target of copy assignment is still observed by a safe_ptr!");
            PPR_ASSERT(
                other.m_safe_ref_count.load(std::memory_order_relaxed) == 0 &&
                "Source of copy assignment is still observed by a safe_ptr!");
        }
        return *this;
    }

#if PPR_ENABLE_SAFE_OBJECT_TRACKING
    SparseKeyId safe_object::incSafeRef([[maybe_unused]] const void *derived) const noexcept {
        m_safe_ref_count.fetch_add(1, std::memory_order_acquire);
        const std::unique_lock scope_lock{m_referencer_barrier};
        return m_references.add({.m_derived = derived, .m_callstack = std::stacktrace::current(2u)});
    }

    void safe_object::decSafeRef(
        [[maybe_unused]] const void *derived, const SparseKeyId referencer_key) const noexcept {
        [[maybe_unused]] const int prev = m_safe_ref_count.fetch_sub(1, std::memory_order_release);
        PPR_ASSERT(prev > 0 && "safe object ref count underflow!");
        const std::unique_lock scope_lock{m_referencer_barrier};
        PPR_VERIFY(m_references.erase(referencer_key));
    }
#else
    void safe_object::incSafeRef([[maybe_unused]] const void *derived) const noexcept {
        m_safe_ref_count.fetch_add(1, std::memory_order_acquire);
    }

    void safe_object::decSafeRef([[maybe_unused]] const void *derived) const noexcept {
        [[maybe_unused]] const int prev = m_safe_ref_count.fetch_sub(1, std::memory_order_release);
        PPR_ASSERT(prev > 0 && "safe object ref count underflow!");
    }
#endif
#endif // PPR_ENABLE_DEBUG
}
