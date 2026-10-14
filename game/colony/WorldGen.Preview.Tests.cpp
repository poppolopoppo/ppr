module;
#include "pP/UnitTest.h"
#include "pP/Macros.h"
module game.tests;
import engine.core;
import engine.sim;
import game.colony.content;
import game.colony.worldgen_preview;
import game.colony.worldgen_panel;
import imgui;
import imgui_internal;
import std;

namespace pP::game::tests::detail::worldgen_preview {
    // ------------------------------------------------------------------
    // world generation previews
    // ------------------------------------------------------------------

    using namespace colony;
    PPR_UNIT_TEST (workshop_widgets_block_invalid_actions_and_focus_nested_property_errors) {
        auto *const previous = ImGui::GetCurrentContext();
        auto *const context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        PPR_DEFER {
            ImGui::DestroyContext(context);
            ImGui::SetCurrentContext(previous);
        };
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1400.0f, 1000.0f};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char *pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        WorldgenPanelState state;
        WorldgenPanelView view;
        const auto frame = [&](const ImVec2 position, const bool down) {
            io.AddMousePosEvent(position.x, position.y);
            io.AddMouseButtonEvent(0, down);
            ImGui::NewFrame();
            auto intents = drawWorldgenPanel(state, view);
            ImGui::Render();
            return intents;
        };
        const auto emitted = [](const Array<WorkshopIntent> &intents, const EWorkshopIntent kind) {
            return std::ranges::any_of(intents, [&](const WorkshopIntent &intent) { return intent.m_kind == kind; });
        };
        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        auto *const window = ImGui::FindWindowByName("Worldgen Workshop");
        PPR_TEST_ASSERT(window != nullptr);
        const auto &style = ImGui::GetStyle();
        const ImVec2 open_position{
            window->WorkRect.Max.x -
                240.0f +
                style.ItemSpacing.x +
                style.FramePadding.x +
                ImGui::CalcTextSize("Open pack").x * 0.5f,
            window->WorkRect.Min.y + ImGui::GetFrameHeight() * 0.5f};
        PPR_TEST_ASSERT(not emitted(frame(open_position, true), EWorkshopIntent::open));
        PPR_TEST_ASSERT(not emitted(frame(open_position, false), EWorkshopIntent::open));
        std::snprintf(state.m_root.data(), state.m_root.size(), "%s", "authored-pack");
        static_cast<void>(frame(open_position, true));
        PPR_TEST_ASSERT(emitted(frame(open_position, false), EWorkshopIntent::open));

        ContentPackSource source;
        source.m_documents[1u].m_bytes = "schema_version = 3\n[biomes.hearth]\ntemperature.base_k = 293.0\n"
                                         "[biomes.hearth.composition.band]\nbed_period_m = 4.0\n";
        view.m_source = &source;
        view.m_revision = 1u;
        view.m_busy = true;
        view.m_dirty = true;
        const ImVec2 save_position{
            window->WorkRect.Min.x + style.FramePadding.x + ImGui::CalcTextSize("Save documents").x * 0.5f,
            window->WorkRect.Min.y + ImGui::GetFrameHeight() * 1.5f + style.ItemSpacing.y};
        PPR_TEST_ASSERT(not emitted(frame(save_position, true), EWorkshopIntent::save));
        PPR_TEST_ASSERT(not emitted(frame(save_position, false), EWorkshopIntent::save));
        view.m_busy = false;
        static_cast<void>(frame(save_position, true));
        PPR_TEST_ASSERT(emitted(frame(save_position, false), EWorkshopIntent::save));

