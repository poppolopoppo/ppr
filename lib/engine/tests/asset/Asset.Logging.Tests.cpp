module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>

module engine.tests.asset;

import engine.core;
import engine.image;
import engine.mesh;
import std;

// Asset-pipeline logging proofs: the first-occurrence helper plus the
// init/upload-time lines from the logging survey (decode rejects, sRGB
// coercion, mesh dedup/empties, GLB success). GPU-owned lines (caches,
// ring, pipeline cache, RHI budget, shader modules) are proven by the
// logging_gpu group. One focused test per TU.
namespace pP::tests::detail {
    namespace Logging {
        [[nodiscard]] std::filesystem::path loggingMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        struct Captured {
            Log::ELevel m_level = Log::ELevel::debug;
            std::string m_category{};
            std::string m_message{};
        };

        struct LogCapture {
            static inline std::vector<Captured> s_entries{};

            static void push_(const Log::Entry &entry) noexcept {
                try {
                    s_entries.push_back(Captured{
                        entry.m_site.m_verbosity,
                        std::string(entry.m_site.m_category.m_name.view()),
                        std::string(entry.m_message),
                    });
                } catch (...) {
                }
            }
        };

        // Capture policy + debug verbosity + a cleared first-occurrence set,
        // restoring the harness policy/verbosity after a synchronous drain so
        // no queued entry can leak into a later leaf. Never downgrades
        // production severity: every asserted line keeps its shipped level.
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

        PPR_UNIT_TEST (once_helper_claims_first_only) {
            Log::Once::resetForTests();
            constexpr u64 kSite = 0x10CECA1u;
            PPR_TEST_ASSERT(Log::Once::claim(kSite));
            PPR_TEST_ASSERT(not Log::Once::claim(kSite));
            PPR_TEST_ASSERT(Log::Once::claim(Log::Once::combine(kSite, 1u)));
            PPR_TEST_ASSERT(not Log::Once::claim(Log::Once::combine(kSite, 1u)));
            PPR_TEST_ASSERT(Log::Once::combine(kSite, 1u) != Log::Once::combine(kSite, 2u));
            Log::Once::resetForTests();
            PPR_TEST_ASSERT(Log::Once::claim(kSite));
        };

