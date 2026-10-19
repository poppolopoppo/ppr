module;
#include "pP/Macros.h"
module game.colony.worldgen_preview;
import game.colony.worldgen;
import engine.asset;
import engine.image;
import std;

namespace pP::colony {
    // ------------------------------------------------------------------
    // preview export
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(PreviewExport, info, none)

    namespace {
        std::string quote(const std::string_view text) {
            std::string result{"\""};
            for (const char raw: text) {
                const auto byte = static_cast<unsigned char>(raw);
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

        constexpr std::ios::openmode kPreviewCreate = std::ios::binary | std::ios::noreplace;
        constexpr std::ios::openmode kPreviewOverwrite = std::ios::binary | std::ios::trunc;

        std::error_code writePreviewText(
            const std::filesystem::path &path, const std::string_view bytes,
            const std::ios::openmode mode = kPreviewCreate) {
            std::ofstream file{path, mode};
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.close();
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, file ? std::error_code{} : std::make_error_code(std::errc::io_error));
            return {};
        }

        // ------------------------------------------------------------------
        // exact owned source export
        // ------------------------------------------------------------------

        using SourceRecords = std::map<std::filesystem::path, const asset::DescriptorSource *>;

        Expected<SourceRecords> matchingSource(
            const ContentSourceSet &source, const ContentDescriptorCatalog &catalog) {
            SourceRecords imported;
            const auto collect = [&](const auto &entries) {
                for (const auto &entry: entries) {
                    imported.emplace(entry.m_descriptor.m_source.m_path, std::addressof(entry.m_descriptor.m_source));
                }
            };

            collect(catalog.elements().definitions());
            collect(catalog.textures().definitions());
            collect(catalog.materials());
            collect(catalog.fields());
            collect(catalog.biomes());
            collect(catalog.features());
            collect(catalog.rules());
            collect(catalog.worlds());
            collect(catalog.backgrounds());

            PPR_RETURN_UNEXPECTED_ON_FAIL(
                PreviewExport, not source.sameMounts(catalog.packs()) or source.m_descriptors.size() != imported.size()
                                   ? std::make_error_code(std::errc::invalid_argument)
                                   : std::error_code{});

            for (const auto &pack: catalog.packs().packs()) {
                const auto found = std::ranges::find(source.m_packs, pack.m_root, &asset::PackSource::m_root);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    PreviewExport,
                    found == source.m_packs.end() or found->m_manifest.m_bytes != pack.m_manifest.m_source.m_bytes
                        ? std::make_error_code(std::errc::invalid_argument)
                        : std::error_code{});
            }

            std::set<std::filesystem::path> seen;
            for (const auto &descriptor: source.m_descriptors) {
                const auto found = imported.find(descriptor.m_path);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    PreviewExport, found == imported.end() or
                                           not seen.insert(descriptor.m_path).second or
                                           descriptor.m_bytes != found->second->m_bytes
                                       ? std::make_error_code(std::errc::invalid_argument)
                                       : std::error_code{});
            }

            return imported;
        }

        std::error_code writeContainedSource(
            const std::filesystem::path &root, const std::filesystem::path &relative, const std::string_view bytes) {
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, relative.empty() or relative.is_absolute() or relative.has_root_name()
                                   ? std::make_error_code(std::errc::invalid_argument)
                                   : std::error_code{});