        const config::Diagnostic diagnostic{
            "biomes.toml:5", "biomes.hearth.composition.band.bed_period_m", {}, "Expected a positive period",
            std::make_error_code(std::errc::result_out_of_range)};
        view.m_property_diagnostics = std::span{&diagnostic, 1u};
        view.m_diagnostics_revision = 1u;
        state.m_selected_key = "hearth";
        state.m_focus_property = diagnostic.m_property;
        std::snprintf(state.m_property_search.data(), state.m_property_search.size(), "%s", "bed_period_m");
        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        PPR_TEST_ASSERT(not state.m_property_tree.empty() and state.m_focus_property.empty());
        PPR_TEST_ASSERT(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ImGui::ClosePopupToLevel(0, false);
        std::snprintf(state.m_property_search.data(), state.m_property_search.size(), "%s", "temperature.base_k");
        static_cast<void>(frame(ImVec2{-1.0f, -1.0f}, false));
        ImGuiWindow *inspector_window{};
        for (auto *candidate: context->Windows) {
            if (std::string_view{candidate->Name}.find("Definition inspector") != std::string_view::npos) {
                inspector_window = candidate;
                break;
            }
        }
        PPR_TEST_ASSERT(inspector_window != nullptr);
        // The tab contributes an ID scope, so find the drawn table through its
        // actual owner rather than reconstructing the tab's internal ID stack.
        const ImGuiTable *table{};
        for (int index = 0; index < context->Tables.GetBufSize(); ++index) {
            const auto *const candidate = context->Tables.GetByIndex(index);
            if (candidate->OuterWindow == inspector_window and candidate->ColumnsCount == 2) {
                table = candidate;
                break;
            }
        }
        PPR_TEST_ASSERT(table != nullptr);
        const float left = table->Columns[1u].WorkMinX;
        const float slider_width = table->Columns[1u].WorkMaxX - left - 38.0f;
        const float slider_y = table->RowPosY1 + ImGui::GetFrameHeight() * 0.5f;
        PPR_TEST_ASSERT(
            not emitted(frame(ImVec2{left + slider_width * 0.3f, slider_y}, true), EWorkshopIntent::edit_definition));
        PPR_TEST_ASSERT(
            not emitted(frame(ImVec2{left + slider_width * 0.7f, slider_y}, true), EWorkshopIntent::edit_definition));
        PPR_TEST_ASSERT(state.m_numeric_edit_key == "biomes.hearth.temperature.base_k");
        const auto dragged = state.m_numeric_edit.m_floating;
        const auto released = frame(ImVec2{left + slider_width * 0.7f, slider_y}, false);
        const auto edit = std::ranges::find(released, EWorkshopIntent::edit_definition, &WorkshopIntent::m_kind);
        PPR_TEST_ASSERT(
            edit != released.end() and
            edit->m_property == "temperature.base_k" and
            edit->m_value.m_floating == dragged);
        PPR_TEST_ASSERT(state.m_numeric_edit_key.empty());
    };
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
        auto &biomes = source->m_documents[static_cast<std::size_t>(EContentDocument::biomes)];
        const auto patch = config::patchScalar(
            biomes.m_bytes, "biomes.hearth.formation.length_m", config::FileValue::floating(8.0),
            biomes.m_path.string(), diagnostics);
        PPR_TEST_ASSERT(patch.has_value());
        auto edited = config::applySourcePatch(biomes.m_bytes, *patch);
        PPR_TEST_ASSERT(edited.has_value());
        biomes.m_bytes = std::move(*edited);
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

