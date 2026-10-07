module;
#include "pP/UnitTest.h"
#if PPR_ENABLE_SANITIZER_ADDRESS
#include <sanitizer/asan_interface.h>
#endif

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    namespace Hal {
        PPR_UNIT_TEST (thread_id) {
            const auto tid = hal::currentThreadId();
            PPR_TEST_ASSERT(tid == hal::currentThreadId());
            if (hal::platformName() != "generic") {
                PPR_TEST_ASSERT(tid.m_value != 0u);
            }
        };

        PPR_UNIT_TEST (set_get_name_roundtrip) {
            const auto tid = hal::currentThreadId();
            const std::string previous = hal::getThreadName(tid);
            PPR_DEFER {
                hal::setThreadName(previous);
            };

            constexpr std::string_view expected = "PPR_HAL_Test"; // 12 chars, fits Linux comm limit
            hal::setThreadName(expected);

            if (hal::platformName() == "generic") {
                PPR_TEST_ASSERT(hal::getThreadName(tid).empty());
                return;
            }

            PPR_TEST_ASSERT(hal::getThreadName(tid) == expected);

            char buffer[64]{};
            const std::size_t written = hal::getThreadName(tid, buffer, sizeof(buffer));
            PPR_TEST_ASSERT(written == expected.size());
            PPR_TEST_ASSERT(std::string_view(buffer, written) == expected);

            const std::size_t required = hal::getThreadName(tid, nullptr, 0u);
            PPR_TEST_ASSERT(required == expected.size());

            // NB: std::format("{}", tid) triggers MSVC C3546 in consteval
            // format-string checking for module-partition types (u64 / ThreadId).
            // The formatter is validated implicitly by the logger TU which uses
            // std::println with hal::ThreadId at runtime.
        };

        PPR_UNIT_TEST (buffer_truncation) {
            const auto tid = hal::currentThreadId();
            const std::string previous = hal::getThreadName(tid);
            PPR_DEFER {
                hal::setThreadName(previous);
            };

            hal::setThreadName("PPR_HAL_Test");
            if (hal::platformName() == "generic") {
                return;
            }

            char small[4]{};
            const std::size_t written = hal::getThreadName(tid, small, sizeof(small));
            PPR_TEST_ASSERT(written > sizeof(small));
        };

        PPR_UNIT_TEST (worker_thread_name) {
            std::atomic<hal::ThreadId> tid{};
            std::atomic<bool> named{false};
            std::atomic<bool> release{false};

            std::thread worker{[&] {
                tid.store(hal::currentThreadId());
                hal::setThreadName("PPR_Worker");
                named.store(true);
                while (not release.load()) {
                    std::this_thread::yield();
                }
            }};

            while (not named.load()) {
                std::this_thread::yield();
            }

            if (hal::platformName() != "generic") {
                PPR_TEST_ASSERT(hal::getThreadName(tid.load()) == "PPR_Worker");
            }

            release.store(true);
            worker.join();
        };

        PPR_UNIT_TEST (profile_begin_end_no_fault) {
            hal::profileBegin("PPR_HAL_Test_Scope");
            hal::profileEnd();
            {
                const hal::ProfileScope scope("PPR_HAL_Test_RAII");
            }
        };

        // P2 Gate 2 ASan provenance regression (fix-20): PPR poisonDestroyed
        // shadow used to survive UnmapViewOfFile / VirtualFree(MEM_RELEASE), so
        // a later occupant remapped at the same address faulted on stale
        // shadow. Both release paths now clear ASan shadow while still owning
        // the range. Shadow is inspected via __asan_address_is_poisoned, which
        // reads shadow only and never dereferences the released memory.
        PPR_UNIT_TEST (ring_buffer_free_clears_asan_shadow) {
#if PPR_ENABLE_SANITIZER_ADDRESS
            const std::size_t buffer_size = static_cast<std::size_t>(hal::page_granularity);
            void *const ring = hal::ringBufferAlloc(buffer_size);
            const auto base = std::bit_cast<std::uintptr_t>(ring);
            const auto second = base + buffer_size;

            // Poison both virtual views (independent shadow per view over the
            // same physical pages), mirroring RawChannel teardown.
            mem::poisonDestroyed(ring, buffer_size);
            mem::poisonDestroyed(static_cast<std::byte *>(ring) + buffer_size, buffer_size);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(ring) != 0);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(second)) != 0);

            hal::ringBufferFree(ring, buffer_size);

            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(base)) == 0);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(second)) == 0);
#endif
        };

        PPR_UNIT_TEST (page_free_clears_asan_shadow) {
#if PPR_ENABLE_SANITIZER_ADDRESS
            const std::size_t size = 2u * static_cast<std::size_t>(hal::page_granularity);
            const auto [ptr, actual] = hal::pageAlloc(size);
            const auto base = std::bit_cast<std::uintptr_t>(ptr);
            const auto half = base + actual / 2u;

            mem::poisonDestroyed(ptr, actual);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(ptr) != 0);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(half)) != 0);

            // Decommit the tail: the release path must clear shadow even for
            // pages that can no longer be touched.
            hal::pageDecommit(std::bit_cast<void *>(half), actual / 2u);

            hal::pageFree(ptr, actual);

            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(base)) == 0);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(half)) == 0);
            PPR_TEST_ASSERT(__asan_address_is_poisoned(std::bit_cast<const volatile void *>(base + actual - 1u)) == 0);
#endif
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest hal = UnitTest::Named("hal") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Hal::thread_id,
            detail::Hal::set_get_name_roundtrip,
            detail::Hal::buffer_truncation,
            detail::Hal::worker_thread_name,
            detail::Hal::profile_begin_end_no_fault,
        });
        if constexpr (mem::is_asan_enabled_v) {
            _.recurse({
                detail::Hal::ring_buffer_free_clears_asan_shadow,
                detail::Hal::page_free_clears_asan_shadow,
            });
        }
    };

    const UnitTest &halTests() noexcept {
        return hal;
    }
} // namespace pP::tests
