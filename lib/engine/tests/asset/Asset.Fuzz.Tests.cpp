module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>

module engine.tests.asset;

import engine.core;
import engine.image;
import engine.mesh;
import std;

namespace pP::tests::detail {
    namespace Fuzz {
        // Seeded deterministic corpus (Phase 5 hardening): the fixed seed makes
        // every mutation identical on every run, so each rejection is a
        // deterministic errc — never a crash, leak, or hang. Bounded by
        // construction: small fixtures, small iteration counts, no loops over
        // attacker-controlled sizes.
        inline constexpr u64 kFuzzSeed{0x50505246555A3121u};

        class FuzzRng final {
            std::mt19937_64 m_state;

        public:
            explicit FuzzRng(const u64 seed = kFuzzSeed) noexcept : m_state(seed) {
            }

            [[nodiscard]] u64 next(const u64 bound) noexcept {
                std::uniform_int_distribution<u64> pick{0u, bound - 1u};
                return pick(m_state);
            }

            [[nodiscard]] std::byte nextByte() noexcept {
                return static_cast<std::byte>(next(256u));
            }
        };

        void discardExpectedFailureLog_(const Log::Entry &) noexcept {
        }

        class ExpectedFailureLogGuard final {
            Log::Policy m_previous;

        public:
            ExpectedFailureLogGuard() noexcept : m_previous(Log::setWriterPolicy(discardExpectedFailureLog_)) {
            }

            ~ExpectedFailureLogGuard() noexcept {
                // Drain under the discard policy (see Asset.Search.Tests.cpp):
                // the logger is async, and an unrestored queue would fail a
                // later test nondeterministically.
                std::ignore = Log::flush(true);
                Log::setWriterPolicy(m_previous);
            }
        };

        [[nodiscard]] std::filesystem::path fuzzDir() {
            const std::filesystem::path dir = std::filesystem::current_path() / "fuzz_tmp";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            return dir;
        }

        [[nodiscard]] std::filesystem::path meshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] bool writeScratch(const std::string &name, const std::span<const std::byte> bytes) {
            std::ofstream file{fuzzDir() / name, std::ios::binary | std::ios::trunc};
            if (not file) {
                return false;
            }
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(file);
        }

