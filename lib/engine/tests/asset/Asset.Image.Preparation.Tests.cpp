module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <mango/image/image.hpp>

module engine.tests.asset;

import engine.asset;
import engine.core;
import engine.image;
import std;

namespace pP::tests::detail::ImagePreparation {
    // ------------------------------------------------------------------
    // owned named image fixtures
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(ImagePreparationTests, verbose, none);

    struct Fixture {
        std::filesystem::path m_root{};

        static void write(const std::filesystem::path &path, const std::string_view bytes) {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            PPR_TEST_ASSERT(static_cast<bool>(output));

            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(static_cast<bool>(output));
        }

        explicit Fixture(const u32 count = 3u) {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();

            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto path = parent / ("image-request-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(path)) {
                    m_root = path;
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            std::string manifest{"schema_version = 1\npack_id = \"ppr.images\"\n"};

            for (u32 index = 0u; index < count; ++index) {
                const auto name = "texture_" + std::to_string(index);
                manifest += "[assets." + name + "]\nkind = \"image.texture\"\ndescriptor = \"" + name + ".toml\"\n";
                write(
                    m_root / (name + ".toml"), "schema_version = 1\nsource = \"" +
                                                   name +
                                                   ".png\"\ninterpretation = \"color\"\nmip_policy = \"mitchell\"\n");

                std::array<std::byte, 8u * 8u * 4u> pixels;

                for (std::size_t pixel = 0u; pixel < pixels.size() / 4u; ++pixel) {
                    pixels[pixel * 4u] = static_cast<std::byte>((pixel * 17u + index) & 255u);
                    pixels[pixel * 4u + 1u] = static_cast<std::byte>((pixel * 7u) & 255u);
                    pixels[pixel * 4u + 2u] = std::byte{96};
                    pixels[pixel * 4u + 3u] = std::byte{255};
                }

                const mango::image::Surface surface{
                    8, 8, mango::image::Format{32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8},
                    8u * 4u, pixels.data()};
                PPR_TEST_ASSERT(static_cast<bool>(surface.save((m_root / (name + ".png")).string())));
            }

            write(m_root / "pack.toml", manifest);
        }

        ~Fixture() {
            // Best-effort fixture teardown after requests/results leave scope.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            PPR_LOG_WARNING_ON_FAIL(ImagePreparationTests, error);
        }

        [[nodiscard]] std::shared_ptr<const asset::PackSet> packs(config::Diagnostics &diagnostics) const {
            auto source = asset::readPackSource(m_root, diagnostics);
            PPR_TEST_ASSERT(source.has_value());

            constexpr std::string_view kinds[]{image::kTextureAssetKind};
            auto packs = asset::PackSet::index({std::move(*source)}, kinds, diagnostics);
            PPR_TEST_ASSERT(packs.has_value());

            return std::make_shared<const asset::PackSet>(std::move(*packs));
        }

        [[nodiscard]] static Array<image::TextureDefinition> definitions(
            const asset::PackSet &packs, config::Diagnostics &diagnostics) {
            auto catalog = image::TextureCatalog::importDefinitions(packs, diagnostics);
            PPR_TEST_ASSERT(catalog.has_value());

            return {catalog->definitions().begin(), catalog->definitions().end()};
        }
    };

    void settle(image::TexturePreparation &request) {
        while (not request.poll()) {
            std::this_thread::yield();
        }
    }

