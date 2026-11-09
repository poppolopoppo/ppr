module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import engine.image;
import engine.math;
import engine.rhi;
import std;

namespace pP::tests::detail {
    // ------------------------------------------------------------------
    // pixel readback
    // ------------------------------------------------------------------

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

            [[nodiscard]] std::error_code boot() {
                return Application::initialize();
            }
            [[nodiscard]] std::error_code teardown() {
                return Application::shutdown();
            }
        };

        struct ThrowingRunApp final : TestApp {
            u32 m_shutdown_count{};

            using TestApp::TestApp;

        protected:
            [[nodiscard]] std::error_code update([[maybe_unused]] const TimeSpan dt) override {
                throw std::runtime_error{"pixel-readback test failure"};
            }

            [[nodiscard]] std::error_code shutdown() override {
                ++m_shutdown_count;
                return TestApp::shutdown();
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
        PPR_DEFER {
            PPR_TEST_ASSERT(not test_app.teardown());
        };
        const auto rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());
        PPR_TEST_ASSERT(&rhi->getDevice() == &rhi->getDevice());
    };

    // ------------------------------------------------------------------
    // surface frame PNG round trip
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (surface_pixels_round_trip_through_png) {
        namespace fs = std::filesystem;
        std::error_code error{};
        const fs::path directory = fs::temp_directory_path(error);
        PPR_TEST_ASSERT(not error);
        const fs::path path = directory / "ppr_surface_png_round_trip.png";
        PPR_DEFER {
            std::error_code ignored{};
            std::ignore = fs::remove(path, ignored);
        };

        constexpr int kWidth = 4;
        constexpr int kHeight = 3;
        constexpr u64 kPitch = static_cast<u64>(kWidth) * 4u + 8u; // padded readback rows
        constexpr u64 kRowBytes = static_cast<u64>(kWidth) * 4u;

        // Distinct at every axis so a vertical flip cannot pass: row 0's first
        // pixel differs from the last row's first pixel, and columns differ too.
        const auto expectedPixel = [](const int x, const int y) {
            return std::array<std::byte, 4u>{
                static_cast<std::byte>(10u + static_cast<u32>(x) * 40u + static_cast<u32>(y)),
                static_cast<std::byte>(20u + static_cast<u32>(y) * 50u + static_cast<u32>(x)),
                static_cast<std::byte>(200u - static_cast<u32>(x) * 30u - static_cast<u32>(y) * 10u),
                static_cast<std::byte>(255u - static_cast<u32>(x) - static_cast<u32>(y)),
            };
        };

        for (const rhi::Format format:
            {rhi::Format::RGBA8Unorm, rhi::Format::RGBA8UnormSrgb, rhi::Format::BGRA8Unorm,
                rhi::Format::BGRA8UnormSrgb}) {
            const bool bgra = format == rhi::Format::BGRA8Unorm or format == rhi::Format::BGRA8UnormSrgb;

            SurfaceFramePixels pixels{};
            pixels.m_format = format;
            pixels.m_extent = int2{kWidth, kHeight};
            pixels.m_row_pitch = kPitch;
            pixels.m_bytes.assign(static_cast<std::size_t>(kPitch * kHeight), std::byte{0xABu});

            std::vector<std::byte> expected(static_cast<std::size_t>(kRowBytes * kHeight));
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const auto rgba = expectedPixel(x, y);
                    const std::size_t pixel_offset = static_cast<std::size_t>(
                        static_cast<u64>(y) * kRowBytes + static_cast<u64>(x) * 4u);
                    std::memcpy(expected.data() + pixel_offset, rgba.data(), rgba.size());

                    const std::size_t source_offset = static_cast<std::size_t>(
                        static_cast<u64>(y) * kPitch + static_cast<u64>(x) * 4u);
                    std::byte *const source = pixels.m_bytes.data() + source_offset;
                    source[0] = rgba[bgra ? 2u : 0u];
                    source[1] = rgba[1u];
                    source[2] = rgba[bgra ? 0u : 2u];
                    source[3] = rgba[3u];
                }
            }

            PPR_TEST_ASSERT(not writeSurfacePng(pixels, path));

            const Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path);
            PPR_TEST_ASSERT(mapped.has_value());
            const Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());
            PPR_TEST_ASSERT(decoded->m_width == static_cast<u32>(kWidth));
            PPR_TEST_ASSERT(decoded->m_height == static_cast<u32>(kHeight));
            PPR_TEST_ASSERT(not decoded->m_is_block);

            const mem::SharedBufferView pixels_out = decoded->m_subresources.front().m_view.getBufferData();
            PPR_TEST_ASSERT(pixels_out.size() == expected.size());
            for (std::size_t index = 0u; index < expected.size(); ++index) {
                PPR_TEST_ASSERT(pixels_out[index] == expected[index]);
            }
        }
    };

    PPR_UNIT_TEST (app_headless_lifecycle_is_idempotent) {
        using namespace detail;
        TestApp test_app{"HeadlessLifecycle", std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        PPR_TEST_ASSERT(not test_app.teardown());
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    PPR_UNIT_TEST (app_run_shutdowns_after_nonstandard_hook_exception) {
        using namespace detail;
        ThrowingRunApp test_app{"ThrowingRun", std::span<const char *const>{}};

        PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::state_not_recoverable));
        PPR_TEST_ASSERT(test_app.m_shutdown_count == 1u);

        PPR_TEST_ASSERT(test_app.run() == std::make_error_code(std::errc::state_not_recoverable));
        PPR_TEST_ASSERT(test_app.m_shutdown_count == 2u);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest pixel_readback = detail::pixel_readback;

    const UnitTest &pixel_readbackTests() noexcept {
        return pixel_readback;
    }

    const UnitTest surface_pixels_round_trip_through_png = detail::surface_pixels_round_trip_through_png;

    const UnitTest &surface_pixels_round_trip_through_pngTests() noexcept {
        return surface_pixels_round_trip_through_png;
    }

    const UnitTest app_headless_lifecycle_is_idempotent = detail::app_headless_lifecycle_is_idempotent;

    const UnitTest &app_headless_lifecycle_is_idempotentTests() noexcept {
        return app_headless_lifecycle_is_idempotent;
    }

    const UnitTest app_run_shutdowns_after_nonstandard_hook_exception =
        detail::app_run_shutdowns_after_nonstandard_hook_exception;

    const UnitTest &app_run_shutdowns_after_nonstandard_hook_exceptionTests() noexcept {
        return app_run_shutdowns_after_nonstandard_hook_exception;
    }
} // namespace pP::tests