        // 8x8 RGBA checker PNG: small, valid, exercises the full
        // file -> mapFile -> decode lifecycle.
        [[nodiscard]] mem::SharedBuffer seedPngBytes() {
            const std::filesystem::path path = fuzzDir() / "fuzz_seed.png";
            Array<std::byte> rgba(8u * 8u * 4u, std::byte{0});
            for (u32 y = 0u; y < 8u; ++y) {
                for (u32 x = 0u; x < 8u; ++x) {
                    const std::size_t base = (static_cast<std::size_t>(y) * 8u + x) * 4u;
                    rgba[base] = ((x + y) % 2u == 0u) ? std::byte{0xFF} : std::byte{0x00};
                    rgba[base + 1u] = std::byte{0x80};
                    rgba[base + 2u] = std::byte{0x40};
                    rgba[base + 3u] = std::byte{0xFF};
                }
            }
            using mango::image::Format;
            const mango::image::Surface surface{
                8, 8, Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8),
                static_cast<std::size_t>(8u) * 4u, rgba.data(),
            };
            if (not
                static_cast<bool>(surface.save(path.string())))
            {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        [[nodiscard]] mem::SharedBuffer seedGlbBytes() {
            if (Expected<mem::SharedBuffer> mapped =
                        mem::SharedBuffer::mapFile(meshDir() / "textured_quad.glb");
                mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        [[nodiscard]] mem::SharedBuffer seedGltfBytes() {
            if (Expected<mem::SharedBuffer> mapped =
                        mem::SharedBuffer::mapFile(meshDir() / "textured_box.gltf");
                mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        struct ImageOutcome {
            bool m_threw = false;
            bool m_ok = false;
            std::error_code m_err{};
            u32 m_width = 0u;
            u32 m_height = 0u;
        };

        [[nodiscard]] ImageOutcome tryDecodeRgba8(
            const std::span<const std::byte> bytes, const std::string_view ext, const image::ImageDecodeDesc desc) noexcept {
            ImageOutcome out{};
            try {
                const Expected<image::ImageAsset> decoded =
                        image::decodeToRgba8(mem::SharedBufferView{bytes.data(), bytes.size()}, ext, desc, image::ImageUsage::color);
                if (decoded.has_value()) {
                    out.m_ok = true;
                    out.m_width = decoded->m_width;
                    out.m_height = decoded->m_height;
                } else {
                    out.m_err = decoded.error();
                }
            } catch (...) {
                out.m_threw = true;
            }
            return out;
        }

        struct MeshOutcome {
            bool m_threw = false;
            bool m_ok = false;
            std::error_code m_err{};
            std::size_t m_meshes = 0u;
        };

        [[nodiscard]] MeshOutcome tryImport(
            const std::filesystem::path &dir, const std::string_view file, const mesh::MeshLimits &limits) noexcept {
            MeshOutcome out{};
            try {
                ExpectedFailureLogGuard expected_failure_log{};
                const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(dir, file, limits);
                if (scene.has_value()) {
                    out.m_ok = true;
                    out.m_meshes = scene->m_meshes.size();
                } else {
                    out.m_err = scene.error();
                }
            } catch (...) {
                out.m_threw = true;
            }
            return out;
        }

        [[nodiscard]] bool sameImageOutcome(const ImageOutcome &lhs, const ImageOutcome &rhs) noexcept {
            if (lhs.m_threw
                or
            rhs.m_threw
            or
            lhs.m_ok != rhs.m_ok)
            {
                return false;
            }
            if (lhs.m_ok) {
                return lhs.m_width == rhs.m_width
                and
                lhs.m_height == rhs.m_height;
            }
            return lhs.m_err == rhs.m_err;
        }

        [[nodiscard]] bool sameMeshOutcome(const MeshOutcome &lhs, const MeshOutcome &rhs) noexcept {
            if (lhs.m_threw
                or
            rhs.m_threw
            or
            lhs.m_ok != rhs.m_ok)
            {
                return false;
            }
            if (lhs.m_ok) {
                return lhs.m_meshes == rhs.m_meshes;
            }
            return lhs.m_err == rhs.m_err;
        }

        // Mutated PNGs (truncations, flips, header munges) must decode or
        // reject deterministically: same bytes, same Outcome, never a throw.
        PPR_UNIT_TEST (fuzz_png_mutations_reject_deterministically) {
            const mem::SharedBuffer seed = seedPngBytes();
            PPR_TEST_ASSERT(not seed.getBufferData().empty());
            const mem::SharedBufferView seed_view = seed.getBufferData();
            const std::size_t seed_size = seed_view.size();
            PPR_TEST_ASSERT(seed_size > 16u);

            FuzzRng rng{};
            u32 rejects = 0u;
            for (u32 i = 0u; i < 240u; ++i) {
                Array<std::byte> mutated{seed_view.begin(), seed_view.end()};
                const u64 kind = rng.next(3u);
                if (kind == 0u) {
                    mutated.resize(static_cast<std::size_t>(rng.next(seed_size - 1u) + 1u));
                } else if (kind == 1u) {
                    const std::size_t at = static_cast<std::size_t>(rng.next(seed_size));
                    mutated[at] = rng.nextByte();
                } else {
                    // Header munge: first 16 bytes cover the PNG signature +
                    // IHDR length/type, so decoders must reject early.
                    const std::size_t at = static_cast<std::size_t>(rng.next(16u));
                    mutated[at] = rng.nextByte();
                }
                const ImageOutcome first = tryDecodeRgba8(mutated, ".png", image::ImageDecodeDesc{});
                PPR_TEST_ASSERT(not first.m_threw);
                const ImageOutcome second = tryDecodeRgba8(mutated, ".png", image::ImageDecodeDesc{});
                PPR_TEST_ASSERT(sameImageOutcome(first, second));
                if (not
                    first.m_ok)
                {
                    ++rejects;
                }
            }
            // The corpus must actually exercise rejections, not just pass through.
            PPR_TEST_ASSERT(rejects > 0u);
        };

        // Valid PNG bytes under a compressed extension (and truncated
        // would-be block sources) must fail closed without recompression.
        PPR_UNIT_TEST (fuzz_compressed_ext_rejects_without_recompress) {
            const mem::SharedBuffer seed = seedPngBytes();
            PPR_TEST_ASSERT(not seed.getBufferData().empty());
            const mem::SharedBufferView seed_view = seed.getBufferData();

            FuzzRng rng{};
            for (u32 i = 0u; i < 40u; ++i) {
                Array<std::byte> mutated{seed_view.begin(), seed_view.end()};
                if (i % 2u == 0u) {
                    mutated.resize(static_cast<std::size_t>(rng.next(seed_view.size() - 1u) + 1u));
                } else {
                    mutated[static_cast<std::size_t>(rng.next(seed_view.size()))] = rng.nextByte();
                }
                for (const std::string_view ext: {".ktx2", ".dds"}) {
                    const Expected<image::ImageAsset> first = image::decodeToBlocks(
                        mem::SharedBufferView{mutated.data(), mutated.size()}, ext, image::BlockTag::bc7,
                        image::ImageDecodeDesc{});
                    PPR_TEST_ASSERT(not first.has_value());
                    const Expected<image::ImageAsset> second = image::decodeToBlocks(
                        mem::SharedBufferView{mutated.data(), mutated.size()}, ext, image::BlockTag::bc7,
                        image::ImageDecodeDesc{});
                    PPR_TEST_ASSERT(not second.has_value());
                    PPR_TEST_ASSERT(first.error() == second.error());
                }
            }
        };

        // Production image limits must fire on real decodes: an 8x8 PNG under
        // a 4x4 / 64-byte budget rejects with invalid_argument.
        PPR_UNIT_TEST (fuzz_image_limits_reject_over_limit) {
            const mem::SharedBuffer seed = seedPngBytes();
            PPR_TEST_ASSERT(not seed.getBufferData().empty());
            const mem::SharedBufferView seed_view = seed.getBufferData();

            image::ImageDecodeDesc tight{};
            tight.m_limits.m_max_width = 4u;
            tight.m_limits.m_max_height = 4u;
            tight.m_limits.m_max_decoded_bytes = 64u;
            const ImageOutcome dims = tryDecodeRgba8({seed_view.data(), seed_view.size()}, ".png", tight);
            PPR_TEST_ASSERT(not dims.m_threw);
            PPR_TEST_ASSERT(not dims.m_ok);
            PPR_TEST_ASSERT(dims.m_err == std::errc::invalid_argument);

            image::ImageDecodeDesc tiny_input{};
            tiny_input.m_limits.m_max_input_bytes = 16u;
            const ImageOutcome input =
                    tryDecodeRgba8({seed_view.data(), seed_view.size()}, ".png", tiny_input);
            PPR_TEST_ASSERT(not input.m_threw);
            PPR_TEST_ASSERT(not input.m_ok);
            PPR_TEST_ASSERT(input.m_err == std::errc::invalid_argument);

            // Defaults still accept the seed: limits must not break valid inputs.
            const ImageOutcome open = tryDecodeRgba8({seed_view.data(), seed_view.size()}, ".png", image::ImageDecodeDesc{});
            PPR_TEST_ASSERT(not open.m_threw);
            PPR_TEST_ASSERT(open.m_ok);
            PPR_TEST_ASSERT(open.m_width == 8u and open.m_height == 8u);
        };

        // GLB container locator: BIN payload start, so flips land in vertex
        // data (memory-safe garbage) and never in structural JSON.
        [[nodiscard]] std::size_t glbBinStart_(const std::span<const std::byte> bytes) noexcept {
            if (bytes.size() < 20u) {
                return bytes.size();
            }
            u32 json_len = 0u;
            std::memcpy(&json_len, bytes.data() + 12u, sizeof(json_len));
            return 12u + 8u + static_cast<std::size_t>(json_len) + 8u;
        }

        // Mutated GLBs (truncations, header flips, BIN-payload flips) must
        // import or reject deterministically: never a throw, never a hang.
        PPR_UNIT_TEST (fuzz_glb_mutations_fail_closed) {
            const mem::SharedBuffer seed = seedGlbBytes();
            PPR_TEST_ASSERT(not seed.getBufferData().empty());
            const mem::SharedBufferView seed_view = seed.getBufferData();
            const std::size_t seed_size = seed_view.size();
            const std::size_t bin_start = glbBinStart_({seed_view.data(), seed_size});
            PPR_TEST_ASSERT(bin_start < seed_size);

            FuzzRng rng{};
            u32 rejects = 0u;
            for (u32 i = 0u; i < 90u; ++i) {
                Array<std::byte> mutated{seed_view.begin(), seed_view.end()};
                const u64 kind = rng.next(3u);
                if (kind == 0u) {
                    mutated.resize(static_cast<std::size_t>(rng.next(seed_size - 1u) + 1u));
                } else if (kind == 1u) {
                    // Header region: magic/version/total-length only.
                    mutated[static_cast<std::size_t>(rng.next(12u))] = rng.nextByte();
                } else {
                    // BIN payload only: structural JSON stays intact.
                    const std::size_t at = bin_start + static_cast<std::size_t>(rng.next(seed_size - bin_start));
                    mutated[at] = rng.nextByte();
                }
                const std::string name = std::format("fuzz_glb_{}.glb", i);
                PPR_TEST_ASSERT(writeScratch(name, mutated));
                const MeshOutcome first = tryImport(fuzzDir() / "", name, mesh::kDefaultMeshLimits);
                PPR_TEST_ASSERT(not first.m_threw);
                const MeshOutcome second = tryImport(fuzzDir() / "", name, mesh::kDefaultMeshLimits);
                PPR_TEST_ASSERT(sameMeshOutcome(first, second));
                if (not
                    first.m_ok)
                {
                    ++rejects;
                }
            }
            PPR_TEST_ASSERT(rejects > 0u);
            std::error_code err{};
            std::filesystem::remove_all(fuzzDir(), err);
        };

        // Truncated .gltf JSON must fail closed at parse time (no external
        // refs are read: truncation alone never resolves a .bin fetch).
        PPR_UNIT_TEST (fuzz_gltf_truncations_fail_closed) {
            const mem::SharedBuffer seed = seedGltfBytes();
            PPR_TEST_ASSERT(not seed.getBufferData().empty());
            const mem::SharedBufferView seed_view = seed.getBufferData();
            const std::size_t seed_size = seed_view.size();
            PPR_TEST_ASSERT(seed_size > 16u);

            FuzzRng rng{};
            u32 rejects = 0u;
            for (u32 i = 0u; i < 40u; ++i) {
                Array<std::byte> mutated{seed_view.begin(), seed_view.end()};
                if (i % 2u == 0u) {
                    mutated.resize(static_cast<std::size_t>(rng.next(seed_size - 1u) + 1u));
                } else {
                    mutated[static_cast<std::size_t>(rng.next(16u))] = rng.nextByte();
                }
                const std::string name = std::format("fuzz_gltf_{}.gltf", i);
                PPR_TEST_ASSERT(writeScratch(name, mutated));
                const MeshOutcome first = tryImport(fuzzDir() / "", name, mesh::kDefaultMeshLimits);
                PPR_TEST_ASSERT(not first.m_threw);
                const MeshOutcome second = tryImport(fuzzDir() / "", name, mesh::kDefaultMeshLimits);
                PPR_TEST_ASSERT(sameMeshOutcome(first, second));
                if (not
                    first.m_ok)
                {
                    ++rejects;
                }
            }
            PPR_TEST_ASSERT(rejects > 0u);
            std::error_code err{};
            std::filesystem::remove_all(fuzzDir(), err);
        };

        // Production mesh limits must fire on valid fixtures: zeroed count
        // caps, a tiny GLB byte cap, and a tiny vertex cap each reject the
        // otherwise-valid quad with invalid_argument.
        PPR_UNIT_TEST (fuzz_mesh_limits_reject_over_limit) {
            ExpectedFailureLogGuard expected_failure_log{};

            mesh::MeshLimits no_meshes = mesh::kDefaultMeshLimits;
            no_meshes.m_max_meshes = 0u;
            PPR_TEST_ASSERT(
                mesh::importAndConvert(meshDir(), "textured_quad.glb", no_meshes).error()
                == std::errc::invalid_argument);

            mesh::MeshLimits tiny_glb = mesh::kDefaultMeshLimits;
            tiny_glb.m_max_glb_bytes = 16u;
            PPR_TEST_ASSERT(
                mesh::importAndConvert(meshDir(), "textured_quad.glb", tiny_glb).error()
                == std::errc::invalid_argument);

            mesh::MeshLimits tiny_verts = mesh::kDefaultMeshLimits;
            tiny_verts.m_max_vertices_per_mesh = 1u;
            PPR_TEST_ASSERT(
                mesh::importAndConvert(meshDir(), "textured_quad.glb", tiny_verts).error()
                == std::errc::invalid_argument);

            mesh::MeshLimits tiny_chunks = mesh::kDefaultMeshLimits;
            tiny_chunks.m_max_glb_chunks = 0u;
            PPR_TEST_ASSERT(
                mesh::importAndConvert(meshDir(), "textured_quad.glb", tiny_chunks).error()
                == std::errc::invalid_argument);

            // Defaults still accept the fixture: limits must not break valid inputs.
            PPR_TEST_ASSERT(mesh::importAndConvert(meshDir(), "textured_quad.glb").has_value());
        };
    } // namespace Fuzz
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest fuzz = UnitTest::Named("fuzz") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Fuzz::fuzz_png_mutations_reject_deterministically,
            detail::Fuzz::fuzz_compressed_ext_rejects_without_recompress,
            detail::Fuzz::fuzz_image_limits_reject_over_limit,
            detail::Fuzz::fuzz_glb_mutations_fail_closed,
            detail::Fuzz::fuzz_gltf_truncations_fail_closed,
            detail::Fuzz::fuzz_mesh_limits_reject_over_limit,
        });
    };

    const UnitTest &fuzzTests() noexcept {
        return fuzz;
    }
} // namespace pP::tests