    // ------------------------------------------------------------------
    // actual processors and bounded owned results
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (prepared_chains_match_existing_processor_and_survive_scratch_reuse) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        auto definitions = Fixture::definitions(*packs, diagnostics);
        const auto baseline_input = definitions.front();
        auto request = image::prepareTextures(
            packs, std::move(definitions), asset::RequestGeneration{7u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        settle(*request);

        const auto progress = request->progress();
        PPR_TEST_ASSERT(progress.m_admitted == 3u and progress.m_completed == 3u and progress.m_active == 0u);
        PPR_TEST_ASSERT(progress.m_peak_active <= 2u);

        const auto result = request->take(diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(*result->m_generation == 7u and result->m_value.m_textures.size() == 3u);
        PPR_TEST_ASSERT(result->m_value.m_output_bytes == 3u * (8u * 8u + 4u * 4u + 2u * 2u + 1u) * 4u);

        auto mapped = mem::SharedBuffer::mapFile(baseline_input.m_source_path);
        PPR_TEST_ASSERT(mapped.has_value());

        auto expected = image::decodeToRgba8(mapped->getBufferData(), ".png", {}, image::EImageUsage::color);
        PPR_TEST_ASSERT(expected.has_value());
        PPR_TEST_ASSERT(not image::generateMipChain(*expected, {.m_full_chain = true}));

        const auto &first = result->m_value.m_textures.front();
        PPR_TEST_ASSERT(first.m_image.m_mip_count == expected->m_mip_count);
        PPR_TEST_ASSERT(
            std::ranges::equal(first.m_image.m_storage.getBufferData(), expected->m_storage.getBufferData()));
        PPR_TEST_ASSERT(first.m_source_bytes == mapped->getBufferData().size_bytes());

        {
            const auto scratch = mem::ScratchPad::open();
            Array<std::byte, mem::ScratchPad> reuse(4096u, std::byte{0x7f});
        }

        PPR_TEST_ASSERT(
            std::ranges::equal(first.m_image.m_storage.getBufferData(), expected->m_storage.getBufferData()));
    };

    PPR_UNIT_TEST (request_limits_and_metadata_share_authoritative_domains) {
        const auto metadata = image::TexturePreparationLimits::describe();
        PPR_TEST_ASSERT(
            std::get<3u>(metadata).m_constraint.m_max == image::TexturePreparationLimits::count_domain.m_max);

        const auto texture = image::TextureDescriptor::describe();
        PPR_TEST_ASSERT(std::get<7u>(texture).m_constraint.m_min == image::MipGenDesc::alpha_cutoff_domain.m_min);
        PPR_TEST_ASSERT(std::get<7u>(texture).m_constraint.m_exclusive_min);

        Fixture fixture{2u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        image::TexturePreparationLimits limits;
        limits.m_max_count = 1u;
        PPR_TEST_ASSERT(not image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics, limits));

        limits = {};
        limits.m_max_input_bytes = 1u;
        PPR_TEST_ASSERT(not image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics, limits));

        limits = {};
        limits.m_max_output_bytes = 400u;
        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics, limits);
        PPR_TEST_ASSERT(request.has_value());

        settle(*request);
        PPR_TEST_ASSERT(request->progress().m_active == 0u);

        const auto rejected = request->take(diagnostics);
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::file_too_large);
    };

    PPR_UNIT_TEST (alpha_domain_reuse_preserves_processor_activation_and_settings_identity) {
        Fixture fixture{1u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        auto definitions = Fixture::definitions(*packs, diagnostics);
        const auto baseline = image::textureSettingsFingerprint(definitions.front().m_descriptor.m_value);
        definitions.front().m_descriptor.m_value.m_alpha_cutoff = 0.0f;
        PPR_TEST_ASSERT(not image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics));

        definitions.front().m_descriptor.m_value.m_alpha_cutoff = image::MipGenDesc::alpha_cutoff_domain.m_max;
        PPR_TEST_ASSERT(image::textureSettingsFingerprint(definitions.front().m_descriptor.m_value) != baseline);
        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        const auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value() and result->m_processing_revision == image::kTextureProcessorVersion);

        auto mapped = mem::SharedBuffer::mapFile(definitions.front().m_source_path);
        PPR_TEST_ASSERT(mapped.has_value());

        const auto decoded = image::decodeToRgba8(mapped->getBufferData(), ".png", {}, image::EImageUsage::color);
        PPR_TEST_ASSERT(decoded.has_value());

        auto inactive = *decoded;
        PPR_TEST_ASSERT(not image::generateMipChain(inactive, {.m_alpha_cutoff = 0.0f}));

        auto active = *decoded;
        const auto rejected = image::generateMipChain(
            active, {.m_alpha_cutoff = 0.0f, .m_has_preserve_coverage = true});
        PPR_TEST_ASSERT(rejected == std::errc::invalid_argument);

