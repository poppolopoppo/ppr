module;
#include "pP/UnitTest.h"
module game.tests;
import engine.core;
import game.colony.content;
import game.colony.worldgen;
import game.colony.digtool;
import game.colony.buildings;
import game.colony.agents;
import game.colony.pathfinding;
import engine.sim;
import std;

namespace pP::game::tests::detail::pack {
    using namespace colony;
    struct Fixture {
        std::filesystem::path m_root;
        Fixture() {
            const auto executable = hal::process::currentExecutablePath();
            static std::atomic<u32> ordinal{};
            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                m_root = executable.parent_path() / ("worldgen-pack-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(m_root)) {
                    break;
                }
                m_root.clear();
            }
            PPR_TEST_ASSERT(not m_root.empty());
            std::filesystem::copy(
                executable.parent_path() / "worldgen/default", m_root,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        }
        ~Fixture() {
            // Best-effort test-fixture cleanup; a failed removal cannot affect catalog ownership.
            std::error_code error{};
            std::filesystem::remove_all(m_root, error);
        }
        std::string read(const std::string_view name) const {
            std::ifstream file{m_root / name, std::ios::binary};
            PPR_TEST_ASSERT(file.good());
            return {std::istreambuf_iterator<char>{file}, {}};
        }
        void write(const std::string_view name, const std::string_view bytes) const {
            std::ofstream file{m_root / name, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(file.good());
        }
        Expected<ContentCatalog> load(config::Diagnostics &diagnostics) const {
            return loadContentCatalog(m_root, diagnostics);
        }
    };
    void replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto at = text.find(before);
        PPR_TEST_ASSERT(at != std::string::npos);
        text.replace(at, before.size(), after);
    }
    std::string reorder(const std::string &text) {
        Array<std::string_view> tables{};
        const auto first = text.find('\n' + std::string{"["});
        PPR_TEST_ASSERT(first != std::string::npos);
        auto at = first;
        while (at != std::string::npos) {
            const auto next = text.find("\n[", at + 1u);
            tables.push_back(std::string_view{text}.substr(at, next == std::string::npos ? next : next - at));
            at = next;
        }
        std::string result = "# Reordered without changing resolved values\n" + text.substr(0u, first);
        for (const auto table: tables | std::views::reverse) {
            result.append(table);
            // Splitting at LF may leave a Windows CR at the end of a block.
            result.push_back('\n');
        }
        return result;
    }

