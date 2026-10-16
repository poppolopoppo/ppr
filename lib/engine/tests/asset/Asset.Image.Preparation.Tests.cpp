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

    Array<image::TextureReuseCandidate> prepareReusable(
        const std::shared_ptr<const asset::PackSet> &packs, const Array<image::TextureDefinition> &definitions,
        config::Diagnostics &diagnostics) {
        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{1u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(request.has_value());

        auto result = request->finish(diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_work.m_read_count == definitions.size());

        Array<image::TextureReuseCandidate> reusable;
        reusable.reserve(definitions.size());

        for (auto &prepared: result->m_value.m_textures) {
            const auto definition = std::ranges::find(definitions, prepared.m_id, &image::TextureDefinition::m_id);
            PPR_TEST_ASSERT(definition != definitions.end());

            const auto indexed = packs->resolve(asset::AssetKey{*prepared.m_id}, image::kTextureAssetKind, diagnostics);
            PPR_TEST_ASSERT(indexed.has_value());

            reusable.push_back(
                {std::move(prepared), definition->m_descriptor.m_value, packs->packs()[(*indexed)->m_pack_index].m_root,
                    definition->m_descriptor.m_source.m_path});
        }

        return reusable;
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

    // ------------------------------------------------------------------
    // independently owned reuse and selected-order budgets
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (all_hit_requests_own_results_without_worker_admission_and_still_cancel) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        auto reusable = prepareReusable(packs, definitions, diagnostics);
        const auto pixels = reusable.front().m_prepared.m_image.m_storage.getBufferData().data();

        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics, {}, reusable);
        PPR_TEST_ASSERT(request.has_value() and request->poll());
        PPR_TEST_ASSERT(request->progress().m_admitted == 0u and request->progress().m_peak_active == 0u);

        auto result = request->take(diagnostics);
        PPR_TEST_ASSERT(result.has_value() and *result->m_generation == 2u);
        PPR_TEST_ASSERT(result->m_value.m_work.m_read_count == 0u and result->m_value.m_work.m_read_bytes == 0u);
        PPR_TEST_ASSERT(result->m_value.m_work.m_prepared_count == 0u and result->m_value.m_work.m_reused_count == 3u);
        PPR_TEST_ASSERT(result->m_value.m_input_bytes != 0u and result->m_value.m_output_bytes == 1020u);

        reusable.clear();
        PPR_TEST_ASSERT(result->m_value.m_textures.front().m_image.m_storage.getBufferData().data() == pixels);
        PPR_TEST_ASSERT(
            result->m_value.m_textures.front().m_source_path ==
            std::filesystem::canonical(fixture.m_root / "texture_0.png"));

        reusable = prepareReusable(packs, definitions, diagnostics);
        auto cancelled = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{3u}, context::background(), diagnostics, {}, reusable);
        PPR_TEST_ASSERT(cancelled.has_value());

        cancelled->cancel();
        settle(*cancelled);
        PPR_TEST_ASSERT(cancelled->progress().m_admitted == 0u);

        const auto stopped = cancelled->take(diagnostics);
        PPR_TEST_ASSERT(not stopped and stopped.error() == std::errc::operation_canceled);

        image::TexturePreparationLimits limits;
        limits.m_max_count = 2u;
        PPR_TEST_ASSERT(not image::prepareTextures(
            packs, definitions, asset::RequestGeneration{4u}, context::background(), diagnostics, limits, reusable));

        limits = {};
        limits.m_max_output_bytes = 500u;
        PPR_TEST_ASSERT(not image::prepareTextures(
            packs, definitions, asset::RequestGeneration{5u}, context::background(), diagnostics, limits, reusable));

        Fixture empty{0u};
        const auto empty_packs = empty.packs(diagnostics);
        auto empty_request = image::prepareTextures(
            empty_packs, {}, asset::RequestGeneration{6u}, context::background(), diagnostics);
        PPR_TEST_ASSERT(empty_request.has_value() and empty_request->poll());
        PPR_TEST_ASSERT(empty_request->progress().m_admitted == 0u);

        const auto empty_result = empty_request->take(diagnostics);
        PPR_TEST_ASSERT(empty_result.has_value() and empty_result->m_value.m_textures.empty());
    };

    PPR_UNIT_TEST (changed_settings_prepare_only_the_affected_owned_image) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        auto definitions = Fixture::definitions(*packs, diagnostics);
        const auto reusable = prepareReusable(packs, definitions, diagnostics);
        definitions[1u].m_descriptor.m_value.m_high_quality = true;

        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics, {}, reusable);
        PPR_TEST_ASSERT(request.has_value());

        settle(*request);
        const auto progress = request->progress();
        PPR_TEST_ASSERT(progress.m_admitted == 1u and progress.m_completed == 1u and progress.m_active == 0u);

        const auto result = request->take(diagnostics);
        PPR_TEST_ASSERT(result.has_value());
        PPR_TEST_ASSERT(result->m_value.m_work.m_read_count == 1u and result->m_value.m_work.m_prepared_count == 1u);
        PPR_TEST_ASSERT(result->m_value.m_work.m_reused_count == 2u);
        PPR_TEST_ASSERT(
            result->m_value.m_textures[0u].m_image.m_storage.getBufferData().data() ==
            reusable[0u].m_prepared.m_image.m_storage.getBufferData().data());
        PPR_TEST_ASSERT(
            result->m_value.m_textures[1u].m_settings_fingerprint != reusable[1u].m_prepared.m_settings_fingerprint);
    };

    PPR_UNIT_TEST (invalid_reuse_storage_layout_and_provenance_require_fresh_preparation) {
        Fixture fixture{1u};
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        const auto original = prepareReusable(packs, definitions, diagnostics);

        for (u32 mutation = 0u; mutation < 6u; ++mutation) {
            auto reusable = original;
            auto &candidate = reusable.front();
            if (mutation == 0u) {
                candidate.m_prepared.m_image.m_subresources.front().m_row_pitch += 4u;
            } else if (mutation == 1u) {
                candidate.m_prepared.m_image.m_storage = candidate.m_prepared.m_image.m_storage.subspan(0u);
                PPR_TEST_ASSERT(not candidate.m_prepared.m_image.m_storage.isOwned());
            } else if (mutation == 2u) {
                candidate.m_descriptor_path = fixture.m_root / "different.toml";
            } else if (mutation == 3u) {
                ++candidate.m_prepared.m_processor_version;
            } else if (mutation == 4u) {
                candidate.m_prepared.m_source_path = fixture.m_root / "different.png";
            } else {
                candidate.m_prepared.m_source_bytes = 0u;
            }

            auto request = image::prepareTextures(
                packs, definitions, asset::RequestGeneration{2u + mutation}, context::background(), diagnostics, {},
                reusable);
            PPR_TEST_ASSERT(request.has_value());

            const auto result = request->finish(diagnostics);
            PPR_TEST_ASSERT(result.has_value());
            PPR_TEST_ASSERT(
                result->m_value.m_work.m_prepared_count == 1u and result->m_value.m_work.m_reused_count == 0u);
            PPR_TEST_ASSERT(not image::validateMipChain(result->m_value.m_textures.front().m_image));
        }
    };

    PPR_UNIT_TEST (mixed_reuse_charges_the_full_selected_prefix_and_cached_compressed_input) {
        Fixture fixture;
        config::Diagnostics diagnostics{32u};
        const auto packs = fixture.packs(diagnostics);
        const auto definitions = Fixture::definitions(*packs, diagnostics);
        auto reusable = prepareReusable(packs, definitions, diagnostics);
        reusable.erase(reusable.begin());
        reusable.pop_back();
        image::TexturePreparationLimits limits;
        limits.m_max_output_bytes = 500u;

        auto request = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{2u}, context::background(), diagnostics, limits, reusable);
        PPR_TEST_ASSERT(request.has_value());

        settle(*request);
        const auto progress = request->progress();
        PPR_TEST_ASSERT(progress.m_admitted == 2u and progress.m_completed == 2u and progress.m_active == 0u);
        PPR_TEST_ASSERT(progress.m_peak_active <= image::TextureProcessor::maximum_active);

        const auto result = request->take(diagnostics);
        PPR_TEST_ASSERT(not result and result.error() == std::errc::file_too_large);

        const auto crossing = std::ranges::find_if(diagnostics.entries(), [](const auto &entry) {
            return entry.m_message == "Selected texture closure exceeds aggregate mip output limit";
        });
        PPR_TEST_ASSERT(crossing != diagnostics.entries().end());
        PPR_TEST_ASSERT(crossing->m_location == (fixture.m_root / "texture_1.toml").string());
        PPR_TEST_ASSERT(std::ranges::count_if(diagnostics.entries(), [](const auto &entry) {
            return entry.m_message == "Selected texture closure exceeds aggregate mip output limit";
        }) == 1u);

        reusable = prepareReusable(packs, definitions, diagnostics);
        limits = {};
        limits.m_max_input_bytes = reusable[0u].m_prepared.m_source_bytes + reusable[1u].m_prepared.m_source_bytes;
        const auto oversized = image::prepareTextures(
            packs, definitions, asset::RequestGeneration{3u}, context::background(), diagnostics, limits, reusable);
        PPR_TEST_ASSERT(not oversized and oversized.error() == config::EConfigErrc::out_of_range);
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
            detail::ImagePreparation::all_hit_requests_own_results_without_worker_admission_and_still_cancel,
            detail::ImagePreparation::changed_settings_prepare_only_the_affected_owned_image,
            detail::ImagePreparation::invalid_reuse_storage_layout_and_provenance_require_fresh_preparation,
            detail::ImagePreparation::mixed_reuse_charges_the_full_selected_prefix_and_cached_compressed_input,
            detail::ImagePreparation::background_worker_cannot_start_or_block_finish,
        });
    };

    const UnitTest &imagePreparationTests() noexcept {
        return image_preparation;
    }
}
