#pragma once

// Private fixture definition: include only after the test implementation's imports.
// Real descriptor import, compilation, generation and presentation retain the
// canonical authored metric; no moving/gas reservoir changes the empty-solver workload.
namespace pP::colony::test_fixture {
    namespace {
        PPR_DEFINE_LOG_CATEGORY(AuthoredFixtureTests, debug, none)

        // ------------------------------------------------------------------
        // deterministic authored fixture owner
        // ------------------------------------------------------------------

        struct AuthoredFixture {
            std::filesystem::path m_root;
            std::string m_namespace, m_world_key, m_manifest;
            std::shared_ptr<const ContentCatalog> m_catalog;

            AuthoredFixture()
                : m_namespace{"resident_fixture"}, m_world_key{"frontier"},
                  m_manifest{"schema_version = 1\npack_id = '" + m_namespace + "'\n"} {
                static std::atomic<u32> ordinal{};
                const auto parent = hal::process::currentExecutablePath().parent_path();

                for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                    const auto candidate = parent / ("authored-fixture-test-" + std::to_string(ordinal.fetch_add(1u)));
                    if (std::filesystem::create_directory(candidate)) {
                        m_root = std::filesystem::canonical(candidate);
                        break;
                    }
                }

                PPR_TEST_ASSERT(not m_root.empty());

                bool initialized = false;
                PPR_DEFER {
                    if (not initialized) {
                        // Best-effort cleanup also covers assertion/allocation unwinding during setup.
                        std::error_code error;
                        std::filesystem::remove_all(m_root, error);
                        PPR_LOG_WARNING_ON_FAIL(AuthoredFixtureTests, error);
                    }
                };

                sim::asset::ElementDescriptor element;
                element.m_schema_version = 1u;
                element.m_density = 2000.0;
                element.m_phase = sim::EGenerationMatter::solid;
                add("solid", sim::asset::kElementKind, "solid.toml", element);

                element.m_density = 1.0;
                element.m_phase = sim::EGenerationMatter::liquid;
                add("liquid", sim::asset::kElementKind, "liquid.toml", element);

                element.m_phase = sim::EGenerationMatter::gas;
                add("gas", sim::asset::kElementKind, "gas.toml", element);

                image::TextureDescriptor texture;
                texture.m_schema_version = 1u;
                texture.m_source = "pixel.png";
                add("albedo", image::kTextureAssetKind, "albedo.toml", texture);

                MaterialAssetDescriptor material;
                material.m_schema_version = 1u;
                material.m_element = "solid";
                material.m_diggable = true;
                material.m_appearance.m_display_name = "Basalt";
                material.m_appearance.m_color_rgba = "808080ff";
                material.m_appearance.m_albedo_texture = "albedo";
                material.m_appearance.m_texture_period_m = 1.0;
                add("basalt", sim::asset::kMaterialKind, "basalt.toml", material);

                material.m_appearance.m_display_name = "Sandstone";
                material.m_appearance.m_alpha_mode = EMaterialAlphaMode::blend;
                material.m_appearance.m_texture_sampling = EMaterialTextureSampling::repeat;
                add("sandstone", sim::asset::kMaterialKind, "sandstone.toml", material);

                material.m_element = "liquid";
                material.m_diggable.reset();
                material.m_representation = sim::definition::EMaterialRepresentation::particle;
                material.m_appearance.m_display_name = "Water";
                add("water", sim::asset::kMaterialKind, "water.toml", material);

                material.m_element = "gas";
                material.m_appearance.m_display_name = "Oxygen";
                add("oxygen", sim::asset::kMaterialKind, "oxygen.toml", material);

                sim::asset::FieldDescriptor field;
                field.m_schema_version = 1u;
                field.m_definition.m_frequency_per_metre = 0.01;
                add("noise", sim::asset::kFieldKind, "noise.toml", field);

                BiomeAssetDescriptor biome;
                biome.m_schema_version = 1u;
                biome.m_appearance.m_display_name = "Hearth";
                biome.m_appearance.m_preview_color_rgba = "808080ff";
                biome.m_definition.m_temperature = {{"noise"}, 300.0, 1.0};
                sim::definition::CompositionBand band;
                band.m_field.m_key = "noise";
                band.m_materials.m_entries.push_back({"stone", {{"basalt"}, 1.0}});
                biome.m_definition.m_composition.m_entries.push_back({"cover", std::move(band)});