    PPR_UNIT_TEST (catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        PPR_TEST_ASSERT(not grid.setCell({127u, 20u}, {16u, 301.0f}));
        PPR_TEST_ASSERT(not grid.setCell({128u, 20u}, {257u, 302.0f}));
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {1001u, 303.0f}));
        grid.clearAllDirty();
        const DigRect rect{{127u, 20u}, 3u, 1u};
        const auto particle = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not particle and particle.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 16u);
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {60000u, 303.0f}));
        const auto unknown = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(not unknown and unknown.error() == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 16u);
        PPR_TEST_ASSERT(not grid.setCell({129u, 20u}, {32u, 303.0f}));
        const auto result = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(result.has_value() and result->m_dug == 2u and result->m_touched.size() == 2u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_element == 0u);
        PPR_TEST_ASSERT(grid.getCell({127u, 20u})->m_temperature == 301.0f);
        PPR_TEST_ASSERT(grid.getCell({128u, 20u})->m_element == 257u);
        PPR_TEST_ASSERT(grid.getCell({129u, 20u})->m_element == 0u);
        const auto repeated = digCells(grid, rect, &*catalog);
        PPR_TEST_ASSERT(repeated.has_value() and repeated->m_dug == 0u and repeated->m_touched.empty());
    };

    PPR_UNIT_TEST (construction_checks_complete_tiles_reservations_and_live_actor_overlap) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        const Footprint print{{126u, 21u}, 3u, 3u, 257u};
        PPR_TEST_ASSERT(not validateConstruction(grid, registry, print, *catalog));
        auto invalid = print;
        invalid.m_element = 1001u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid = print;
        invalid.m_min.m_x = 127u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        invalid.m_min.m_x = 4095u;
        PPR_TEST_ASSERT(validateConstruction(grid, registry, invalid, *catalog) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(not grid.setCell({128u, 23u}, {16u, 299.0f}));
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        PPR_TEST_ASSERT(not grid.setCell({128u, 23u}, {0u, 299.0f}));
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, Agent{});
        registry.emplace<sim::BodyState>(actor, sim::BodyState{42.5f, 7.5f});
        registry.emplace<sim::BodyDefinition>(actor, sim::BodyDefinition{.m_half_width = 0.4f, .m_half_height = 0.9f});
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        registry.get<sim::BodyState>(actor)->m_x = 10.0f;
        PPR_TEST_ASSERT(not validateConstruction(grid, registry, print, *catalog));
        const auto entity = registry.create();
        registry.emplace<BuildErrand>(entity, BuildErrand{print, 0u, 9u});
        PPR_TEST_ASSERT(validateConstruction(grid, registry, print, *catalog) == std::errc::device_or_resource_busy);
        Pathfinder finder;
        Array<sim::ChunkPos> touched;
        registry.get<sim::BodyState>(actor)->m_x = 42.5f;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 0u and touched.empty());
        registry.get<sim::BodyState>(actor)->m_x = 10.0f;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 4u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 4u and touched.size() == 2u);
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(registry.get<BuildErrand>(entity)->m_progress == 9u);
        registry.get<BuildErrand>(entity)->m_print.m_solid = false;
        registry.get<BuildErrand>(entity)->m_print.m_element = 0u;
        registry.get<BuildErrand>(entity)->m_progress = 0u;
        PPR_TEST_ASSERT(not advanceErrands(grid, registry, finder, touched, 64u, &*catalog));
        PPR_TEST_ASSERT(grid.getCell({128u, 23u})->m_element == 257u);
    };

    PPR_UNIT_TEST (errand_preflight_rejects_invalid_actor_before_any_demolition_writes) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        sim::ChunkGrid grid;
        sim::Registry registry;
        PPR_TEST_ASSERT(not registerColonyComponents(registry));
        PPR_TEST_ASSERT(not grid.setCell({30u, 30u}, {16u, 293.0f}));
        const auto demolition = registry.create();
        registry.emplace<BuildErrand>(demolition, BuildErrand{Footprint{{30u, 30u}, 3u, 3u, 0u, false}, 0u, 9u});
        const auto build = registry.create();
        registry.emplace<BuildErrand>(build, BuildErrand{Footprint{{60u, 30u}, 3u, 3u, 257u}, 0u, 9u});
        const auto actor = registry.create();
        registry.emplace<Agent>(actor, Agent{});
        registry.emplace<sim::BodyState>(actor, sim::BodyState{10.0f, 10.0f});
        Pathfinder finder;
        Array<sim::ChunkPos> touched;
        PPR_TEST_ASSERT(advanceErrands(grid, registry, finder, touched, 64u, &*catalog) == std::errc::invalid_argument);
        PPR_TEST_ASSERT(grid.getCell({30u, 30u})->m_element == 16u and touched.empty());
        PPR_TEST_ASSERT(registry.get<BuildErrand>(demolition)->m_progress == 0u);
        PPR_TEST_ASSERT(registry.get<BuildErrand>(build)->m_progress == 0u);
    };

    PPR_UNIT_TEST (complete_shipped_catalog_is_owned) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        if (not catalog) {
            std::println("{}", diagnostics.format());
        }
        PPR_TEST_ASSERT(catalog.has_value() and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(catalog->packId() == "ppr_frontier");
        PPR_TEST_ASSERT(catalog->materials().size() == 12u and catalog->textures().size() == 7u);
        PPR_TEST_ASSERT(catalog->biomes().size() == 4u and catalog->features().size() == 5u);
        PPR_TEST_ASSERT(catalog->worlds().size() == 2u and catalog->fields().size() == 7u);
        PPR_TEST_ASSERT(catalog->findWorld("frontier") != nullptr and catalog->findWorld("hollows") != nullptr);
        PPR_TEST_ASSERT(catalog->findWorld("unknown") == nullptr);
        PPR_TEST_ASSERT(catalog->contentFingerprint().has_value());
        PPR_TEST_ASSERT(catalog->contentFingerprint() == 0x857b3878fe9f3470ull);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{1001})->m_representation == EMaterialRepresentation::particle);
        for (const auto &texture: catalog->textures()) {
            PPR_TEST_ASSERT(texture.m_image.m_width == 64u and texture.m_image.m_height == 64u);
            PPR_TEST_ASSERT(texture.m_image.m_storage.isOwned());
        }
        const auto before = catalog->textures().front().m_image.m_storage.getBufferData().front();
        // SharedBuffer exposes read-only bytes. Extracting a mutable copy while
        // the catalog owns the image must clone rather than alter its storage.
        auto shared = catalog->textures().front().m_image.m_storage;
        auto mutable_copy = shared.moveToUnique();
        auto mutable_bytes = mutable_copy.getMutableData();
        PPR_TEST_ASSERT(mutable_bytes.has_value());
        mutable_bytes->front() ^= std::byte{1u};
        PPR_TEST_ASSERT(catalog->textures().front().m_image.m_storage.getBufferData().front() == before);
        fixture.write("textures/basalt.png", "corrupt");
        const auto failed = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not failed);
        PPR_TEST_ASSERT(catalog->textures().front().m_image.m_storage.getBufferData().front() == before);
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "materials.basalt.albedo_texture");
        PPR_TEST_ASSERT(diagnostics.entries().back().m_location.ends_with("materials.toml:9"));
        PPR_TEST_ASSERT(diagnostics.entries().back().m_message.find("basalt.png") != std::string::npos);
    };

    PPR_UNIT_TEST (shipped_profiles_compile_owned_numeric_plans) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        PPR_TEST_ASSERT(not compileWorldPlan(*catalog, "unknown", 42u));
        for (const auto profile: {"frontier", "hollows"}) {
            for (const u64 seed: {0ull, 1ull, 42ull, 0xffffffffffffffffull}) {
                auto plan = compileWorldPlan(*catalog, profile, seed);
                PPR_TEST_ASSERT(plan.has_value());
                PPR_TEST_ASSERT(plan->seed() == seed);
                PPR_TEST_ASSERT(plan->recipe().m_materials.size() == 12u);
                PPR_TEST_ASSERT(plan->fields().size() == 7u);
                PPR_TEST_ASSERT(plan->recipe().m_boundary_material == 257u);
                PPR_TEST_ASSERT(plan->recipe().m_cell_size_m == 1.0 / 3.0);
                auto layout = sim::compileBiomeLayout(*plan);
                PPR_TEST_ASSERT(layout.has_value());
                PPR_TEST_ASSERT(layout->sites().size() >= plan->recipe().m_min_sites);
                auto geometry = sim::compileGenerationGeometry(*plan, *layout);
                PPR_TEST_ASSERT(geometry.has_value());
                PPR_TEST_ASSERT(geometry->m_instances.size() >= 5u);
                PPR_TEST_ASSERT(not geometry->m_segments.empty());
                std::array<u32, 4u> counts{};
                for (const auto &site: layout->sites()) {
                    ++counts[site.m_biome];
                }
                for (const auto &constraint: plan->recipe().m_constraints) {
                    PPR_TEST_ASSERT(counts[constraint.m_biome] >= constraint.m_required_count);
                }
            }
        }
        auto owned = [&] {
            auto temporary = fixture.load(diagnostics);
            PPR_TEST_ASSERT(temporary.has_value());
            return compileWorldPlan(*temporary, "frontier", 42u);
        }();
        PPR_TEST_ASSERT(owned.has_value());
        PPR_TEST_ASSERT(std::isfinite(owned->fields().front().sample(100.0, 200.0)));
        PPR_TEST_ASSERT(not sim::validateGenerationRecipe(owned->recipe()));
    };

    PPR_UNIT_TEST (prepared_world_rejects_invalid_inputs_and_cancellation_before_generation) {
        struct Clock final : ITimerClock {
            mutable u32 m_calls{};
            TimePoint now() const noexcept override {
                ++m_calls;
                return {};
            }
        } clock;
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        const auto unknown = prepareWorld(*catalog, "unknown", 42u, clock);
        PPR_TEST_ASSERT(not unknown and clock.m_calls == 0u);

        auto materials = decodeMaterialCatalog(fixture.read("materials.toml"), "materials.toml", diagnostics);
        PPR_TEST_ASSERT(materials.has_value());
        const auto incomplete = prepareWorld(std::move(*materials), "frontier", 42u, clock);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == std::errc::invalid_argument);

        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = prepareWorld(
            std::move(*catalog), "frontier", 0xffffffffffffffffull, clock, {.m_context = job_context.get()});
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        const auto cancelled_load = prepareWorld(
            fixture.m_root, "hollows", 42u, clock, diagnostics, {.m_context = job_context.get()});
        PPR_TEST_ASSERT(not cancelled_load and cancelled_load.error() == std::errc::operation_canceled);
        PPR_TEST_ASSERT(clock.m_calls == 0u);
    };

    PPR_UNIT_TEST (numeric_streams_ignore_document_order_and_unused_materials) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        auto catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        auto baseline = compileWorldPlan(*catalog, "frontier", 42u);
        PPR_TEST_ASSERT(baseline.has_value());
        for (const auto name: {"materials.toml", "fields.toml", "biomes.toml", "features.toml", "worlds.toml"}) {
            fixture.write(name, reorder(fixture.read(name)));
        }
        auto materials = fixture.read("materials.toml");
        materials += "\n[materials.aaa_unused]\nnumeric_id = 60000\ndisplay_name = \"Unused\"\nphase = \"solid\"\n"
                     "representation = \"particle\"\ncolor_rgba = \"ffffffff\"\nreference_density_kg_m3 = 1000.0\n";
        fixture.write("materials.toml", materials);
        catalog = fixture.load(diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        auto changed = compileWorldPlan(*catalog, "frontier", 42u);
        PPR_TEST_ASSERT(changed.has_value());
        PPR_TEST_ASSERT(changed->recipe().m_materials.size() == 13u);
        PPR_TEST_ASSERT(baseline->recipe().m_rules.size() == changed->recipe().m_rules.size());
        for (std::size_t i = 0u; i < baseline->recipe().m_rules.size(); ++i) {
            PPR_TEST_ASSERT(baseline->recipe().m_rules[i].m_key == changed->recipe().m_rules[i].m_key);
        }
        for (std::size_t i = 0u; i < baseline->fields().size(); ++i) {
            for (u32 sample = 0u; sample < 32u; ++sample) {
                PPR_TEST_ASSERT(
                    baseline->fields()[i].sample(sample * 13.25, 123.75) ==
                    changed->fields()[i].sample(sample * 13.25, 123.75));
            }
        }
    };

    PPR_UNIT_TEST (complete_identity_ignores_order_comments_and_source_paths) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto first = fixture.load(diagnostics);
        PPR_TEST_ASSERT(first.has_value());
        const auto identity = first->contentFingerprint();
        for (const auto name: {"materials.toml", "fields.toml", "biomes.toml", "features.toml", "worlds.toml"}) {
            fixture.write(name, reorder(fixture.read(name)));
        }
        auto manifest = fixture.read("pack.toml");
        replace(manifest, "materials.toml", "renamed.toml");
        std::filesystem::rename(fixture.m_root / "materials.toml", fixture.m_root / "renamed.toml");
        fixture.write("pack.toml", manifest);
        auto material = fixture.read("renamed.toml");
        replace(material, "textures/basalt.png", "textures/renamed.png");
        fixture.write("renamed.toml", material);
        std::filesystem::rename(fixture.m_root / "textures/basalt.png", fixture.m_root / "textures/renamed.png");
        const auto reordered = fixture.load(diagnostics);
        if (not reordered) {
            std::println("{}", diagnostics.format());
        }
        PPR_TEST_ASSERT(reordered.has_value());
        PPR_TEST_ASSERT(reordered->contentFingerprint() == identity);
    };

    PPR_UNIT_TEST (identity_changes_for_every_definition_kind_and_texture_bytes) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto baseline = fixture.load(diagnostics);
        PPR_TEST_ASSERT(baseline.has_value());
        struct Change {
            std::string_view file, before, after;
        };
        const Change changes[] = {
            {"materials.toml", "2900.0", "2901.0"},
            {"fields.toml", "0.055", "0.056"},
            {"biomes.toml", "293.0", "294.0"},
            {"features.toml", "Hearth landing", "Hearth landing revised"},
            {"worlds.toml", "site_spacing_m = 100.0", "site_spacing_m = 101.0"},
            {"worlds.toml", "turn_probability = 0.2", "turn_probability = 0.3"},
            {"worlds.toml", "fill_height_m = 4.0", "fill_height_m = 5.0"},
        };
        for (const auto &change: changes) {
            const auto original = fixture.read(change.file);
            auto changed = original;
            replace(changed, change.before, change.after);
            fixture.write(change.file, changed);
            const auto candidate = fixture.load(diagnostics);
            PPR_TEST_ASSERT(candidate.has_value());
            PPR_TEST_ASSERT(candidate->contentFingerprint() != baseline->contentFingerprint());
            fixture.write(change.file, original);
        }
        fixture.write("textures/basalt.png", fixture.read("textures/ice.png"));
        const auto texture_change = fixture.load(diagnostics);
        PPR_TEST_ASSERT(texture_change.has_value());
        PPR_TEST_ASSERT(texture_change->contentFingerprint() != baseline->contentFingerprint());
    };

    PPR_UNIT_TEST (required_texture_limits_missing_and_cancel_are_errors) {
        Fixture fixture{};
        config::Diagnostics diagnostics{};
        const auto original = fixture.read("textures/basalt.png");
        std::filesystem::resize_file(fixture.m_root / "textures/basalt.png", kMaxTextureInputBytes + 1u);
        const auto large = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not large and large.error() == std::errc::file_too_large);
        fixture.write("textures/basalt.png", original);
        std::filesystem::remove(fixture.m_root / "textures/basalt.png");
        const auto missing = fixture.load(diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_such_file_or_directory);
        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = loadContentCatalog(fixture.m_root, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
    };
}
namespace pP::game::tests {
    const UnitTest &gamePackContentTests() noexcept {
        static const UnitTest tests = UnitTest::Named("pack_content") / [](UnitTest::IRun &_) -> void {
            _.recurse(
                {detail::pack::catalog_dig_respects_protected_material_and_preflights_the_complete_rectangle,
                    detail::pack::construction_checks_complete_tiles_reservations_and_live_actor_overlap,
                    detail::pack::errand_preflight_rejects_invalid_actor_before_any_demolition_writes,
                    detail::pack::complete_shipped_catalog_is_owned,
                    detail::pack::shipped_profiles_compile_owned_numeric_plans,
                    detail::pack::prepared_world_rejects_invalid_inputs_and_cancellation_before_generation,
                    detail::pack::numeric_streams_ignore_document_order_and_unused_materials,
                    detail::pack::complete_identity_ignores_order_comments_and_source_paths,
                    detail::pack::identity_changes_for_every_definition_kind_and_texture_bytes,
                    detail::pack::required_texture_limits_missing_and_cancel_are_errors});
        };
        return tests;
    }
}
