module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"
#include <slang.h>

module engine.tests.app;

import std;
import engine.core;
import engine.app;
import engine.math;
import engine.rhi;
import engine.shader;

namespace pP::tests::detail::WorldLightingSettlement {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(Settlement, debug, none)

    // ------------------------------------------------------------------
    // fixture: one private shader/RHI service pair per leaf
    // ------------------------------------------------------------------

    struct Fixture {
        safe_ptr<IShaderService> m_shaders{IShaderService::get()};
        safe_ptr<IRhiService> m_rhi{IRhiService::get()};
        rhi::ComPtr<rhi::ICommandQueue> m_queue{};

        void initialize() {
            PPR_TEST_ASSERT(not m_shaders->initialize());
#ifdef _WIN32
            constexpr auto backend = rhi::DeviceType::D3D12;
#else
            constexpr auto backend = rhi::DeviceType::Vulkan;
#endif
            PPR_TEST_ASSERT(not m_rhi->initialize(backend, *m_shaders));
            PPR_TEST_ASSERT(
                not make_error_code(m_rhi->getDevice().getQueue(rhi::QueueType::Graphics, m_queue.writeRef())));

            const rhi::DeviceInfo &info = m_rhi->getDevice().getInfo();
            std::println(
                "WORLD_LIGHTING_SETTLEMENT api={} adapter={}", info.apiName != nullptr ? info.apiName : "unknown",
                info.adapterName != nullptr ? info.adapterName : "unknown");
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

    void submit(Fixture &fixture, rhi::ICommandEncoder &encoder) {
        rhi::ComPtr<rhi::ICommandBuffer> commands;
        PPR_TEST_ASSERT(not make_error_code(encoder.finish(commands.writeRef())));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->submit(commands)));
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->waitOnHost()));
    }

    [[nodiscard]] rhi::ComPtr<rhi::ICommandEncoder> makeEncoder(Fixture &fixture) {
        rhi::ComPtr<rhi::ICommandEncoder> encoder;
        PPR_TEST_ASSERT(not make_error_code(fixture.m_queue->createCommandEncoder(encoder.writeRef())));
        return encoder;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeOffscreenTarget(rhi::IDevice &device) {
        rhi::TextureDesc target{};
        target.type = rhi::TextureType::Texture2D;
        target.size = {64u, 64u, 1u};
        target.arrayLength = 1u;
        target.mipCount = 1u;
        target.format = rhi::Format::RGBA8Unorm;
        target.sampleCount = 1u;
        target.memoryType = rhi::MemoryType::DeviceLocal;
        target.usage = rhi::TextureUsage::RenderTarget;
        target.defaultState = rhi::ResourceState::RenderTarget;
        target.label = "settlement offscreen target";
        rhi::ComPtr<rhi::ITexture> texture;
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(target, nullptr, texture.writeRef())));
        return texture;
    }

    [[nodiscard]] rhi::ComPtr<rhi::ITexture> makeResolvedTarget(rhi::IDevice &device) {
        rhi::TextureDesc target{};
        target.type = rhi::TextureType::Texture2D;
        target.size = {64u, 64u, 1u};
        target.arrayLength = 1u;
        target.mipCount = 1u;
        target.format = rhi::Format::RGBA16Float;
        target.sampleCount = 1u;
        target.memoryType = rhi::MemoryType::DeviceLocal;
        target.usage = static_cast<rhi::TextureUsage>(
            static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::ShaderResource));
        target.defaultState = rhi::ResourceState::ShaderResource;
        target.label = "settlement resolved source";
        rhi::ComPtr<rhi::ITexture> texture;
        PPR_TEST_ASSERT(not make_error_code(device.createTexture(target, nullptr, texture.writeRef())));
        return texture;
    }

    // ------------------------------------------------------------------
    // stage result values and the submission credit bound (CPU)
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (render_stage_result_is_value_only_ordered_and_aggregated) {
        PPR_TEST_ASSERT(kRenderStageScene == 0u);
        PPR_TEST_ASSERT(kRenderStageResolve == 1u);
        PPR_TEST_ASSERT(kRenderStagePresent == 2u);
        PPR_TEST_ASSERT(kRenderStageCount == 3u);
        PPR_TEST_ASSERT(sizeof(RenderStageResult) == 32u);
        PPR_TEST_ASSERT(alignof(RenderStageResult) == 8u);
        PPR_TEST_ASSERT(std::is_trivially_copyable_v<RenderStageResult>);

        RenderFrameResult result{};
        PPR_TEST_ASSERT(result.m_stages.size() == kRenderStageCount);
        for (const RenderStageResult &stage: result.m_stages) {
            PPR_TEST_ASSERT(stage.m_submission == kFenceValueNone);
            PPR_TEST_ASSERT(stage.m_settlement == FrameSettlement::kRejected);
            PPR_TEST_ASSERT(not stage.m_error);
        }

        PPR_TEST_ASSERT(aggregateAcceptedSubmission(result.m_stages) == kFenceValueNone);
        PPR_TEST_ASSERT(result.m_submit_value == kFenceValueNone);
        PPR_TEST_ASSERT(not result.m_submit_possibly_executed);

        // A accepted, B rejected, C never attempted: the aggregate retains
        // A's value and the failing stage keeps its error. Aggregation is the
        // production helper, not a test-local restatement.
        result.m_stages[kRenderStageScene].m_submission = 11u;
        result.m_stages[kRenderStageScene].m_settlement = FrameSettlement::kSubmitted;
        result.m_stages[kRenderStageResolve].m_error = std::make_error_code(std::errc::device_or_resource_busy);
        result.m_submit_value = aggregateAcceptedSubmission(result.m_stages);
        PPR_TEST_ASSERT(result.m_submit_value == 11u);
        PPR_TEST_ASSERT(
            result.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected and
            result.m_stages[kRenderStagePresent].m_submission == kFenceValueNone);

        // A+B accepted, C rejected: B's value is the latest accepted one.
        result.m_stages[kRenderStageResolve].m_submission = 12u;
        result.m_stages[kRenderStageResolve].m_settlement = FrameSettlement::kSubmitted;
        result.m_stages[kRenderStagePresent].m_error = std::make_error_code(std::errc::resource_unavailable_try_again);
        result.m_submit_value = aggregateAcceptedSubmission(result.m_stages);
        PPR_TEST_ASSERT(result.m_submit_value == 12u);

        // An ambiguous stage is never an accepted value.
        RenderFrameResult ambiguous{};
        ambiguous.m_stages[kRenderStageScene].m_settlement = FrameSettlement::kSubmitFailed;
        PPR_TEST_ASSERT(aggregateAcceptedSubmission(ambiguous.m_stages) == kFenceValueNone);
    };

    PPR_UNIT_TEST (credit_bound_covers_two_logical_frames_of_three_stages) {
        // The renderer bounds stage SUBMISSIONS at the same two logical frames
        // the application construction reservations allow.
        PPR_TEST_ASSERT(kMaxOutstandingFrames == 2u);
        PPR_TEST_ASSERT(kFrameStageCount == kRenderStageCount);
        PPR_TEST_ASSERT(kMaxOutstandingSubmissions == kMaxOutstandingFrames * kFrameStageCount);
        PPR_TEST_ASSERT(FrameCreditLedger::capacity() == kMaxOutstandingSubmissions);

        FrameCreditLedger ledger{};
        for (u32 stage = 0u; stage < kFrameStageCount; ++stage) {
            PPR_TEST_ASSERT(not ledger.acquire());
        }
        PPR_TEST_ASSERT(ledger.outstanding() == kRenderStageCount);

        // The next logical frame's A/B/C would still fit; a third logical
        // frame does not.
        while (ledger.outstanding() != ledger.capacity()) {
            PPR_TEST_ASSERT(not ledger.acquire());
        }
        PPR_TEST_ASSERT(ledger.acquire() == std::make_error_code(std::errc::device_or_resource_busy));
        for (u32 credit = 0u; credit < ledger.capacity(); ++credit) {
            PPR_TEST_ASSERT(not ledger.release());
        }
        PPR_TEST_ASSERT(ledger.outstanding() == 0u);
    };

    // ------------------------------------------------------------------
    // ordered submissions and their independent settlement (real device)
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (three_ordered_submissions_settle_one_value_each) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(fixture.device());
        PPR_DEFER {
            target.setNull();
            PPR_TEST_ASSERT(not renderer.shutdown());
        };

        std::array<RenderStageResult, kRenderStageCount> stages{};
        FenceCompletionValue before = renderer.lastSubmittedFenceValue();
        for (u32 stage = 0u; stage < kRenderStageCount; ++stage) {
            // Settlement evidence is read immediately after the call and before
            // any later call can overwrite it.
            const std::error_code error = renderer.renderToTexture(*target, {});
            RenderStageResult &result = stages[stage];
            result.m_error = error;
            result.m_settlement = renderer.lastSubmitSettlement();
            if (not error and result.m_settlement == FrameSettlement::kSubmitted) {
                result.m_submission = renderer.lastSubmittedFenceValue();
            }
            PPR_TEST_ASSERT(result.m_settlement == FrameSettlement::kSubmitted);
            PPR_TEST_ASSERT(result.m_submission > before);
            before = result.m_submission;
        }

        PPR_TEST_ASSERT(stages[kRenderStageScene].m_submission < stages[kRenderStageResolve].m_submission);
        PPR_TEST_ASSERT(stages[kRenderStageResolve].m_submission < stages[kRenderStagePresent].m_submission);
        PPR_TEST_ASSERT(aggregateAcceptedSubmission(stages) == stages[kRenderStagePresent].m_submission);

        PPR_TEST_ASSERT(not renderer.waitOnHost());
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        PPR_TEST_ASSERT(renderer.completedFenceValue() >= stages[kRenderStagePresent].m_submission);
    };

    PPR_UNIT_TEST (capacity_busy_later_stage_keeps_the_earlier_accepted_value) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(fixture.device());
        bool torn_down = false;
        PPR_DEFER {
            renderer.setTestHoldSweep(false);
            if (not torn_down) {
                std::ignore = renderer.waitForFenceValue(renderer.lastSubmittedFenceValue());
                std::ignore = renderer.retireCompletedFrames();
                target.setNull();
                std::ignore = renderer.shutdown();
            } else {
                target.setNull();
            }
        };

        renderer.setTestHoldSweep(true);

        // Stage A of this logical frame is accepted while every earlier credit
        // is still held.
        RenderStageResult scene{};
        scene.m_error = renderer.renderToTexture(*target, {});
        scene.m_settlement = renderer.lastSubmitSettlement();
        scene.m_submission = renderer.lastSubmittedFenceValue();
        PPR_TEST_ASSERT(scene.m_settlement == FrameSettlement::kSubmitted);

        // Saturate the submission bound with unrelated pending stages.
        while (renderer.outstandingFrameCount() != kMaxOutstandingSubmissions) {
            PPR_TEST_ASSERT(not renderer.renderToTexture(*target, {}));
        }

        // The later stage is refused as ordinary capacity backpressure: no
        // value advances, no submission is retried in place.
        const FenceCompletionValue frontier = renderer.lastSubmittedFenceValue();
        RenderStageResult resolve{};
        resolve.m_error = renderer.renderToTexture(*target, {});
        resolve.m_settlement = renderer.lastSubmitSettlement();
        PPR_TEST_ASSERT(resolve.m_error == std::make_error_code(std::errc::device_or_resource_busy));
        PPR_TEST_ASSERT(resolve.m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(resolve.m_submission == kFenceValueNone);
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == frontier);

        // The frame's aggregate stays at A's actual value; A's commit is not
        // rolled back by the later rejection.
        std::array stages{scene, resolve, RenderStageResult{}};
        PPR_TEST_ASSERT(aggregateAcceptedSubmission(stages) == scene.m_submission);

        renderer.setTestHoldSweep(false);
        PPR_TEST_ASSERT(not renderer.waitForFenceValue(frontier));
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        target.setNull();
        PPR_TEST_ASSERT(not renderer.shutdown());
        torn_down = true;
    };

    PPR_UNIT_TEST (ambiguous_submit_quarantines_and_stops_admission) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        Renderer renderer{};
        PPR_TEST_ASSERT(not renderer.initialize(*fixture.m_rhi));
        rhi::ComPtr<rhi::ITexture> target = makeOffscreenTarget(fixture.device());
        bool torn_down = false;
        PPR_DEFER {
            renderer.setTestHoldSweep(false);
            if (not torn_down) {
                std::ignore = renderer.waitForFenceValue(renderer.lastSubmittedFenceValue());
                std::ignore = renderer.retireCompletedFrames();
                target.setNull();
                std::ignore = renderer.shutdown();
            } else {
                target.setNull();
            }
        };

        // Stage A accepted; stage B possibly executed (no committed value).
        RenderStageResult scene{};
        scene.m_error = renderer.renderToTexture(*target, {});
        scene.m_settlement = renderer.lastSubmitSettlement();
        scene.m_submission = renderer.lastSubmittedFenceValue();
        PPR_TEST_ASSERT(scene.m_settlement == FrameSettlement::kSubmitted);

        renderer.setInjectedSubmitFailure(true);
        RenderStageResult resolve{};
        resolve.m_error = renderer.renderToTexture(*target, {});
        resolve.m_settlement = renderer.lastSubmitSettlement();
        PPR_TEST_ASSERT(resolve.m_error != std::error_code{}); // The invocation's own error rides the stage.
        PPR_TEST_ASSERT(resolve.m_settlement == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(resolve.m_submission == kFenceValueNone);
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == scene.m_submission);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::state_not_recoverable));

        // No retry of the ambiguous stage, and the aggregate keeps A's value.
        std::array stages{scene, resolve, RenderStageResult{}};
        PPR_TEST_ASSERT(aggregateAcceptedSubmission(stages) == scene.m_submission);

        // Idle proof returns the accepted stage's credit; the ambiguous
        // stage's credit stays held (never reused) until shutdown settles the
        // quarantine.
        renderer.setTestHoldSweep(false);
        PPR_TEST_ASSERT(not renderer.waitOnHost());
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 1u);
        target.setNull();
        PPR_TEST_ASSERT(not renderer.shutdown());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        torn_down = true;
    };

    // ------------------------------------------------------------------
    // publication-epoch admission and stage-A lighting commitment
    // ------------------------------------------------------------------

    struct GridScene {
        rhi::ComPtr<rhi::IBuffer> m_terrain{};
        u32 m_terrain_words{};

        // One staged chunk is enough for the publication owners world lighting
        // consumes; the projection still covers its full 1024-square domain.
        static void stageChunk(GridPass &grid, Fixture &fixture, GridScene &scene) {
            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xffff'ffffu;

            GridPass::GridStagedFrame stage{};
            stage.m_world_edge = WorldLightingPass::kWorldEdge;
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
            stage.m_uploads.push_back(std::move(upload));
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto prepared = grid.tryPrepareCellUploads(fixture.device(), kFenceValueNone);
            PPR_TEST_ASSERT(prepared.has_value() and *prepared);
            scene.m_terrain_words = WorldLightingPass::kWorldEdge;
        }
    };

    PPR_UNIT_TEST (accepted_scene_commits_lighting_once_and_discard_commits_nothing) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        GridScene scene;
        GridScene::stageChunk(grid, fixture, scene);
        PPR_TEST_ASSERT(grid.lightingInputsAvailable());

        // Stage A encodes the projection/transport attempt in its own
        // submission; the numeric-status tail rides the same encoder.
        const auto encodeScene = [&](const FenceCompletionValue completed) -> std::error_code {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, completed, {}};
            PPR_RETURN_ERROR_ON_FAIL(Settlement, grid.prepareWorldLighting(context, WorldLightingFrame{}));
            PPR_RETURN_ERROR_ON_FAIL(Settlement, grid.finalizeWorldLighting(context));
            submit(fixture, *encoder);
            return {};
        };

        // A1 accepted: the attempt commit is exactly once.
        PPR_TEST_ASSERT(not encodeScene(kFenceValueNone));
        WorldLightingPass *const lighting = grid.activeWorldLightingForTest();
        PPR_TEST_ASSERT(lighting != nullptr);
        grid.notifyWorldLightingSubmitted(1u);
        PPR_TEST_ASSERT(not grid.collectWorldLightingCompleted(1u));
        PPR_TEST_ASSERT(lighting->acceptedFrameForTest() == 1u);

        // A2 definitely rejected: the armed attempt is discarded, nothing is
        // committed, and the previous accepted frame/phase remains.
        PPR_TEST_ASSERT(not encodeScene(1u));
        grid.discardWorldLightingUnsubmitted();
        PPR_TEST_ASSERT(lighting->acceptedFrameForTest() == 1u);

        // A3 accepted: the phase advances exactly once from the retained one.
        PPR_TEST_ASSERT(not encodeScene(1u));
        grid.notifyWorldLightingSubmitted(2u);
        PPR_TEST_ASSERT(not grid.collectWorldLightingCompleted(2u));
        PPR_TEST_ASSERT(lighting->acceptedFrameForTest() == 2u);
    };

    PPR_UNIT_TEST (stale_publication_epoch_is_rejected_before_encode) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not grid.shutdown());
        };

        GridScene scene;
        GridScene::stageChunk(grid, fixture, scene);

        const auto makeViewBuffer = [&](const u32 stride) {
            rhi::BufferDesc description{};
            description.size = stride;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = rhi::BufferUsage::ShaderResource;
            description.defaultState = rhi::ResourceState::ShaderResource;
            rhi::ComPtr<rhi::IBuffer> buffer;
            PPR_TEST_ASSERT(
                not make_error_code(fixture.device().createBuffer(description, nullptr, buffer.writeRef())));
            return buffer;
        };

        auto live = std::make_shared<std::atomic<u64>>(1u);
        GridParticleGpuView view{};
        view.m_numerical = makeViewBuffer(kGridParticleNumericalStride);
        view.m_readonly = makeViewBuffer(kGridParticleReadonlyStride);
        view.m_live_epoch = live;
        view.m_world = grid.activeWorldIdentity();
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;
        view.m_origin = 3.0;
        PPR_TEST_ASSERT(view.current());
        PPR_TEST_ASSERT(grid.gpuParticleFrameCurrent(view));
        PPR_TEST_ASSERT(grid.gpuParticleFrameCurrent(std::nullopt));

        // The publisher released a newer epoch after this view was captured:
        // the un-encoded view is rejected rather than rebound to the new bytes.
        live->store(2u, std::memory_order_release);
        PPR_TEST_ASSERT(not view.current());
        PPR_TEST_ASSERT(not grid.gpuParticleFrameCurrent(view));
    };

    // ------------------------------------------------------------------
    // stage-B acceptance and the retained picture
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (rejected_resolve_preserves_exposure_and_retained_picture) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        PostProcessPass post{};
        PPR_TEST_ASSERT(not post.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not post.shutdown());
        };

        const GridWorldIdentity world{1u, 1u, 1u};
        rhi::ComPtr<rhi::ITexture> resolved = makeResolvedTarget(fixture.device());
        rhi::ComPtr<rhi::ITextureView> resolved_view;
        PPR_TEST_ASSERT(not make_error_code(resolved->getDefaultView(resolved_view.writeRef())));
        const int2 extent{64, 64};
        const ResolvedHdrView view{
            .m_view = resolved_view,
            .m_world = world,
            .m_backing = extent,
            .m_valid = extent,
        };
        const PostProcessSettings settings{};
        const TimeSpan active_elapsed = std::chrono::milliseconds{250};

        PPR_TEST_ASSERT(not post.beginFrame(fixture.device(), extent, world, kFenceValueNone));
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not post.prepare(context, view, settings, active_elapsed));
            submit(fixture, *encoder);
        }

        PPR_TEST_ASSERT(not post.retainedPictureAvailable(extent, world));
        post.notifySubmitted(1u);
        PPR_TEST_ASSERT(post.retainedPictureAvailable(extent, world));
        PPR_TEST_ASSERT(not post.retainedPictureAvailable(int2{32, 64}, world));
        PPR_TEST_ASSERT(not post.retainedPictureAvailable(extent, GridWorldIdentity{2u, 1u, 1u}));

        // A later resolve attempt is definitely rejected: it commits no
        // exposure role and the previously accepted picture stays presentable.
        PPR_TEST_ASSERT(not post.beginFrame(fixture.device(), extent, world, 1u));
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, 1u, {}};
            PPR_TEST_ASSERT(not post.prepare(context, view, settings, active_elapsed));
        }
        post.discardUnsubmitted();
        PPR_TEST_ASSERT(post.retainedPictureAvailable(extent, world));
        post.selectRetainedPicture(extent);

        // Stage C presenting that retained picture extends its reader fence and
        // commits no new exposure/history state.
        post.notifyPresented(2u);
        PPR_TEST_ASSERT(post.retainedPictureAvailable(extent, world));
        PPR_TEST_ASSERT(not post.collectCompleted(2u));
        PPR_TEST_ASSERT(post.retainedPictureAvailable(extent, world));
    };

    // ------------------------------------------------------------------
    // editor-level A/B/C settlement (real window, inline reference lane)
    // ------------------------------------------------------------------

    struct SettlementEditorApp : ApplicationEditor {
        std::optional<RenderFrameResult> m_last_result{};

        explicit SettlementEditorApp(const std::span<const char *const> argv)
            : ApplicationEditor("SettlementEditor", argv) {
            setRenderLaneEnabled(false);
        }

        [[nodiscard]] std::error_code boot() {
            return ApplicationEditor::initialize();
        }
        [[nodiscard]] std::error_code teardown() {
            return ApplicationEditor::shutdown();
        }
        [[nodiscard]] std::error_code tick(const TimeSpan dt) {
            return ApplicationEditor::update(dt);
        }
        [[nodiscard]] std::error_code draw() {
            return ApplicationEditor::render();
        }
        std::optional<GridWorldResult> m_last_world_result{};

        void onGridWorldCompleted_(const GridWorldResult &result) override {
            m_last_world_result = result;
        }
        void injectStageFailure(const std::optional<u32> stage) noexcept {
            setInjectedStageSubmitFailureForTest(stage);
        }

        void onLaneFrameConsumed_(const RenderFrameResult &result) override {
            ApplicationEditor::onLaneFrameConsumed_(result);
            m_last_result = result;
        }
    };

    [[nodiscard]] FenceCompletionValue stageValue(const RenderFrameResult &result, const u32 stage) noexcept {
        return result.m_stages[stage].m_submission;
    }

    // ------------------------------------------------------------------
    // shared editor staging helpers
    // ------------------------------------------------------------------

    void stageEditorChunk(SettlementEditorApp &app, const u32 chunk_id = 0u) {
        const i32 base = static_cast<i32>(chunk_id) * 128;
        GridPass::GridStagedFrame staged{};
        staged.m_world_edge = WorldLightingPass::kWorldEdge;
        staged.m_cell_size_m = 1.0f;
        staged.m_terrain_visuals.push_back(GridTerrainVisual{.m_material_id = 1u, .m_color_rgba = 0xffff'ffffu});
        staged.m_tiles.push_back(
            GridTileSubmission{
                .m_dirty_mask = 1u,
                .m_tile_range = GridTileRange{base, 0, base + 128, 128},
                .m_chunk_id = chunk_id,
            });
        GridPass::GridStagedUpload upload{};
        upload.m_chunk_id = chunk_id;
        upload.m_payload.m_rect[0] = static_cast<float>(base);
        upload.m_payload.m_rect[1] = 0.0f;
        upload.m_payload.m_rect[2] = static_cast<float>(base + 128);
        upload.m_payload.m_rect[3] = 128.0f;
        upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 1u);
        upload.m_cell_words.assign(GridPass::kCellsPerChunk, 0u);
        staged.m_uploads.push_back(std::move(upload));
        app.submitGridStage(std::move(staged));
    }

    void retainEditorTerrain(SettlementEditorApp &app) {
        GridPass::GridStagedFrame retained{};
        retained.m_retain_content = true;
        app.submitGridStage(std::move(retained));
    }

    void submitEditorWorld(SettlementEditorApp &app, const EGridWorldCommand kind, const bool final_batch) {
        auto command = std::make_shared<GridWorldCommand>();
        command->m_kind = kind;
        command->m_sequence = 2u;
        command->m_job_id = 1u;
        command->m_world_generation = 1u;
        command->m_content_fingerprint = 1u;
        command->m_final_batch = final_batch;
        if (kind == EGridWorldCommand::begin) {
            command->m_stage.m_world_edge = WorldLightingPass::kWorldEdge;
            command->m_stage.m_cell_size_m = 1.0f;
            command->m_stage.m_content_fingerprint = 1u;
            command->m_stage.m_terrain_visuals.push_back(
                GridTerrainVisual{.m_material_id = 1u, .m_color_rgba = 0xffff'ffffu});
            command->m_stage.m_tiles.push_back(
                GridTileSubmission{
                    .m_dirty_mask = 1u,
                    .m_tile_range = GridTileRange{128, 0, 256, 128},
                    .m_chunk_id = 1u,
                });
        } else if (kind == EGridWorldCommand::append) {
            GridPass::GridStagedUpload upload{};
            upload.m_chunk_id = 1u;
            upload.m_payload.m_rect[0] = 128.0f;
            upload.m_payload.m_rect[1] = 0.0f;
            upload.m_payload.m_rect[2] = 256.0f;
            upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 1u);
            upload.m_cell_words.assign(GridPass::kCellsPerChunk, 0u);
            command->m_stage.m_uploads.push_back(std::move(upload));
        }
        PPR_TEST_ASSERT(not app.submitGridWorld(std::move(command)));
        PPR_TEST_ASSERT(app.m_last_world_result.has_value());
        PPR_TEST_ASSERT(not app.m_last_world_result->m_error);
        PPR_TEST_ASSERT(app.m_last_world_result->m_applied);
    }

    void replaceEditorWorld(SettlementEditorApp &app) {
        submitEditorWorld(app, EGridWorldCommand::begin, false);
        submitEditorWorld(app, EGridWorldCommand::append, true);
        submitEditorWorld(app, EGridWorldCommand::advance, false);
        PPR_TEST_ASSERT(app.m_last_world_result->m_ready);
        submitEditorWorld(app, EGridWorldCommand::activate, false);
        PPR_TEST_ASSERT(app.getGridPass().activeWorldIdentity().m_job_id == 1u);
    }

    // Fill the submission ledger, leaving exactly `free` credits.
    void holdCreditsLeaving(
        Renderer &renderer, rhi::IDevice &device, const u32 free, rhi::ComPtr<rhi::ITexture> &filler) {
        renderer.setTestHoldSweep(true);
        filler = makeOffscreenTarget(device);
        PPR_TEST_ASSERT(filler);
        while (renderer.outstandingFrameCount() + free < FrameCreditLedger::capacity()) {
            PPR_TEST_ASSERT(not renderer.renderToTexture(*filler, {}));
        }
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() + free == FrameCreditLedger::capacity());
    }

    void releaseCredits(Renderer &renderer, rhi::ComPtr<rhi::ITexture> &filler) {
        renderer.setTestHoldSweep(false);
        PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        filler.setNull();
    }

    // ------------------------------------------------------------------
    // authoritative editor observation for non-contamination evidence
    // ------------------------------------------------------------------

    [[nodiscard]] std::vector<std::byte> readEditorTexture(rhi::IDevice &device, rhi::ITexture &texture) {
        shader::ComPtr<ISlangBlob> blob{};
        rhi::SubresourceLayout layout{};
        PPR_TEST_ASSERT(not make_error_code(device.readTexture(&texture, 0u, 0u, blob.writeRef(), &layout)));
        PPR_TEST_ASSERT(blob.get() != nullptr and blob->getBufferPointer() != nullptr);
        const auto *const source = static_cast<const std::byte *>(blob->getBufferPointer());
        return {source, source + blob->getBufferSize()};
    }

    // Encodes one stage-B attempt (reconstruction then post) in its own
    // submission and waits for it, so the test controls acceptance explicitly.
    struct ResolveAttempt {
        CameraSnapshot m_snapshot{};
        PresentationGrid m_grid{};
        GridWorldIdentity m_world{1u, 1u, 1u};
        TimeSpan m_active_elapsed{};
    };

    void encodeResolve(
        Fixture &fixture, ReconstructionPass &reconstruction, PostProcessPass &post, const ResolveAttempt &attempt,
        const FenceCompletionValue completed) {
        auto encoder = makeEncoder(fixture);
        PreRenderContext context{fixture.device(), *encoder, completed, {}};
        PPR_TEST_ASSERT(not reconstruction.prepare(context));
        PPR_TEST_ASSERT(
            not post.prepare(context, reconstruction.resolved(), PostProcessSettings{}, attempt.m_active_elapsed));
        submit(fixture, *encoder);
    }

    // ------------------------------------------------------------------
    // forced lighting-status inspection at pass shutdown
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (lighting_status_failure_is_observed_at_pass_shutdown) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        GridPass grid{};
        PPR_TEST_ASSERT(not grid.initialize(*fixture.m_rhi, *fixture.m_shaders, std::filesystem::current_path()));
        bool grid_live = true;
        PPR_DEFER {
            if (grid_live) {
                std::ignore = grid.shutdown();
            }
        };

        GridScene scene;
        GridScene::stageChunk(grid, fixture, scene);

        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, kFenceValueNone, {}};
            PPR_TEST_ASSERT(not grid.prepareWorldLighting(context, WorldLightingFrame{}));
            PPR_TEST_ASSERT(not grid.finalizeWorldLighting(context));
            submit(fixture, *encoder);
        }
        grid.notifyWorldLightingSubmitted(1u);

        // A submitted-but-uninspected ticket with a representational overflow
        // in its cascade record: only completion collection or pass shutdown
        // observes it. Shutdown must report it instead of a clean teardown.
        WorldLightingPass *const lighting = grid.activeWorldLightingForTest();
        PPR_TEST_ASSERT(lighting != nullptr);
        {
            auto words = lighting->pendingStatusWordsForTest();
            PPR_TEST_ASSERT(words.size() == kNumericStatusRecords * kNumericStatusWords);
            words[kNumericStageCascade * kNumericStatusWords + 1u] = 5u;
        }

        grid_live = false;
        PPR_TEST_ASSERT(grid.shutdown() == std::errc::result_out_of_range);
    };

    // ------------------------------------------------------------------
    // accepted resolve commits history/exposure exactly once
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (accepted_resolve_commits_history_and_exposure_exactly_once) {
        Fixture fixture;
        PPR_DEFER {
            fixture.shutdown();
        };
        fixture.initialize();

        ReconstructionPass reconstruction{};
        PostProcessPass post{};
        PPR_TEST_ASSERT(not reconstruction.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        PPR_TEST_ASSERT(not post.initialize(*fixture.m_shaders, std::filesystem::current_path()));
        PPR_DEFER {
            PPR_TEST_ASSERT(not post.shutdown());
            PPR_TEST_ASSERT(not reconstruction.shutdown());
        };

        Camera camera{ECameraProjection::orthographic};
        CameraModel model{};
        model.m_camera_mode = ECameraProjection::orthographic;
        model.m_ortho_scale = 2048.0f;
        camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{1280, 720}}, ViewportLayout{}});
        const auto grid_expected = makePresentationGrid(int2{1280, 720}, 1u);
        PPR_TEST_ASSERT(grid_expected.has_value());

        ResolveAttempt attempt{};
        attempt.m_snapshot = camera.getSnapshot();
        attempt.m_grid = *grid_expected;
        attempt.m_active_elapsed = std::chrono::milliseconds{250};

        // Accepted B 1: history role/phase/world and exposure baseline commit.
        PPR_TEST_ASSERT(not reconstruction.beginFrame(
            fixture.device(), attempt.m_snapshot, attempt.m_grid, attempt.m_world, 1u, kFenceValueNone));
        PPR_TEST_ASSERT(
            not post.beginFrame(fixture.device(), attempt.m_grid.m_resolved, attempt.m_world, kFenceValueNone));
        encodeResolve(fixture, reconstruction, post, attempt, kFenceValueNone);
        reconstruction.notifySubmitted(10u);
        post.notifySubmitted(10u);
        PPR_TEST_ASSERT(reconstruction.historyEstablishedForTest());
        const u32 phase_one = reconstruction.acceptedPhaseForTest();
        const u32 index_one = reconstruction.historyIndexForTest();
        PPR_TEST_ASSERT(reconstruction.historyWorldForTest() == attempt.m_world);
        PPR_TEST_ASSERT(reconstruction.historyUseForTest() == 10u);
        const auto exposure_one = post.exposureRoleForTest();
        PPR_TEST_ASSERT(exposure_one.m_valid and exposure_one.m_world == attempt.m_world);
        PPR_TEST_ASSERT(exposure_one.m_active_baseline == attempt.m_active_elapsed);

        // Rejected B: its prepare is encoded but never accepted. Nothing moves.
        PPR_TEST_ASSERT(not reconstruction.beginFrame(
            fixture.device(), attempt.m_snapshot, attempt.m_grid, attempt.m_world, 1u, 10u));
        PPR_TEST_ASSERT(not post.beginFrame(fixture.device(), attempt.m_grid.m_resolved, attempt.m_world, 10u));
        {
            auto encoder = makeEncoder(fixture);
            PreRenderContext context{fixture.device(), *encoder, 10u, {}};
            PPR_TEST_ASSERT(not reconstruction.prepare(context));
            PPR_TEST_ASSERT(not post.prepare(
                context, reconstruction.resolved(), PostProcessSettings{}, std::chrono::milliseconds{999}));
        }
        reconstruction.discardUnsubmitted();
        post.discardUnsubmitted();
        PPR_TEST_ASSERT(reconstruction.acceptedPhaseForTest() == phase_one);
        PPR_TEST_ASSERT(reconstruction.historyIndexForTest() == index_one);
        PPR_TEST_ASSERT(reconstruction.historyUseForTest() == 10u);
        const auto exposure_rejected = post.exposureRoleForTest();
        PPR_TEST_ASSERT(exposure_rejected.m_slot == exposure_one.m_slot);
        PPR_TEST_ASSERT(exposure_rejected.m_active_baseline == exposure_one.m_active_baseline);
        PPR_TEST_ASSERT(exposure_rejected.m_world == exposure_one.m_world);

        // Accepted B 2: history and exposure advance exactly one step.
        attempt.m_active_elapsed = std::chrono::milliseconds{500};
        PPR_TEST_ASSERT(not reconstruction.beginFrame(
            fixture.device(), attempt.m_snapshot, attempt.m_grid, attempt.m_world, 1u, 10u));
        PPR_TEST_ASSERT(not post.beginFrame(fixture.device(), attempt.m_grid.m_resolved, attempt.m_world, 10u));
        encodeResolve(fixture, reconstruction, post, attempt, 10u);
        reconstruction.notifySubmitted(11u);
        post.notifySubmitted(11u);
        PPR_TEST_ASSERT(reconstruction.acceptedPhaseForTest() == (phase_one + 1u) % ReconstructionPass::kPhaseCount);
        PPR_TEST_ASSERT(reconstruction.historyIndexForTest() != index_one);
        PPR_TEST_ASSERT(reconstruction.historyUseForTest() == 11u);
        const auto exposure_two = post.exposureRoleForTest();
        PPR_TEST_ASSERT(exposure_two.m_active_baseline == std::chrono::milliseconds{500});
        PPR_TEST_ASSERT(exposure_two.m_world == attempt.m_world);
    };

    PPR_UNIT_TEST (editor_scene_survives_a_rejected_resolve_and_settles_its_fence) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        // Stage one full-domain chunk: the scene path then really draws the
        // background and the opaque categorical terrain at the 4x sample-rate
        // signature, so the alignment/admission path is exercised rather than
        // an empty scene.
        GridPass::GridStagedFrame staged{};
        staged.m_world_edge = WorldLightingPass::kWorldEdge;
        staged.m_cell_size_m = 1.0f;
        staged.m_terrain_visuals.push_back(GridTerrainVisual{.m_material_id = 1u, .m_color_rgba = 0xffff'ffffu});
        staged.m_tiles.push_back(
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
        staged.m_uploads.push_back(std::move(upload));
        test_app.submitGridStage(std::move(staged));

        // One clean frame establishes the screen history and the present path.
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult first = *test_app.m_last_result;
        PPR_TEST_ASSERT(not first.m_error);
        PPR_TEST_ASSERT(first.m_presented);
        PPR_TEST_ASSERT(first.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(first.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(first.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(
            first.m_stages[kRenderStageScene].m_submission < first.m_stages[kRenderStageResolve].m_submission);
        PPR_TEST_ASSERT(
            first.m_stages[kRenderStageResolve].m_submission < first.m_stages[kRenderStagePresent].m_submission);
        PPR_TEST_ASSERT(first.m_submit_value == first.m_stages[kRenderStagePresent].m_submission);
        const FenceCompletionValue first_scene = stageValue(first, kRenderStageScene);

        // Second resolution (P=3) on the same world: the padded S/B scene and
        // the R-sized resolve/post chain remain accepted and presented, so the
        // background/opaque scene path is covered at two sampling grids.
        // Subsequent frames retain the admitted terrain instead of restaging it.
        const auto retainTerrain = [&test_app]() {
            GridPass::GridStagedFrame retained{};
            retained.m_retain_content = true;
            test_app.submitGridStage(std::move(retained));
        };
        test_app.setPostProcessSettings(PostProcessSettings{}, 3u);
        retainTerrain();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult second = *test_app.m_last_result;
        PPR_TEST_ASSERT(not second.m_error);
        PPR_TEST_ASSERT(second.m_presented);
        for (const RenderStageResult &stage: second.m_stages) {
            PPR_TEST_ASSERT(stage.m_settlement == FrameSettlement::kSubmitted);
            PPR_TEST_ASSERT(stage.m_submission != kFenceValueNone);
        }
        PPR_TEST_ASSERT(second.m_submit_value == second.m_stages[kRenderStagePresent].m_submission);
        PPR_TEST_ASSERT(second.m_stages[kRenderStageScene].m_submission > first_scene);

        // Return to P=1 for the rejection matrix below.
        test_app.setPostProcessSettings(PostProcessSettings{}, 1u);

        // Reject stage B as ordinary capacity backpressure: fill the
        // submission bound so A is admitted but B is not. Nothing is retried
        // in place and A's acceptance stands.
        safe_ptr<IRhiService> rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());
        renderer.setTestHoldSweep(true);
        rhi::ComPtr<rhi::ITexture> filler = makeOffscreenTarget(rhi->getDevice());
        const u32 held = renderer.outstandingFrameCount();
        PPR_TEST_ASSERT(FrameCreditLedger::capacity() > held);
        while (renderer.outstandingFrameCount() + 1u < FrameCreditLedger::capacity()) {
            PPR_TEST_ASSERT(not renderer.renderToTexture(*filler, {}));
        }

        retainTerrain();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult rejected = *test_app.m_last_result;
        PPR_TEST_ASSERT(not rejected.m_error);
        PPR_TEST_ASSERT(not rejected.m_presented);
        PPR_TEST_ASSERT(not rejected.m_dropped);
        PPR_TEST_ASSERT(not rejected.m_submit_possibly_executed);
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(
            rejected.m_stages[kRenderStageResolve].m_error == std::make_error_code(std::errc::device_or_resource_busy));
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(rejected.m_submit_value == rejected.m_stages[kRenderStageScene].m_submission);
        PPR_TEST_ASSERT(rejected.m_submit_value > first_scene);
        PPR_TEST_ASSERT(aggregateAcceptedSubmission(rejected.m_stages) == rejected.m_submit_value);

        // Release the held credits: the next frame presents again, so the
        // retained history still works after a rejected resolve.
        renderer.setTestHoldSweep(false);
        PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        filler.setNull();

        retainTerrain();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented);
        PPR_TEST_ASSERT(
            test_app.m_last_result->m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(
            test_app.m_last_result->m_stages[kRenderStageResolve].m_submission >
            rejected.m_stages[kRenderStageScene].m_submission);

        // A real resize to an odd extent: the next frame rebuilds the padded
        // scene/history/output chain and still submits and presents A/B/C.
        safe_ptr<IWindowService> windows = test_app.getServices().get<IWindowService>();
        PPR_TEST_ASSERT(windows.isValid());
        SharedWindow main_window = windows->getMainWindow();
        PPR_TEST_ASSERT(main_window.isValid());
        const auto resized = windows->configureDisplayWindow(
            *main_window, nullptr, main_window->m_window_position, int2{1281, 721}, std::nullopt);
        PPR_TEST_ASSERT(resized.has_value());
        // A native resize can leave the acquired swapchain outdated: that
        // frame is an explicit non-presenting outcome (the Renderer refreshes
        // the surface and leaves the frame unpresented), never an app error.
        // A bounded follow-up presents again through a fully accepted A/B/C.
        bool resized_presented = false;
        for (u32 attempt = 0u; attempt < 5u and not resized_presented; ++attempt) {
            retainTerrain();
            PPR_TEST_ASSERT(not test_app.tick(frame_delta));
            PPR_TEST_ASSERT(not test_app.draw());
            PPR_TEST_ASSERT(test_app.m_last_result.has_value());
            PPR_TEST_ASSERT(not test_app.m_last_result->m_error);
            resized_presented = test_app.m_last_result->m_presented;
        }
        PPR_TEST_ASSERT(resized_presented);
        for (const RenderStageResult &stage: test_app.m_last_result->m_stages) {
            PPR_TEST_ASSERT(stage.m_settlement == FrameSettlement::kSubmitted);
        }

        // Replace the world with an independently staged candidate, then make
        // the following frame's stage B ordinary capacity-busy: A commits the
        // new world's scene, B commits nothing, and the old world's screen role
        // is never manufactured as a presentation of the new one.
        const auto submitWorld = [&test_app](const EGridWorldCommand kind, const bool final_batch) {
            auto command = std::make_shared<GridWorldCommand>();
            command->m_kind = kind;
            command->m_sequence = 2u;
            command->m_job_id = 1u;
            command->m_world_generation = 1u;
            command->m_content_fingerprint = 1u;
            command->m_final_batch = final_batch;
            if (kind == EGridWorldCommand::begin) {
                command->m_stage.m_world_edge = WorldLightingPass::kWorldEdge;
                command->m_stage.m_cell_size_m = 1.0f;
                command->m_stage.m_content_fingerprint = 1u;
                command->m_stage.m_terrain_visuals.push_back(
                    GridTerrainVisual{.m_material_id = 1u, .m_color_rgba = 0xffff'ffffu});
                command->m_stage.m_tiles.push_back(
                    GridTileSubmission{
                        .m_dirty_mask = 1u,
                        .m_tile_range = GridTileRange{128, 0, 256, 128},
                        .m_chunk_id = 1u,
                    });
            } else if (kind == EGridWorldCommand::append) {
                GridPass::GridStagedUpload upload{};
                upload.m_chunk_id = 1u;
                upload.m_payload.m_rect[0] = 128.0f;
                upload.m_payload.m_rect[1] = 0.0f;
                upload.m_payload.m_rect[2] = 256.0f;
                upload.m_payload.m_rect[3] = 128.0f;
                upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 1u);
                upload.m_cell_words.assign(GridPass::kCellsPerChunk, 0u);
                command->m_stage.m_uploads.push_back(std::move(upload));
            }
            PPR_TEST_ASSERT(not test_app.submitGridWorld(std::move(command)));
            PPR_TEST_ASSERT(test_app.m_last_world_result.has_value());
            PPR_TEST_ASSERT(not test_app.m_last_world_result->m_error);
            PPR_TEST_ASSERT(test_app.m_last_world_result->m_applied);
        };
        submitWorld(EGridWorldCommand::begin, false);
        submitWorld(EGridWorldCommand::append, true);
        submitWorld(EGridWorldCommand::advance, false);
        PPR_TEST_ASSERT(test_app.m_last_world_result->m_ready);
        submitWorld(EGridWorldCommand::activate, false);
        PPR_TEST_ASSERT(test_app.getGridPass().activeWorldIdentity().m_job_id == 1u);

        safe_ptr<IRhiService> replacement_rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(replacement_rhi.isValid());
        renderer.setTestHoldSweep(true);
        rhi::ComPtr<rhi::ITexture> replacement_filler = makeOffscreenTarget(replacement_rhi->getDevice());
        while (renderer.outstandingFrameCount() + 1u < FrameCreditLedger::capacity()) {
            PPR_TEST_ASSERT(not renderer.renderToTexture(*replacement_filler, {}));
        }
        retainTerrain();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult replaced_rejected = *test_app.m_last_result;
        PPR_TEST_ASSERT(not replaced_rejected.m_error);
        PPR_TEST_ASSERT(not replaced_rejected.m_presented);
        PPR_TEST_ASSERT(replaced_rejected.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(replaced_rejected.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(replaced_rejected.m_submit_value == replaced_rejected.m_stages[kRenderStageScene].m_submission);

        // Releasing the credits lets the replacement world present through a
        // fresh accepted resolve; the reported presented identity is the new
        // world, never the old one.
        renderer.setTestHoldSweep(false);
        PPR_TEST_ASSERT(not renderer.waitForFenceValue(renderer.lastSubmittedFenceValue()));
        PPR_TEST_ASSERT(not renderer.retireCompletedFrames());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() == 0u);
        replacement_filler.setNull();

        retainTerrain();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented);
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented_grid.m_job_id == 1u);
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented_grid.m_world_generation == 1u);
        PPR_TEST_ASSERT(
            test_app.m_last_result->m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted);

        // Service/window borrows never outlive the run's teardown.
        main_window.reset();
        windows.reset();
        replacement_rhi.reset();
        rhi.reset();

        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    // The C-stage overlay must stamp the world/camera actually submitted and
    // stop stamping (while still presenting) when the diagnostic is off.
    PPR_UNIT_TEST (editor_inspection_stamp_matches_submitted_world_camera_and_revision) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        stageEditorChunk(test_app);
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        test_app.setLightingInspectionSettings({ELightingDiagnostic::occupancy, 1u}, 12);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        std::size_t captured_camera_revision{};
        {
            const safe_ptr<const Camera> camera = test_app.getMainCamera();
            PPR_TEST_ASSERT(camera.isValid());
            captured_camera_revision = camera->getRevision();
        }
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult inspected = *test_app.m_last_result;
        PPR_TEST_ASSERT(not inspected.m_error);
        PPR_TEST_ASSERT(inspected.m_presented);
        PPR_TEST_ASSERT(inspected.m_inspection.has_value());

        const LightingInspectionStamp &stamp = *inspected.m_inspection;
        PPR_TEST_ASSERT(stamp.m_settings.m_diagnostic == ELightingDiagnostic::occupancy);
        PPR_TEST_ASSERT(stamp.m_settings.m_cascade == 1u);
        PPR_TEST_ASSERT(stamp.m_world == inspected.m_presented_grid);
        PPR_TEST_ASSERT(stamp.m_world == test_app.getGridPass().activeWorldIdentity());
        PPR_TEST_ASSERT(stamp.m_camera_revision == captured_camera_revision);
        PPR_TEST_ASSERT(stamp.m_submission == inspected.m_stages[kRenderStagePresent].m_submission);
        PPR_TEST_ASSERT(stamp.m_submission > inspected.m_stages[kRenderStageScene].m_submission);

        // Off still presents the ordinary picture and posts no inspection
        // stamp; the overlay is an added reader, never a presentation mode.
        GridPass::GridStagedFrame retained{};
        retained.m_retain_content = true;
        test_app.submitGridStage(std::move(retained));
        test_app.setLightingInspectionSettings({ELightingDiagnostic::off, 5u}, 12);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented);
        PPR_TEST_ASSERT(not test_app.m_last_result->m_inspection.has_value());

        torn_down = true;
        PPR_TEST_ASSERT(not test_app.teardown());
    };

    // Mid-history edits use one editor run: establish Hable/lighting state,
    // then apply each retained setting without restarting. A reset triggered by
    // the edit itself is observed immediately; the following frame must advance
    // the normal history/lighting cadence and each setting must reach the real
    // production path (positive controls).
    PPR_UNIT_TEST (editor_mid_history_edits_do_not_reset_or_contaminate) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        stageEditorChunk(test_app);
        const PostProcessSettings post_baseline{};
        PostProcessSettings post_edited{};
        post_edited.m_auto_exposure = false;
        post_edited.m_exposure_ev = 2.0f;
        post_edited.m_bloom_enabled = false;
        post_edited.m_bloom_threshold = 4.0f;
        test_app.setPostProcessSettings(post_baseline, 1u);
        test_app.setLightingInspectionSettings({ELightingDiagnostic::off, 1u}, -1);

        constexpr auto frame_delta = std::chrono::milliseconds{16};
        const auto run_frame = [&](const bool retain) {
            if (retain) {
                retainEditorTerrain(test_app);
            }
            PPR_TEST_ASSERT(not test_app.tick(frame_delta));
            PPR_TEST_ASSERT(not test_app.draw());
            PPR_TEST_ASSERT(test_app.m_last_result.has_value());
            PPR_TEST_ASSERT(not test_app.m_last_result->m_error);
            PPR_TEST_ASSERT(test_app.m_last_result->m_presented);
            return *test_app.m_last_result;
        };

        safe_ptr<IRhiService> rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());
        rhi::IDevice &device = rhi->getDevice();

        struct Observation {
            std::vector<std::byte> m_scene{};
            std::vector<std::byte> m_history{};
            std::vector<std::byte> m_exposure{};
            std::vector<std::byte> m_fine_ch{};
            PostProcessSettings m_post_settings{};
            PostProcessPass::ExposureRoleForTest m_role{};
            u64 m_accepted_frames{};
            u32 m_history_index{};
            u32 m_phase{};
            bool m_history_established{};
            GridWorldIdentity m_history_world{};
            i32 m_sky_hour{};
        };

        const auto observe = [&]() -> Observation {
            PPR_TEST_ASSERT(not test_app.getRenderer().waitOnHost());
            const ReconstructionPass &reconstruction = test_app.getReconstructionPassForTest();
            const PostProcessPass &post_pass = test_app.getPostProcessPassForTest();
            rhi::ITexture *const scene = reconstruction.sceneTextureForTest();
            rhi::ITexture *const history = reconstruction.historyTextureForTest();
            rhi::ITexture *const exposure = post_pass.exposureTextureForTest();
            PPR_TEST_ASSERT(scene != nullptr and history != nullptr and exposure != nullptr);
            const WorldLightingPass *const lighting = test_app.getGridPass().activeWorldLightingForTest();
            PPR_TEST_ASSERT(lighting != nullptr);
            rhi::IBuffer *const fine_ch = lighting->fineCHForTest();
            PPR_TEST_ASSERT(fine_ch != nullptr);

            Observation observation{};
            observation.m_scene = readEditorTexture(device, *scene);
            observation.m_history = readEditorTexture(device, *history);
            observation.m_exposure = readEditorTexture(device, *exposure);
            observation.m_post_settings = post_pass.lastPreparedSettingsForTest();
            observation.m_role = post_pass.exposureRoleForTest();
            observation.m_accepted_frames = lighting->acceptedFrameForTest();
            observation.m_sky_hour = lighting->lastPreparedSkyHourForTest();
            observation.m_history_index = reconstruction.historyIndexForTest();
            observation.m_phase = reconstruction.acceptedPhaseForTest();
            observation.m_history_established = reconstruction.historyEstablishedForTest();
            observation.m_history_world = reconstruction.historyWorldForTest();
            observation.m_fine_ch.resize(64u * sizeof(lighting::PackedCH));
            const u64 fine_offset = (30u * 512u + 30u) * sizeof(lighting::PackedCH);
            PPR_TEST_ASSERT(not make_error_code(
                device.readBuffer(fine_ch, fine_offset, observation.m_fine_ch.size(), observation.m_fine_ch.data())));
            return observation;
        };

        const auto same_post_settings = [](const PostProcessSettings &lhs, const PostProcessSettings &rhs) {
            return lhs.m_exposure_ev == rhs.m_exposure_ev and
                   lhs.m_adaptation_tau == rhs.m_adaptation_tau and
                   lhs.m_ev_min == rhs.m_ev_min and
                   lhs.m_ev_max == rhs.m_ev_max and
                   lhs.m_bloom_intensity == rhs.m_bloom_intensity and
                   lhs.m_bloom_threshold == rhs.m_bloom_threshold and
                   lhs.m_auto_exposure == rhs.m_auto_exposure and
                   lhs.m_bloom_enabled == rhs.m_bloom_enabled;
        };

        const auto assert_same_committed = [&](const Observation &before, const Observation &after) {
            PPR_TEST_ASSERT(before.m_scene == after.m_scene);
            PPR_TEST_ASSERT(before.m_history == after.m_history);
            PPR_TEST_ASSERT(before.m_exposure == after.m_exposure);
            PPR_TEST_ASSERT(before.m_fine_ch == after.m_fine_ch);
            PPR_TEST_ASSERT(same_post_settings(before.m_post_settings, after.m_post_settings));
            PPR_TEST_ASSERT(before.m_role.m_slot == after.m_role.m_slot);
            PPR_TEST_ASSERT(before.m_role.m_world == after.m_role.m_world);
            PPR_TEST_ASSERT(before.m_role.m_active_baseline == after.m_role.m_active_baseline);
            PPR_TEST_ASSERT(before.m_accepted_frames == after.m_accepted_frames);
            PPR_TEST_ASSERT(before.m_history_index == after.m_history_index);
            PPR_TEST_ASSERT(before.m_phase == after.m_phase);
            PPR_TEST_ASSERT(before.m_history_established == after.m_history_established);
            PPR_TEST_ASSERT(before.m_history_world == after.m_history_world);
            PPR_TEST_ASSERT(before.m_sky_hour == after.m_sky_hour);
        };

        const auto assert_continuity = [](const Observation &before, const Observation &after) {
            PPR_TEST_ASSERT(after.m_history_established);
            PPR_TEST_ASSERT(after.m_history_world == before.m_history_world);
            PPR_TEST_ASSERT(after.m_history_index == (before.m_history_index + 1u) % 2u);
            PPR_TEST_ASSERT(after.m_phase == (before.m_phase + 1u) % 4u);
            PPR_TEST_ASSERT(after.m_accepted_frames == before.m_accepted_frames + 1u);
        };

        // Three warm-up frames establish Hable history and lighting; the
        // fourth observation is the mid-history baseline.
        std::ignore = run_frame(false);
        std::ignore = run_frame(true);
        const RenderFrameResult baseline_result = run_frame(true);
        const Observation baseline = observe();
        PPR_TEST_ASSERT(baseline.m_history_established);
        PPR_TEST_ASSERT(baseline.m_accepted_frames >= 2u);
        PPR_TEST_ASSERT(baseline.m_sky_hour == -1);
        PPR_TEST_ASSERT(same_post_settings(baseline.m_post_settings, post_baseline));

        // Diagnostic toggle at the same committed point. The inspection stamp
        // is the production-observable control; camera jitter makes a raw pixel
        // comparison non-discriminating.
        test_app.setLightingInspectionSettings({ELightingDiagnostic::occupancy, 1u}, -1);
        const Observation diagnostic_boundary = observe();
        assert_same_committed(baseline, diagnostic_boundary);

        const RenderFrameResult diagnostic_result = run_frame(true);
        const Observation diagnostic = observe();
        PPR_TEST_ASSERT(diagnostic_result.m_inspection.has_value());
        PPR_TEST_ASSERT(diagnostic_result.m_inspection->m_settings.m_diagnostic == ELightingDiagnostic::occupancy);
        assert_continuity(diagnostic_boundary, diagnostic);
        PPR_TEST_ASSERT(diagnostic.m_sky_hour == -1);
        PPR_TEST_ASSERT(same_post_settings(diagnostic.m_post_settings, post_baseline));

        // Post-only edit: the committed state is untouched at the edit boundary;
        // the next frame proves the edited tuple reached PostProcessPass
        // (value-only positive control, no auto-exposure byte inference) while
        // history and lighting cadence continue normally.
        test_app.setPostProcessSettings(post_edited, 1u);
        const Observation post_boundary = observe();
        assert_same_committed(diagnostic, post_boundary);

        const RenderFrameResult post_result = run_frame(true);
        const Observation post = observe();
        PPR_TEST_ASSERT(same_post_settings(post.m_post_settings, post_edited));
        assert_continuity(post_boundary, post);
        PPR_TEST_ASSERT(post.m_sky_hour == -1);

        // Sky edit: the retained selection reaches the lighting pass, changes
        // fine-CH content, and never resets the history/lighting cadence.
        test_app.setLightingInspectionSettings({ELightingDiagnostic::off, 1u}, 12);
        const Observation sky_boundary = observe();
        assert_same_committed(post, sky_boundary);

        const RenderFrameResult sky_result = run_frame(true);
        const Observation sky = observe();
        assert_continuity(sky_boundary, sky);
        PPR_TEST_ASSERT(sky.m_sky_hour == 12);
        PPR_TEST_ASSERT(sky.m_fine_ch != post.m_fine_ch);
        PPR_TEST_ASSERT(not sky_result.m_inspection.has_value());

        rhi.reset();
        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    PPR_UNIT_TEST (editor_ambiguous_resolve_quarantines_and_teardown_recovers) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value() and test_app.m_last_result->m_presented);
        const FenceCompletionValue first_scene = stageValue(*test_app.m_last_result, kRenderStageScene);

        // Stage A accepted, stage B possibly executed: the frame reports the
        // ambiguity, commits no B value and stops admission.
        test_app.injectStageFailure(kRenderStageResolve);
        GridPass::GridStagedFrame retained{};
        retained.m_retain_content = true;
        test_app.submitGridStage(std::move(retained));
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        // The ambiguous submit's error propagates out of the frame (the run
        // boundary latches the quarantine); the settlement matrix still
        // reports which stage was accepted and which one is ambiguous.
        PPR_TEST_ASSERT(test_app.draw() != std::error_code{});
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult ambiguous = *test_app.m_last_result;
        PPR_TEST_ASSERT(ambiguous.m_submit_possibly_executed);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageResolve].m_submission == kFenceValueNone);
        PPR_TEST_ASSERT(ambiguous.m_submit_value == ambiguous.m_stages[kRenderStageScene].m_submission);
        PPR_TEST_ASSERT(ambiguous.m_submit_value > first_scene);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kRejected);
        // C is never attempted after the ambiguity: no later renderer call may
        // overwrite the ambiguous settlement or advance the frontier, and no
        // retained-picture selection runs outside a submission.
        PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == ambiguous.m_submit_value);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::state_not_recoverable));

        // Dependent passes keep their quarantined storage; teardown proves
        // idle and reports the first cleanup result.
        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };
    // ------------------------------------------------------------------
    // ambiguity at the first and last stages, and the injection seam
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (editor_ambiguous_scene_stops_before_resolve_and_present) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value() and test_app.m_last_result->m_presented);
        const FenceCompletionValue before = renderer.lastSubmittedFenceValue();

        test_app.injectStageFailure(kRenderStageScene);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(test_app.draw() != std::error_code{});
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult ambiguous = *test_app.m_last_result;
        PPR_TEST_ASSERT(ambiguous.m_submit_possibly_executed);
        PPR_TEST_ASSERT(not ambiguous.m_presented);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageScene].m_submission == kFenceValueNone);
        PPR_TEST_ASSERT(ambiguous.m_submit_value == kFenceValueNone);
        // B and C were never reached: no error, no value, rejected default.
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(not ambiguous.m_stages[kRenderStageResolve].m_error);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(not ambiguous.m_stages[kRenderStagePresent].m_error);
        // No later renderer call ran: the ambiguous settlement and frontier
        // still stand.
        PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == before);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::state_not_recoverable));

        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    PPR_UNIT_TEST (editor_ambiguous_present_keeps_scene_and_resolve_values) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value() and test_app.m_last_result->m_presented);

        test_app.injectStageFailure(kRenderStagePresent);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(test_app.draw() != std::error_code{});
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult ambiguous = *test_app.m_last_result;
        PPR_TEST_ASSERT(ambiguous.m_submit_possibly_executed);
        PPR_TEST_ASSERT(not ambiguous.m_presented);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(ambiguous.m_stages[kRenderStagePresent].m_submission == kFenceValueNone);
        // A and B stand; C committed no value of its own and the aggregate is
        // the accepted resolve.
        PPR_TEST_ASSERT(ambiguous.m_submit_value == ambiguous.m_stages[kRenderStageResolve].m_submission);
        PPR_TEST_ASSERT(
            ambiguous.m_stages[kRenderStageScene].m_submission < ambiguous.m_stages[kRenderStageResolve].m_submission);
        PPR_TEST_ASSERT(renderer.lastSubmitSettlement() == FrameSettlement::kSubmitFailed);
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == ambiguous.m_submit_value);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::make_error_code(std::errc::state_not_recoverable));

        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    PPR_UNIT_TEST (editor_stale_publication_epoch_drops_and_leaves_payload_pending) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};

        safe_ptr<IRhiService> rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());
        const auto makeViewBuffer = [&](const u32 stride) {
            rhi::BufferDesc description{};
            description.size = stride;
            description.elementSize = stride;
            description.memoryType = rhi::MemoryType::DeviceLocal;
            description.usage = rhi::BufferUsage::ShaderResource;
            description.defaultState = rhi::ResourceState::ShaderResource;
            rhi::ComPtr<rhi::IBuffer> buffer;
            PPR_TEST_ASSERT(
                not make_error_code(rhi->getDevice().createBuffer(description, nullptr, buffer.writeRef())));
            return buffer;
        };

        auto live = std::make_shared<std::atomic<u64>>(1u);
        GridParticleGpuView view{};
        view.m_numerical = makeViewBuffer(kGridParticleNumericalStride);
        view.m_readonly = makeViewBuffer(kGridParticleReadonlyStride);
        view.m_live_epoch = live;
        view.m_world = test_app.getGridPass().activeWorldIdentity();
        view.m_device_epoch = 1u;
        view.m_publication = 1u;
        view.m_terrain_revision = 1u;
        view.m_joint_tick = 1u;
        view.m_origin = 3.0;
        PPR_TEST_ASSERT(view.current());

        GridPass::GridStagedFrame staged{};
        staged.m_retain_content = true;
        staged.m_gpu_particles = view;
        staged.m_ack_seed = 7u;
        staged.m_ack_chunk_ids.push_back(3u);
        staged.m_ack_revisions.push_back(11u);
        test_app.submitGridStage(std::move(staged));

        // The publisher released a newer epoch after this packet was captured
        // but before the owner lane encodes it: the un-encoded view is rejected
        // and the payload stays pending for a fresh coherent packet.
        live->store(2u, std::memory_order_release);
        const FenceCompletionValue before = renderer.lastSubmittedFenceValue();
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult stale = *test_app.m_last_result;
        PPR_TEST_ASSERT(stale.m_dropped);
        PPR_TEST_ASSERT(not stale.m_error);
        PPR_TEST_ASSERT(not stale.m_presented);
        PPR_TEST_ASSERT(not stale.m_submit_possibly_executed);
        PPR_TEST_ASSERT(stale.m_submit_value == kFenceValueNone);
        PPR_TEST_ASSERT(not stale.m_grid_staging_applied);
        for (const RenderStageResult &stage: stale.m_stages) {
            PPR_TEST_ASSERT(stage.m_settlement == FrameSettlement::kRejected);
            PPR_TEST_ASSERT(stage.m_submission == kFenceValueNone);
        }
        // No backend work and no consumption ack: the client can restage.
        PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == before);
        PPR_TEST_ASSERT(stale.m_grid_ack_seed == 7u);
        PPR_TEST_ASSERT(stale.m_grid_ack_chunk_ids.size() == 1u and stale.m_grid_ack_chunk_ids[0] == 3u);
        PPR_TEST_ASSERT(stale.m_grid_ack_revisions.size() == 1u and stale.m_grid_ack_revisions[0] == 11u);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::error_code{});

        // The view's buffers are device children: release them before the
        // editor's RHI shutdown proves there are no live devices.
        view = GridParticleGpuView{};
        live.reset();
        rhi.reset();
        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    PPR_UNIT_TEST (editor_armed_resolve_failure_is_discarded_when_the_scene_is_rejected) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};
        safe_ptr<IRhiService> rhi = test_app.getServices().get<IRhiService>();
        PPR_TEST_ASSERT(rhi.isValid());

        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value() and test_app.m_last_result->m_presented);

        // No credit is free: A is definitely rejected and the armed B
        // injection is never reached.
        rhi::ComPtr<rhi::ITexture> filler{};
        holdCreditsLeaving(renderer, rhi->getDevice(), 0u, filler);
        test_app.injectStageFailure(kRenderStageResolve);
        retainEditorTerrain(test_app);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult refused = *test_app.m_last_result;
        PPR_TEST_ASSERT(not refused.m_error);
        PPR_TEST_ASSERT(not refused.m_submit_possibly_executed);
        PPR_TEST_ASSERT(refused.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(refused.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(refused.m_submit_value == kFenceValueNone);
        releaseCredits(renderer, filler);

        // The injection was consumed by the refused frame: this frame reaches
        // B normally, with no quarantine and no stale ambiguous submission.
        retainEditorTerrain(test_app);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult recovered = *test_app.m_last_result;
        PPR_TEST_ASSERT(not recovered.m_error);
        PPR_TEST_ASSERT(not recovered.m_submit_possibly_executed);
        PPR_TEST_ASSERT(recovered.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(recovered.m_presented);
        PPR_TEST_ASSERT(renderer.checkFrameAdmission() == std::error_code{});

        rhi.reset();
        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

    // ------------------------------------------------------------------
    // review focus: rejection after A across a resize and world replacement
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (editor_review_focus_keeps_old_screen_role_until_accepted_resolve) {
        SettlementEditorApp test_app{std::span<const char *const>{}};
        PPR_TEST_ASSERT(not test_app.boot());
        bool torn_down = false;
        PPR_DEFER {
            if (not torn_down) {
                std::ignore = test_app.teardown();
            }
        };

        Renderer &renderer = test_app.getRenderer();
        constexpr auto frame_delta = std::chrono::milliseconds{16};
        safe_ptr<IRhiService> rhi = test_app.getServices().get<IRhiService>();
        safe_ptr<IWindowService> windows = test_app.getServices().get<IWindowService>();
        PPR_TEST_ASSERT(rhi.isValid() and windows.isValid());

        // P=1 keeps the resolved extent equal to the framebuffer, so the old
        // world's retained picture is directly addressable after the resize.
        test_app.setPostProcessSettings(PostProcessSettings{}, 1u);

        // Frame 1: an accepted, presented screen role for the initial world.
        stageEditorChunk(test_app);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value() and test_app.m_last_result->m_presented);
        const GridWorldIdentity old_world = test_app.getGridPass().activeWorldIdentity();
        const auto old_exposure = test_app.getPostProcessPassForTest().exposureRoleForTest();
        PPR_TEST_ASSERT(old_exposure.m_valid and old_exposure.m_world == old_world);
        const u32 old_phase = test_app.getReconstructionPassForTest().acceptedPhaseForTest();
        const u32 old_index = test_app.getReconstructionPassForTest().historyIndexForTest();
        PPR_TEST_ASSERT(test_app.getReconstructionPassForTest().historyWorldForTest() == old_world);
        SharedWindow main_window = windows->getMainWindow();
        PPR_TEST_ASSERT(main_window.isValid());
        const int2 old_resolved_extent = main_window->m_framebuffer_size;

        // Resize and replace the world while earlier accepted work is still
        // unretired: the following frame gets A admitted but B refused.
        rhi::ComPtr<rhi::ITexture> filler{};
        holdCreditsLeaving(renderer, rhi->getDevice(), 1u, filler);
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() > 0u);
        const auto resized = windows->configureDisplayWindow(
            *main_window, nullptr, main_window->m_window_position, int2{1281, 721}, std::nullopt);
        PPR_TEST_ASSERT(resized.has_value());
        replaceEditorWorld(test_app);
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() > 0u);

        retainEditorTerrain(test_app);
        PPR_TEST_ASSERT(not test_app.tick(frame_delta));
        PPR_TEST_ASSERT(not test_app.draw());
        PPR_TEST_ASSERT(test_app.m_last_result.has_value());
        const RenderFrameResult rejected = *test_app.m_last_result;
        PPR_TEST_ASSERT(not rejected.m_error);
        PPR_TEST_ASSERT(not rejected.m_presented);
        PPR_TEST_ASSERT(not rejected.m_submit_possibly_executed);
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStageScene].m_settlement == FrameSettlement::kSubmitted);
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStageResolve].m_settlement == FrameSettlement::kRejected);
        PPR_TEST_ASSERT(
            rejected.m_stages[kRenderStageResolve].m_error == std::make_error_code(std::errc::device_or_resource_busy));
        PPR_TEST_ASSERT(rejected.m_stages[kRenderStagePresent].m_settlement == FrameSettlement::kRejected);
        // The aggregate is A's ACTUAL value: the same value the renderer last
        // accepted, never none and never a stale present/C value.
        PPR_TEST_ASSERT(rejected.m_submit_value == rejected.m_stages[kRenderStageScene].m_submission);
        PPR_TEST_ASSERT(rejected.m_submit_value == renderer.lastSubmittedFenceValue());
        PPR_TEST_ASSERT(renderer.outstandingFrameCount() > 0u);

        // A's lighting phase commit for the replacement world remains (exactly
        // one accepted attempt) even though B was rejected.
        const WorldLightingPass *const lighting = test_app.getGridPass().activeWorldLightingForTest();
        PPR_TEST_ASSERT(lighting != nullptr);
        PPR_TEST_ASSERT(lighting->acceptedFrameForTest() == 1u);

        // The screen role is still the old world until a later accepted B:
        // exposure role, history role/phase/index and the old retained picture
        // are untouched (old resources retained).
        const auto held_exposure = test_app.getPostProcessPassForTest().exposureRoleForTest();
        PPR_TEST_ASSERT(held_exposure.m_valid);
        PPR_TEST_ASSERT(held_exposure.m_world == old_world);
        PPR_TEST_ASSERT(held_exposure.m_slot == old_exposure.m_slot);
        PPR_TEST_ASSERT(held_exposure.m_active_baseline == old_exposure.m_active_baseline);
        const auto &held_reconstruction = test_app.getReconstructionPassForTest();
        PPR_TEST_ASSERT(held_reconstruction.historyWorldForTest() == old_world);
        PPR_TEST_ASSERT(held_reconstruction.acceptedPhaseForTest() == old_phase);
        PPR_TEST_ASSERT(held_reconstruction.historyIndexForTest() == old_index);
        PPR_TEST_ASSERT(test_app.getPostProcessPassForTest().retainedPictureAvailable(old_resolved_extent, old_world));

        // Release everything: a later accepted B presents the replacement world
        // and then, and only then, moves the screen role.
        releaseCredits(renderer, filler);
        bool presented = false;
        for (u32 attempt = 0u; attempt < 5u and not presented; ++attempt) {
            retainEditorTerrain(test_app);
            PPR_TEST_ASSERT(not test_app.tick(frame_delta));
            PPR_TEST_ASSERT(not test_app.draw());
            PPR_TEST_ASSERT(test_app.m_last_result.has_value());
            PPR_TEST_ASSERT(not test_app.m_last_result->m_error);
            presented = test_app.m_last_result->m_presented;
        }
        PPR_TEST_ASSERT(presented);
        PPR_TEST_ASSERT(test_app.m_last_result->m_presented_grid.m_job_id == 1u);
        const GridWorldIdentity new_world = test_app.getGridPass().activeWorldIdentity();
        PPR_TEST_ASSERT(new_world.m_job_id == 1u);
        const auto new_exposure = test_app.getPostProcessPassForTest().exposureRoleForTest();
        PPR_TEST_ASSERT(new_exposure.m_valid and new_exposure.m_world == new_world);
        PPR_TEST_ASSERT(new_exposure.m_active_baseline > old_exposure.m_active_baseline);
        PPR_TEST_ASSERT(test_app.getReconstructionPassForTest().historyEstablishedForTest());
        PPR_TEST_ASSERT(test_app.getReconstructionPassForTest().historyWorldForTest() == new_world);
        // Every recovery attempt that reached acceptance committed one more
        // scene attempt; the rejected frame above committed exactly one.
        PPR_TEST_ASSERT(lighting->acceptedFrameForTest() > 1u);

        // Service/window borrows never outlive teardown.
        main_window.reset();
        windows.reset();
        rhi.reset();
        PPR_TEST_ASSERT(not test_app.teardown());
        torn_down = true;
    };

} // namespace pP::tests::detail::WorldLightingSettlement

