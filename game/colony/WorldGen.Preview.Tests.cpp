module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.worldgen_preview;
import std;

namespace pP::game::tests::detail::worldgen_preview {
    using namespace colony;
    struct OutputDirectory {
        std::filesystem::path m_parent{hal::process::currentExecutablePath().parent_path()};
        std::filesystem::path m_root;
        OutputDirectory() {
            static std::atomic<u32> ordinal{};
            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = m_parent / ("worldgen-preview-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = candidate;
                    break;
                }
            }
            PPR_TEST_ASSERT(not m_root.empty());
        }
        ~OutputDirectory() {
            // Best-effort fixture cleanup only; never remove anything outside
            // the explicitly owned sibling directory created above.
            if (not m_root.empty() and m_root.parent_path() == m_parent) {
                std::error_code error;
                std::filesystem::remove_all(m_root, error);
            }
        }
    };
    std::string readBytes(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        PPR_TEST_ASSERT(file.good());
        return {std::istreambuf_iterator<char>{file}, {}};
    }
    void verifySample(const WorldgenPreviewResult &sample, const sim::GlobalCellPos origin) {
        PPR_TEST_ASSERT(
            sample.m_visible ==
            (sim::GenerationCellBounds{
                origin.m_x, origin.m_y, origin.m_x + kWorldgenPreviewEdge, origin.m_y + kWorldgenPreviewEdge}));
        PPR_TEST_ASSERT(sample.m_grid.residentChunkCount() <= kWorldgenPreviewMaxResidentChunks);
        PPR_TEST_ASSERT(sample.m_grid.residentChunkCount() > 0u);
        PPR_TEST_ASSERT(sample.m_checksum != 0u);
        u64 cells{};
        u64 solid{};
        for (const auto count: sample.m_material_counts) {
            cells += count.m_cells;
            solid += count.m_material != 0u ? count.m_cells : 0u;
        }
        PPR_TEST_ASSERT(cells == 256u * 256u and solid == sample.m_solid_cells);
        PPR_TEST_ASSERT(sample.m_geometry.m_instances.empty());
        PPR_TEST_ASSERT(not sample.m_omissions.empty());
        PPR_TEST_ASSERT(sample.m_known_scratch_peak_bytes > 0u and sample.m_known_retained_payload_bytes > 0u);
        // Eight metres at 3:1 is a24-cell formation; its complete candidate
        // roots and four-cell support/validation ring must be resident.
        constexpr u32 reach = 28u;
        const auto &domain = sample.m_formation_domain;
        const auto &support = sample.m_support;
        PPR_TEST_ASSERT(domain.m_left <= (origin.m_x > 24u ? origin.m_x - 24u : 0u));
        PPR_TEST_ASSERT(domain.m_bottom <= (origin.m_y > 24u ? origin.m_y - 24u : 0u));
        PPR_TEST_ASSERT(domain.m_right >= std::min(sim::kWorldEdge, origin.m_x + 256u + 24u));
        PPR_TEST_ASSERT(domain.m_top >= std::min(sim::kWorldEdge, origin.m_y + 256u + 24u));
        PPR_TEST_ASSERT(support.m_left <= (domain.m_left > reach ? domain.m_left - reach : 0u));
        PPR_TEST_ASSERT(support.m_bottom <= (domain.m_bottom > reach ? domain.m_bottom - reach : 0u));
        PPR_TEST_ASSERT(support.m_right >= std::min(sim::kWorldEdge, domain.m_right + reach));
        PPR_TEST_ASSERT(support.m_top >= std::min(sim::kWorldEdge, domain.m_top + reach));
        for (u32 y = support.m_bottom; y < support.m_top; y += sim::kChunkEdge) {
            for (u32 x = support.m_left; x < support.m_right; x += sim::kChunkEdge) {
                PPR_TEST_ASSERT(sample.m_grid.residentCells(sim::chunkOf({x, y})).has_value());
            }
        }
    }
    void verifyImages(const WorldgenPreviewResult &sample, const std::filesystem::path &directory) {
        constexpr std::string_view header{"P6\n256 256\n255\n"};
        constexpr std::size_t image_bytes = 256u * 256u * 3u;
        const auto materials = readBytes(directory / "material.ppm");
        const auto biomes = readBytes(directory / "biome.ppm");
        const auto temperatures = readBytes(directory / "temperature.ppm");
        for (const std::string_view bytes:
            {std::string_view{materials}, std::string_view{biomes}, std::string_view{temperatures}}) {
            PPR_TEST_ASSERT(bytes.starts_with(header) and bytes.size() == header.size() + image_bytes);
        }
        const u32 biome_rgba = sample.m_catalog.biomes()[sample.m_biome_index].m_preview_color_rgba;
        bool unequal_rows{};
        for (u32 row = 0u; row < 256u; ++row) {
            const u32 y = sample.m_visible.m_top - 1u - row;
            for (u32 column = 0u; column < 256u; ++column) {
                const u32 x = sample.m_visible.m_left + column;
                const auto cell = sample.m_grid.getCell({x, y});
                PPR_TEST_ASSERT(cell.has_value());
                const auto *const material = sample.m_catalog.findMaterial(cell->m_element);
                const u32 rgba = material ? material->m_color_rgba : 0u;
                const std::size_t pixel = header.size() + (row * 256u + column) * 3u;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const u32 shift = 24u - channel * 8u;
                    PPR_TEST_ASSERT(static_cast<u8>(materials[pixel + channel]) == ((rgba >> shift) & 255u));
                    PPR_TEST_ASSERT(static_cast<u8>(biomes[pixel + channel]) == ((biome_rgba >> shift) & 255u));
                }
                unequal_rows = unequal_rows or
                               materials[pixel] != materials[header.size() + ((255u - row) * 256u + column) * 3u];
            }
        }
        // This authored subsurface sample contains an asymmetric raster, so the
        // per-cell comparison above also detects a vertical image inversion.
        PPR_TEST_ASSERT(unequal_rows);
    }
    PPR_UNIT_TEST (bounded_samples_replay_and_export_the_exact_visible_draft) {
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        config::Diagnostics diagnostics;
        auto source = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(source.has_value());
        auto &biome_bytes = source->m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes;
        const auto hearth = biome_bytes.find("[biomes.hearth]");
        PPR_TEST_ASSERT(hearth != std::string::npos);
        const auto length = biome_bytes.find("formation_length_m = ", hearth);
        PPR_TEST_ASSERT(length != std::string::npos);
        const auto value = length + std::string_view{"formation_length_m = "}.size();
        const auto value_end = biome_bytes.find_first_of("\r\n", value);
        PPR_TEST_ASSERT(value_end != std::string::npos);
        biome_bytes.replace(value, value_end - value, "8.0");
        const auto catalog = loadContentCatalog(root, *source, diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());

        const WorldgenPreviewSpec spec{"frontier", "hearth", 0xfedcba9876543210ull, 73u, {1879u, 2993u}};
        auto sample = prepareWorldgenPreview(*catalog, spec);
        PPR_TEST_ASSERT(sample.has_value());
        PPR_TEST_ASSERT(sample->m_plan.recipe().m_biomes[sample->m_biome_index].m_formation_length_m == 8.0);
        verifySample(*sample, spec.m_origin);
        {
            const auto replay = prepareWorldgenPreview(*catalog, spec);
            PPR_TEST_ASSERT(replay.has_value() and replay->m_checksum == sample->m_checksum);
            PPR_TEST_ASSERT(
                replay->m_support == sample->m_support and replay->m_formation_domain == sample->m_formation_domain);
            for (u32 y = sample->m_visible.m_bottom; y < sample->m_visible.m_top; ++y) {
                for (u32 x = sample->m_visible.m_left; x < sample->m_visible.m_right; ++x) {
                    PPR_TEST_ASSERT(sample->m_grid.getCell({x, y}) == replay->m_grid.getCell({x, y}));
                }
            }
        }
        for (const sim::GlobalCellPos origin: {sim::GlobalCellPos{0u, 0u}, sim::GlobalCellPos{3840u, 3840u}}) {
            auto boundary_spec = spec;
            boundary_spec.m_origin = origin;
            const auto boundary = prepareWorldgenPreview(*catalog, boundary_spec);
            PPR_TEST_ASSERT(boundary.has_value());
            verifySample(*boundary, origin);
        }
        auto invalid_spec = spec;
        invalid_spec.m_origin.m_x = 3841u;
        const auto out_of_bounds = prepareWorldgenPreview(*catalog, invalid_spec);
        PPR_TEST_ASSERT(not out_of_bounds and out_of_bounds.error() == std::errc::result_out_of_range);
        invalid_spec = spec;
        invalid_spec.m_biome = "missing-biome";
        const auto missing_biome = prepareWorldgenPreview(*catalog, invalid_spec);
        PPR_TEST_ASSERT(not missing_biome and missing_biome.error() == std::errc::invalid_argument);
        invalid_spec = spec;
        invalid_spec.m_profile = "missing-profile";
        const auto missing_profile = prepareWorldgenPreview(*catalog, invalid_spec);
        PPR_TEST_ASSERT(not missing_profile and missing_profile.error() == std::errc::invalid_argument);
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        const auto cancelled = prepareWorldgenPreview(*catalog, spec, context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);

        OutputDirectory output;
        const auto successful = output.m_root / "complete";
        const WorldgenPreviewDisplay display{670.5, 1040.0, 1.75, 0u};
        PPR_TEST_ASSERT(not exportWorldgenPreview(*sample, *source, display, successful));
        verifyImages(*sample, successful);
        PPR_TEST_ASSERT(readBytes(successful / "source/pack.toml") == source->m_manifest_source.m_bytes);
        for (std::size_t i = 0u; i < source->m_documents.size(); ++i) {
            PPR_TEST_ASSERT(
                readBytes(successful / "source" / source->m_manifest.m_files[i]) == source->m_documents[i].m_bytes);
        }
        const auto report = readBytes(successful / "preview.json");
        PPR_TEST_ASSERT(report.find("\"semantics\":\"Biome sample\"") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"revision\":73") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"temperature_min_k\":200") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"temperature_max_k\":1000") != std::string::npos);
        PPR_TEST_ASSERT(
            report.find(std::format("\"cell_checksum\":\"{:016x}\"", sample->m_checksum)) != std::string::npos);
        const auto completion = readBytes(successful / "complete.json");
        PPR_TEST_ASSERT(completion.find("\"revision\":73") != std::string::npos);
        PPR_TEST_ASSERT(exportWorldgenPreview(*sample, *source, display, successful) == std::errc::file_exists);
        PPR_TEST_ASSERT(readBytes(successful / "complete.json") == completion);
        const auto cancelled_output = output.m_root / "cancelled";
        PPR_TEST_ASSERT(
            exportWorldgenPreview(*sample, *source, display, cancelled_output, context.get()) ==
            std::errc::operation_canceled);
        PPR_TEST_ASSERT(not std::filesystem::exists(cancelled_output / "complete.json"));
    };
}
namespace pP::game::tests {
    const UnitTest worldgen_preview = UnitTest::Named("worldgen_preview") / [](UnitTest::IRun &_) -> void {
        _.recurse({detail::worldgen_preview::bounded_samples_replay_and_export_the_exact_visible_draft});
    };
    const UnitTest &gameWorldgenPreviewTests() noexcept {
        return worldgen_preview;
    }
}