    PPR_UNIT_TEST (whole_world_crops_preserve_production_formations_seams_and_matter) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};
            TimePoint now() const noexcept override {
                ++m_calls;
                return time::now();
            }
        } clock;
        const auto root = hal::process::currentExecutablePath().parent_path() / "worldgen/default";
        config::Diagnostics diagnostics;
        auto source = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(source.has_value());
        // Exercise the maximum supported formation probability and length across
        // tile/chunk seams, retaining every authored production terrain phase.
        auto &bytes = source->m_documents[static_cast<std::size_t>(EContentDocument::biomes)].m_bytes;
        for (const std::string_view key: {"formation_probability = ", "formation_length_m = "}) {
            std::size_t offset{};
            while ((offset = bytes.find(key, offset)) != std::string::npos) {
                const auto begin = offset + key.size(), end = bytes.find_first_of("\r\n", begin);
                PPR_TEST_ASSERT(end != std::string::npos);
                const std::string_view value = key == "formation_probability = " ? "0.2" : "8.0";
                bytes.replace(begin, end - begin, value);
                offset = begin + value.size();
            }
        }
        const auto catalog = loadContentCatalog(root, *source, diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        WorldgenPreviewSpec spec{
            "frontier", "ignored-in-world-mode", 42u, 9u, {127u, 127u}, EWorldgenPreviewMode::whole_world_crop};
        const auto missing = prepareWorldgenPreview(*catalog, spec);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::invalid_argument);
        ParallelScheduler generation_pool{
            "Preview generation test", std::max(1u, std::min(8u, std::thread::hardware_concurrency())),
            4u * static_cast<std::size_t>(hal::page_granularity)};
        const auto prepared = prepareWorldgenPreviewWorld(*catalog, spec, clock, nullptr, &generation_pool);
        PPR_TEST_ASSERT(prepared.has_value() and clock.m_calls > 0u);
        const auto &world = (*prepared)->m_world;
        for (std::size_t phase = 0u; phase < world.m_report.m_phase_times.size(); ++phase) {
            std::println(
                "Workshop exact production phase {}: {:.3f}s", phase,
                time::seconds(world.m_report.m_phase_times[phase]));
        }
        std::println(
            "Workshop exact production known cache payload: {} bytes", (*prepared)->m_known_retained_payload_bytes);
        PPR_TEST_ASSERT(world.m_grid.residentChunkCount() == sim::kChunkCount);
        PPR_TEST_ASSERT(world.m_report.m_output_checksum != 0u);
        PPR_TEST_ASSERT((*prepared)->matches(*catalog, spec));
        auto raster = sim::compileGenerationRaster(world.m_plan, world.m_layout, world.m_geometry);
        PPR_TEST_ASSERT(raster.has_value());
        std::optional<sim::GlobalCellPos> formation;
        for (u32 cy = 0u; cy < sim::kChunksPerEdge and not formation; ++cy) {
            for (u32 cx = 0u; cx < sim::kChunksPerEdge and not formation; ++cx) {
                const auto final = world.m_grid.residentCells({cx, cy});
                PPR_TEST_ASSERT(final.has_value());
                const bool candidate = std::ranges::any_of(*final, [&](const sim::Cell cell) {
                    return cell.m_element != 0u and
                           std::ranges::any_of(world.m_plan.recipe().m_biomes, [&](const auto &biome) {
                               return cell.m_element == biome.m_formation_material or
                                      cell.m_element == biome.m_rind_material;
                           });
                });
                if (not candidate) {
                    continue;
                }
                const auto before = raster->chunk({cx, cy});
                PPR_TEST_ASSERT(before.has_value());
                for (u32 index = 0u; index < sim::kCellsPerChunk; ++index) {
                    if ((*before)[index] != (*final)[index]) {
                        formation = sim::GlobalCellPos{
                            cx * sim::kChunkEdge + index % sim::kChunkEdge,
                            cy * sim::kChunkEdge + index / sim::kChunkEdge};
                        break;
                    }
                }
            }
        }
        PPR_TEST_ASSERT(formation.has_value());
        const auto cropOrigin = [](const sim::GlobalCellPos center) {
            return sim::GlobalCellPos{
                std::min(center.m_x > 127u ? center.m_x - 127u : 0u, sim::kWorldEdge - kWorldgenPreviewEdge),
                std::min(center.m_y > 127u ? center.m_y - 127u : 0u, sim::kWorldEdge - kWorldgenPreviewEdge)};
        };
        PPR_TEST_ASSERT(not world.m_matter.m_regions.empty());
        const auto &matter_region = world.m_matter.m_regions.front();
        PPR_TEST_ASSERT(not matter_region.m_spans.empty());
        const auto matter_span = matter_region.m_spans.front();
        const sim::GlobalCellPos matter_cell{matter_span.m_begin_x, matter_span.m_y};
        const auto matter_origin = cropOrigin(matter_cell);
        const auto matter_batch = std::ranges::find(
            (*prepared)->m_particles.m_chunks, sim::chunkOf(matter_cell), &sim::MatterParticleBatch::m_position);
        PPR_TEST_ASSERT(matter_batch != (*prepared)->m_particles.m_chunks.end());
        PPR_TEST_ASSERT(not matter_batch->records().empty());
        const auto matter_particle = std::ranges::find_if(matter_batch->records(), [&](const auto &particle) {
            return particle.m_x_m == (matter_cell.m_x + 0.5) * world.m_plan.recipe().m_cell_size_m and
                   particle.m_y_m == (matter_cell.m_y + 0.5) * world.m_plan.recipe().m_cell_size_m;
        });
        PPR_TEST_ASSERT(matter_particle != matter_batch->records().end());
        for (const auto origin:
            {spec.m_origin, sim::GlobalCellPos{3840u, 3840u}, cropOrigin(*formation),
                cropOrigin({world.m_plan.recipe().m_start_x_fine, world.m_plan.recipe().m_start_y_fine}),
                matter_origin}) {
            spec.m_origin = origin;
            ++spec.m_revision;
            const u32 clock_calls = clock.m_calls;
            const auto crop = prepareWorldgenPreview(*catalog, spec, nullptr, *prepared);
            PPR_TEST_ASSERT(crop.has_value() and clock.m_calls == clock_calls);
            PPR_TEST_ASSERT(crop->m_spec == spec and crop->m_exact_world == *prepared);
            PPR_TEST_ASSERT(crop->m_grid.residentChunkCount() <= 16u);
            PPR_TEST_ASSERT(std::addressof(crop->geometry()) == std::addressof(world.m_geometry));
            PPR_TEST_ASSERT(not crop->m_layout.sampleBiome());
            u64 cell_count{};
            for (const auto count: crop->m_material_counts) {
                cell_count += count.m_cells;
            }
            PPR_TEST_ASSERT(cell_count == 256u * 256u);
            PPR_TEST_ASSERT(crop->m_known_world_cache_payload_bytes >= world.m_report.m_retained_known_payload_bytes);
            for (u32 y = crop->m_visible.m_bottom; y < crop->m_visible.m_top; ++y) {
                for (u32 x = crop->m_visible.m_left; x < crop->m_visible.m_right; ++x) {
                    PPR_TEST_ASSERT(crop->m_grid.getCell({x, y}) == world.m_grid.getCell({x, y}));
                    const double xm = (x + 0.5) * world.m_plan.recipe().m_cell_size_m,
                                 ym = (y + 0.5) * world.m_plan.recipe().m_cell_size_m;
                    PPR_TEST_ASSERT(
                        sim::generationBiomeAt(crop->m_plan, crop->m_layout, xm, ym) ==
                        sim::generationBiomeAt(world.m_plan, world.m_layout, xm, ym));
                }
            }
            for (const auto &batch: crop->m_particles.m_chunks) {
                const auto expected = std::ranges::find(
                    (*prepared)->m_particles.m_chunks, batch.m_position, &sim::MatterParticleBatch::m_position);
                PPR_TEST_ASSERT(expected != (*prepared)->m_particles.m_chunks.end());
                PPR_TEST_ASSERT(expected->m_particles == batch.m_particles);
            }
            for (const auto &region: crop->m_matter.m_regions) {
                const auto expected = std::ranges::find(
                    world.m_matter.m_regions, region.m_identity, &sim::InitialMatterRegion::m_identity);
                PPR_TEST_ASSERT(expected != world.m_matter.m_regions.end());
                PPR_TEST_ASSERT(region.m_material == expected->m_material and region.m_mass_kg > 0.0);
                for (const auto span: region.m_spans) {
                    PPR_TEST_ASSERT(
                        span.m_y >= crop->m_visible.m_bottom and
                        span.m_y < crop->m_visible.m_top and
                        span.m_begin_x >= crop->m_visible.m_left and
                        span.m_end_x <= crop->m_visible.m_right);
                }
            }
            if (origin == matter_origin) {
                PPR_TEST_ASSERT(not crop->m_matter.m_regions.empty() and not crop->m_particles.m_chunks.empty());
                const auto clipped_region = std::ranges::find(
                    crop->m_matter.m_regions, matter_region.m_identity, &sim::InitialMatterRegion::m_identity);
                PPR_TEST_ASSERT(clipped_region != crop->m_matter.m_regions.end());
                PPR_TEST_ASSERT(std::ranges::any_of(clipped_region->m_spans, [&](const auto span) {
                    return span.m_y == matter_cell.m_y and
                           span.m_begin_x <= matter_cell.m_x and
                           matter_cell.m_x < span.m_end_x;
                }));
                const auto clipped_batch = std::ranges::find(
                    crop->m_particles.m_chunks, matter_batch->m_position, &sim::MatterParticleBatch::m_position);
                PPR_TEST_ASSERT(clipped_batch != crop->m_particles.m_chunks.end());
                const auto clipped_particle = std::ranges::find(
                    clipped_batch->records(), matter_particle->m_id, &sim::MatterParticle::m_id);
                PPR_TEST_ASSERT(clipped_particle != clipped_batch->records().end());
                PPR_TEST_ASSERT(*clipped_particle == *matter_particle);
            }
        }
        auto mismatch = spec;
        ++mismatch.m_seed;
        const auto rejected = prepareWorldgenPreview(*catalog, mismatch, nullptr, *prepared);
        PPR_TEST_ASSERT(not rejected and rejected.error() == std::errc::invalid_argument);
        const auto [context, cancel] = context::withCancel(context::background());
        cancel();
        const u32 clock_calls = clock.m_calls;
        const auto cancelled_world = prepareWorldgenPreviewWorld(*catalog, spec, clock, context.get());
        PPR_TEST_ASSERT(not cancelled_world and cancelled_world.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(clock.m_calls == clock_calls);
        const auto cancelled_crop = prepareWorldgenPreview(*catalog, spec, context.get(), *prepared);
        PPR_TEST_ASSERT(not cancelled_crop and cancelled_crop.error() == std::errc::operation_canceled);

        spec.m_origin = cropOrigin(*formation);
        const auto crop = prepareWorldgenPreview(*catalog, spec, nullptr, *prepared);
        PPR_TEST_ASSERT(crop.has_value());
        OutputDirectory output;
        const auto directory = output.m_root / "exact";
        PPR_TEST_ASSERT(not exportWorldgenPreview(*crop, *source, {}, directory));
        const auto report = readBytes(directory / "preview.json");
        PPR_TEST_ASSERT(report.find("\"semantics\":\"Exact whole-world crop\"") != std::string::npos);
        PPR_TEST_ASSERT(report.find("\"whole_world_preparation\":") != std::string::npos);
        constexpr std::string_view header{"P6\n256 256\n255\n"};
        const auto biomes = readBytes(directory / "biome.ppm");
        const auto matter = readBytes(directory / "initial-matter.ppm");
        PPR_TEST_ASSERT(biomes.starts_with(header) and biomes.size() == header.size() + 256u * 256u * 3u);
        PPR_TEST_ASSERT(matter.starts_with(header) and matter.size() == biomes.size());
        for (u32 row = 0u; row < 256u; ++row) {
            for (u32 column = 0u; column < 256u; ++column) {
                const u32 x = crop->m_visible.m_left + column, y = crop->m_visible.m_top - 1u - row;
                const u32 biome = sim::generationBiomeAt(
                    world.m_plan, world.m_layout, (x + 0.5) * world.m_plan.recipe().m_cell_size_m,
                    (y + 0.5) * world.m_plan.recipe().m_cell_size_m);
                const u32 rgba = catalog->biomes()[biome].m_preview_color_rgba;
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    PPR_TEST_ASSERT(
                        static_cast<u8>(biomes[header.size() + (row * 256u + column) * 3u + channel]) ==
                        ((rgba >> (24u - channel * 8u)) & 255u));
                }
            }
        }
        PPR_TEST_ASSERT(std::filesystem::exists(directory / "complete.json"));
    };
}

namespace pP::game::tests {
    const UnitTest worldgen_preview = UnitTest::Named("worldgen_preview") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::worldgen_preview::bounded_samples_replay_and_export_the_exact_visible_draft,
                detail::worldgen_preview::workshop_widgets_block_invalid_actions_and_focus_nested_property_errors,
                detail::worldgen_preview::whole_world_crops_preserve_production_formations_seams_and_matter});
    };
    const UnitTest &gameWorldgenPreviewTests() noexcept {
        return worldgen_preview;
    }
}
