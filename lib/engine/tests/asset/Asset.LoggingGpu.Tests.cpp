module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.core;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import std;

// GPU-owned asset-pipeline logging proofs: cache shutdown/release/notify
// telemetry, texture-upload dedup, empty-scene publish skip, and the shader
// module success line. The RHI budget/target line fires at application boot
// (before any leaf can capture), so it stays code-covered. One focused test
// per TU.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();

    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace LoggingGpu {
        struct Captured {
            Log::ELevel m_level = Log::ELevel::debug;
            std::string m_message{};
        };

        struct LogCapture {
            static inline std::vector<Captured> s_entries{};

            static void push_(const Log::Entry &entry) noexcept {
                try {
                    s_entries.push_back(Captured{
                        entry.m_site.m_verbosity,
                        std::string(entry.m_message),
                    });
                } catch (...) {
                }
            }
        };

        class CaptureGuard final {
            Log::Policy m_previous_policy;
            Log::ELevel m_previous_level;

        public:
            CaptureGuard() noexcept
                : m_previous_policy(Log::setWriterPolicy(LogCapture::push_)),
                  m_previous_level(Log::setMinimumVerboseLevel(Log::ELevel::debug)) {
                LogCapture::s_entries.clear();
                Log::Once::resetForTests();
            }

            ~CaptureGuard() noexcept {
                std::ignore = Log::flush(true);
                Log::setWriterPolicy(m_previous_policy);
                Log::setMinimumVerboseLevel(m_previous_level);
            }

            CaptureGuard(const CaptureGuard &) = delete;

            CaptureGuard &operator=(const CaptureGuard &) = delete;
        };

        [[nodiscard]] u64 countAt_(const Log::ELevel level, const std::string_view needle) noexcept {
            u64 found = 0u;
            try {
                for (const Captured &entry: LogCapture::s_entries) {
                    if (entry.m_level == level and
                        entry.m_message.find(needle) != std::string::npos) {
                        ++found;
                    }
                }
            } catch (...) {
            }
            return found;
        }

        [[nodiscard]] Expected<image::ImageAsset> decodeBoxPng_() {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(std::filesystem::current_path() / "meshes" / "textured_box.png");
            if (not mapped.has_value()) {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        PPR_UNIT_TEST (gpu_cache_lifecycle_logs) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            CaptureGuard capture{};
            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{std::ignore = pass.shutdown(); };

            const Expected<image::ImageAsset> decoded = decodeBoxPng_();
            PPR_TEST_ASSERT(decoded.has_value());

            const Expected<TextureHandle> first = pass.textureCache().upload(*decoded);
            PPR_TEST_ASSERT(first.has_value());
            const Expected<TextureHandle> second = pass.textureCache().upload(*decoded);
            PPR_TEST_ASSERT(second.has_value());
            PPR_TEST_ASSERT(not pass.textureCache().release(*first));
            PPR_TEST_ASSERT(not pass.textureCache().release(*second));

            // Empty scene publishes nothing: two calls still log once.
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));

            PPR_TEST_ASSERT(not pass.shutdown());
            std::ignore = Log::flush(true);

            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "shader module loaded from file") >= 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::debug, "texture upload dedup hit") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "released texture") == 2u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::debug, "texture entry evicted") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::debug, "publish skipped: empty scene") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "TriangleBagCache shut down") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "BindlessTextureCache shut down") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "BindlessMaterialCache shut down") == 1u);
        };
    } // namespace LoggingGpu
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest logging_gpu = UnitTest::Named("logging_gpu") / [](UnitTest::IRun &_) -> void {
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        PPR_DEFER{PPR_TEST_ASSERT(not detail::SharedGpu::release()); };
        _.recurse({
            detail::LoggingGpu::gpu_cache_lifecycle_logs,
        });
    };

    const UnitTest &loggingGpuTests() noexcept {
        return logging_gpu;
    }
} // namespace pP::tests