        PPR_UNIT_TEST (decode_rejects_warn_once_per_process) {
            CaptureGuard capture{};
            const std::vector<std::byte> tiny(4u, std::byte{0x00});
            const mem::SharedBufferView tiny_view{tiny.data(), tiny.size()};
            for (int i = 0; i < 2; ++i) {
                const Expected<image::ImageAsset> bad_ext =
                        image::decodeToRgba8(tiny_view, ".tga", image::ImageDecodeDesc{}, image::ImageUsage::color);
                PPR_TEST_ASSERT(not bad_ext.has_value());
                const Expected<image::ImageAsset> empty =
                        image::decodeToRgba8(tiny_view.subspan(0u, 0u), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
                PPR_TEST_ASSERT(not empty.has_value());
            }
            // Below-floor input and an unsound PNG chain reject deterministically.
            const std::vector<std::byte> zeros(40u, std::byte{0x00});
            const mem::SharedBufferView zeros_view{zeros.data(), zeros.size()};
            for (int i = 0; i < 2; ++i) {
                const Expected<image::ImageAsset> bad_chain =
                        image::decodeToRgba8(zeros_view, ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
                PPR_TEST_ASSERT(not bad_chain.has_value());
            }
            std::ignore = Log::flush(true);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::warning, "unsupported extension") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::warning, "empty input") == 1u);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::warning, "unsound PNG chunk chain") == 1u);
        };

        [[nodiscard]] mem::SharedBuffer encodeLoggingPng_(
            const std::string &name, const std::span<const std::byte> rgba) {
            using mango::image::Format;
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_logging_fixtures";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path path = dir / name;
            const mango::image::Surface surface{
                2, 2, Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8), 8u, rgba.data(),
            };
            if (not static_cast<bool>(surface.save(path.string()))) {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        PPR_UNIT_TEST (decode_data_usage_coerces_srgb_to_linear) {
            CaptureGuard capture{};
            constexpr std::byte kRed[16] = {
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
                std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
            };
            const mem::SharedBuffer file = encodeLoggingPng_("coerce.png", kRed);
            PPR_TEST_ASSERT(file.isValid());
            const Expected<image::ImageAsset> color = image::decodeToRgba8(
                file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
            PPR_TEST_ASSERT(color.has_value());
            PPR_TEST_ASSERT(color->m_is_srgb);
            const Expected<image::ImageAsset> data = image::decodeToRgba8(
                file.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::data);
            PPR_TEST_ASSERT(data.has_value());
            PPR_TEST_ASSERT(not data->m_is_srgb);
            std::ignore = Log::flush(true);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "coerces sRGB source to linear") == 1u);
        };

        PPR_UNIT_TEST (glb_import_logs_success_with_embeds) {
            CaptureGuard capture{};
            const Expected<mesh::SceneAsset> scene =
                    mesh::importAndConvert(loggingMeshDir(), "textured_quad.glb");
            PPR_TEST_ASSERT(scene.has_value());
            std::ignore = Log::flush(true);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::info, "GLB import succeeded") == 1u);
        };

        [[nodiscard]] std::filesystem::path writeTwinImageGltf_() {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_logging_dedup" / "";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            std::filesystem::copy_file(
                loggingMeshDir() / "textured_box.png", dir / "textured_box.png",
                std::filesystem::copy_options::overwrite_existing, ec);
            // Two image entries name the same file: the second convert hits
            // the texture-file dedup cache. Geometry/materials mirror
            // textured_box.gltf otherwise.
            const std::string gltf =
                    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0}]}],)"
                    R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0.0,"roughnessFactor":0.9}}],)"
                    R"("textures":[{"source":0}],"images":[{"uri":"textured_box.png"},{"uri":"textured_box.png"}],)"
                    R"("buffers":[{"byteLength":840,"uri":"textured_box.bin"}],)"
                    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":288},{"buffer":0,"byteOffset":288,"byteLength":288},)"
                    R"({"buffer":0,"byteOffset":576,"byteLength":192},{"buffer":0,"byteOffset":768,"byteLength":72}],)"
                    R"("accessors":[{"bufferView":0,"count":24,"type":"VEC3","componentType":5126},)"
                    R"({"bufferView":1,"count":24,"type":"VEC3","componentType":5126},)"
                    R"({"bufferView":2,"count":24,"type":"VEC2","componentType":5126},)"
                    R"({"bufferView":3,"count":36,"type":"SCALAR","componentType":5123}]})";
            std::filesystem::copy_file(
                loggingMeshDir() / "textured_box.bin", dir / "textured_box.bin",
                std::filesystem::copy_options::overwrite_existing, ec);
            std::ofstream out{dir / "twin_box.gltf", std::ios::binary | std::ios::trunc};
            out.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
            out.close();
            return dir;
        }

        PPR_UNIT_TEST (mesh_duplicate_texture_logs_dedup_hit) {
            CaptureGuard capture{};
            const std::filesystem::path dir = writeTwinImageGltf_();
            PPR_TEST_ASSERT(std::filesystem::exists(dir / "textured_box.png"));
            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(dir, "twin_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_images.size() == 2u);
            std::ignore = Log::flush(true);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::debug, "texture-file dedup hit") == 1u);
        };

        [[nodiscard]] std::filesystem::path writeEmptyImageGltf_() {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_logging_empty" / "";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            // A primitive without POSITION leaves an empty Mango mesh:
            // convert keeps no empty asset and warns instead.
            const std::string gltf =
                    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"NORMAL":0},"indices":1,"material":0}]}],)"
                    R"("materials":[{"pbrMetallicRoughness":{"metallicFactor":0.0,"roughnessFactor":0.9}}],)"
                    R"("textures":[],"images":[],)"
                    R"("buffers":[{"byteLength":840,"uri":"textured_box.bin"}],)"
                    R"("bufferViews":[{"buffer":0,"byteOffset":288,"byteLength":288},)"
                    R"({"buffer":0,"byteOffset":768,"byteLength":72}],)"
                    R"("accessors":[{"bufferView":0,"count":24,"type":"VEC3","componentType":5126},)"
                    R"({"bufferView":1,"count":36,"type":"SCALAR","componentType":5123}]})";
            std::filesystem::copy_file(
                loggingMeshDir() / "textured_box.bin", dir / "textured_box.bin",
                std::filesystem::copy_options::overwrite_existing, ec);
            std::ofstream out{dir / "hole_box.gltf", std::ios::binary | std::ios::trunc};
            out.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
            out.close();
            return dir;
        }

        PPR_UNIT_TEST (mesh_empty_mesh_warns_and_rejects) {
            CaptureGuard capture{};
            const std::filesystem::path dir = writeEmptyImageGltf_();
            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(dir, "hole_box.gltf");
            PPR_TEST_ASSERT(not scene.has_value());
            PPR_TEST_ASSERT(scene.error() == std::errc::invalid_argument);
            std::ignore = Log::flush(true);
            PPR_TEST_ASSERT(countAt_(Log::ELevel::warning, "no vertices or primitives") == 1u);
        };
    } // namespace Logging
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest logging = UnitTest::Named("logging") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Logging::once_helper_claims_first_only,
            detail::Logging::decode_rejects_warn_once_per_process,
            detail::Logging::decode_data_usage_coerces_srgb_to_linear,
            detail::Logging::glb_import_logs_success_with_embeds,
            detail::Logging::mesh_duplicate_texture_logs_dedup_hit,
            detail::Logging::mesh_empty_mesh_warns_and_rejects,
        });
    };

    const UnitTest &loggingTests() noexcept {
        return logging;
    }
} // namespace pP::tests