                sim::definition::Deposit deposit;
                deposit.m_field.m_key = "noise";
                deposit.m_material.m_key = "sandstone";
                deposit.m_host_material.m_key = "basalt";
                deposit.m_blob.emplace();
                deposit.m_blob->m_count = 2u;
                deposit.m_blob->m_radius = {1.0, 2.0};
                deposit.m_blob->m_stretch = {1.0, 2.0};
                deposit.m_blob->m_orientation = {30.0, 180.0};
                biome.m_definition.m_deposits.m_entries.push_back({"lens", std::move(deposit)});
                add("hearth", sim::asset::kBiomeKind, "hearth.toml", biome);

                FeatureAssetDescriptor feature;
                feature.m_schema_version = 1u;
                feature.m_appearance.m_display_name = "Landing";
                feature.m_definition.m_reservation = {0, 0, 16, 16};
                sim::definition::Operation operation;
                operation.m_action = sim::definition::Carve{sim::definition::Rectangle{16u, 16u}};
                feature.m_definition.m_operations.m_entries.push_back({"clear", std::move(operation)});
                feature.m_definition.m_markers.m_entries.push_back({"spawn", {sim::EGenerationMarker::start, 8, 8}});
                add("landing", sim::asset::kFeatureKind, "landing.toml", feature);

                BackgroundAssetDescriptor background;
                background.m_schema_version = 1u;

                for (u32 index = 0u; index < 4u; ++index) {
                    BackgroundAppearanceLayer layer;
                    layer.m_order = index;
                    layer.m_texture = "albedo";
                    layer.m_color_rgba = "ffffffff";
                    layer.m_repeat_period_m = 1.0;
                    background.m_definition.m_layers.m_entries.push_back(
                        {"layer_" + std::to_string(index), std::move(layer)});
                }

                add("skies", kBackgroundAssetKind, "skies.toml", background);

                WorldAssetDescriptor world;
                world.m_schema_version = 1u;
                world.m_semantic_key = "frontier";
                world.m_appearance.m_display_name = "Frontier";
                world.m_appearance.m_background_stack = "skies";
                world.m_palette.m_materials.m_entries = {
                    {"basalt", {"basalt", 1u}}, {"sandstone", {"sandstone", 2u}}, {"water", {"water", 3u}},
                    {"oxygen", {"oxygen", 4u}}};
                world.m_palette.m_fields.m_entries.push_back({"noise", {"noise", 0u}});
                world.m_palette.m_biomes.m_entries.push_back({"hearth", {"hearth", 0u}});
                world.m_palette.m_features.m_entries.push_back({"landing", {"landing", 0u}});
                world.m_profile.m_layout = {1u, 1u, 16u, 1.0};
                world.m_profile.m_boundaries = {{"basalt"}, 1u, 0.0, {"noise"}, {"noise"}};
                world.m_profile.m_start.m_biome.m_key = "hearth";
                world.m_profile.m_start.m_feature.m_key = "landing";
                world.m_profile.m_start.m_marker.m_key = "spawn";
                world.m_profile.m_start.m_x_fine = 128u;
                world.m_profile.m_start.m_y_fine = 128u;
                world.m_profile.m_start.m_clearance_m = 2.0;
                world.m_profile.m_start.m_route_width_m = 1.0;
                sim::definition::BiomeChoice choice;
                choice.m_biome.m_key = "hearth";
                choice.m_required_count = 1u;
                choice.m_distance_max_m = 1024.0;
                world.m_profile.m_biomes.m_entries.push_back({"hearth", std::move(choice)});
                add(m_world_key, sim::asset::kWorldKind, "world.toml", world);
                write("pack.toml", m_manifest);

                config::Diagnostics diagnostics;
                auto imported = loadContentCatalog(m_root, diagnostics);
                PPR_TEST_ASSERT(imported.has_value());

                auto definitions = std::make_shared<const ContentDescriptorCatalog>(std::move(*imported));
                const auto id = resolveContentWorld(*definitions, m_namespace + ":" + m_world_key, diagnostics);
                PPR_TEST_ASSERT(id.has_value());

                auto selected = selectWorldCatalog(definitions, *id, kWorldCompileParameters, diagnostics);
                PPR_TEST_ASSERT(selected.has_value());

                m_catalog = std::make_shared<const ContentCatalog>(std::move(*selected));

                PPR_TEST_ASSERT(m_catalog->materials().size() == 4u);
                const auto *const oxygen = m_catalog->findMaterial("oxygen");
                PPR_TEST_ASSERT(oxygen != nullptr);
                PPR_TEST_ASSERT(oxygen->m_numeric_id == 4u and oxygen->m_phase == EMaterialPhase::gas);