        const auto nonfinite = image::generateMipChain(
            active, {.m_alpha_cutoff = std::numeric_limits<float>::quiet_NaN(), .m_has_preserve_coverage = true});
        PPR_TEST_ASSERT(nonfinite == std::errc::invalid_argument);
    };

    // ------------------------------------------------------------------
    // cancellation, source failure and external coordination
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (cancellation_drains_admitted_jobs_and_keeps_owned_generation) {
        Fixture fixture{4u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        auto request = image::prepareTextures(
            packs, Fixture::definitions(*packs, diagnostics), asset::RequestGeneration{9u}, context::background(),
            diagnostics);
        PPR_TEST_ASSERT(request.has_value());
        PPR_TEST_ASSERT(not request->poll());

        request->cancel();
        settle(*request);

        const auto progress = request->progress();
        PPR_TEST_ASSERT(
            progress.m_admitted <= 2u and progress.m_completed == progress.m_admitted and progress.m_active == 0u);

        const auto canceled = request->take(diagnostics);
        PPR_TEST_ASSERT(not canceled and canceled.error() == std::errc::operation_canceled);
    };

    PPR_UNIT_TEST (binary_failure_and_revision_conflict_drain_the_request) {
        Fixture fixture{2u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        Fixture::write(fixture.m_root / "texture_0.png", "corrupt encoded image bytes");
        auto corrupt = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(corrupt.has_value());

        settle(*corrupt);
        PPR_TEST_ASSERT(corrupt->progress().m_active == 0u);
        PPR_TEST_ASSERT(not corrupt->take(diagnostics));
        PPR_TEST_ASSERT(diagnostics.entries().front().m_location.find("texture_0.toml") != std::string::npos);

        auto changed = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(changed.has_value());

        Fixture::write(fixture.m_root / "texture_0.png", "replacement with a different byte count");
        const auto conflict = changed->finish(diagnostics);
        PPR_TEST_ASSERT(not conflict and conflict.error() == std::errc::resource_unavailable_try_again);
    };

    PPR_UNIT_TEST (source_containment_is_rechecked_after_definition_import) {
        Fixture fixture{1u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        const auto escaped = fixture.m_root.parent_path() / (fixture.m_root.filename().string() + "-outside.png");
        Fixture::write(escaped, "outside");
        PPR_DEFER {
            std::error_code error;
            std::filesystem::remove(escaped, error);
        };

        std::filesystem::remove(fixture.m_root / "texture_0.png");
        std::error_code error;
        std::filesystem::create_symlink(escaped, fixture.m_root / "texture_0.png", error);
        PPR_TEST_ASSERT(not error);

        const auto rejected = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(not rejected);
        PPR_TEST_ASSERT(diagnostics.entries().back().m_message.find("escapes") != std::string::npos);
    };

    PPR_UNIT_TEST (background_worker_cannot_start_or_block_finish) {
        Fixture fixture{1u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        auto &pool = parallel::backgroundPool();
        auto attempt = pool.schedule(
            [packs, definitions = definitions](const IContext &) {
                config::Diagnostics local{8u};
                const auto request = image::prepareTextures(
                    packs, definitions, asset::RequestGeneration{1u}, context::background(), local);
                return request ? std::error_code{} : request.error();
            },
            std::nullopt, ParallelScheduler::wait_if_full);
        PPR_TEST_ASSERT(attempt.has_value());
        PPR_TEST_ASSERT(attempt->get() == std::errc::resource_deadlock_would_occur);

        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        auto transfer = pool.schedule(
            [owned = std::move(*request)](const IContext &) mutable {
                config::Diagnostics local{8u};
                const auto result = owned.finish(local);
                PPR_TEST_ASSERT(not result and result.error() == std::errc::resource_deadlock_would_occur);
                return std::move(owned);
            },
            std::nullopt, ParallelScheduler::wait_if_full);
        PPR_TEST_ASSERT(transfer.has_value());

        auto returned = transfer->get();
        PPR_TEST_ASSERT(returned.finish(diagnostics).has_value());
    };


}

namespace pP::tests {
    // ------------------------------------------------------------------
    // preparation suite registration
    // ------------------------------------------------------------------

    const UnitTest image_preparation = UnitTest::Named("image_preparation") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::ImagePreparation::prepared_chains_match_existing_processor_and_survive_scratch_reuse,
            detail::ImagePreparation::request_limits_and_metadata_share_authoritative_domains,
            detail::ImagePreparation::alpha_domain_reuse_preserves_processor_activation_and_settings_identity,
            detail::ImagePreparation::cancellation_drains_admitted_jobs_and_keeps_owned_generation,
            detail::ImagePreparation::binary_failure_and_revision_conflict_drain_the_request,
            detail::ImagePreparation::source_containment_is_rechecked_after_definition_import,
            detail::ImagePreparation::background_worker_cannot_start_or_block_finish,
        });
    };

    const UnitTest &imagePreparationTests() noexcept {
        return image_preparation;
    }
}
