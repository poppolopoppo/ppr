module;
#include "pP/Macros.h"

export module engine.tests.core:hal;

import engine.core;
import std;

export namespace pP::tests {
    namespace HALTests {
        PPR_UNIT_TEST(thread_id) {
            const auto tid = hal::currentThreadId();
            PPR_ASSERT(tid == hal::currentThreadId());
            if (hal::platformName() != "generic") {
                PPR_ASSERT(tid.m_value != 0u);
            }
        };

        PPR_UNIT_TEST(set_get_name_roundtrip) {
            const auto tid = hal::currentThreadId();
            const std::string previous = hal::getThreadName(tid);
            PPR_DEFER { hal::setThreadName(previous); };

            constexpr std::string_view expected = "PPR_HAL_Test"; // 12 chars, fits Linux comm limit
            hal::setThreadName(expected);

            if (hal::platformName() == "generic") {
                PPR_ASSERT(hal::getThreadName(tid).empty());
                return;
            }

            PPR_ASSERT(hal::getThreadName(tid) == expected);

            char buffer[64]{};
            const std::size_t written = hal::getThreadName(tid, buffer, sizeof(buffer));
            PPR_ASSERT(written == expected.size());
            PPR_ASSERT(std::string_view(buffer, written) == expected);

            const std::size_t required = hal::getThreadName(tid, nullptr, 0u);
            PPR_ASSERT(required == expected.size());

            // NB: std::format("{}", tid) triggers MSVC C3546 in consteval
            // format-string checking for module-partition types (u64 / ThreadId).
            // The formatter is validated implicitly by the logger TU which uses
            // std::println with hal::ThreadId at runtime.
        };

        PPR_UNIT_TEST(buffer_truncation) {
            const auto tid = hal::currentThreadId();
            const std::string previous = hal::getThreadName(tid);
            PPR_DEFER { hal::setThreadName(previous); };

            hal::setThreadName("PPR_HAL_Test");
            if (hal::platformName() == "generic") {
                return;
            }

            char small[4]{};
            const std::size_t written = hal::getThreadName(tid, small, sizeof(small));
            PPR_ASSERT(written > sizeof(small));
        };

        PPR_UNIT_TEST(worker_thread_name) {
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
                PPR_ASSERT(hal::getThreadName(tid.load()) == "PPR_Worker");
            }

            release.store(true);
            worker.join();
        };
    }

    PPR_UNIT_TEST(hal) {
        _.recurse({
            HALTests::thread_id,
            HALTests::set_get_name_roundtrip,
            HALTests::buffer_truncation,
            HALTests::worker_thread_name,
        });
    };
}
