module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // logger
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(WriterProbe, warning, none)

    namespace Logger {
        std::atomic<bool> g_writer_probe_hit{false};

        void writerProbe_(const Log::Entry &) noexcept {
            g_writer_probe_hit.store(true, std::memory_order_relaxed);
        }

        std::atomic<bool> g_saturate_release{true};
        std::atomic<bool> g_saturate_entered{false};
        std::atomic<int> g_saturate_processed{0};

        void saturatingWriter_(const Log::Entry &) noexcept {
            g_saturate_entered.store(true, std::memory_order_relaxed);
            while (not g_saturate_release.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            g_saturate_processed.fetch_add(1, std::memory_order_relaxed);
        }

        // Helper leaf: runs nested inside writer_policy_restored_after_nested_run so the
        // runner installs (and, with the fix, restores) a child writer policy around it.
        PPR_UNIT_TEST (nested_logging_probe) {
            PPR_LOG(WriterProbe, warning, "nested probe");
        };

        // Regression test: RunImpl::stop must restore the previously installed log writer
        // policy in every configuration. Before the fix the restore only happened with
        // PPR_ENABLE_ASSERTIONS, so in release builds the async log worker kept dispatching
        // queued entries into the destroyed nested run (wild jumps / heap corruption in
        // log-heavy suites).
        PPR_UNIT_TEST (writer_policy_restored_after_nested_run) {
            g_writer_probe_hit.store(false, std::memory_order_relaxed);

            const Log::Policy previous = Log::setWriterPolicy({std23::nontype<&writerProbe_>});
            PPR_DEFER {
                std::ignore = Log::setWriterPolicy(previous);
            };

            _.recurse({nested_logging_probe});

            PPR_LOG(WriterProbe, warning, "outer probe");
            PPR_FLUSH_LOG(true);
            PPR_TEST_ASSERT(g_writer_probe_hit.load(std::memory_order_relaxed));
        };

        // Controlled full-ring test: with the background writer stalled the
        // ~2 MiB ring cannot hold the flood, so drop_if_full entries below
        // error must drop (no UB, no hang, no assert) instead of stalling.
        PPR_UNIT_TEST (saturation_drops_without_blocking) {
            g_saturate_release.store(false, std::memory_order_relaxed);
            g_saturate_entered.store(false, std::memory_order_relaxed);
            g_saturate_processed.store(0, std::memory_order_relaxed);

            const Log::Policy previous = Log::setWriterPolicy({std23::nontype<&saturatingWriter_>});
            PPR_DEFER {
                // Release the stalled worker first: restoring the policy
                // takes the writer barrier the worker is blocked on.
                g_saturate_release.store(true, std::memory_order_relaxed);
                std::ignore = Log::setWriterPolicy(previous);
            };

            PPR_LOG(WriterProbe, warning, "saturation kick");

            for (int spin = 0; spin < 100'000'000; ++spin) {
                if (g_saturate_entered.load(std::memory_order_relaxed)) {
                    break;
                }
                std::this_thread::yield();
            }
            PPR_TEST_ASSERT(g_saturate_entered.load(std::memory_order_relaxed));

            constexpr int flood_count = 200'000;
            for (int i = 0; i < flood_count; ++i) {
                PPR_LOG(WriterProbe, warning, "saturation flood");
            }

            g_saturate_release.store(true, std::memory_order_relaxed);
            PPR_FLUSH_LOG(true);

            // Reaching here proves no stall/assert; fewer consumed than sent
            // proves the overflow dropped instead of blocking the producer.
            PPR_TEST_ASSERT(g_saturate_processed.load(std::memory_order_relaxed) < flood_count);
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest logger = UnitTest::Named("logger") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Logger::writer_policy_restored_after_nested_run,
            detail::Logger::saturation_drops_without_blocking,
        });
    };

    const UnitTest &loggerTests() noexcept {
        return logger;
    }
} // namespace pP::tests
