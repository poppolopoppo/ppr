module;
#include "pP/Macros.h"
module game.colony.worldgen_preview;
import game.colony.worldgen;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // preview export
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(PreviewExport, info, none);
    namespace {
        std::string quote(const std::string_view text) {
            std::string result{"\""};
            for (const unsigned char byte: text) {
                if (byte == '"' or byte == '\\') {
                    result += '\\';
                    result += static_cast<char>(byte);
                } else if (byte < 32u) {
                    result += std::format("\\u{:04x}", byte);
                } else {
                    result += static_cast<char>(byte);
                }
            }
            return result + '"';
        }
        std::string boundsJson(const sim::GenerationCellBounds bounds) {
            return std::format("[{},{},{},{}]", bounds.m_left, bounds.m_bottom, bounds.m_right, bounds.m_top);
        }
        std::string stringsJson(const Array<std::string> &strings) {
            std::string result{"["};
            for (std::size_t i = 0u; i < strings.size(); ++i) {
                if (i != 0u) {
                    result += ',';
                }
                result += quote(strings[i]);
            }
            return result + ']';
        }
        std::error_code writePreviewText(const std::filesystem::path &path, const std::string_view bytes) {
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.close();
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            return {};
        }
        std::error_code writeSource(
            const ContentPackSource &source, const std::filesystem::path &directory, const IContext *const context) {
            std::error_code error;
            std::filesystem::create_directory(directory, error);
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, writePreviewText(directory / "pack.toml", source.m_manifest_source.m_bytes));

            for (std::size_t i = 0u; i < source.m_documents.size(); ++i) {
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
                const std::filesystem::path relative{source.m_manifest.m_files[i]};
                if (relative.empty() or relative.is_absolute() or relative.has_root_name()) {
                    PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::invalid_argument);
                }
                for (const auto &component: relative) {
                    if (component == "..") {
                        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::invalid_argument);
                    }
                }
                const auto target = directory / relative;
                std::filesystem::create_directories(target.parent_path(), error);
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writePreviewText(target, source.m_documents[i].m_bytes));
            }
            return {};
        }
        std::error_code writeExactContextImages(
            const WorldgenPreviewResult &preview, const std::filesystem::path &directory,
            const IContext *const context) {
            constexpr u32 edge = kWorldgenPreviewEdge;
            Array<u16> matter(edge * edge, 0u);

            for (const auto &region: preview.m_matter.m_regions) {
                for (const auto span: region.m_spans) {
                    for (u32 x = span.m_begin_x; x < span.m_end_x; ++x) {
                        matter[(span.m_y - preview.m_visible.m_bottom) * edge + x - preview.m_visible.m_left] =
                            region.m_material;
                    }
                }
            }

            std::string biomes{"P6\n256 256\n255\n"}, fills{biomes};
            biomes.reserve(biomes.size() + edge * edge * 3u);
            fills.reserve(fills.size() + edge * edge * 3u);
            const auto append = [](std::string &bytes, const u32 rgba) {
                for (const u32 shift: {24u, 16u, 8u}) {
                    bytes += static_cast<char>((rgba >> shift) & 255u);
                }
            };

            for (u32 row = 0u; row < edge; ++row) {
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
                const u32 y = preview.m_visible.m_top - 1u - row;
                for (u32 column = 0u; column < edge; ++column) {
                    const u32 x = preview.m_visible.m_left + column;
                    const u32 biome = sim::generationBiomeAt(
                        preview.m_plan, preview.m_layout, (x + 0.5) * preview.m_plan.recipe().m_cell_size_m,
                        (y + 0.5) * preview.m_plan.recipe().m_cell_size_m);
                    append(biomes, preview.m_catalog.biomes()[biome].m_preview_color_rgba);
                    const auto *const material = preview.m_catalog.findMaterial(
                        matter[(y - preview.m_visible.m_bottom) * edge + column]);
                    append(fills, material ? material->m_color_rgba : 0u);
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writePreviewText(directory / "biome.ppm", biomes));
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writePreviewText(directory / "initial-matter.ppm", fills));
            return {};
        }
        std::string previewReport(
            const WorldgenPreviewResult &preview, const ContentPackSource &source,
            const WorldgenPreviewDisplay &display) {
            const auto &spec = preview.m_spec;
            const bool exact = spec.m_mode == EWorldgenPreviewMode::whole_world_crop;
            std::string result = std::format(
                "{{\n\"export_version\":1,\"semantics\":{},\"generator_version\":{},"
                "\"pack\":{},\"profile\":{},\"biome\":{},\"seed_hex\":\"{:016x}\",\"revision\":{},"
                "\"content_fingerprint\":\"{:016x}\",\"cell_checksum\":\"{:016x}\","
                "\"visible_bounds_half_open\":{},\"resident_support_bounds_half_open\":{},\"formation_domain_half_open\":{},"
                "\"filter_halo_fine\":{},\"cells_per_metre\":{},\"reference_thickness_m\":1,"
                "\"image_dimensions\":[256,256],\"image_row_order\":\"top row is highest +Y\","
                "\"temperature_min_k\":200,\"temperature_max_k\":1000,"
                "\"formation_semantics\":{},"
                "\"participating_rules\":{},\"omissions\":{},\"solid_cells\":{},\"resident_chunks\":{},"
                "\"known_numeric_retained_payload_bytes\":{},\"known_scratch_peak_bytes\":{},\"known_world_cache_payload_bytes\":{},"
                "\"memory_scope\":\"Numeric vector payloads only; crop retained bytes exclude shared world cache; scratch excludes particle seeding/validation transients, catalog/assets, allocator overhead, opaque noise allocations and stacks\","
                "\"display\":{{\"center_x_m\":{},\"center_y_m\":{},\"zoom\":{},\"view_mode\":{}}},"
                "\"assets\":{{\"requires_original_unchanged_root\":true,\"original_root\":{},\"textures\":[",
                quote(exact ? "Exact whole-world crop" : "Biome sample"), sim::kGeneratorAlgorithmVersion,
                quote(preview.m_catalog.packId()), quote(spec.m_profile), quote(spec.m_biome), spec.m_seed,
                spec.m_revision, *preview.m_catalog.contentFingerprint(), preview.m_checksum,
                boundsJson(preview.m_visible), boundsJson(preview.m_support), boundsJson(preview.m_formation_domain),
                preview.m_filter_halo_fine, kFineCellsPerMetre,
                quote(
                    exact
                        ? "Production whole-world admission: first 4096 valid roots in global row order; production overlap order and backbone certification"
                        : "Global 16-cell tiles in fixed local candidate domain; first 4096 valid roots in row order; shared production validation and overlap order"),
                stringsJson(preview.m_participating_rules), stringsJson(preview.m_omissions), preview.m_solid_cells,
                preview.m_grid.residentChunkCount(), preview.m_known_retained_payload_bytes,
                preview.m_known_scratch_peak_bytes, preview.m_known_world_cache_payload_bytes, display.m_center_x_m,
                display.m_center_y_m, display.m_zoom, display.m_view_mode,
                quote(source.m_manifest_source.m_path.parent_path().generic_string()));
            bool first = true;

            for (const auto &texture: preview.m_catalog.textures()) {
                result += std::format(
                    "{}{{\"path\":{},\"source_size\":{},\"source_fingerprint\":\"{:016x}\",\"normal_map\":{}}}",
                    first ? "" : ",", quote(texture.m_path), texture.m_source_size, texture.m_source_fingerprint,
                    texture.m_normal_map);
                first = false;
            }

            result += "]},\"materials\":[";
            first = true;

            for (const auto count: preview.m_material_counts) {
                result += std::format(
                    "{}{{\"id\":{},\"cells\":{}}}", first ? "" : ",", count.m_material, count.m_cells);
                first = false;
            }

            result += "],\"initial_matter_regions\":[";
            first = true;

            for (const auto &region: preview.m_matter.m_regions) {
                result += std::format(
                    "{}{{\"identity\":\"{:016x}\",\"material\":{},\"temperature_k\":{},\"bulk_density_kg_m3\":{},\"crop_volume_m3\":{},\"crop_mass_kg\":{},\"spans\":[",
                    first ? "" : ",", region.m_identity, region.m_material, region.m_temperature_k,
                    region.m_bulk_density_kg_m3, region.m_volume_m3, region.m_mass_kg);
                bool first_span = true;
                for (const auto span: region.m_spans) {
                    result += std::format(
                        "{}[{},{},{}]", first_span ? "" : ",", span.m_y, span.m_begin_x, span.m_end_x);
                    first_span = false;
                }
                result += "]}";
                first = false;
            }

            result += ']';

            if (preview.m_exact_world) {
                const auto &world = preview.m_exact_world->m_world;
                result += std::format(
                    ",\"whole_world_preparation\":{{\"resident_chunks\":{},\"terrain_checksum\":\"{:016x}\",\"output_checksum\":\"{:016x}\",\"phase_seconds\":[",
                    world.m_grid.residentChunkCount(), world.m_report.m_terrain_checksum,
                    world.m_report.m_output_checksum);
                for (std::size_t phase = 0u; phase < world.m_report.m_phase_times.size(); ++phase) {
                    result += std::format(
                        "{}{}", phase == 0u ? "" : ",", time::seconds(world.m_report.m_phase_times[phase]));
                }
                result += "]}";
            }
            return result + "\n}\n";
        }
    }

    // ------------------------------------------------------------------
    // preview export entry point
    // ------------------------------------------------------------------

    std::error_code exportWorldgenPreview(
        const WorldgenPreviewResult &preview, const ContentPackSource &source, const WorldgenPreviewDisplay &display,
        const std::filesystem::path &directory, const IContext *const context) {
        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
        if (not std::isfinite(display.m_center_x_m) or
            not std::isfinite(display.m_center_y_m) or
            not std::isfinite(display.m_zoom) or
            display.m_zoom <= 0.0 or
            source.m_manifest.m_pack_id != preview.m_catalog.packId() or
            source.m_manifest_source.m_bytes.empty()) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::invalid_argument);
        }

        std::error_code error;
        const bool created = std::filesystem::create_directory(directory, error);
        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
        if (not created) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::file_exists);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport,
            exportWorldgenRegionImages(
                preview.m_catalog, preview.m_grid, preview.m_visible, preview.m_biome_index, directory, context));

        if (preview.m_spec.m_mode == EWorldgenPreviewMode::whole_world_crop) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writeExactContextImages(preview, directory, context));
        }

        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writeSource(source, directory / "source", context));

        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport, writePreviewText(directory / "preview.json", previewReport(preview, source, display)));

        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport, writePreviewText(
                               directory / "complete.json",
                               std::format(
                                   "{{\"export_version\":1,\"revision\":{},\"cell_checksum\":\"{:016x}\"}}\n",
                                   preview.m_spec.m_revision, preview.m_checksum)));
        return {};
    }
}
