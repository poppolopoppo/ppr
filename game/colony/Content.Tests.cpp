module;

#include "pP/UnitTest.h"
#include "../Game.TestAllocationFailure.h"

module game.tests;

import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::content {
    using namespace colony;

    const std::string rock = R"(
[materials.sandstone]
numeric_id = 16
display_name = "Sandstone"
phase = "solid"
representation = "terrain"
color_rgba = "B59C72FF"
albedo_texture = "textures/sandstone.png"
texture_period_m = 2.0
diggable = true
reference_density_kg_m3 = 2200.0
)";

    const std::string snow = R"(
[materials.snow]
numeric_id = 1024
display_name = "Snow"
phase = "solid"
representation = "particle"
color_rgba = "FFFFFFFF"
reference_density_kg_m3 = 300.0
)";

    const std::string manifest = R"(
schema_version = 1
pack_id = "default"
[files]
materials = "materials.toml"
biomes = "biomes.toml"
fields = "fields.toml"
features = "features.toml"
worlds = "worlds.toml"
)";

    [[nodiscard]] static std::string replace(
        std::string source, const std::string_view from, const std::string_view to) {
        const auto position = source.find(from);
        PPR_TEST_ASSERT(position != std::string::npos);
        source.replace(position, from.size(), to);
        return source;
    }

    PPR_UNIT_TEST (manifest_requires_complete_distinct_portable_file_set) {
        config::Diagnostics diagnostics{};
        auto decoded = decodePackManifest(manifest, "pack.toml", diagnostics);
        PPR_TEST_ASSERT(decoded and decoded->m_pack_id == "default");
        PPR_TEST_ASSERT(decoded->m_files[static_cast<std::size_t>(EContentDocument::worlds)] == "worlds.toml");
        for (const auto &invalid:
            {replace(manifest, "schema_version = 1", "schema_version = 2"),
                replace(manifest, "pack_id = \"default\"", "pack_id = \"Default\""),
                replace(manifest, "materials.toml", "../materials.toml"),
                replace(manifest, "worlds.toml", "biomes.toml"), replace(manifest, "worlds.toml", "pack.toml"),
                replace(manifest, "worlds.toml", "C:/worlds.toml"), replace(manifest, "worlds.toml", "worlds.json"),
                replace(manifest, "worlds = \"worlds.toml\"", "unknown = \"worlds.toml\""), manifest + "extra = 2\n"}) {
            config::Diagnostics errors{};
            PPR_TEST_ASSERT(not decodePackManifest(invalid, "pack.toml", errors));
            PPR_TEST_ASSERT(errors.hasErrors());
        }
    };

    PPR_UNIT_TEST (pack_source_owns_bytes_and_bounds_io) {
        const auto root = std::filesystem::temp_directory_path() /
                          ("ppr-worldgen-pack-" + std::to_string(std::random_device{}()));
        PPR_TEST_ASSERT(std::filesystem::create_directory(root));
        PPR_DEFER {
            std::error_code cleanup{};
            std::filesystem::remove_all(root, cleanup);
            PPR_TEST_ASSERT(not cleanup);
        };
        const auto write = [&](const char *name, const std::string_view bytes) {
            std::ofstream file{root / name, std::ios::binary | std::ios::trunc};
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            PPR_TEST_ASSERT(file.good());
        };
        write("pack.toml", manifest);
        for (const char *name: {"materials.toml", "biomes.toml", "fields.toml", "features.toml", "worlds.toml"}) {
            write(name, "schema_version = 1\n");
        }
        config::Diagnostics diagnostics{};
        auto loaded = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(loaded.has_value());
        write("materials.toml", "changed\n");
        PPR_TEST_ASSERT(loaded->m_documents[0u].m_bytes == "schema_version = 1\n");

        auto [job_context, cancel] = context::withCancelClause(context::background());
        cancel(std::make_error_code(std::errc::operation_canceled));
        const auto cancelled = loadContentPackSource(root, diagnostics, job_context.get());
        PPR_TEST_ASSERT(not cancelled and cancelled.error() == std::errc::operation_canceled);
        std::filesystem::resize_file(root / "materials.toml", kMaxMaterialDocumentBytes + 1u);
        const auto oversized = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(
            not oversized and oversized.error() == config::make_error_code(config::EConfigErrc::out_of_range));
        std::filesystem::remove(root / "materials.toml");
        const auto missing = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(not missing and missing.error() == std::errc::no_such_file_or_directory);

        write("materials.toml", "schema_version = 1\n");
        std::filesystem::create_hard_link(root / "materials.toml", root / "alias.toml");
        write("pack.toml", replace(manifest, "worlds.toml", "alias.toml"));
        const auto duplicate = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(
            not duplicate and duplicate.error() == config::make_error_code(config::EConfigErrc::duplicate_key));
        std::filesystem::remove(root / "alias.toml");
        std::filesystem::create_hard_link(root / "pack.toml", root / "alias.toml");
        const auto manifest_alias = loadContentPackSource(root, diagnostics);
        PPR_TEST_ASSERT(
            not manifest_alias and
            manifest_alias.error() == config::make_error_code(config::EConfigErrc::duplicate_key));
    };

    PPR_UNIT_TEST (owned_noncontiguous_materials) {
        config::Diagnostics diagnostics{};
        auto catalog = decodeMaterialCatalog("schema_version = 1\n" + rock + snow, "fixture.toml", diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());
        PPR_TEST_ASSERT(catalog->materials().size() == 2u);
        const auto *const sandstone = catalog->findMaterial(u16{16});
        PPR_TEST_ASSERT(sandstone != nullptr);
        PPR_TEST_ASSERT(sandstone == catalog->findMaterial("sandstone"));
        PPR_TEST_ASSERT(sandstone->m_color_rgba == 0xB59C72FFu);
        PPR_TEST_ASSERT(sandstone->m_texture_period_m == 2.0);
        PPR_TEST_ASSERT(sandstone->m_diggable == true);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{1024})->m_representation == EMaterialRepresentation::particle);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{0}) == nullptr);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{65535}) == nullptr);
        PPR_TEST_ASSERT(catalog->findMaterial("unknown") == nullptr);
    };

    PPR_UNIT_TEST (canonical_material_identity) {
        config::Diagnostics diagnostics{};
        auto first = decodeMaterialCatalog("schema_version = 1\n" + rock + snow, "one.toml", diagnostics);
        auto second = decodeMaterialCatalog("# reordered\nschema_version = 1\n" + snow + rock, "two.toml", diagnostics);
        PPR_TEST_ASSERT(first.has_value() and second.has_value());
        PPR_TEST_ASSERT(first->materialFingerprint() == second->materialFingerprint());
        PPR_TEST_ASSERT(first->materialFingerprint() == 0x6e172b31ab5d1b68ull);
        PPR_TEST_ASSERT(first->materials().front().m_key == "sandstone");
        auto changed = decodeMaterialCatalog(
            "schema_version = 1\n" + replace(rock, "2.0", "3.0") + snow, "three.toml", diagnostics);
        PPR_TEST_ASSERT(changed.has_value());
        PPR_TEST_ASSERT(first->materialFingerprint() != changed->materialFingerprint());
        auto extended = decodeMaterialCatalog("schema_version = 1\n" + rock, "four.toml", diagnostics);
        PPR_TEST_ASSERT(extended.has_value());
        PPR_TEST_ASSERT(first->materialFingerprint() != extended->materialFingerprint());
    };

    PPR_UNIT_TEST (required_values_are_not_cpp_defaults) {
        for (const auto field:
            {"numeric_id", "display_name", "phase", "representation", "color_rgba", "albedo_texture",
                "texture_period_m", "diggable", "reference_density_kg_m3"}) {
            std::string source = "schema_version = 1\n" + rock;
            const auto begin = source.find(std::string{field} + " =");
            PPR_TEST_ASSERT(begin != std::string::npos);
            source.erase(begin, source.find('\n', begin) - begin + 1u);
            config::Diagnostics diagnostics{};
            const auto catalog = decodeMaterialCatalog(source, "required.toml", diagnostics);
            PPR_TEST_ASSERT(not catalog.has_value());
            PPR_TEST_ASSERT(catalog.error() == config::make_error_code(config::EConfigErrc::missing_value));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == std::string{"materials.sandstone."} + field);
        }
    };

    PPR_UNIT_TEST (invalid_materials_fail_with_authored_key) {
        constexpr std::pair<std::string_view, std::string_view> cases[] = {
            {"numeric_id = 16", "numeric_id = 0"},
            {"numeric_id = 16", "numeric_id = 65535"},
            {"numeric_id = 16", "numeric_id = -1"},
            {"numeric_id = 16", "numeric_id = 1.5"},
            {"display_name = \"Sandstone\"", "display_name = \"\""},
            {"phase = \"solid\"", "phase = \"gas\""},
            {"phase = \"solid\"", "phase = \"plasma\""},
            {"representation = \"terrain\"", "representation = \"fluid\""},
            {"color_rgba = \"B59C72FF\"", "color_rgba = \"B59C72\""},
            {"color_rgba = \"B59C72FF\"", "color_rgba = \"ZZ9C72FF\""},
            {"texture_period_m = 2.0", "texture_period_m = 0.0"},
            {"texture_period_m = 2.0", "texture_period_m = nan"},
            {"texture_period_m = 2.0", "texture_period_m = 1e300"},
            {"texture_period_m = 2.0", "texture_period_m = 1e-300"},
            {"texture_period_m = 2.0", "texture_period_m = 1e-38"},
            {"reference_density_kg_m3 = 2200.0", "reference_density_kg_m3 = inf"},
            {"reference_density_kg_m3 = 2200.0", "reference_density_kg_m3 = -2.0"},
            {"diggable = true", "diggable = \"yes\""},
            {"diggable = true", "diggable = true\nunknown_field = 42"},
        };
        for (const auto &[from, to]: cases) {
            config::Diagnostics diagnostics{};
            const auto catalog = decodeMaterialCatalog(
                "schema_version = 1\n" + replace(rock, from, to), "invalid.toml", diagnostics);
            PPR_TEST_ASSERT(not catalog.has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property.starts_with("materials.sandstone."));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_location.starts_with("invalid.toml"));
        }
    };

    PPR_UNIT_TEST (texture_paths_cannot_escape_pack) {
        for (const auto path:
            {"../rock.png", "/rock.png", "C:/rock.png", "textures/../../rock.png", "textures//rock.png",
                "textures/./rock.png", "textures/", "textures\\\\rock.png"}) {
            config::Diagnostics diagnostics{};
            const auto source = "schema_version = 1\n" + replace(rock, "textures/sandstone.png", path);
            const auto catalog = decodeMaterialCatalog(source, "paths.toml", diagnostics);
            PPR_TEST_ASSERT(not catalog.has_value());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property == "materials.sandstone.albedo_texture");
        }
    };

    PPR_UNIT_TEST (particle_fields_and_duplicate_ids_rejected) {
        for (const auto field: {"diggable = false", "albedo_texture = \"snow.png\"", "texture_period_m = 1.0"}) {
            config::Diagnostics diagnostics{};
            const auto catalog = decodeMaterialCatalog(
                "schema_version = 1\n" + snow + field, "particle.toml", diagnostics);
            PPR_TEST_ASSERT(not catalog.has_value());
        }
        config::Diagnostics diagnostics{};
        const auto duplicate = decodeMaterialCatalog(
            "schema_version = 1\n" + rock + replace(snow, "1024", "16"), "duplicate.toml", diagnostics);
        PPR_TEST_ASSERT(not duplicate.has_value());
        PPR_TEST_ASSERT(duplicate.error() == config::make_error_code(config::EConfigErrc::duplicate_key));
        PPR_TEST_ASSERT(diagnostics.entries().back().m_property == "materials.snow.numeric_id");
    };

    PPR_UNIT_TEST (version_names_types_and_budgets) {
        for (const auto source:
            {rock, "schema_version = 2\n" + rock, "schema_version = \"1\"\n" + rock,
                std::string{"schema_version = 1\n"},
                "schema_version = 1\n" + replace(rock, "materials.sandstone", "materials.Sandstone"),
                "schema_version = 1\n" + replace(rock, "materials.sandstone", "materials.\"sand.stone\""),
                "schema_version = 1\n" + rock + "numeric_id = 17\n",
                "schema_version = 1\n" + rock + "values = [1,2]\n"}) {
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeMaterialCatalog(source, "invalid.toml", diagnostics).has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }
        config::Diagnostics diagnostics{};
        PPR_TEST_ASSERT(
            not decodeMaterialCatalog(std::string(kMaxMaterialDocumentBytes + 1u, ' '), "large.toml", diagnostics)
                .has_value());
        std::string excessive = "schema_version = 1\n";
        for (std::size_t i = 0u; i <= kMaxContentMaterials; ++i) {
            excessive += replace(rock, "materials.sandstone", "materials.rock_" + std::to_string(i));
        }
        diagnostics.clear();
        const auto result = decodeMaterialCatalog(excessive, "count.toml", diagnostics);
        PPR_TEST_ASSERT(not result.has_value());
        PPR_TEST_ASSERT(result.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };

    PPR_UNIT_TEST (failed_candidate_preserves_previous_catalog) {
        config::Diagnostics diagnostics{};
        auto catalog = decodeMaterialCatalog("schema_version = 1\n" + rock, "good.toml", diagnostics);
        PPR_TEST_ASSERT(catalog.has_value());
        const auto before = catalog->materialFingerprint();
        auto candidate = decodeMaterialCatalog(
            "schema_version = 1\n" + replace(rock, "numeric_id = 16", "numeric_id = 0"), "bad.toml", diagnostics);
        if (candidate) {
            catalog = std::move(candidate);
        }
        PPR_TEST_ASSERT(catalog->materialFingerprint() == before);
        PPR_TEST_ASSERT(catalog->findMaterial(u16{16}) != nullptr);
    };

    PPR_UNIT_TEST (field_optional_controls_accept_finite_inclusive_endpoints) {
        const std::string header =
            "schema_version = 1\n[fields.boundary]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.001\n";
        for (const auto controls:
            {"lacunarity = 1.0\ngain = 0.0\nwarp_amplitude_m = 0.0\n",
                "lacunarity = 4.0\ngain = 1.0\nwarp_amplitude_m = 4096.0\n"}) {
            config::Diagnostics diagnostics{};
            const auto fields = decodeFieldDefinitions(header + controls, "fields.toml", diagnostics);
            PPR_TEST_ASSERT(fields.has_value() and fields->size() == 1u);
            PPR_TEST_ASSERT(not diagnostics.hasErrors());
        }
        for (const auto property: {"lacunarity", "gain", "warp_amplitude_m"}) {
            for (const auto value: {"nan", "inf"}) {
                config::Diagnostics diagnostics{};
                const auto fields = decodeFieldDefinitions(
                    header + property + " = " + value + "\n", "fields.toml", diagnostics);
                PPR_TEST_ASSERT(not fields.has_value());
                PPR_TEST_ASSERT(diagnostics.entries().back().m_property == std::string{"fields.boundary."} + property);
            }
        }
    };
}

