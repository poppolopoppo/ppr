module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
    namespace detail {
        constexpr ApplicationDomain kRenderingDomain{
            .m_is_headless = false,
            .m_is_interactive = true,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = true,
        };

        struct TestApp : Application {
            explicit TestApp(const std::string_view name, const std::span<const char *const> argv)
                : Application(kRenderingDomain, name, argv) {
            }

            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        struct ThrowingRunApp final : TestApp {
            using TestApp::TestApp;

        protected:
            [[nodiscard]] std::error_code update([[maybe_unused]] const TimeSpan dt) override {
                throw 42;
            }
        };
    }

    PPR_UNIT_TEST (pixel_readback) {
        using namespace detail;
        TestApp test_app{"PixelReadback", std::span<const char *const>{}};
        const ApplicationDomain &domain = test_app.getDomain();
        PPR_TEST_ASSERT(not domain.m_is_headless);
        PPR_TEST_ASSERT(domain.m_is_interactive);
        PPR_TEST_ASSERT(domain.m_needs_rendering);
        PPR_TEST_ASSERT(domain.m_needs_user_interface);
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
        const auto rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());
        PPR_TEST_ASSERT(&rhi->getDevice() == &rhi->getDevice());
    };

    PPR_UNIT_TEST (app_headless_lifecycle_is_idempotent) {
        using namespace detail;
        TestApp test_app{"HeadlessLifecycle", std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST(app_run_shutdowns_after_nonstandard_hook_exception, UnitTest::expect_fail) {
        using namespace detail;
        ThrowingRunApp test_app{"ThrowingRun", std::span<const char *const>{}};
        PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::state_not_recoverable));
        PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::operation_not_permitted));
    };
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest pixel_readback = detail::pixel_readback;

    const UnitTest &pixel_readbackTests() noexcept {
        return pixel_readback;
    }

    const UnitTest app_headless_lifecycle_is_idempotent = detail::app_headless_lifecycle_is_idempotent;

    const UnitTest &app_headless_lifecycle_is_idempotentTests() noexcept {
        return app_headless_lifecycle_is_idempotent;
    }

    const UnitTest app_run_shutdowns_after_nonstandard_hook_exception = detail::app_run_shutdowns_after_nonstandard_hook_exception;

    const UnitTest &app_run_shutdowns_after_nonstandard_hook_exceptionTests() noexcept {
        return app_run_shutdowns_after_nonstandard_hook_exception;
    }
} // namespace pP::tests
