module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
// Internal engine header, not installed: the exact binary16 codec the
// presentation row's motion pair shares with the resolved-cell codecs.
#include "../../app/renderer/App.Renderer.Binary16.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP::tests::detail::WorldLightingProjection {
    // ------------------------------------------------------------------
    // projection fixture values
    // ------------------------------------------------------------------

    inline constexpr u32 kEdge = WorldLightingPass::kWorldEdge;
    inline constexpr u32 kCells = WorldLightingPass::kCellCount;
    inline constexpr u32 kStatusStageClear = 0u;
    inline constexpr u32 kStatusStageKeys = 1u;
    inline constexpr u32 kStatusStageRows = 2u;
    inline constexpr u32 kStatusStageCells = 3u;
    inline constexpr u32 kMaterialCount = 8u;

    // Material 1 is opaque, 2 is transparent receive-only, 3 is emissive
    // opaque and 4 is an emissive liquid. Classification comes from the same
    // authored visible palette the projection consumes.
    inline constexpr u32 kOpaque = 1u;
    inline constexpr u32 kReceiveOnly = 2u;
    inline constexpr u32 kEmissiveOpaque = 3u;
    inline constexpr u32 kEmissiveLiquid = 4u;

    // Q16 physical-fill words: one quarter-volume candidate, the two-candidate
    // half cell, and the exactly-full cell volume.
    inline constexpr u32 kFillQuarter{0x4000u};
    inline constexpr u32 kFillHalf{0x8000u};
    inline constexpr u32 kFillFull{0x10000u};

    constexpr u32 cellIndex(const u32 x, const u32 y) noexcept {
        return y * kEdge + x;
    }

    // Committed world-cell coordinate for an exact native-cell offset. The
    // published origin subtracts out; every fixture uses binary-exact offsets so
    // distance ties are genuine ties.
    constexpr float worldPosition(const float native) noexcept {
        return 3.0f + native;
    }

    // Committed numerical (hot64) and readonly (cold24) rows.
    struct HotRow {
        float m_position[2]{};
        float m_carried[14]{};
    };

    struct ColdRow {
        float m_fields[6]{};
    };

    static_assert(sizeof(HotRow) == 64u and alignof(HotRow) == 4u);
    static_assert(sizeof(ColdRow) == 24u and alignof(ColdRow) == 4u);
    static_assert(PPR_OFFSETOF(HotRow, m_carried) == 8u);

    struct ParticleSpec {
        u64 m_id{};
        u32 m_source_row{};
        u32 m_material{kOpaque};
        double m_temperature{300.0};
        float m_x{};
        float m_y{};
        bool m_enabled{true};
        bool m_gas{};
        float m_gas_x{};
        float m_gas_y{};
        // Committed physical fill V/(cell^2*thickness). Zero is the legacy
        // "unavailable" word; the transport source then keeps its full value.
        float m_physical_fill{};
        // Solver hot-row displacement in native cells per step (carried[0..1]
        // of ParticleWrite), the only motion the presentation row carries.
        float m_displacement_x{};
        float m_displacement_y{};
    };

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue{};

        void initialize(UnitTest::IRun &_) {
            PPR_TEST_ASSERT(not m_shaders->initialize());
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            PPR_TEST_ASSERT(m_rhi->getDevice().getDeviceType() == backend);
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            const rhi::DeviceInfo &info = m_rhi->getDevice().getInfo();
            _.log(
                std::format(
                    "WORLD_LIGHTING_PROJECTION api={} adapter={} backend={}",
                    info.apiName != nullptr ? info.apiName : "unknown",
                    info.adapterName != nullptr ? info.adapterName : "unknown", static_cast<int>(info.deviceType)));
        }

        void shutdown() {
            m_queue.setNull();
            PPR_TEST_ASSERT(not m_rhi->shutdown());
            PPR_TEST_ASSERT(not m_shaders->shutdown());
        }

        [[nodiscard]] rhi::IDevice &device() const {
            return m_rhi->getDevice();
        }
    };

    // ------------------------------------------------------------------
    // buffer and submission plumbing
    // ------------------------------------------------------------------

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeShaderBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = std::max<u64>(bytes, stride);
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = rhi::BufferUsage::ShaderResource;
        description.defaultState = rhi::ResourceState::ShaderResource;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    [[nodiscard]] rhi::ComPtr<rhi::IBuffer> makeStorageBuffer(
        rhi::IDevice &device, const u64 bytes, const u32 stride, const void *const initial = nullptr) {
        rhi::BufferDesc description{};
        description.size = std::max<u64>(bytes, stride);
        description.elementSize = stride;
        description.memoryType = rhi::MemoryType::DeviceLocal;
        description.usage = static_cast<rhi::BufferUsage>(
            static_cast<u32>(rhi::BufferUsage::ShaderResource) |
            static_cast<u32>(rhi::BufferUsage::UnorderedAccess) |
            static_cast<u32>(rhi::BufferUsage::CopySource) |
            static_cast<u32>(rhi::BufferUsage::CopyDestination));
        description.defaultState = rhi::ResourceState::UnorderedAccess;
        rhi::ComPtr<rhi::IBuffer> buffer;
        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(description, initial, buffer.writeRef())));
        return buffer;
    }

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder, rhi::ComPtr<rhi::ICommandBuffer> *out = nullptr) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
        if (out != nullptr) {
            *out = std::move(commands);
        }
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    // ------------------------------------------------------------------
    // committed publication scene
    // ------------------------------------------------------------------

    struct Scene {
        std::shared_ptr<std::atomic<u64>> m_live_epoch{std::make_shared<std::atomic<u64>>(0u)};
        Array<GridParticleMetadataRow> m_rows{};
        std::vector<u32> m_terrain_words{};
        rhi::ComPtr<rhi::IBuffer> m_numerical{};
        rhi::ComPtr<rhi::IBuffer> m_readonly{};
        rhi::ComPtr<rhi::IBuffer> m_terrain{};
        rhi::ComPtr<rhi::IBuffer> m_visuals{};
        rhi::ComPtr<rhi::IBuffer> m_source_materials{};
        rhi::ComPtr<rhi::IBuffer> m_thermal_lut{};
        lighting::WorldLightingInputs m_inputs{};
        // Source palette uploaded by the next publish. Zero (the default) keeps
        // the cached source black; a magnitude assertion sets its own entries.
        std::array<lighting::SourceMaterial, kMaterialCount> m_source_palette{};
        u64 m_publication{};

        Scene() {
            m_terrain_words.assign(kCells, 0u);
        }

        // Publishes exactly the committed GPU state the projection consumes:
        // stable-ID-ordered metadata, hot64/cold24 numerical rows under the
        // published origin, and the full-domain packed terrain words. `cell_metres`
        // is the published native cell edge; the presentation row must convert the
        // committed cell coordinates through it.
        void publish(Fixture &fixture, const std::span<const ParticleSpec> specs, const double cell_metres = 1.0) {
            rhi::IDevice &device = fixture.device();

            m_rows.clear();
            u32 moving_count{};
            for (const auto &spec: specs) {
                GridParticleMetadataRow row{};
                row.m_id_low = static_cast<u32>(spec.m_id & 0xffff'ffffu);
                row.m_id_high = static_cast<u32>(spec.m_id >> 32u);
                row.m_source_row = spec.m_gas ? 0xffff'ffffu : spec.m_source_row;
                row.m_material_temperature = *lighting::packCell(static_cast<u16>(spec.m_material), spec.m_temperature);
                row.m_radius = spec.m_gas ? 0.5f : 1.0f;
                row.m_physical_fill_bits = std::bit_cast<u32>(spec.m_physical_fill);
                if (spec.m_gas) {
                    row.m_gas_x = spec.m_gas_x;
                    row.m_gas_y = spec.m_gas_y;
                } else {
                    moving_count = std::max(moving_count, spec.m_source_row + 1u);
                }

                m_rows.push_back(row);
            }

            Array<HotRow> hot(std::max(1u, moving_count));
            Array<ColdRow> cold(std::max(1u, moving_count));
            for (const auto &spec: specs) {
                if (spec.m_gas) {
                    continue;
                }

                PPR_TEST_ASSERT(spec.m_source_row < hot.size());
                hot[spec.m_source_row].m_position[0] = spec.m_x;
                hot[spec.m_source_row].m_position[1] = spec.m_y;
                // ParticleWrite field order: carried[0..1] are displacementX/Y
                // and carried[13] is enabled.
                hot[spec.m_source_row].m_carried[0] = spec.m_displacement_x;
                hot[spec.m_source_row].m_carried[1] = spec.m_displacement_y;
                hot[spec.m_source_row].m_carried[13] = spec.m_enabled ? 1.0f : 0.0f;
                cold[spec.m_source_row].m_fields[1] = std::numbers::pi_v<float>;
            }

            const auto metadata = prepareGridParticleMetadata(
                device, m_rows, moving_count, cell_metres, ++m_publication);
            PPR_TEST_ASSERT(metadata.has_value());

            m_numerical = makeShaderBuffer(device, hot.size() * sizeof(HotRow), sizeof(HotRow), hot.data());
            m_readonly = makeShaderBuffer(device, cold.size() * sizeof(ColdRow), sizeof(ColdRow), cold.data());
            m_terrain = makeShaderBuffer(
                device, m_terrain_words.size() * sizeof(u32), sizeof(u32), m_terrain_words.data());
            buildMaterials(device);

            GridParticleGpuView view{};
            view.m_numerical = m_numerical;
            view.m_readonly = m_readonly;
            view.m_metadata = *metadata;
            view.m_live_epoch = m_live_epoch;
            view.m_world = GridWorldIdentity{1u, 1u, 1u};
            view.m_device_epoch = m_publication;
            view.m_publication = m_publication;
            view.m_terrain_revision = 1u;
            view.m_joint_tick = 1u;
            view.m_origin = 3.0;
            m_live_epoch->store(m_publication, std::memory_order_release);
            PPR_TEST_ASSERT(view.current());

            m_inputs = {};
            m_inputs.m_particles = view;
            m_inputs.m_identity = {view.m_world, view.m_publication, view.m_terrain_revision, view.m_joint_tick};
            m_inputs.m_terrain = m_terrain;
            m_inputs.m_visible_materials = m_visuals;
            m_inputs.m_source_materials = m_source_materials;
            m_inputs.m_thermal_lut = m_thermal_lut;
            m_inputs.m_material_count = kMaterialCount;
        }

        // The validated empty publication resource: a committed view with no
        // candidates, not an invented epoch or a fabricated identity.
        void publishEmpty(Fixture &fixture) {
            publish(fixture, std::span<const ParticleSpec>{});
        }

    private:
        void buildMaterials(rhi::IDevice &device) {
            std::array<GridTerrainVisual, kMaterialCount> visuals{};
            for (u32 index = 0u; index < visuals.size(); ++index) {
                visuals[index].m_material_id = index;
                visuals[index].m_color_rgba = 0xffff'ffffu;
            }

            visuals[kReceiveOnly].m_surface_flags = kGridSurfaceOpticalBlend;
            visuals[kReceiveOnly].m_opacity = 0.5f;
            visuals[kEmissiveOpaque].m_emissive_rgb[0] = 1.0f;
            visuals[kEmissiveOpaque].m_emissive_strength = 5.0f;
            visuals[kEmissiveLiquid].m_surface_flags = kGridSurfaceParticleLiquid;
            visuals[kEmissiveLiquid].m_emissive_rgb[2] = 0.5f;
            visuals[kEmissiveLiquid].m_emissive_strength = 2.0f;

            m_visuals = makeShaderBuffer(device, sizeof(visuals), 96u, visuals.data());

            // The same accepted attempt now also prepares the cached source, so
            // the world's full publication set is present: the source palette
            // (zero unless a test authors one) and an all-zero thermal table
            // make the source cache black without changing the
            // projection/acceleration contract under test.
            m_source_materials = makeShaderBuffer(
                device, sizeof(m_source_palette), sizeof(lighting::SourceMaterial), m_source_palette.data());
            const std::vector<u8> thermal(lighting::kThermalLutBytes, 0u);
            m_thermal_lut = makeShaderBuffer(device, thermal.size(), lighting::kThermalLutStride, thermal.data());
        }
    };

    // ------------------------------------------------------------------
    // attempt driving and readback
    // ------------------------------------------------------------------

    struct Attempt {
        std::error_code m_prepare{};
        std::error_code m_finalize{};
    };

    // Encodes one projection attempt into its own submission. The caller owns
    // acceptance (notifySubmitted) and completion inspection.
    [[nodiscard]] Attempt encodeAttempt(
        Fixture &fixture, WorldLightingPass &pass, const lighting::WorldLightingInputs &inputs,
        const FenceCompletionValue completed = kFenceValueNone) {
        Attempt attempt{};
        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, completed, {}};
        attempt.m_prepare = pass.prepare(context, inputs, WorldLightingFrame{});
        if (attempt.m_prepare) {
            return attempt;
        }

        attempt.m_finalize = pass.finalize(context);
        submit(fixture, *encoder);
        return attempt;
    }

    [[nodiscard]] u32 readCell(Fixture &fixture, WorldLightingPass &pass, const u32 cell) {
        u32 word{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(
            pass.resolvedCellsForTest(), static_cast<u64>(cell) * sizeof(u32), sizeof(word), &word)));
        return word;
    }

    [[nodiscard]] std::vector<u32> readCells(Fixture &fixture, WorldLightingPass &pass) {
        std::vector<u32> cells(kCells);
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(pass.resolvedCellsForTest(), 0u, cells.size() * sizeof(u32), cells.data())));
        return cells;
    }

    [[nodiscard]] u32 readFillWord(Fixture &fixture, WorldLightingPass &pass, const u32 cell) {
        u32 word{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(
            pass.fillCellsForTest(), static_cast<u64>(cell) * sizeof(u32), sizeof(word), &word)));
        return word;
    }

    [[nodiscard]] u32 readSourceWord(Fixture &fixture, WorldLightingPass &pass, const u32 cell) {
        u32 word{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(
            pass.sourceCellsForTest(), static_cast<u64>(cell) * sizeof(u32), sizeof(word), &word)));
        return word;
    }

    [[nodiscard]] u32 statusWord(WorldLightingPass &pass, const u32 record, const u32 word) {
        const auto words = pass.pendingStatusWordsForTest();
        PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
        return words[record * kNumericStatusWords + word];
    }

    [[nodiscard]] u32 packMaterialTemperature(const u32 material, const double temperature) {
        return *lighting::packCell(static_cast<u16>(material), temperature);
    }

    // ------------------------------------------------------------------
    // terrain precedence and empty cells
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (terrain_precedence_wins_over_a_particle_candidate) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        // Committed positions are world-cell coordinates; the published origin
        // of 3.0 cells subtracts out. (13.5 - 3.0) is the center of native
        // cell (10, 10).
        Scene scene;
        const std::array specs{
            ParticleSpec{.m_id = 1u, .m_source_row = 0u, .m_material = kOpaque, .m_x = 13.5f, .m_y = 13.5f},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(10u, 10u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(11u, 10u)) == 0u);

        // Existing terrain material takes precedence in the same native cell.
        scene.m_terrain_words[cellIndex(10u, 10u)] = packMaterialTemperature(7u, 900.0);
        scene.publish(fixture, specs);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(10u, 10u)) == packMaterialTemperature(7u, 900.0));
        // A cell with neither terrain nor a candidate stays empty.
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(12u, 12u)) == 0u);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // classification, distance and stable-ID ties
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (opaque_precedes_receive_only_transparent_and_nearest_center_wins) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const std::array specs{
            // Cell (20, 20): the receive-only particle sits exactly on the
            // center, but the opaque class bit outranks distance.
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kReceiveOnly,
                .m_x = worldPosition(20.5f),
                .m_y = worldPosition(20.5f)},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(20.75f),
                .m_y = worldPosition(20.5f)},
            // Cell (40, 40): two opaque candidates; the nearer center wins.
            ParticleSpec{
                .m_id = 3u,
                .m_source_row = 2u,
                .m_material = kOpaque,
                .m_x = worldPosition(40.5f),
                .m_y = worldPosition(40.125f)},
            ParticleSpec{
                .m_id = 4u,
                .m_source_row = 3u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(40.5f),
                .m_y = worldPosition(40.5f)},
            // Cell (60, 60): only a receive-only particle.
            ParticleSpec{
                .m_id = 5u,
                .m_source_row = 4u,
                .m_material = kReceiveOnly,
                .m_x = worldPosition(60.5f),
                .m_y = worldPosition(60.5f)},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        const auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(20u, 20u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(
            readCell(fixture, pass, cellIndex(40u, 40u)) == packMaterialTemperature(kEmissiveOpaque, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(60u, 60u)) == packMaterialTemperature(kReceiveOnly, 300.0));

        // The emissive word is the selected material's packed cell exactly
        // once; overlap never multiplies emission.
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(40u, 40u)) == *lighting::packCell(kEmissiveOpaque, 300.0));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (exact_distance_tie_uses_stable_id_rank_not_numerical_source_order) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        // Two exact binary32 distance ties: mirror positions around each cell
        // center and two coincident particles. Stable IDs order the winners.
        // Distinct materials make the winning stable ID observable in the
        // resolved word; only the metadata rank can break these exact ties.
        const std::array specs{
            ParticleSpec{
                .m_id = 10u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(30.25f),
                .m_y = worldPosition(30.5f)},
            ParticleSpec{
                .m_id = 11u,
                .m_source_row = 1u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(30.75f),
                .m_y = worldPosition(30.5f)},
            ParticleSpec{
                .m_id = 12u,
                .m_source_row = 2u,
                .m_material = kOpaque,
                .m_x = worldPosition(31.5f),
                .m_y = worldPosition(31.5f)},
            ParticleSpec{
                .m_id = 13u,
                .m_source_row = 3u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(31.5f),
                .m_y = worldPosition(31.5f)},
        };

        Scene scene;
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));
        const auto original = readCells(fixture, pass);

        // IDs 10 and 12 are the smaller stable IDs in their exact ties.
        PPR_TEST_ASSERT(original[cellIndex(30u, 30u)] == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(original[cellIndex(31u, 31u)] == packMaterialTemperature(kOpaque, 300.0));

        // Republish the identical committed geometry with the numerical source
        // rows in reverse order. Metadata rank (stable ID) is unchanged, so the
        // winner that survives the key tie must be identical.
        auto reordered = specs;
        for (u32 index = 0u; index < reordered.size(); ++index) {
            reordered[index].m_source_row = safe_narrowing(reordered.size() - 1u - index);
        }

        PPR_TEST_ASSERT(reordered[0u].m_id == 10u and reordered[3u].m_id == 13u);

        scene.publish(fixture, reordered);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        const auto permuted = readCells(fixture, pass);

        PPR_TEST_ASSERT(permuted == original);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // exclusions and next-frame rebuild
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (gas_inactive_and_removed_candidates_never_leave_a_stale_cell) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const std::array specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_gas = true,
                .m_gas_x = 50.5f,
                .m_gas_y = 50.5f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(49.5f),
                .m_y = worldPosition(49.5f),
                .m_enabled = false},
            ParticleSpec{
                .m_id = 3u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(51.5f),
                .m_y = worldPosition(51.5f)},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(50u, 50u)) == 0u);
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(49u, 49u)) == 0u);
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(51u, 51u)) == packMaterialTemperature(kOpaque, 300.0));

        // The committed publication is replaced by the validated empty
        // resource: every previously projected cell is rebuilt to zero.
        scene.m_terrain_words.assign(kCells, 0u);
        scene.publishEmpty(fixture);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));

        const auto cleared = readCells(fixture, pass);
        PPR_TEST_ASSERT(std::ranges::none_of(cleared, [](const u32 word) { return word != 0u; }));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (zero_one_and_dense_overlaps_rebuild_without_particle_readback) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Zero particles: the empty publication still overwrites every cell.
        scene.publishEmpty(fixture);
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));
        {
            const auto empty = readCells(fixture, pass);
            PPR_TEST_ASSERT(std::ranges::none_of(empty, [](const u32 word) { return word != 0u; }));
        }

        // One particle.
        const std::array single{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(67.5f),
                .m_y = worldPosition(67.5f)},
        };
        scene.publish(fixture, single);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        {
            const auto one = readCells(fixture, pass);
            PPR_TEST_ASSERT(one[cellIndex(67u, 67u)] == packMaterialTemperature(kOpaque, 300.0));
            PPR_TEST_ASSERT(std::ranges::count_if(one, [](const u32 word) { return word != 0u; }) == 1);
        }

        // A dense overlap in one cell plus many separate cells.
        Array<ParticleSpec> dense;
        for (u32 index = 0u; index < 64u; ++index) {
            dense.push_back(
                ParticleSpec{
                    .m_id = index + 1u,
                    .m_source_row = index,
                    .m_material = kOpaque,
                    .m_x = worldPosition(77.25f),
                    .m_y = worldPosition(77.25f)});
        }

        for (u32 index = 0u; index < 64u; ++index) {
            dense.push_back(
                ParticleSpec{
                    .m_id = 100u + index,
                    .m_source_row = 64u + index,
                    .m_material = kOpaque,
                    .m_x = worldPosition(97.5f + static_cast<float>(index)),
                    .m_y = worldPosition(97.5f)});
        }

        scene.publish(fixture, dense);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 2u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(3u);
        PPR_TEST_ASSERT(not pass.collectCompleted(3u));
        {
            const auto overlapped = readCells(fixture, pass);
            PPR_TEST_ASSERT(overlapped[cellIndex(77u, 77u)] == packMaterialTemperature(kOpaque, 300.0));
            PPR_TEST_ASSERT(std::ranges::count_if(overlapped, [](const u32 word) { return word != 0u; }) == 65);
        }

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // domain endpoints and rejection
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (maximum_endpoint_maps_to_last_cell_and_invalid_positions_are_rejected) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto nan = std::numeric_limits<float>::quiet_NaN();
        Scene scene;
        const std::array specs{
            // The exact closed maximum endpoint (origin + 1024 cells) belongs
            // to the last row-major cell.
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(1024.0f),
                .m_y = worldPosition(1024.0f)},
            // One cell past the endpoint is rejected, not clamped.
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(1025.0f),
                .m_y = worldPosition(10.5f)},
            // Nonfinite coordinates are rejected and report a status index.
            ParticleSpec{
                .m_id = 3u, .m_source_row = 2u, .m_material = kOpaque, .m_x = nan, .m_y = worldPosition(10.5f)},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);

        // The invalid and outside candidates both report the invalid-state
        // category, keep the lowest metadata row, and write no cell.
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageKeys, 0u) == 1u);
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageKeys, 1u) == kNoNumericIndex);
        for (u32 word = 2u; word < kNumericStatusWords; ++word) {
            PPR_TEST_ASSERT(statusWord(pass, kStatusStageKeys, word) == 0u);
            PPR_TEST_ASSERT(statusWord(pass, kStatusStageCells, word) == 0u);
        }

        // Containment is not success: the frame fails with the invalid-state
        // category while the endpoint cell still resolves its winner.
        PPR_TEST_ASSERT(pass.collectCompleted(1u) == std::errc::state_not_recoverable);
        PPR_TEST_ASSERT(readCell(fixture, pass, kCells - 1u) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(10u, 10u)) == 0u);

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // numeric status categories and propagation
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (invalid_and_range_minima_stay_separate_and_reach_the_caller) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        const auto nan = std::numeric_limits<float>::quiet_NaN();
        Scene scene;
        const std::array specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(10.5f),
                .m_y = worldPosition(10.5f)},
            ParticleSpec{
                .m_id = 2u, .m_source_row = 1u, .m_material = kOpaque, .m_x = nan, .m_y = worldPosition(10.5f)},
            ParticleSpec{
                .m_id = 3u, .m_source_row = 2u, .m_material = kOpaque, .m_x = 4000.0f, .m_y = worldPosition(10.5f)},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);

        // Two failing invocations in one category: the minimum row survives,
        // the range word stays unused and the reserved words stay zero.
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageKeys, 0u) == 1u);
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageKeys, 1u) == kNoNumericIndex);
        PPR_TEST_ASSERT(pass.collectCompleted(1u) == std::errc::state_not_recoverable);

        // A completed representational overflow is injected into the real
        // readback inspection path; it must propagate as result_out_of_range
        // without touching the reserved words.
        scene.publish(fixture, std::span<const ParticleSpec>{});
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        {
            auto words = pass.pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            words[kStatusStageRows * kNumericStatusWords + 1u] = 7u;
        }

        PPR_TEST_ASSERT(pass.collectCompleted(2u) == std::errc::result_out_of_range);
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageRows, 0u) == kNoNumericIndex);
        PPR_TEST_ASSERT(statusWord(pass, kStatusStageRows, 1u) == 7u);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (status_slots_require_completion_and_consumption_before_reuse) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        scene.publishEmpty(fixture);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Three submitted-but-unconsumed tickets occupy the whole ring.
        for (u32 attempt = 0u; attempt < WorldLightingPass::kStatusRingSize; ++attempt) {
            const auto encoded = encodeAttempt(fixture, pass, scene.m_inputs);
            PPR_TEST_ASSERT(not encoded.m_prepare);
            PPR_TEST_ASSERT(not encoded.m_finalize);
            pass.notifySubmitted(attempt + 1u);
        }

        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            PPR_TEST_ASSERT(pass.statusSlotPendingForTest(slot));
            PPR_TEST_ASSERT(pass.statusSlotSubmittedForTest(slot));
        }

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(
                pass.prepare(context, scene.m_inputs, WorldLightingFrame{}) == std::errc::device_or_resource_busy);
        }

        // Discard releases only definitely-unsubmitted tickets: every submitted
        // slot stays occupied.
        pass.discardUnsubmitted();
        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            PPR_TEST_ASSERT(pass.statusSlotPendingForTest(slot));
        }

        // Attempts at or below the completed value are inspected; the newest
        // submission stays occupied until its own fence is covered.
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        u32 remaining = 0u;
        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            if (pass.statusSlotPendingForTest(slot)) {
                ++remaining;
                PPR_TEST_ASSERT(pass.statusSlotSubmittedForTest(slot));
            }
        }

        PPR_TEST_ASSERT(remaining == 1u);

        // Completion plus consumption frees the last slot.
        PPR_TEST_ASSERT(not pass.collectCompleted(3u));
        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(slot));
        }

        // The consumed slots are reusable without waiting; an armed attempt is
        // released by discard as the definitely-unsubmitted case.
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, 3u, {}};
            PPR_TEST_ASSERT(not pass.prepare(context, scene.m_inputs, WorldLightingFrame{}));
            u32 armed_slot = WorldLightingPass::kStatusRingSize;
            for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
                if (pass.statusSlotPendingForTest(slot)) {
                    armed_slot = slot;
                }
            }

            PPR_TEST_ASSERT(armed_slot < WorldLightingPass::kStatusRingSize);
            pass.discardUnsubmitted();
            PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(armed_slot));
        }

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (second_prepare_before_submission_is_rejected_without_disturbing_the_attempt) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const std::array armed_specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(10.5f),
                .m_y = worldPosition(10.5f)},
        };
        scene.publish(fixture, armed_specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Prepare and finalize one attempt, then hold it: its submission fence
        // is not known yet.
        auto encoder = makeEncoder(fixture);
        {
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not pass.prepare(context, scene.m_inputs, WorldLightingFrame{}));
            PPR_TEST_ASSERT(not pass.finalize(context));
        }

        u32 armed_slot = WorldLightingPass::kStatusRingSize;
        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            if (pass.statusSlotPendingForTest(slot)) {
                PPR_TEST_ASSERT(armed_slot == WorldLightingPass::kStatusRingSize);
                armed_slot = slot;
            }
        }

        PPR_TEST_ASSERT(armed_slot < WorldLightingPass::kStatusRingSize);
        PPR_TEST_ASSERT(not pass.statusSlotSubmittedForTest(armed_slot));

        // Different committed content in a second prepare is rejected before it
        // arms another slot or rewrites the armed attempt's constants and roots.
        Scene other;
        const std::array other_specs{
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(20.5f),
                .m_y = worldPosition(20.5f)},
        };
        other.publish(fixture, other_specs);
        {
            auto second = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *second, kFenceValueNone, {}};
            PPR_TEST_ASSERT(
                pass.prepare(context, other.m_inputs, WorldLightingFrame{}) == std::errc::operation_not_permitted);
        }

        for (u32 slot = 0u; slot < WorldLightingPass::kStatusRingSize; ++slot) {
            PPR_TEST_ASSERT(pass.statusSlotPendingForTest(slot) == (slot == armed_slot));
        }

        PPR_TEST_ASSERT(not pass.statusSlotSubmittedForTest(armed_slot));

        // The armed submission still owns its fence, status copy and content:
        // collection at its own fence succeeds and no other scene leaked in.
        submit(fixture, *encoder);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(pass.statusSlotSubmittedForTest(armed_slot));
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(10u, 10u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(20u, 20u)) == 0u);

        // The rejected call needed no discard: the consumed slot is reusable and
        // the next committed publication resolves normally.
        auto attempt = encodeAttempt(fixture, pass, other.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        PPR_TEST_ASSERT(
            readCell(fixture, pass, cellIndex(20u, 20u)) == packMaterialTemperature(kEmissiveOpaque, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(10u, 10u)) == 0u);

        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (shutdown_reports_an_uninspected_pending_error) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const std::array specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(10.5f),
                .m_y = worldPosition(10.5f)},
        };
        scene.publish(fixture, specs);

        auto pass = std::make_unique<WorldLightingPass>();
        PPR_TEST_ASSERT(not pass->initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, *pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass->notifySubmitted(1u);
        {
            auto words = pass->pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            words[kStatusStageCells * kNumericStatusWords + 1u] = 3u;
        }

        // Teardown drains and inspects the outstanding ticket instead of
        // reporting a successful shutdown.
        PPR_TEST_ASSERT(pass->shutdown() == std::errc::result_out_of_range);
    };

    // ------------------------------------------------------------------
    // per-cell physical-fill reduction
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (fill_reduction_sums_each_candidate_once_and_keeps_the_winner) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;

        // One quarter-volume candidate contributes exactly one quarter.
        const std::array single{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.5f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
        };
        scene.publish(fixture, single);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(70u, 70u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(70u, 70u)) == kFillQuarter);
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(71u, 70u)) == 0u);

        // Two candidates in that cell: the nearer centre still owns the single
        // resolved word while the fill word is the exact sum of both rows.
        const std::array pair{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.5f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(70.75f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
        };
        scene.publish(fixture, pair);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(70u, 70u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(70u, 70u)) == kFillHalf);

        // The same two candidates with their stable IDs, and therefore their
        // metadata row order, exchanged: the winner rank and the integer fill
        // sum are unchanged, so dispatch and row order cannot select them.
        const std::array reversed{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(70.75f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.5f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
        };
        scene.publish(fixture, reversed);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 2u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(3u);
        PPR_TEST_ASSERT(not pass.collectCompleted(3u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(70u, 70u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(70u, 70u)) == kFillHalf);

        // Four quarter-volume candidates fill exactly one cell volume. Each
        // admitted row is counted once: a fill accumulated by both reduction
        // passes, or once per duplicate row, would double this word.
        const std::array dense{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.5f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.75f),
                .m_y = worldPosition(70.5f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 3u,
                .m_source_row = 2u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.5f),
                .m_y = worldPosition(70.75f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 4u,
                .m_source_row = 3u,
                .m_material = kOpaque,
                .m_x = worldPosition(70.75f),
                .m_y = worldPosition(70.75f),
                .m_physical_fill = 0.25f},
        };
        scene.publish(fixture, dense);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 3u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(4u);
        PPR_TEST_ASSERT(not pass.collectCompleted(4u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(70u, 70u)) == packMaterialTemperature(kOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(70u, 70u)) == kFillFull);

        _.log(
            std::format(
                "WORLD_LIGHTING_PROJECTION fill_reduction quarter={:x} half={:x} full={:x}", kFillQuarter, kFillHalf,
                kFillFull));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // fill-weighted transport source
    // ------------------------------------------------------------------

    // Binary-exact authored emission for the source-magnitude assertions:
    // (1.0, 0.5, 0.25) survives RGB9E5 exactly. The fixture's thermal table is
    // all zero and its fine CH starts black, so the cached source is exactly
    // f*L with no tolerance involved.
    [[nodiscard]] lighting::SourceMaterial fillTestSource() {
        const auto record = lighting::makeSourceMaterial(
            float3{1.0f, 0.5f, 0.25f}, 1.0, float3{0.0f, 0.0f, 0.0f}, float3{0.0f, 0.0f, 0.0f}, 1.0f);
        PPR_TEST_ASSERT(record.has_value());
        return *record;
    }

    [[nodiscard]] u32 scaledSourceWord(const u32 emission_word, const float factor) {
        const float3 emission = lighting::unpackRGB9E5(emission_word);
        const auto word = lighting::packRGB9E5(float3{emission.x * factor, emission.y * factor, emission.z * factor});
        PPR_TEST_ASSERT(word.has_value());
        return *word;
    }

    PPR_UNIT_TEST (source_scales_with_cell_fill_and_is_bit_identical_at_full_fill) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const lighting::SourceMaterial authored = fillTestSource();
        scene.m_source_palette[kEmissiveOpaque] = authored;

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // The same native cell is republished with 1, 2, 4 and 6 quarter-volume
        // candidates at its centre. Reusing the cell also proves clearWinners
        // resets the fill word instead of accumulating it across frames.
        const auto quarter_particles = [](const u32 count) {
            std::vector<ParticleSpec> specs{};
            for (u32 index = 0u; index < count; ++index) {
                specs.push_back(
                    ParticleSpec{
                        .m_id = index + 1u,
                        .m_source_row = index,
                        .m_material = kEmissiveOpaque,
                        .m_x = worldPosition(80.5f),
                        .m_y = worldPosition(80.5f),
                        .m_physical_fill = 0.25f});
            }

            return specs;
        };

        const std::array<u32, 4u> counts{1u, 2u, 4u, 6u};
        const std::array<float, 4u> factors{0.25f, 0.5f, 1.0f, 1.0f};
        for (u32 step = 0u; step < counts.size(); ++step) {
            const auto specs = quarter_particles(counts[step]);
            scene.publish(fixture, specs);
            const auto attempt = encodeAttempt(fixture, pass, scene.m_inputs, step == 0u ? kFenceValueNone : step);
            PPR_TEST_ASSERT(not attempt.m_prepare);
            PPR_TEST_ASSERT(not attempt.m_finalize);
            pass.notifySubmitted(step + 1u);
            PPR_TEST_ASSERT(not pass.collectCompleted(step + 1u));

            const u32 expected = scaledSourceWord(authored.m_emission, factors[step]);
            const u32 measured = readSourceWord(fixture, pass, cellIndex(80u, 80u));
            _.log(
                std::format(
                    "WORLD_LIGHTING_PROJECTION fill_source particles={} factor={} word={:x} expected={:x}",
                    counts[step], factors[step], measured, expected));
            PPR_TEST_ASSERT(measured == expected);
        }

        // Sigma V = 1.0 is the Lab's normal full cell: the fill factor is
        // exactly 1.0 and the packed word is the unweighted source.
        PPR_TEST_ASSERT(scaledSourceWord(authored.m_emission, 1.0f) == authored.m_emission);
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (cell_source_never_accumulates_per_particle) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const lighting::SourceMaterial authored = fillTestSource();
        scene.m_source_palette[kEmissiveOpaque] = authored;

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // One particle fills the whole cell.
        const std::array full{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(80.5f),
                .m_y = worldPosition(80.5f),
                .m_physical_fill = 1.0f},
        };
        scene.publish(fixture, full);
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));
        const u32 single_word = readSourceWord(fixture, pass, cellIndex(80u, 80u));

        // Two particles that together fill the same cell. The source is one
        // value per cell, so the two configurations are identical; a source
        // that summed per-particle emission would double it. (Two particles
        // each identical to the first one above would instead double the fill,
        // which is the fill reduction's contract, not a second source.)
        const std::array halves{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(80.5f),
                .m_y = worldPosition(80.5f),
                .m_physical_fill = 0.5f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(80.5f),
                .m_y = worldPosition(80.5f),
                .m_physical_fill = 0.5f},
        };
        scene.publish(fixture, halves);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));
        const u32 halves_word = readSourceWord(fixture, pass, cellIndex(80u, 80u));

        PPR_TEST_ASSERT(single_word == authored.m_emission);
        PPR_TEST_ASSERT(halves_word == single_word);
        _.log(
            std::format("WORLD_LIGHTING_PROJECTION no_accumulation single={:x} halves={:x}", single_word, halves_word));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (two_quarter_filled_cells_add_up_to_one_half_filled_cell) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const lighting::SourceMaterial authored = fillTestSource();
        scene.m_source_palette[kEmissiveOpaque] = authored;

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Two quarter-filled emissive particles in two independent cells.
        const std::array two_cells{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(90.5f),
                .m_y = worldPosition(90.5f),
                .m_physical_fill = 0.25f},
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 1u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(91.5f),
                .m_y = worldPosition(90.5f),
                .m_physical_fill = 0.25f},
        };
        scene.publish(fixture, two_cells);
        auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        const float3 left = lighting::unpackRGB9E5(readSourceWord(fixture, pass, cellIndex(90u, 90u)));
        const float3 right = lighting::unpackRGB9E5(readSourceWord(fixture, pass, cellIndex(91u, 90u)));

        // One particle filling half of one cell.
        const std::array half{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(92.5f),
                .m_y = worldPosition(90.5f),
                .m_physical_fill = 0.5f},
        };
        scene.publish(fixture, half);
        attempt = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(2u);
        PPR_TEST_ASSERT(not pass.collectCompleted(2u));

        const u32 half_word = readSourceWord(fixture, pass, cellIndex(92u, 90u));
        const float3 half_rgb = lighting::unpackRGB9E5(half_word);

        // Quarter and half magnitudes are exact powers of two here, so the
        // per-channel sum is binary-exact.
        PPR_TEST_ASSERT(half_word == scaledSourceWord(authored.m_emission, 0.5f));
        PPR_TEST_ASSERT(left.x + right.x == half_rgb.x);
        PPR_TEST_ASSERT(left.y + right.y == half_rgb.y);
        PPR_TEST_ASSERT(left.z + right.z == half_rgb.z);

        _.log(
            std::format(
                "WORLD_LIGHTING_PROJECTION two_cells sum=({}, {}, {}) single_half=({}, {}, {})", left.x + right.x,
                left.y + right.y, left.z + right.z, half_rgb.x, half_rgb.y, half_rgb.z));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    PPR_UNIT_TEST (fill_factor_leaves_gas_receive_only_terrain_and_legacy_cells_alone) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        const lighting::SourceMaterial authored = fillTestSource();
        scene.m_source_palette[kEmissiveOpaque] = authored;

        // Terrain-owned: a candidate in an emissive terrain cell returns before
        // the fill atomic, so the cell keeps the full authored value.
        scene.m_terrain_words[cellIndex(97u, 97u)] = packMaterialTemperature(kEmissiveOpaque, 300.0);

        const std::array specs{
            // Gas: no cell nomination, no fill and no source.
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_gas = true,
                .m_gas_x = 95.5f,
                .m_gas_y = 95.5f},
            // Receive-only with half a cell of fill: still exactly zero source.
            ParticleSpec{
                .m_id = 2u,
                .m_source_row = 0u,
                .m_material = kReceiveOnly,
                .m_x = worldPosition(96.5f),
                .m_y = worldPosition(96.5f),
                .m_physical_fill = 0.5f},
            // The candidate inside the terrain-owned cell.
            ParticleSpec{
                .m_id = 3u,
                .m_source_row = 1u,
                .m_material = kOpaque,
                .m_x = worldPosition(97.5f),
                .m_y = worldPosition(97.5f),
                .m_physical_fill = 0.25f},
            // A legacy presentation-only row: zero means fill unavailable, so
            // the cell keeps today's full value instead of going black.
            ParticleSpec{
                .m_id = 4u,
                .m_source_row = 2u,
                .m_material = kEmissiveOpaque,
                .m_x = worldPosition(98.5f),
                .m_y = worldPosition(98.5f)},
        };
        scene.publish(fixture, specs);

        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        const auto attempt = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not attempt.m_prepare);
        PPR_TEST_ASSERT(not attempt.m_finalize);
        pass.notifySubmitted(1u);
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(95u, 95u)) == 0u);
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(95u, 95u)) == 0u);
        PPR_TEST_ASSERT(readSourceWord(fixture, pass, cellIndex(95u, 95u)) == 0u);

        PPR_TEST_ASSERT(readCell(fixture, pass, cellIndex(96u, 96u)) == packMaterialTemperature(kReceiveOnly, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(96u, 96u)) == kFillHalf);
        PPR_TEST_ASSERT(readSourceWord(fixture, pass, cellIndex(96u, 96u)) == 0u);

        PPR_TEST_ASSERT(
            readCell(fixture, pass, cellIndex(97u, 97u)) == packMaterialTemperature(kEmissiveOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(97u, 97u)) == 0u);
        PPR_TEST_ASSERT(readSourceWord(fixture, pass, cellIndex(97u, 97u)) == authored.m_emission);

        PPR_TEST_ASSERT(
            readCell(fixture, pass, cellIndex(98u, 98u)) == packMaterialTemperature(kEmissiveOpaque, 300.0));
        PPR_TEST_ASSERT(readFillWord(fixture, pass, cellIndex(98u, 98u)) == 0u);
        PPR_TEST_ASSERT(readSourceWord(fixture, pass, cellIndex(98u, 98u)) == authored.m_emission);

        _.log(std::format("WORLD_LIGHTING_PROJECTION fill_guards gas=0 receive_only=0 terrain=full legacy=full"));
        PPR_TEST_ASSERT(not pass.shutdown());
    };

    // ------------------------------------------------------------------
    // reflected shader ABI
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (reflected_projection_records_match_the_cpu_shapes) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        shader::SharedModule module{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "world_lighting_projection.slang",
            "world_lighting_projection", module.writeRef()));

        auto *const layout = module->getLayout();
        PPR_TEST_ASSERT(layout != nullptr);
        const auto stride = [&](const std::string_view name) -> u32 {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (name == std::string_view{parameter->getName()}) {
                    return safe_narrowing(parameter->getTypeLayout()->getElementTypeLayout()->getStride(
                        SLANG_PARAMETER_CATEGORY_UNIFORM));
                }
            }

            return 0u;
        };

        for (const auto &[name, expected]: std::array<std::pair<const char *, u32>, 9u>{{
                 {"g_numerical", 64u},
                 {"g_metadata", 32u},
                 {"g_visuals", 96u},
                 {"g_terrain", 4u},
                 {"g_best_key", 4u},
                 {"g_best_row", 4u},
                 {"g_cell_fill", 4u},
                 {"g_resolved", 4u},
                 {"g_numeric_status", 4u},
             }}) {
            const u32 measured = stride(name);
            _.log(
                std::format(
                    "WORLD_LIGHTING_PROJECTION reflected name={} stride={} expected={}", name, measured, expected));
            PPR_TEST_ASSERT(measured == expected);
        }

        auto *const constants = [&]() -> slang::TypeLayoutReflection * {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (std::string_view{"g_projection"} == std::string_view{parameter->getName()}) {
                    return parameter->getTypeLayout()->getElementTypeLayout();
                }
            }

            return nullptr;
        }();
        PPR_TEST_ASSERT(constants != nullptr);
        PPR_TEST_ASSERT(constants->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) == 32u);
        PPR_TEST_ASSERT(constants->getFieldByIndex(0u)->getOffset() == 0u);
        PPR_TEST_ASSERT(constants->getFieldByIndex(2u)->getOffset() == 8u);
        PPR_TEST_ASSERT(constants->getFieldByIndex(4u)->getOffset() == 16u);
        PPR_TEST_ASSERT(constants->getFieldByIndex(5u)->getOffset() == 20u);

        // The projection reads the authored classification and the committed
        // metadata word by name; their offsets are the shared record contract.
        const auto field_offset = [&](const std::string_view element, const std::string_view field) -> u32 {
            for (unsigned index = 0u; index < layout->getParameterCount(); ++index) {
                auto *const parameter = layout->getParameterByIndex(index);
                if (element != std::string_view{parameter->getName()}) {
                    continue;
                }

                auto *const type = parameter->getTypeLayout()->getElementTypeLayout();
                for (unsigned member = 0u; member < type->getFieldCount(); ++member) {
                    auto *const entry = type->getFieldByIndex(member);
                    if (field == std::string_view{entry->getName()}) {
                        return safe_narrowing(entry->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
                    }
                }
            }

            return std::numeric_limits<u32>::max();
        };

        for (const auto &[element, field, expected]: std::array<std::tuple<const char *, const char *, u32>, 7u>{{
                 {"g_visuals", "emissiveStrength", 60u},
                 {"g_visuals", "surfaceFlags", 76u},
                 {"g_metadata", "materialTemperature", 12u},
                 {"g_metadata", "sourceRow", 8u},
                 {"g_metadata", "physicalFillBits", 28u},
                 {"g_numerical", "positionX", 0u},
                 {"g_numerical", "positionY", 4u},
             }}) {
            const u32 measured = field_offset(element, field);
            _.log(
                std::format(
                    "WORLD_LIGHTING_PROJECTION reflected field={}.{} offset={} expected={}", element, field, measured,
                    expected));
            PPR_TEST_ASSERT(measured == expected);
        }

        for (const char *const entry: {"clearWinners", "reduceKeys", "reduceRows", "resolveCells"}) {
            shader::ComPtr<slang::IEntryPoint> point{};
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName(entry, point.writeRef())));
        }
    };

    // ------------------------------------------------------------------
    // owner binding and GridPass integration
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (grid_pass_owns_the_projection_and_binds_resolved_cells) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        GridPass grid;
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        GridTerrainVisual visual{};
        visual.m_material_id = 1u;
        visual.m_color_rgba = 0xffff'ffffu;

        GridPass::GridStagedFrame stage{};
        stage.m_world_edge = kEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals.push_back(visual);
        stage.m_tiles.push_back(
            GridTileSubmission{
                .m_dirty_mask = 1u,
                .m_tile_range = GridTileRange{0, 0, 128, 128},
                .m_chunk_id = 0u,
            });

        GridPass::GridStagedUpload upload{};
        upload.m_chunk_id = 0u;
        upload.m_payload.m_rect[0] = 0.0f;
        upload.m_payload.m_rect[1] = 0.0f;
        upload.m_payload.m_rect[2] = 128.0f;
        upload.m_payload.m_rect[3] = 128.0f;
        upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 1u);
        upload.m_cell_words.assign(GridPass::kCellsPerChunk, 0u);
        // World cell (40, 40) carries terrain and must win over the particle.
        upload.m_cell_words[40u * 128u + 40u] = packMaterialTemperature(7u, 900.0);
        // World cell (30, 30) is terrain with the *same material id* as the
        // particle in (60, 60). Ownership is the committed winner grid, so this
        // cell must keep its categorical base.
        upload.m_cell_words[30u * 128u + 30u] = packMaterialTemperature(1u, 300.0);
        stage.m_uploads.push_back(std::move(upload));
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
        const auto cells_prepared = grid.tryPrepareCellUploads(fixture.device(), kFenceValueNone);
        PPR_TEST_ASSERT(cells_prepared.has_value() and *cells_prepared);

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_ortho_scale = 2048.0f;
        Camera camera;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{256, 256}}, ViewportLayout{}});
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not grid.prepareGpuVisibility(context));
            submit(fixture, *encoder);
        }

        std::array<GridParticleMetadataRow, 2u> rows{};
        rows[0].m_id_low = 1u;
        rows[0].m_source_row = 0u;
        rows[0].m_material_temperature = packMaterialTemperature(1u, 400.0);
        rows[0].m_radius = 1.0f;
        rows[1].m_id_low = 2u;
        rows[1].m_source_row = 1u;
        rows[1].m_material_temperature = packMaterialTemperature(1u, 500.0);
        rows[1].m_radius = 1.0f;
        const auto metadata = prepareGridParticleMetadata(fixture.device(), rows, 2u, 1.0, 1u);
        PPR_TEST_ASSERT(metadata.has_value());

        std::array<HotRow, 2u> hot{};
        hot[0].m_position[0] = 3.0f + 40.5f;
        hot[0].m_position[1] = 3.0f + 40.5f;
        hot[0].m_carried[13] = 1.0f;
        hot[1].m_position[0] = 3.0f + 60.5f;
        hot[1].m_position[1] = 3.0f + 60.5f;
        hot[1].m_carried[13] = 1.0f;
        std::array<ColdRow, 2u> cold{};

        auto live = std::make_shared<std::atomic<u64>>(0u);
        GridParticleGpuView view{};
        view.m_numerical = makeShaderBuffer(fixture.device(), sizeof(hot), sizeof(HotRow), hot.data());
        view.m_readonly = makeShaderBuffer(fixture.device(), sizeof(cold), sizeof(ColdRow), cold.data());
        view.m_metadata = *metadata;
        view.m_live_epoch = live;
        view.m_world = grid.activeWorldIdentity();
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;
        view.m_origin = 3.0;
        live->store(1u, std::memory_order_release);
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not grid.prepareWorldLighting(context, WorldLightingFrame{}));
            PPR_TEST_ASSERT(not grid.finalizeWorldLighting(context));
            submit(fixture, *encoder);
        }

        grid.notifyWorldLightingSubmitted(1u);
        PPR_TEST_ASSERT(not grid.collectWorldLightingCompleted(1u));

        WorldLightingPass *const pass = grid.activeWorldLightingForTest();
        PPR_TEST_ASSERT(pass != nullptr);
        PPR_TEST_ASSERT(readCell(fixture, *pass, cellIndex(40u, 40u)) == packMaterialTemperature(7u, 900.0));
        // Terrain sharing the particle's material id: ownership is the committed
        // winner grid, so this cell stays terrain-owned.
        PPR_TEST_ASSERT(readCell(fixture, *pass, cellIndex(30u, 30u)) == packMaterialTemperature(1u, 300.0));
        PPR_TEST_ASSERT(readCell(fixture, *pass, cellIndex(60u, 60u)) == packMaterialTemperature(1u, 500.0));
        PPR_TEST_ASSERT(readCell(fixture, *pass, cellIndex(0u, 0u)) == 0u);

        // bind() supplies the same resolved cell and status words a material
        // or inspection shader consumes.
        shader::SharedModule probe{};
        PPR_TEST_ASSERT(not fixture.m_shaders->loadModuleFromFile(
            std::filesystem::current_path() / "shaders" / "tests" / "world_lighting_projection_bind.slang",
            "world_lighting_projection_bind", probe.writeRef()));
        shader::ComPtr<slang::IEntryPoint> entry{};
        PPR_TEST_ASSERT(not make_error_code(probe->findEntryPointByName("probeBindings", entry.writeRef())));
        slang::IComponentType *entries[]{entry.get()};
        rhi::ShaderProgramDesc description{};
        description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        description.slangGlobalScope = probe.get();
        description.slangEntryPoints = entries;
        description.slangEntryPointCount = 1u;
        rhi::ComPtr<rhi::IShaderProgram> program;
        shader::Diagnose diagnostics;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().createShaderProgram(description, program.writeRef(), diagnostics.writeRef())));
        rhi::ComputePipelineDesc pipeline_description{};
        pipeline_description.program = program;
        rhi::ComPtr<rhi::IComputePipeline> pipeline;
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createComputePipeline(pipeline_description, pipeline.writeRef())));
        rhi::ComPtr<rhi::IShaderObject> root;
        PPR_TEST_ASSERT(not make_error_code(fixture.device().createRootShaderObject(program, root.writeRef())));

        struct ProbeParameters {
            u32 m_cell;
            u32 m_reserved[3];
        };
        const ProbeParameters parameters{cellIndex(60u, 60u), {}};
        rhi::ShaderCursor root_cursor{root};
        rhi::ShaderCursor constants;
        PPR_TEST_ASSERT(not make_error_code(root_cursor["g_bind_probe"].getDereferenced(constants)));
        PPR_TEST_ASSERT(not make_error_code(constants.setData(&parameters, sizeof(parameters))));
        const auto out = makeStorageBuffer(fixture.device(), 4u * sizeof(u32), sizeof(u32));
        PPR_TEST_ASSERT(not make_error_code(root_cursor["g_bind_out"].setBinding(rhi::Binding(out))));
        PPR_TEST_ASSERT(not grid.bindWorldLighting(root_cursor));

        auto encoder = makeEncoder(fixture);
        encoder->setBufferState(
            grid.activeWorldLightingForTest()->resolvedCellsForTest(), rhi::ResourceState::ShaderResource);
        {
            auto *const compute = encoder->beginComputePass();
            PPR_TEST_ASSERT(compute != nullptr);
            compute->bindPipeline(pipeline, root);
            compute->dispatchCompute(1u, 1u, 1u);
            compute->end();
        }

        submit(fixture, *encoder);

        std::array<u32, 4u> bound{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(out, 0u, sizeof(bound), bound.data())));
        PPR_TEST_ASSERT(bound[0u] == *lighting::packCell(1u, 500.0));

        // ------------------------------------------------------------------
        // categorical base retirement
        // ------------------------------------------------------------------
        // The terrain fragment retires a cell only when the committed winner
        // grid assigned it to a particle. The probe reads the same resolved and
        // ownership words the terrain addresses and writes the decision, so
        // particle-owned, terrain-owned (including the same-material-ID cell) and
        // empty cells are asserted without a visual harness.
        shader::ComPtr<slang::IEntryPoint> ownership_entry{};
        PPR_TEST_ASSERT(not make_error_code(probe->findEntryPointByName("probeOwnership", ownership_entry.writeRef())));
        slang::IComponentType *ownership_entries[]{ownership_entry.get()};
        rhi::ShaderProgramDesc ownership_description{};
        ownership_description.linkingStyle = rhi::LinkingStyle::SingleProgram;
        ownership_description.slangGlobalScope = probe.get();
        ownership_description.slangEntryPoints = ownership_entries;
        ownership_description.slangEntryPointCount = 1u;
        rhi::ComPtr<rhi::IShaderProgram> ownership_program;
        PPR_TEST_ASSERT(not make_error_code(fixture.device().createShaderProgram(
            ownership_description, ownership_program.writeRef(), diagnostics.writeRef())));
        rhi::ComputePipelineDesc ownership_pipeline_description{};
        ownership_pipeline_description.program = ownership_program;
        rhi::ComPtr<rhi::IComputePipeline> ownership_pipeline;
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().createComputePipeline(ownership_pipeline_description, ownership_pipeline.writeRef())));
        rhi::ComPtr<rhi::IShaderObject> ownership_root;
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().createRootShaderObject(ownership_program, ownership_root.writeRef())));
        rhi::ShaderCursor ownership_cursor{ownership_root};

        // The first attempt is submitted and collected, so its scratch winner
        // grid is no longer this submission's output. The binder rejects it.
        PPR_TEST_ASSERT(pass->bindTerrainOwnership(ownership_cursor) == std::errc::resource_unavailable_try_again);

        const std::array<u32, 4u> ownership_cells{
            cellIndex(60u, 60u), cellIndex(40u, 40u), cellIndex(30u, 30u), cellIndex(0u, 0u)};
        const auto ownership_cells_buffer = makeShaderBuffer(
            fixture.device(), sizeof(ownership_cells), sizeof(u32), ownership_cells.data());
        const auto ownership_out = makeStorageBuffer(
            fixture.device(), ownership_cells.size() * sizeof(u32) * 4u, sizeof(u32) * 4u);

        struct OwnershipProbeParameters {
            u32 m_count;
            u32 m_reserved[3];
        };
        const OwnershipProbeParameters ownership_parameters{safe_narrowing(ownership_cells.size()), {}};
        rhi::ShaderCursor ownership_constants;
        PPR_TEST_ASSERT(
            not make_error_code(ownership_cursor["g_ownership_probe"].getDereferenced(ownership_constants)));
        PPR_TEST_ASSERT(
            not make_error_code(ownership_constants.setData(&ownership_parameters, sizeof(ownership_parameters))));
        PPR_TEST_ASSERT(not make_error_code(
            ownership_cursor["g_ownership_cells"].setBinding(rhi::Binding(ownership_cells_buffer))));
        PPR_TEST_ASSERT(
            not make_error_code(ownership_cursor["g_ownership_out"].setBinding(rhi::Binding(ownership_out))));

        {
            // A current attempt: the probe dispatch follows the projection in
            // this submission, and the binder reads this attempt's grid.
            auto ownership_encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *ownership_encoder, 1u, {}};
            PPR_TEST_ASSERT(not grid.prepareWorldLighting(context, WorldLightingFrame{}));
            PPR_TEST_ASSERT(not pass->bindTerrainOwnership(ownership_cursor));
            auto *const resolved = pass->resolvedCellsForTest();
            PPR_TEST_ASSERT(
                not make_error_code(ownership_cursor["g_resolved_cells"].setBinding(rhi::Binding(resolved))));
            {
                auto *const compute = ownership_encoder->beginComputePass();
                PPR_TEST_ASSERT(compute != nullptr);
                compute->bindPipeline(ownership_pipeline, ownership_root);
                compute->dispatchCompute(1u, 1u, 1u);
                compute->end();
            }
            PPR_TEST_ASSERT(not grid.finalizeWorldLighting(context));
            submit(fixture, *ownership_encoder);
            grid.notifyWorldLightingSubmitted(2u);
            PPR_TEST_ASSERT(not grid.collectWorldLightingCompleted(2u));
        }

        std::array<u32, 16u> ownership{};
        PPR_TEST_ASSERT(
            not make_error_code(fixture.device().readBuffer(ownership_out, 0u, sizeof(ownership), ownership.data())));
        for (u32 index = 0u; index < ownership_cells.size(); ++index) {
            const u32 resolved = ownership[index * 4u + 0u];
            const u32 best_row = ownership[index * 4u + 1u];
            const u32 owned = ownership[index * 4u + 2u];
            _.log(
                std::format(
                    "WORLD_LIGHTING_PROJECTION ownership cell={} resolved={} best_row={} owned={}", index, resolved,
                    best_row, owned));
        }

        // Particle-owned: the committed winner row and the retirement decision.
        PPR_TEST_ASSERT(ownership[0u * 4u + 0u] == packMaterialTemperature(1u, 500.0));
        PPR_TEST_ASSERT(ownership[0u * 4u + 1u] == 1u);
        PPR_TEST_ASSERT(ownership[0u * 4u + 2u] == 1u);
        // Opaque terrain: no winner word, so the base is kept.
        PPR_TEST_ASSERT(ownership[1u * 4u + 0u] == packMaterialTemperature(7u, 900.0));
        PPR_TEST_ASSERT(ownership[1u * 4u + 1u] == 0xffff'ffffu);
        PPR_TEST_ASSERT(ownership[1u * 4u + 2u] == 0u);
        // Terrain with the particle's own material id: still terrain-owned.
        PPR_TEST_ASSERT(ownership[2u * 4u + 0u] == packMaterialTemperature(1u, 300.0));
        PPR_TEST_ASSERT(ownership[2u * 4u + 1u] == 0xffff'ffffu);
        PPR_TEST_ASSERT(ownership[2u * 4u + 2u] == 0u);
        // Empty cell: unchanged, the lit backdrop remains.
        PPR_TEST_ASSERT(ownership[3u * 4u + 0u] == 0u);
        PPR_TEST_ASSERT(ownership[3u * 4u + 1u] == 0xffff'ffffu);
        PPR_TEST_ASSERT(ownership[3u * 4u + 2u] == 0u);

        // A completed numeric failure reaches the GridPass owner boundary with
        // the range category, while the already-submitted resolved cells stay
        // exactly as published (no rollback of in-place state).
        {
            auto failure_encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *failure_encoder, 2u, {}};
            PPR_TEST_ASSERT(not grid.prepareWorldLighting(context, WorldLightingFrame{}));
            PPR_TEST_ASSERT(not grid.finalizeWorldLighting(context));
            submit(fixture, *failure_encoder);
            grid.notifyWorldLightingSubmitted(3u);
            auto words = pass->pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            words[kStatusStageCells * kNumericStatusWords + 1u] = 5u;
            PPR_TEST_ASSERT(grid.collectWorldLightingCompleted(3u) == std::errc::result_out_of_range);
        }

        PPR_TEST_ASSERT(readCell(fixture, *pass, cellIndex(60u, 60u)) == *lighting::packCell(1u, 500.0));
    };

    // ------------------------------------------------------------------
    // packed particle presentation row
    // ------------------------------------------------------------------

    // One authored palette entry; `rgba` is the presentation tint source and the
    // flags carry the phase/optics class bits the packer reads at byte 76.
    [[nodiscard]] GridTerrainVisual gridVisual(const u32 material, const u32 rgba, const u32 flags) noexcept {
        GridTerrainVisual visual{};
        visual.m_material_id = material;
        visual.m_color_rgba = rgba;
        visual.m_surface_flags = flags;
        return visual;
    }

    // Authored palette tint in the row's r/g/b byte order (low byte is alpha).
    [[nodiscard]] float3 paletteTint(const u32 rgba) noexcept {
        return float3{
            static_cast<float>((rgba >> 24u) & 255u) / 255.0f, static_cast<float>((rgba >> 16u) & 255u) / 255.0f,
            static_cast<float>((rgba >> 8u) & 255u) / 255.0f};
    }

    // One binary16 ulp at the magnitude of `value`.
    [[nodiscard]] double halfUlp(const float value) noexcept {
        const double magnitude = std::abs(static_cast<double>(value));
        return magnitude == 0.0 ? std::ldexp(1.0, -24) : std::ldexp(1.0, std::ilogb(magnitude) - 10);
    }

    [[nodiscard]] bool motionMatches(const u16 bits, const float expected) noexcept {
        return std::abs(static_cast<double>(::pP::details::decodeHalf(bits)) - static_cast<double>(expected)) <=
               halfUlp(expected);
    }

    PPR_UNIT_TEST (packed_presentation_row_quantises_and_filters_rows) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        constexpr u32 kOverRangeMaterial = kGridParticleMaterialLimit + 1u;
        constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

        // Eight committed rows: packed, disabled, clamped, non-finite motion,
        // gas, optical-blend material, out-of-domain material, non-finite
        // position. The seven moving rows own source rows 0..6 exactly once —
        // admission requires every row in [0, moving_count) to be present — and
        // the gas row carries no source row. Committed positions are world cells;
        // the published origin of 3.0 cells subtracts out, so 3.5 cells is 0.5
        // world metres.
        Scene scene;
        const std::array specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = 3.5f,
                .m_y = 3.5f,
                .m_displacement_x = 0.1f,
                .m_displacement_y = -0.25f},
            ParticleSpec{
                .m_id = 2u, .m_source_row = 1u, .m_material = kOpaque, .m_x = 4.5f, .m_y = 3.5f, .m_enabled = false},
            ParticleSpec{
                .m_id = 3u,
                .m_source_row = 2u,
                .m_material = kOpaque,
                .m_x = 5.5f,
                .m_y = 3.5f,
                .m_displacement_x = 128.0f,
                .m_displacement_y = -128.0f},
            ParticleSpec{
                .m_id = 4u,
                .m_source_row = 3u,
                .m_material = kOpaque,
                .m_x = 6.5f,
                .m_y = 3.5f,
                .m_displacement_x = kNotANumber,
                .m_displacement_y = 0.5f},
            ParticleSpec{.m_id = 5u, .m_material = kEmissiveLiquid, .m_gas = true, .m_gas_x = 1.5f, .m_gas_y = 2.5f},
            ParticleSpec{
                .m_id = 6u,
                .m_source_row = 4u,
                .m_material = kReceiveOnly,
                .m_x = 7.5f,
                .m_y = 3.5f,
                .m_displacement_x = 0.5f},
            ParticleSpec{.m_id = 7u, .m_source_row = 5u, .m_material = kOverRangeMaterial, .m_x = 8.5f, .m_y = 3.5f},
            ParticleSpec{.m_id = 8u, .m_source_row = 6u, .m_material = kOpaque, .m_x = kNotANumber, .m_y = 3.5f},
        };
        scene.publish(fixture, specs);

        const std::array visuals{
            gridVisual(kOpaque, 0xff8040ffu, 0u),
            gridVisual(kReceiveOnly, 0x204080ffu, kGridSurfaceOpticalBlend),
            gridVisual(kEmissiveLiquid, 0x40a0c0ffu, kGridSurfaceParticleLiquid),
            gridVisual(kOverRangeMaterial, 0xffffffffu, 0u),
        };

        GridPass grid;
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        GridPass::GridStagedFrame stage{};
        stage.m_world_edge = kEdge;
        stage.m_cell_size_m = 1.0f;
        stage.m_terrain_visuals.assign(visuals.begin(), visuals.end());
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = GridTileRange{0, 0, 128, 128}, .m_chunk_id = 0u});
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        auto view = scene.m_inputs.m_particles;
        view.m_world = grid.activeWorldIdentity();
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_ortho_scale = 2048.0f;
        Camera camera;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{256, 256}}, ViewportLayout{}});
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not grid.prepareGpuParticles(context));
            submit(fixture, *encoder);
        }

        const auto buffers = grid.particleBuffersForTest();
        PPR_TEST_ASSERT(buffers[0] != nullptr and buffers[1] != nullptr and buffers[2] != nullptr);
        PPR_TEST_ASSERT(sizeof(ParticleGpu) == 24u and alignof(ParticleGpu) == 8u);
        PPR_TEST_ASSERT(buffers[0]->getDesc().elementSize == sizeof(ParticleGpu));

        std::array<ParticleGpu, specs.size()> rows{};
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(buffers[0], 0u, rows.size() * sizeof(ParticleGpu), rows.data())));
        std::array<u32, 4u> args{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(buffers[1], 0u, sizeof(args), args.data())));
        std::array<u32, 8u> ids{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(buffers[2], 0u, sizeof(ids), ids.data())));

        // Only enabled, finite, in-domain rows are compacted. The indirect
        // instance count is therefore the rejection count, and a rejected id can
        // never appear in the draw list.
        PPR_TEST_ASSERT(args[0u] == 6u);
        PPR_TEST_ASSERT(args[1u] == 5u);
        const std::array<u32, 5u> expected_ids{0u, 2u, 3u, 4u, 5u};
        for (u32 index = 0u; index < expected_ids.size(); ++index) {
            PPR_TEST_ASSERT(ids[index] == expected_ids[index]);
        }

        // Row 0: world metres (not cells) with every binary32 quantum intact,
        // the derived physical radius, no class flag and a bit-exact canonical
        // material word.
        PPR_TEST_ASSERT(rows[0u].m_position[0u] == 0.5f and rows[0u].m_position[1u] == 0.5f);
        PPR_TEST_ASSERT(std::abs(rows[0u].m_radius_m - 1.0f) <= 1.0e-6f);
        PPR_TEST_ASSERT((rows[0u].m_material_flags_temperature & 0xfffu) == kOpaque);
        PPR_TEST_ASSERT(((rows[0u].m_material_flags_temperature >> 12u) & 0xfu) == 0u);
        const u32 canonical = (rows[0u].m_material_flags_temperature & 0xfffu) |
                              (rows[0u].m_material_flags_temperature & 0xffff'0000u);
        PPR_TEST_ASSERT(canonical == *lighting::packCell(kOpaque, 300.0));
        PPR_TEST_ASSERT(motionMatches(rows[0u].m_motion[0u], 0.1f));
        PPR_TEST_ASSERT(motionMatches(rows[0u].m_motion[1u], -0.25f));

        // The tint is the authored palette colour within the RGBA8 source's own
        // one-byte quantum; alpha was never stored.
        const float3 decoded_tint = lighting::unpackRGB9E5(rows[0u].m_color_rgb9e5);
        const float3 authored_tint = paletteTint(visuals[0u].m_color_rgba);
        const std::array<float, 3u> decoded{decoded_tint.x, decoded_tint.y, decoded_tint.z};
        const std::array<float, 3u> authored{authored_tint.x, authored_tint.y, authored_tint.z};
        for (u32 channel = 0u; channel < authored.size(); ++channel) {
            PPR_TEST_ASSERT(std::abs(decoded[channel] - authored[channel]) <= 1.0f / 255.0f);
        }

        // Row 2 clamps to exactly +/-64 native cells per step.
        PPR_TEST_ASSERT(::pP::details::decodeHalf(rows[2u].m_motion[0u]) == 64.0f);
        PPR_TEST_ASSERT(::pP::details::decodeHalf(rows[2u].m_motion[1u]) == -64.0f);
        // Row 3 sanitises only the non-finite component to 0.
        PPR_TEST_ASSERT(rows[3u].m_motion[0u] == 0u);
        PPR_TEST_ASSERT(motionMatches(rows[3u].m_motion[1u], 0.5f));

        // Row 4 is a gas row: metres from the metadata, no displacement, and the
        // gas class bit set beside its material id.
        PPR_TEST_ASSERT(rows[4u].m_position[0u] == 1.5f and rows[4u].m_position[1u] == 2.5f);
        PPR_TEST_ASSERT(rows[4u].m_radius_m == 0.5f);
        PPR_TEST_ASSERT(((rows[4u].m_material_flags_temperature >> 12u) & 0xfu) == 1u);
        PPR_TEST_ASSERT(rows[4u].m_motion[0u] == 0u and rows[4u].m_motion[1u] == 0u);
        // Row 5 derives the optical-blend class bit from its own visual.
        PPR_TEST_ASSERT(((rows[5u].m_material_flags_temperature >> 12u) & 0xfu) == 2u);

        // Disabled, out-of-domain material and non-finite position rows are
        // rejected wholesale: a truncated 4097 would have decoded as material 1.
        const ParticleGpu rejected{};
        const auto isRejected = [&](const u32 row) {
            return std::memcmp(&rows[row], &rejected, sizeof(ParticleGpu)) == 0;
        };
        PPR_TEST_ASSERT((kOverRangeMaterial & 0xfffu) == kOpaque);
        PPR_TEST_ASSERT(isRejected(1u));
        PPR_TEST_ASSERT(isRejected(6u));
        PPR_TEST_ASSERT(isRejected(7u));

        // Nothing in the draw list can carry a non-finite coordinate or a
        // non-positive radius into a clip position.
        for (u32 index = 0u; index < args[1u]; ++index) {
            const ParticleGpu &row = rows[ids[index]];
            PPR_TEST_ASSERT(std::isfinite(row.m_position[0u]) and std::isfinite(row.m_position[1u]));
            PPR_TEST_ASSERT(std::isfinite(row.m_radius_m) and row.m_radius_m > 0.0f);
        }

        _.log(
            std::format(
                "WORLD_LIGHTING_PROJECTION presentation_row bytes={} instances={} rejected={} tint=({:.4f},{:.4f},"
                "{:.4f}) motion=({:.5f},{:.5f})",
                sizeof(ParticleGpu), args[1u], specs.size() - args[1u], decoded[0u], decoded[1u], decoded[2u],
                ::pP::details::decodeHalf(rows[0u].m_motion[0u]), ::pP::details::decodeHalf(rows[0u].m_motion[1u])));
    };

    PPR_UNIT_TEST (packed_presentation_row_pins_world_metres_at_the_production_cell_size) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        // 1/3 m is the production cell, and it is the case where a cell-space
        // packer and a metres-space packer disagree: hot 3.5 cells is 0.5 native
        // cells past the origin, so the row must hold 0.5 * cell_metres metres and
        // a physical radius of cell_metres * sqrt(V/pi) metres. A packer that
        // stored cells, or dropped the cell scale, would write 0.5 and 1.0.
        // Published as the binary64 1/3 the metadata admission accepts; the pack
        // shader receives its binary32 rounding.
        constexpr double kCellMetresPublished = 1.0 / 3.0;
        constexpr float kCellMetres = 1.0f / 3.0f;
        constexpr float kMotionCells = 0.5f;

        Scene scene;
        const std::array specs{
            ParticleSpec{
                .m_id = 1u,
                .m_source_row = 0u,
                .m_material = kOpaque,
                .m_x = 3.5f,
                .m_y = 3.0f,
                .m_displacement_x = kMotionCells},
        };
        scene.publish(fixture, specs, kCellMetresPublished);

        const std::array visuals{gridVisual(kOpaque, 0xff8040ffu, 0u)};

        GridPass grid;
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        GridPass::GridStagedFrame stage{};
        stage.m_world_edge = kEdge;
        stage.m_cell_size_m = kCellMetres;
        stage.m_terrain_visuals.assign(visuals.begin(), visuals.end());
        stage.m_tiles.push_back(
            GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = GridTileRange{0, 0, 128, 128}, .m_chunk_id = 0u});
        PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

        auto view = scene.m_inputs.m_particles;
        view.m_world = grid.activeWorldIdentity();
        PPR_TEST_ASSERT(not grid.applyGpuParticleView(view));

        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_ortho_scale = 2048.0f;
        Camera camera;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{256, 256}}, ViewportLayout{}});
        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not grid.prepareGpuParticles(context));
            submit(fixture, *encoder);
        }

        const auto buffers = grid.particleBuffersForTest();
        PPR_TEST_ASSERT(buffers[0] != nullptr and buffers[1] != nullptr and buffers[2] != nullptr);
        std::array<ParticleGpu, specs.size()> rows{};
        PPR_TEST_ASSERT(not make_error_code(
            fixture.device().readBuffer(buffers[0], 0u, rows.size() * sizeof(ParticleGpu), rows.data())));
        std::array<u32, 4u> args{};
        PPR_TEST_ASSERT(not make_error_code(fixture.device().readBuffer(buffers[1], 0u, sizeof(args), args.data())));
        PPR_TEST_ASSERT(args[1u] == 1u);

        // 0.5 native cells at the 1/3 m cell edge. The shader and this expectation
        // perform the same single binary32 multiply, so the comparison is exact.
        const float expected_x = 0.5f * kCellMetres;
        PPR_TEST_ASSERT(rows[0u].m_position[0u] == expected_x);
        PPR_TEST_ASSERT(rows[0u].m_position[1u] == 0.0f);
        // Cold volume is pi cells^2, so the physical radius is h*sqrt(pi/pi) = h
        // metres. A cell-space radius would have stored 1.0.
        PPR_TEST_ASSERT(std::abs(rows[0u].m_radius_m - kCellMetres) <= 1.0e-7f);
        PPR_TEST_ASSERT(rows[0u].m_position[0u] < 0.25f and rows[0u].m_radius_m < 0.5f);
        // The motion proxy stays in native cells per solver step: no cell scale.
        PPR_TEST_ASSERT(motionMatches(rows[0u].m_motion[0u], kMotionCells));

        _.log(
            std::format(
                "WORLD_LIGHTING_PROJECTION presentation_row_cell_sizes metres=({:.6f},{:.6f}) radius={:.6f} "
                "motion={:.5f} cell={:.6f}",
                rows[0u].m_position[0u], rows[0u].m_position[1u], rows[0u].m_radius_m,
                ::pP::details::decodeHalf(rows[0u].m_motion[0u]), kCellMetres));
    };

    // ------------------------------------------------------------------
    // C-stage inspection reader fences
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (inspection_read_extends_only_the_decoded_attempt) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize(_);

        Scene scene;
        scene.publish(fixture, std::span<const ParticleSpec>{});
        WorldLightingPass pass;
        PPR_TEST_ASSERT(not pass.initialize(*fixture.m_shaders, std::filesystem::current_path()));

        // Attempt 0 is accepted at 1 but has no C-stage read and is never
        // collected yet. A second attempt is accepted at 3 and is the one a
        // C-stage overlay actually reads at 4.
        auto first = encodeAttempt(fixture, pass, scene.m_inputs);
        PPR_TEST_ASSERT(not first.m_prepare and not first.m_finalize);
        pass.notifySubmitted(1u);

        auto second = encodeAttempt(fixture, pass, scene.m_inputs, 1u);
        PPR_TEST_ASSERT(not second.m_prepare and not second.m_finalize);
        pass.notifySubmitted(3u);
        pass.notifyReadSubmitted(4u);

        // Only attempt 1 was decoded: attempt 0 must still complete at its own
        // write fence and release. An implementation extending every pending
        // attempt with submission <= 4 would leave slot 0 pending here.
        PPR_TEST_ASSERT(not pass.collectCompleted(1u));
        PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(0u));
        PPR_TEST_ASSERT(pass.statusSlotPendingForTest(1u));

        // The decoded attempt remains retained until its read fence completes.
        PPR_TEST_ASSERT(not pass.collectCompleted(3u));
        PPR_TEST_ASSERT(pass.statusSlotPendingForTest(1u));
        PPR_TEST_ASSERT(not pass.collectCompleted(4u));
        PPR_TEST_ASSERT(not pass.statusSlotPendingForTest(1u));
        PPR_TEST_ASSERT(not pass.shutdown());
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // categorical projection suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_projectionTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_projection") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingProjection::terrain_precedence_wins_over_a_particle_candidate,
                detail::WorldLightingProjection::opaque_precedes_receive_only_transparent_and_nearest_center_wins,
                detail::WorldLightingProjection::exact_distance_tie_uses_stable_id_rank_not_numerical_source_order,
                detail::WorldLightingProjection::gas_inactive_and_removed_candidates_never_leave_a_stale_cell,
                detail::WorldLightingProjection::zero_one_and_dense_overlaps_rebuild_without_particle_readback,
                detail::WorldLightingProjection::maximum_endpoint_maps_to_last_cell_and_invalid_positions_are_rejected,
                detail::WorldLightingProjection::invalid_and_range_minima_stay_separate_and_reach_the_caller,
                detail::WorldLightingProjection::status_slots_require_completion_and_consumption_before_reuse,
                detail::WorldLightingProjection::
                    second_prepare_before_submission_is_rejected_without_disturbing_the_attempt,
                detail::WorldLightingProjection::shutdown_reports_an_uninspected_pending_error,
                detail::WorldLightingProjection::fill_reduction_sums_each_candidate_once_and_keeps_the_winner,
                detail::WorldLightingProjection::source_scales_with_cell_fill_and_is_bit_identical_at_full_fill,
                detail::WorldLightingProjection::cell_source_never_accumulates_per_particle,
                detail::WorldLightingProjection::two_quarter_filled_cells_add_up_to_one_half_filled_cell,
                detail::WorldLightingProjection::fill_factor_leaves_gas_receive_only_terrain_and_legacy_cells_alone,
                detail::WorldLightingProjection::reflected_projection_records_match_the_cpu_shapes,
                detail::WorldLightingProjection::grid_pass_owns_the_projection_and_binds_resolved_cells,
                detail::WorldLightingProjection::packed_presentation_row_quantises_and_filters_rows,
                detail::WorldLightingProjection::packed_presentation_row_pins_world_metres_at_the_production_cell_size,
                detail::WorldLightingProjection::inspection_read_extends_only_the_decoded_attempt,
            });
        };
        return group;
    }
}