namespace pP::game::tests::detail::content {
    PPR_UNIT_TEST (typed_fields_are_owned_sorted_and_compile_to_noise) {
        const std::string caves = R"(
[fields.caves]
algorithm = "open_simplex2"
frequency_per_metre = 0.025
fractal = "fbm"
octaves = 5
lacunarity = 2.0
gain = 0.45
warp_amplitude_m = 12.0
warp_frequency_per_metre = 0.006
)";
        const std::string deposits = R"(
[fields.deposits]
algorithm = "cellular_distance"
frequency_per_metre = 0.125
)";
        config::Diagnostics diagnostics{};
        auto fields = decodeFieldDefinitions("schema_version = 1\n" + deposits + caves, "fields.toml", diagnostics);
        auto reordered = decodeFieldDefinitions("schema_version = 1\n" + caves + deposits, "other.toml", diagnostics);
        PPR_TEST_ASSERT(fields and reordered and not diagnostics.hasErrors());
        PPR_TEST_ASSERT(fields->size() == 2u and fields->front().m_key == "caves");
        PPR_TEST_ASSERT(fields->back().m_noise.m_fractal == sim::ENoiseFractal::none);
        PPR_TEST_ASSERT(fields->back().m_noise.m_octaves == 4u);
        auto compiled = sim::compileNoiseField(fields->front().m_noise, 42u);
        auto replay = sim::compileNoiseField(reordered->front().m_noise, 42u);
        PPR_TEST_ASSERT(compiled and replay);
        PPR_TEST_ASSERT(compiled->sample(12.25, 13.75) == replay->sample(12.25, 13.75));
    };

    PPR_UNIT_TEST (field_required_values_and_numeric_limits_are_enforced) {
        const std::string valid =
            "schema_version = 1\n[fields.caves]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.025\n";
        const std::pair<std::string_view, std::string_view> cases[] = {
            {"algorithm = \"perlin\"\n", ""}, {"frequency_per_metre = 0.025\n", ""}, {"\"perlin\"", "\"unsupported\""},
            {"0.025", "0.0"}, {"0.025", "1e-300"}, {"0.025", "1e300"}, {"0.025", "nan"}, {"0.025", "inf"},
            {"0.025", "0.025\noctaves = 17"}, {"0.025", "0.025\nwarp_amplitude_m = -1.0"},
            {"0.025", "0.025\nwarp_frequency_per_metre = 1e300"}, {"0.025", "0.025\nlacunarity = 5.0"},
            {"0.025", "0.025\ngain = 1.01"}, {"0.025", "0.025\nunknown = true"}};
        for (const auto &[from, to]: cases) {
            config::Diagnostics diagnostics{};
            const auto fields = decodeFieldDefinitions(replace(valid, from, to), "invalid-fields.toml", diagnostics);
            PPR_TEST_ASSERT(not fields and diagnostics.hasErrors());
            PPR_TEST_ASSERT(diagnostics.entries().front().m_property.starts_with("fields.caves."));
            PPR_TEST_ASSERT(diagnostics.entries().front().m_location.starts_with("invalid-fields.toml"));
        }
        std::string excessive = "schema_version = 1\n";
        for (std::size_t index = 0u; index <= kMaxContentFields; ++index) {
            excessive += "[fields.field_" +
                         std::to_string(index) +
                         "]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.025\n";
        }
        config::Diagnostics diagnostics{};
        const auto rejected = decodeFieldDefinitions(excessive, "count.toml", diagnostics);
        PPR_TEST_ASSERT(
            not rejected and rejected.error() == config::make_error_code(config::EConfigErrc::out_of_range));
    };

    PPR_UNIT_TEST (empty_and_ambiguous_records_are_not_erased) {
        for (const auto suffix: {"\n[materials.snow]\n", "\nunknown_field = {}\n", "\n[unknown]\n"}) {
            config::Diagnostics diagnostics{};
            PPR_TEST_ASSERT(not decodeMaterialCatalog("schema_version = 1\n" + rock + suffix, "empty.toml", diagnostics)
                    .has_value());
            PPR_TEST_ASSERT(diagnostics.hasErrors());
        }
        config::Diagnostics diagnostics{};
        const auto source = "schema_version = 1\n" + replace(rock, "materials.sandstone", "\"materials.sandstone\"");
        PPR_TEST_ASSERT(not decodeMaterialCatalog(source, "quoted.toml", diagnostics).has_value());
    };

    PPR_UNIT_TEST (excess_diagnostics_remain_bounded_and_reject_content) {
        std::string extras{};
        for (u32 i = 0u; i < 200u; ++i) {
            extras += "unknown_" + std::to_string(i) + " = 1\n";
        }
        config::Diagnostics forwarded{};
        const auto material = decodeMaterialCatalog("schema_version = 1\n" + rock + extras, "many.toml", forwarded);
        PPR_TEST_ASSERT(not material and forwarded.hasErrors());
        PPR_TEST_ASSERT(forwarded.entries().size() == 64u);
        PPR_TEST_ASSERT(forwarded.size() == 200u and forwarded.omittedCount() == 136u);
        PPR_TEST_ASSERT(forwarded.entries().front().m_property.starts_with("materials.sandstone."));
        PPR_TEST_ASSERT(forwarded.format().find("136 further diagnostics omitted") != std::string::npos);

        config::Diagnostics capped{2u};
        const auto fields = decodeFieldDefinitions(
            "schema_version = 1\n[fields.caves]\nalgorithm = \"perlin\"\nfrequency_per_metre = 0.025\n" + extras,
            "many-fields.toml", capped);
        PPR_TEST_ASSERT(not fields and capped.hasErrors());
        PPR_TEST_ASSERT(capped.entries().size() == 2u and capped.size() == 200u and capped.omittedCount() == 198u);
        PPR_TEST_ASSERT(capped.entries().front().m_property.starts_with("fields.caves."));
        // A full collector must still reject a later invalid record.
        const auto repeated = decodeMaterialCatalog("schema_version = 1\n" + rock + extras, "again.toml", capped);
        PPR_TEST_ASSERT(not repeated and capped.entries().size() == 2u and capped.size() == 400u);
    };

    PPR_UNIT_TEST (allocation_failure_reaches_fatal_boundary) {
        const std::string source = "schema_version = 1\n" + rock;
        config::Diagnostics diagnostics{};
        bool propagated = false;
        try {
            AllocationFailure failure{};
            (void) decodeMaterialCatalog(source, "allocation.toml", diagnostics);
        } catch (const std::bad_alloc &) {
            // The test observes the exception at the caller boundary; the
            // executable routes escaping exceptions to its fatal handler.
            propagated = true;
        }
        PPR_TEST_ASSERT(propagated);
    };
}

namespace pP::game::tests {
    const UnitTest content = UnitTest::Named("content") / [](UnitTest::IRun &_) -> void {
        _.recurse(
            {detail::content::manifest_requires_complete_distinct_portable_file_set,
                detail::content::pack_source_owns_bytes_and_bounds_io, detail::content::owned_noncontiguous_materials,
                detail::content::canonical_material_identity, detail::content::required_values_are_not_cpp_defaults,
                detail::content::invalid_materials_fail_with_authored_key,
                detail::content::texture_paths_cannot_escape_pack,
                detail::content::particle_fields_and_duplicate_ids_rejected,
                detail::content::version_names_types_and_budgets,
                detail::content::failed_candidate_preserves_previous_catalog,
                detail::content::empty_and_ambiguous_records_are_not_erased,
                detail::content::typed_fields_are_owned_sorted_and_compile_to_noise,
                detail::content::field_required_values_and_numeric_limits_are_enforced,
                detail::content::field_optional_controls_accept_finite_inclusive_endpoints,
                detail::content::excess_diagnostics_remain_bounded_and_reject_content,
                detail::content::allocation_failure_reaches_fatal_boundary});
    };
    const UnitTest &gameContentTests() noexcept {
        return content;
    }
}