                initialized = true;
            }

            ~AuthoredFixture() {
                // Best-effort fixture cleanup; retained catalog and sidecar owners are independent of disk.
                std::error_code error;
                std::filesystem::remove_all(m_root, error);
                PPR_LOG_WARNING_ON_FAIL(AuthoredFixtureTests, error);
            }

            AuthoredFixture(const AuthoredFixture &) = delete;
            AuthoredFixture &operator=(const AuthoredFixture &) = delete;

            void write(const std::string_view file, const std::string_view bytes) const {
                std::ofstream stream{m_root / file, std::ios::binary | std::ios::trunc};
                stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                stream.close();
                PPR_TEST_ASSERT(not stream.fail());
            }

            template<typename T>
                requires requires { T::describe(); }
            void write(const std::string_view file, const T &value) const {
                const auto document = config::documentFrom(value);
                PPR_TEST_ASSERT(document.has_value());

                const auto bytes = config::encode(*document);
                PPR_TEST_ASSERT(bytes.has_value());

                write(file, std::string_view{*bytes});
            }

            template<typename T>
            void add(
                const std::string_view key, const std::string_view kind, const std::string_view file, const T &value) {
                m_manifest += "\n[assets." +
                              std::string{key} +
                              "]\nkind = '" +
                              std::string{kind} +
                              "'\ndescriptor = '" +
                              std::string{file} +
                              "'\n";

                write(file, value);
            }

            std::shared_ptr<const ContentPresentation> presentation(const u64 generation) const {
                constexpr std::array<u8, 75u> pixel{
                    0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au, 0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du, 0x49u, 0x48u,
                    0x44u, 0x52u, 0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x04u, 0x08u, 0x06u, 0x00u, 0x00u,
                    0x00u, 0xa9u, 0xf1u, 0x9eu, 0x7eu, 0x00u, 0x00u, 0x00u, 0x12u, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u,
                    0x9cu, 0x63u, 0x68u, 0x68u, 0xf8u, 0xffu, 0x1fu, 0x19u, 0x33u, 0x90u, 0x2eu, 0x00u, 0x00u, 0x4cu,
                    0x7eu, 0x2fu, 0xe1u, 0x74u, 0xdcu, 0xdau, 0x7cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4eu,
                    0x44u, 0xaeu, 0x42u, 0x60u, 0x82u};
                write("pixel.png", {reinterpret_cast<const char *>(pixel.data()), pixel.size()});

                config::Diagnostics diagnostics;
                auto request = ContentPresentationRequest::start(
                    m_catalog, asset::RequestGeneration{generation}, context::background(), diagnostics);
                PPR_TEST_ASSERT(request.has_value());

                auto prepared = request->finish(diagnostics);
                PPR_TEST_ASSERT(prepared.has_value());
                return *prepared;
            }

            // generated owner:
            [[nodiscard]] Expected<PreparedWorld> prepare(
                const u64 seed, const u64 generation, const sim::GenerationExecution &execution = {}) const {
                auto result = prepareWorld(
                    m_catalog, m_catalog->worlds().front().m_key, seed, ITimerClock::steady(), execution);
                PPR_RETURN_UNEXPECTED_ON_FAIL(AuthoredFixtureTests, result);

                PPR_TEST_ASSERT(result->generated().m_matter.m_regions.empty());

                auto images = presentation(generation);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    AuthoredFixtureTests,
                    result->attachPresentation(std::move(images), asset::RequestGeneration{generation}));
                return std::move(*result);
            }

            [[nodiscard]] std::error_code initialize(
                ColonyDriver &driver, ColonyDriverDesc desc = {.m_agent_count = 1u},
                const sim::GenerationExecution *const execution = nullptr) const {
                PPR_TEST_ASSERT(desc.m_agent_count == 1u);

                if (desc.m_world_generation == 0u) {
                    desc.m_world_generation = driver.worldGeneration() + 1u;
                }

                auto world = prepare(
                    desc.m_seed, desc.m_world_generation, execution ? *execution : sim::GenerationExecution{});
                PPR_RETURN_ERROR_ON_FAIL(AuthoredFixtureTests, world);

                PPR_RETURN_ERROR_ON_FAIL(AuthoredFixtureTests, driver.initPrepared(desc, std::move(*world), execution));
                return {};
            }
        };
    }
}
