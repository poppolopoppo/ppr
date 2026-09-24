module;
#include "pP/UnitTest.h"

module engine.tests.core;

import engine.core;
import std;

namespace pP::tests::detail {
    PPR_DEFINE_LOG_CATEGORY(WriterProbe, warning, none)

    namespace Logger {
        std::atomic<bool> g_writer_probe_hit{false};

        void writerProbe_(const Log::Entry &) noexcept {
            g_writer_probe_hit.store(true, std::memory_order_relaxed);
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
            PPR_DEFER{ std::ignore = Log::setWriterPolicy(previous); };

            _.recurse({nested_logging_probe});

            PPR_LOG(WriterProbe, warning, "outer probe");
            PPR_FLUSH_LOG(true);
            PPR_TEST_ASSERT(g_writer_probe_hit.load(std::memory_order_relaxed));
        };
    }
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest logger = UnitTest::Named("logger") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Logger::writer_policy_restored_after_nested_run,
        });
    };

    const UnitTest &loggerTests() noexcept {
        return logger;
    }
} // namespace pP::tests
