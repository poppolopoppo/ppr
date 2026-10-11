module;
#include "pP/Macros.h"
module game.colony.worldgen;
import std;

namespace pP::colony {
    PPR_DEFINE_LOG_CATEGORY(WorldExport, info, none);
    namespace {
        constexpr u32 kPreviewEdge = 1024u;
        constexpr u32 kSamplesPerEdge = sim::kWorldEdge / kPreviewEdge;
        using Color = std::array<double, 3u>;
        Color rgb(const u32 rgba) noexcept {
            return {
                static_cast<double>((rgba >> 24u) & 255u), static_cast<double>((rgba >> 16u) & 255u),
                static_cast<double>((rgba >> 8u) & 255u)};
        }
        Color temperatureColor(const double kelvin) noexcept {
            const double value = std::clamp((kelvin - 200.0) / 800.0, 0.0, 1.0);
            return {255.0 * value, 255.0 * (1.0 - std::abs(2.0 * value - 1.0)), 255.0 * (1.0 - value)};
        }
        void add(Color &target, const Color &value, const double weight = 1.0) noexcept {
            for (u32 channel = 0u; channel < 3u; ++channel) {
                target[channel] += value[channel] * weight;
            }
        }
        std::error_code matterRow(
            const ContentCatalog &catalog, const sim::GeneratedWorld &world, const u32 y, Array<Color> &row) {
            std::ranges::fill(row, Color{});
            for (const auto &region: world.m_matter.m_regions) {
                const auto *const material = catalog.findMaterial(region.m_material);
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldExport, material ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
                auto span = std::ranges::lower_bound(region.m_spans, y, {}, &sim::InitialMatterSpan::m_y);
                for (; span != region.m_spans.end() and span->m_y == y; ++span) {
                    for (u32 x = span->m_begin_x; x < span->m_end_x; ++x) {
                        const auto area = sim::generationEmptyArea(
                            world.m_grid, {x, y}, kFineCellMetres, region.m_fill_height_m);
                        PPR_RETURN_ERROR_ON_FAIL(WorldExport, area);
                        add(row[x], rgb(material->m_color_rgba), *area / (kFineCellMetres * kFineCellMetres));
                    }
                }
            }
            return {};
        }
        std::string report(
            const ContentCatalog &catalog, const sim::GeneratedWorld &world, const WorldDescriptor &descriptor) {
            std::string result = std::format(
                "{{\n\"schema_version\":1,\"profile\":\"{}\",\"seed_hex\":\"{:016x}\",\"content_fingerprint\":\"{:016x}\","
                "\"generator_version\":{},\"cells_per_metre\":{},\"cells_per_tile\":{},\"reference_thickness_m\":{},"
                "\"terrain_checksum\":\"{:016x}\",\"output_checksum\":\"{:016x}\",\"grid_payload_bytes\":{},"
                "\"preview_edge\":{},\"temperature_min_k\":200,\"temperature_max_k\":1000,\"initial_matter_is_unsimulated_baseline\":true,"
                "\"site_count\":{},\"feature_count\":{},\"segment_count\":{},\"omitted_count\":{},\"worst_placement_attempts\":{},\n\"phase_seconds\":[",
                descriptor.m_profile, descriptor.m_seed, descriptor.m_content_fingerprint,
                descriptor.m_generator_version, descriptor.m_metric.m_cells_per_metre,
                descriptor.m_metric.m_cells_per_tile, descriptor.m_metric.m_reference_thickness_m,
                world.m_report.m_terrain_checksum, world.m_report.m_output_checksum,
                world.m_report.m_grid_payload_bytes, kPreviewEdge, world.m_layout.sites().size(),
                world.m_geometry.m_instances.size(), world.m_geometry.m_segments.size(),
                world.m_geometry.m_omitted_instances.size(), world.m_geometry.m_worst_placement_attempts);
            for (u32 phase = 0u; phase < world.m_report.m_phase_times.size(); ++phase) {
                result += std::format("{}{:.9f}", phase ? "," : "", time::seconds(world.m_report.m_phase_times[phase]));
            }
            result += "],\n\"materials\":[";
            bool first = true;
            for (const auto count: world.m_report.m_material_counts) {
                const auto *const material = catalog.findMaterial(count.m_material);
                result += std::format(
                    "{}{{\"id\":{},\"key\":\"{}\",\"cells\":{}}}", first ? "" : ",", count.m_material,
                    material ? material->m_key : "vacuum", count.m_cells);
                first = false;
            }
            result += "],\n\"biomes\":[";
            first = true;
            for (const auto &biome: catalog.biomes()) {
                const auto color = rgb(biome.m_preview_color_rgba);
                result += std::format(
                    "{}{{\"key\":\"{}\",\"rgb\":[{},{},{}]}}", first ? "" : ",", biome.m_key,
                    static_cast<u32>(color[0u]), static_cast<u32>(color[1u]), static_cast<u32>(color[2u]));
                first = false;
            }
            result += "],\n\"initial_matter\":[";
            first = true;
            for (const auto &region: world.m_matter.m_regions) {
                result += std::format(
                    "{}{{\"identity\":\"{:016x}\",\"material\":{},\"temperature_k\":{},\"bulk_density_kg_m3\":{},"
                    "\"volume_m3\":{},\"mass_kg\":{},\"fill_height_m\":{},\"span_count\":{}}}",
                    first ? "" : ",", region.m_identity, region.m_material, region.m_temperature_k,
                    region.m_bulk_density_kg_m3, region.m_volume_m3, region.m_mass_kg,
                    region.m_fill_height_m ? std::format("{}", *region.m_fill_height_m) : "null",
                    region.m_spans.size());
                first = false;
            }
            result += "]\n}\n";
            return result;
        }
        std::error_code writeText(const std::filesystem::path &path, const std::string &text) {
            std::ofstream file{path, std::ios::binary | std::ios::trunc};
            PPR_RETURN_ERROR_ON_FAIL(WorldExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            PPR_RETURN_ERROR_ON_FAIL(WorldExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            return {};
        }
    }

    std::error_code exportWorldDiagnostics(
        const ContentCatalog &catalog, const sim::GeneratedWorld &world, const WorldDescriptor &descriptor,
        const std::filesystem::path &directory, const IContext *const context) {
        PPR_RETURN_ERROR_ON_FAIL(WorldExport, context ? context->error() : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(WorldExport, validateWorldMetric(descriptor.m_metric));
        const auto machine_report = report(catalog, world, descriptor);
        PPR_RETURN_ERROR_ON_FAIL(
            WorldExport, machine_report.size() <= 1024u * 1024u ? std::error_code{}
                                                                : std::make_error_code(std::errc::value_too_large));
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        PPR_RETURN_ERROR_ON_FAIL(WorldExport, error);
        constexpr std::array<std::string_view, 4u> names{
            "material.ppm", "biome.ppm", "temperature.ppm", "initial-matter.ppm"};
        std::array<std::ofstream, 4u> files;
        for (u32 mode = 0u; mode < files.size(); ++mode) {
            files[mode].open(directory / names[mode], std::ios::binary | std::ios::trunc);
            PPR_RETURN_ERROR_ON_FAIL(
                WorldExport, files[mode] ? std::error_code{} : std::make_error_code(std::errc::io_error));
            files[mode] << "P6\n" << kPreviewEdge << ' ' << kPreviewEdge << "\n255\n";
        }
        Array<Color> material_colors(65536u);
        for (const auto &material: catalog.materials()) {
            material_colors[material.m_numeric_id] = rgb(material.m_color_rgba);
        }
        Array<Color> matter_row(sim::kWorldEdge);
        std::array<Array<Color>, 4u> rows;
        for (auto &row: rows) {
            row.resize(kPreviewEdge);
        }
        Array<u8> bytes(kPreviewEdge * 3u);
        for (u32 image_y = 0u; image_y < kPreviewEdge; ++image_y) {
            PPR_RETURN_ERROR_ON_FAIL(WorldExport, context ? context->error() : std::error_code{});
            for (auto &row: rows) {
                std::ranges::fill(row, Color{});
            }
            for (u32 dy = 0u; dy < kSamplesPerEdge; ++dy) {
                const u32 y = sim::kWorldEdge - 1u - (image_y * kSamplesPerEdge + dy);
                PPR_RETURN_ERROR_ON_FAIL(WorldExport, matterRow(catalog, world, y, matter_row));
                for (u32 x = 0u; x < sim::kWorldEdge; ++x) {
                    const u32 pixel = x / kSamplesPerEdge;
                    const auto cell = world.m_grid.getCell({x, y});
                    PPR_RETURN_ERROR_ON_FAIL(WorldExport, cell);
                    add(rows[0u][pixel], material_colors[cell->m_element]);
                    const u32 biome = sim::generationBiomeAt(
                        world.m_plan, world.m_layout, (x + 0.5) * kFineCellMetres, (y + 0.5) * kFineCellMetres);
                    add(rows[1u][pixel], rgb(catalog.biomes()[biome].m_preview_color_rgba));
                    if (cell->m_element != 0u) {
                        add(rows[2u][pixel], temperatureColor(cell->m_temperature));
                    }
                    add(rows[3u][pixel], matter_row[x]);
                }
            }
            for (u32 mode = 0u; mode < files.size(); ++mode) {
                for (u32 x = 0u; x < kPreviewEdge; ++x) {
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        bytes[3u * x + channel] = static_cast<u8>(std::clamp(
                            std::lround(rows[mode][x][channel] / (kSamplesPerEdge * kSamplesPerEdge)), 0l, 255l));
                    }
                }
                files[mode].write(
                    reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                PPR_RETURN_ERROR_ON_FAIL(
                    WorldExport, files[mode] ? std::error_code{} : std::make_error_code(std::errc::io_error));
            }
        }
        for (auto &file: files) {
            file.close();
            PPR_RETURN_ERROR_ON_FAIL(WorldExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
        }
        PPR_RETURN_ERROR_ON_FAIL(WorldExport, context ? context->error() : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(WorldExport, writeText(directory / "report.json", machine_report));
        PPR_RETURN_ERROR_ON_FAIL(
            WorldExport,
            writeText(
                directory / "reproduce.txt",
                std::format(
                    "Flat diagnostic overview; not GPU surface/texture acceptance.\nProfile: {}\nSeed: 0x{:016x}\nContent: {:016x}\n"
                    "P6 RGB, top row is +Y world top. Each pixel averages 4x4 fine-cell samples.\n"
                    "Temperature ramp clamps 200..1000 K. Initial matter is an unsimulated contour/fill-height clipped baseline.\n",
                    descriptor.m_profile, descriptor.m_seed, descriptor.m_content_fingerprint)));
        return {};
    }
}