namespace pP::tests {
    // ------------------------------------------------------------------
    // A/B/C settlement suite
    // ------------------------------------------------------------------

    const UnitTest &world_lighting_settlementTests() noexcept {
        static const UnitTest group = UnitTest::Named("world_lighting_settlement") / [](UnitTest::IRun &run) {
            run.recurse({
                detail::WorldLightingSettlement::render_stage_result_is_value_only_ordered_and_aggregated,
                detail::WorldLightingSettlement::credit_bound_covers_two_logical_frames_of_three_stages,
                detail::WorldLightingSettlement::three_ordered_submissions_settle_one_value_each,
                detail::WorldLightingSettlement::capacity_busy_later_stage_keeps_the_earlier_accepted_value,
                detail::WorldLightingSettlement::ambiguous_submit_quarantines_and_stops_admission,
                detail::WorldLightingSettlement::accepted_scene_commits_lighting_once_and_discard_commits_nothing,
                detail::WorldLightingSettlement::stale_publication_epoch_is_rejected_before_encode,
                detail::WorldLightingSettlement::rejected_resolve_preserves_exposure_and_retained_picture,
                detail::WorldLightingSettlement::editor_scene_survives_a_rejected_resolve_and_settles_its_fence,
                detail::WorldLightingSettlement::editor_inspection_stamp_matches_submitted_world_camera_and_revision,
                detail::WorldLightingSettlement::editor_mid_history_edits_do_not_reset_or_contaminate,
                detail::WorldLightingSettlement::editor_ambiguous_resolve_quarantines_and_teardown_recovers,
                detail::WorldLightingSettlement::lighting_status_failure_is_observed_at_pass_shutdown,
                detail::WorldLightingSettlement::accepted_resolve_commits_history_and_exposure_exactly_once,
                detail::WorldLightingSettlement::editor_ambiguous_scene_stops_before_resolve_and_present,
                detail::WorldLightingSettlement::editor_ambiguous_present_keeps_scene_and_resolve_values,
                detail::WorldLightingSettlement::editor_stale_publication_epoch_drops_and_leaves_payload_pending,
                detail::WorldLightingSettlement::editor_armed_resolve_failure_is_discarded_when_the_scene_is_rejected,
                detail::WorldLightingSettlement::editor_review_focus_keeps_old_screen_role_until_accepted_resolve,
            });
        };
        return group;
    }
} // namespace pP::tests
