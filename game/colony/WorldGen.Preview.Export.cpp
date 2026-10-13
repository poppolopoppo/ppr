module;
#include "pP/Macros.h"
module game.colony.worldgen_preview;
import game.colony.worldgen;
import std;

namespace pP::colony {
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
        std::string previewReport(
            const WorldgenPreviewResult &preview, const ContentPackSource &source,
            const WorldgenPreviewDisplay &display) {
            const auto &spec = preview.m_spec;
            std::string result = std::format(
                "{{\n\"export_version\":1,\"semantics\":\"Biome sample\",\"generator_version\":{},"
                "\"pack\":{},\"profile\":{},\"biome\":{},\"seed_hex\":\"{:016x}\",\"revision\":{},"
                "\"content_fingerprint\":\"{:016x}\",\"cell_checksum\":\"{:016x}\","
                "\"visible_bounds_half_open\":{},\"resident_support_bounds_half_open\":{},\"formation_domain_half_open\":{},"
                "\"filter_halo_fine\":{},\"cells_per_metre\":{},\"reference_thickness_m\":1,"
                "\"image_dimensions\":[256,256],\"image_row_order\":\"top row is highest +Y\","
                "\"temperature_min_k\":200,\"temperature_max_k\":1000,"
                "\"formation_semantics\":\"Global 16-cell tiles in fixed local candidate domain; first 4096 valid roots in row order; shared production validation and overlap order\","
                "\"participating_rules\":{},\"omissions\":{},\"solid_cells\":{},\"resident_chunks\":{},"
                "\"known_numeric_retained_payload_bytes\":{},\"known_scratch_peak_bytes\":{},"
                "\"memory_scope\":\"Numeric vector payloads only; excludes catalog/assets, allocator overhead, opaque noise allocations and stacks\","
                "\"display\":{{\"center_x_m\":{},\"center_y_m\":{},\"zoom\":{},\"view_mode\":{}}},"
                "\"assets\":{{\"requires_original_unchanged_root\":true,\"original_root\":{},\"textures\":[",
                sim::kGeneratorAlgorithmVersion, quote(preview.m_catalog.packId()), quote(spec.m_profile),
                quote(spec.m_biome), spec.m_seed, spec.m_revision, *preview.m_catalog.contentFingerprint(),
                preview.m_checksum, boundsJson(preview.m_visible), boundsJson(preview.m_support),
                boundsJson(preview.m_formation_domain), preview.m_filter_halo_fine, kFineCellsPerMetre,
                stringsJson(preview.m_participating_rules), stringsJson(preview.m_omissions), preview.m_solid_cells,
                preview.m_grid.residentChunkCount(), preview.m_known_retained_payload_bytes,
                preview.m_known_scratch_peak_bytes, display.m_center_x_m, display.m_center_y_m, display.m_zoom,
                display.m_view_mode, quote(source.m_manifest_source.m_path.parent_path().generic_string()));
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
            return result + "]\n}\n";
        }
    }
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