            std::error_code error;
            auto target = std::filesystem::weakly_canonical(root / relative, error);
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
            const auto contained = [&](const std::filesystem::path &path) {
                const auto suffix = path.lexically_relative(root);
                return not suffix.empty() and
                       not suffix.is_absolute() and
                       std::ranges::none_of(suffix, [](const auto &component) { return component == ".."; });
            };

            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport,
                contained(target) ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            std::filesystem::create_directories(target.parent_path(), error);
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);

            target = std::filesystem::weakly_canonical(root / relative, error);
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport,
                contained(target) ? std::error_code{} : std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writePreviewText(target, bytes));
            return {};
        }

        std::error_code writeSource(
            const ContentDescriptorCatalog &catalog, const SourceRecords &sources,
            const std::filesystem::path &directory, const IContext *const context) {
            std::string selection{"schema_version = 1\n"};
            std::size_t index{};
            for (const auto &pack: catalog.packs().packs()) {
                selection += std::format(
                    "\n[packs.pack_{}]\nroot = {}\n", index++,
                    quote("packs/pack-" + pack.m_manifest.m_value.m_pack_id));
            }

            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, selection.size() > asset::kMaxManifestBytes
                                   ? std::make_error_code(std::errc::value_too_large)
                                   : std::error_code{});
            std::error_code error;
            std::filesystem::create_directories(directory / "packs", error);
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);

            for (const auto &pack: catalog.packs().packs()) {
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
                const auto destination = directory / "packs" / ("pack-" + pack.m_manifest.m_value.m_pack_id);
                std::filesystem::create_directory(destination, error);
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
                const auto root = std::filesystem::canonical(destination, error);
                PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
                PPR_RETURN_ERROR_ON_FAIL(
                    PreviewExport, writeContainedSource(root, "pack.toml", pack.m_manifest.m_source.m_bytes));

                for (const auto &entry: catalog.packs().assets()) {
                    if (entry.m_name.m_pack != pack.m_manifest.m_value.m_pack_id) {
                        continue;
                    }

                    PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
                    const auto &declarations = pack.m_manifest.m_value.m_assets.m_entries;
                    const auto declaration = std::ranges::find_if(
                        declarations, [&](const auto &value) { return value.m_key == entry.m_name.m_local; });
                    PPR_RETURN_ERROR_ON_FAIL(
                        PreviewExport, declaration == declarations.end()
                                           ? std::make_error_code(std::errc::state_not_recoverable)
                                           : std::error_code{});
                    PPR_RETURN_ERROR_ON_FAIL(
                        PreviewExport,
                        writeContainedSource(
                            root, declaration->m_value.m_descriptor, sources.at(entry.m_descriptor)->m_bytes));
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writePreviewText(directory / "pack-set.toml", selection));
            return {};
        }
        // ------------------------------------------------------------------
        // reproduction images and metadata
        // ------------------------------------------------------------------

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
                    append(biomes, preview.m_catalog->biomes()[biome].m_preview_color_rgba);
                    const auto *const material = preview.m_catalog->findMaterial(
                        matter[(y - preview.m_visible.m_bottom) * edge + column]);
                    append(fills, material ? material->m_color_rgba : 0u);
                }
            }

            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, writePreviewText(directory / "biome.ppm", biomes, kPreviewOverwrite));
            PPR_RETURN_ERROR_ON_FAIL(
                PreviewExport, writePreviewText(directory / "initial-matter.ppm", fills, kPreviewOverwrite));
            return {};
        }

        std::string rootsJson(const ContentDescriptorCatalog &catalog) {
            std::string result{"["};
            bool first = true;
            for (const auto &pack: catalog.packs().packs()) {
                result += std::format(
                    "{}{{\"namespace\":{},\"root\":{}}}", first ? "" : ",", quote(pack.m_manifest.m_value.m_pack_id),
                    quote(pack.m_root.generic_string()));
                first = false;
            }

            return result + ']';
        }

        std::string previewReport(const WorldgenPreviewResult &preview, const WorldgenPreviewDisplay &display) {
            const auto &spec = preview.m_spec;
            const bool exact = spec.m_mode == EWorldgenPreviewMode::whole_world_crop;
            std::string result = std::format(
                "{{\n\"export_version\":2,\"semantics\":{},\"generator_version\":{},"
                "\"pack\":{},\"profile\":{},\"biome\":{},\"seed_hex\":\"{:016x}\",\"revision\":{},"
                "\"numeric_fingerprint\":\"{:016x}\",\"presentation_fingerprint\":\"{:016x}\",\"cell_checksum\":\"{:016x}\","
                "\"visible_bounds_half_open\":{},\"resident_support_bounds_half_open\":{},\"formation_domain_half_open\":{},"
                "\"filter_halo_fine\":{},\"cell_size_m\":{},\"cells_per_metre\":{},\"reference_thickness_m\":{},"
                "\"actor_half_width_m\":{},\"actor_half_height_m\":{},"
                "\"image_dimensions\":[256,256],\"image_row_order\":\"top row is highest +Y\","
                "\"temperature_min_k\":200,\"temperature_max_k\":1000,"
                "\"formation_semantics\":{},"
                "\"participating_rules\":{},\"omissions\":{},\"solid_cells\":{},\"resident_chunks\":{},"
                "\"known_numeric_retained_payload_bytes\":{},\"known_scratch_peak_bytes\":{},\"known_world_cache_payload_bytes\":{},"
                "\"memory_scope\":\"Numeric vector payloads only; crop retained bytes exclude shared world cache; scratch excludes particle seeding/validation transients, catalog/assets, allocator overhead, opaque noise allocations and stacks\","
                "\"display\":{{\"center_x_m\":{},\"center_y_m\":{},\"zoom\":{},\"view_mode\":{}}},"
                "\"assets\":{{\"binary_sources_copied\":false,\"reproduction\":\"Provide the original binary layout or reconstruct owned sources against original roots; the exported pack-set alone is not runnable\",\"original_roots\":{},\"textures\":[",
                quote(exact ? "Exact whole-world crop" : "Biome sample"), sim::kGeneratorAlgorithmVersion,
                quote(preview.m_catalog->packId()), quote(spec.m_profile), quote(spec.m_biome), spec.m_seed,
                spec.m_revision, *preview.m_catalog->contentFingerprint(), preview.m_presentation->fingerprint(),
                preview.m_checksum, boundsJson(preview.m_visible), boundsJson(preview.m_support),
                boundsJson(preview.m_formation_domain), preview.m_filter_halo_fine,
                preview.m_plan.recipe().m_cell_size_m, 1.0 / preview.m_plan.recipe().m_cell_size_m,
                preview.m_plan.recipe().m_reference_thickness_m, preview.m_plan.recipe().m_actor_half_width_m,
                preview.m_plan.recipe().m_actor_half_height_m,
                quote(
                    exact
                        ? "Production whole-world admission: first 4096 valid roots in global row order; production overlap order and backbone certification"
                        : "Global 16-cell tiles in fixed local candidate domain; first 4096 valid roots in row order; shared production validation and overlap order"),
                stringsJson(preview.m_participating_rules), stringsJson(preview.m_omissions), preview.m_solid_cells,
                preview.m_grid.residentChunkCount(), preview.m_known_retained_payload_bytes,
                preview.m_known_scratch_peak_bytes, preview.m_known_world_cache_payload_bytes, display.m_center_x_m,
                display.m_center_y_m, display.m_zoom, display.m_view_mode,
                rootsJson(*preview.m_catalog->descriptorOwner()));
            bool first = true;

            for (const auto &texture: preview.m_presentation->textures()) {
                const auto name = asset::qualifyName(texture.m_path, {});
                // Ready sidecars contain only checked qualified texture names.
                PPR_ASSERT(name.has_value());

                const auto definitions = preview.m_catalog->textureDefinitions();
                const auto definition = std::ranges::find_if(definitions, [&](const auto &entry) {
                    return asset::identityKey(*name, image::kTextureAssetKind) == asset::AssetKey{*entry.m_id};
                });
                // A ready sidecar contains exactly the selected catalog's checked closure.
                PPR_ASSERT(definition != definitions.end());
                const auto settings = image::textureSettingsFingerprint(definition->m_descriptor.m_value);

                result += std::format(
                    "{}{{\"path\":{},\"source_size\":{},\"source_fingerprint\":\"{:016x}\","
                    "\"settings_fingerprint\":\"{:016x}\",\"processor_version\":{},\"normal_map\":{}}}",
                    first ? "" : ",", quote(texture.m_path), texture.m_source_size, texture.m_source_fingerprint,
                    settings, image::TextureProcessor::processing_revision, texture.m_normal_map);
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
        const WorldgenPreviewResult &preview, const ContentSourceSet &source, const WorldgenPreviewDisplay &display,
        const std::filesystem::path &directory, const IContext *const context) {
        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
        if (not std::isfinite(display.m_center_x_m) or
            not std::isfinite(display.m_center_y_m) or
            not std::isfinite(display.m_zoom) or
            display.m_zoom <= 0.0 or
            not preview.m_catalog or
            not preview.m_catalog->descriptorOwner() or
            not preview.m_presentation or
            preview.m_presentation->catalog() != preview.m_catalog or
            preview.m_presentation_generation == asset::RequestGeneration{} or
            preview.m_presentation->generation() != preview.m_presentation_generation) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::invalid_argument);
        }

        auto sources = matchingSource(source, *preview.m_catalog->descriptorOwner());
        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, sources);

        std::error_code error;
        const bool created = std::filesystem::create_directory(directory, error);
        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, error);
        if (not created) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, std::errc::file_exists);
        }

        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport,
            exportWorldgenRegionImages(
                *preview.m_catalog, preview.m_grid, preview.m_visible, preview.m_biome_index, directory, context));

        if (preview.m_spec.m_mode == EWorldgenPreviewMode::whole_world_crop) {
            PPR_RETURN_ERROR_ON_FAIL(PreviewExport, writeExactContextImages(preview, directory, context));
        }

        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport, writeSource(*preview.m_catalog->descriptorOwner(), *sources, directory / "source", context));

        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport, writePreviewText(directory / "preview.json", previewReport(preview, display)));

        PPR_RETURN_ERROR_ON_FAIL(PreviewExport, context ? context->error() : std::error_code{});
        PPR_RETURN_ERROR_ON_FAIL(
            PreviewExport, writePreviewText(
                               directory / "complete.json",
                               std::format(
                                   "{{\"export_version\":2,\"revision\":{},\"cell_checksum\":\"{:016x}\"}}\n",
                                   preview.m_spec.m_revision, preview.m_checksum)));
        return {};
    }
}
