module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/import3d/mesh.hpp>
#include <slang.h>
#include <slang-com-ptr.h>

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.mesh;
import std;

// Draw-path proofs. The pass has ONE drawInstanced path, so these cover the
// three things that could still be wrong in it: the payload base a group binds
// (the g_payload_base regression), instancing actually differentiating
// instances, and the device-loss restart through the same path. Pixel tests
// share the single SharedGpu fixture (Asset.GpuFixture.cpp); the device-loss
// leaf boots its own private headless app and lives in the quarantine group,
// which runs after the shared session is torn down.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace Draws {
        // ------------------------------------------------------------------
        // shared helpers (pixel tests)
        // ------------------------------------------------------------------

        [[nodiscard]] CameraSnapshot drawsCamera_(
            const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        // Row-major / row-vector transform with the translation in row 3 (the
        // engine-wide convention mirrored by the vertex shader).
        [[nodiscard]] float4x4 drawsPlace_(const float x, const float y, const float z) noexcept {
            return float4x4{
                float4{1.0f, 0.0f, 0.0f, 0.0f},
                float4{0.0f, 1.0f, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{x, y, z, 1.0f},
            };
        }

        // An axis-aligned quad of the requested half-extent, facing +Z.
        [[nodiscard]] Array<mesh::StaticMeshVertex> drawsQuad_(const float half) {
            const auto vertex = [half](const float x, const float y) {
                return mesh::StaticMeshVertex{
                    .m_position = {x, y, 0.0f},
                    .m_normal = {0.0f, 0.0f, 1.0f},
                    .m_texcoord = {(x / half) * 0.5f + 0.5f, (y / half) * 0.5f + 0.5f},
                    .m_tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                    .m_color = {1.0f, 1.0f, 1.0f, 1.0f},
                };
            };
            return {vertex(-half, -half), vertex(half, -half), vertex(half, half), vertex(-half, half)};
        }

        // Winded CLOCKWISE as seen by the camera, which is the front face this
        // pipeline configuration accepts. pipelineFor_ leaves the rasterizer's
        // front-face mode at its default and sets CullMode::Back, so the
        // opposite winding is back-facing and is culled: the quad rasterizes
        // nothing at all and the frame stays a flat clear. Verified empirically,
        // not derived — do not "simplify" this back to 0,1,2 / 0,2,3.
        [[nodiscard]] Array<u32> drawsQuadIndices_() {
            return {0u, 3u, 2u, 0u, 2u, 1u};
        }

        struct DrawsPixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] Expected<DrawsPixels> drawsReadback_(rhi::IDevice &device, rhi::ITexture &target) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err =
                make_error_code(device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) {
                return std::unexpected{err};
            }
            if (blob.get() == nullptr
                or
            blob->getBufferPointer() == nullptr)
            {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            DrawsPixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::array<u8, 4u> drawsTexel_(
            const DrawsPixels &pixels, const u32 x, const u32 y) {
            const std::size_t off =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.m_row_pitch) +
                static_cast<std::size_t>(x) * 4u;
            return {
                static_cast<u8>(pixels.m_bytes[off]),
                static_cast<u8>(pixels.m_bytes[off + 1u]),
                static_cast<u8>(pixels.m_bytes[off + 2u]),
                static_cast<u8>(pixels.m_bytes[off + 3u]),
            };
        }

        [[nodiscard]] u32 drawsDist_(
            const std::array<u8, 4u> &a, const std::array<u8, 4u> &b) noexcept {
            u32 dist = 0u;
            for (u32 i = 0u; i < 4u; ++i) {
                const u32 delta = a[i] > b[i] ? static_cast<u32>(a[i] - b[i]) : static_cast<u32>(b[i] - a[i]);
                dist = std::max(dist, delta);
            }
            return dist;
        }

        // Mean texel of a small window: robust to the exact pixel a thin quad
        // lands on, still decisive about which colour a region is.
        [[nodiscard]] std::array<u8, 4u> drawsWindow_(
            const DrawsPixels &pixels, const u32 x, const u32 y, const u32 radius) {
            std::array<u32, 4u> sum{};
            u32 count = 0u;
            for (u32 dy = 0u; dy <= radius * 2u; ++dy) {
                for (u32 dx = 0u; dx <= radius * 2u; ++dx) {
                    const std::array<u8, 4u> texel = drawsTexel_(pixels, x + dx, y + dy);
                    for (u32 c = 0u; c < 4u; ++c) {
                        sum[c] += texel[c];
                    }
                    ++count;
                }
            }
            return {
                static_cast<u8>(sum[0] / count),
                static_cast<u8>(sum[1] / count),
                static_cast<u8>(sum[2] / count),
                static_cast<u8>(sum[3] / count),
            };
        }

        [[nodiscard]] std::error_code drawsTarget_(
            rhi::IDevice &device, const char *label, rhi::ComPtr<rhi::ITexture> &out_target) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {256u, 256u, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = rhi::TextureUsage::RenderTarget;
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = label;
            return make_error_code(device.createTexture(desc, nullptr, out_target.writeRef()));
        }

        // Untextured material of a flat base colour: the shader multiplies the
        // albedo factor by the fallback white texture, so the drawn result is
        // that colour modulated by the fixed light rig.
        [[nodiscard]] Expected<MaterialHandle> drawsColorMaterial_(
            TrianglePass &pass, const float4 &rgb) {
            mesh::MaterialAsset material{};
            material.m_base_color = float4{rgb.x, rgb.y, rgb.z, 1.0f};
            material.m_roughness = 1.0f;
            material.m_metallic = 0.0f;
            const TextureHandle none[4] = {
                TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}
            };
            return pass.packMaterial(material, none);
        }

        // World-space X of a screen column, for this camera. Sampling a region
        // is about "is the instance that belongs here drawn here", not about
        // exact rasterisation.
        [[nodiscard]] u32 drawsScreenX_(const float world_x) noexcept {
            // 60 deg vertical fov, aspect 1, camera 3 units out on +Z.
            constexpr float kHalfWidth = 3.0f * 0.57735026f;
            const float ndc = world_x / kHalfWidth;
            return static_cast<u32>(128.0f + 128.0f * ndc);
        }

        // ------------------------------------------------------------------
        // tests
        // ------------------------------------------------------------------

        // The g_payload_base regression. Three instances are submitted in the
        // order red, green, red, which plans as two groups: group 0 (red quad)
        // owns payloads 0-1 and group 1 (green quad) owns payload 2. Group 1
        // therefore has a NON-ZERO payload base. If its draw started at
        // instance 0 without the base, it would read payload 0 — the red
        // instance's transform AND its red material — and the centre column
        // would come out red instead of green.
        PPR_UNIT_TEST (group_binds_its_own_payload_base) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Array<mesh::StaticMeshVertex> quad = drawsQuad_(0.25f);
            const Array<u32> indices = drawsQuadIndices_();
            const Expected<TriangleBagHandle> red_bag = pass.uploadMesh(quad, indices);
            PPR_TEST_ASSERT(red_bag.has_value());
            const Expected<TriangleBagHandle> green_bag = pass.uploadMesh(quad, indices);
            PPR_TEST_ASSERT(green_bag.has_value());

            const Expected<MaterialHandle> red = drawsColorMaterial_(pass, float4{1.0f, 0.0f, 0.0f, 1.0f});
            PPR_TEST_ASSERT(red.has_value());
            const Expected<MaterialHandle> green = drawsColorMaterial_(pass, float4{0.0f, 1.0f, 0.0f, 1.0f});
            PPR_TEST_ASSERT(green.has_value());

            // Interleaved red/green/red → the plan must be two groups with
            // bases 0 and 2. Asserted on the planner so the test fails with a
            // clear reason if the grouping itself regresses.
            // Interleaved red/green/red. The bags share a vertex layout, so
            // they share a bucket and its buffers — the groups split on the
            // resolved RANGE instead, and the plan must still be two groups
            // with bases 0 and 2. Asserted on the planner so the test fails
            // with a clear reason if the grouping itself regresses.
            const TrianglePipelineVariant opaque{};
            const TrianglePipelineVariant variants[3] = {opaque, opaque, opaque};
            rhi::IBuffer *const vertex_buffers[3] = {nullptr, nullptr, nullptr};
            rhi::IBuffer *const index_buffers[3] = {nullptr, nullptr, nullptr};
            const TriangleBagRange ranges[3] = {
                {.m_vb_offset = 0u, .m_count = 6u},
                {.m_vb_offset = 4u, .m_count = 6u},
                {.m_vb_offset = 0u, .m_count = 6u},
            };
            const Expected<TrianglePass::DrawPlan> plan = TrianglePass::planDraws(
                std::span<const TrianglePipelineVariant>{variants, 3u},
                std::span<rhi::IBuffer *const>{vertex_buffers, 3u},
                std::span<rhi::IBuffer *const>{index_buffers, 3u},
                std::span<const TriangleBagRange>{ranges, 3u});
            PPR_TEST_ASSERT(plan.has_value());
            PPR_TEST_ASSERT(plan->m_groups.size() == 2u);
            PPR_TEST_ASSERT(plan->m_groups[0].m_first_payload == 0u);
            PPR_TEST_ASSERT(plan->m_groups[0].m_instance_count == 2u);
            PPR_TEST_ASSERT(plan->m_groups[1].m_first_payload == 2u);
            PPR_TEST_ASSERT(plan->m_groups[1].m_instance_count == 1u);

            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.submitInstance(*red_bag, *red, drawsPlace_(-0.8f, 0.0f, 0.0f)));
            PPR_TEST_ASSERT(not pass.submitInstance(*green_bag, *green, drawsPlace_(0.0f, 0.0f, 0.0f)));
            PPR_TEST_ASSERT(not pass.submitInstance(*red_bag, *red, drawsPlace_(0.8f, 0.0f, 0.0f)));

            const float3 eye{0.0f, 0.0f, 3.0f};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, drawsCamera_(eye, float3{0.0f, 0.0f, 0.0f}, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(not drawsTarget_(device, "payload base", target));
            PPR_TEST_ASSERT(not p_renderer->renderToTexture(*target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not p_renderer->waitOnHost());

            const Expected<DrawsPixels> pixels = drawsReadback_(device, *target);
            PPR_TEST_ASSERT(pixels.has_value());
            PPR_TEST_ASSERT(not pixels->m_bytes.empty());

            const std::array<u8, 4u> background = drawsWindow_(*pixels, 4u, 4u, 2u);
            const std::array<u8, 4u> centre = drawsWindow_(*pixels, drawsScreenX_(0.0f), 128u, 2u);
            const std::array<u8, 4u> left = drawsWindow_(*pixels, drawsScreenX_(-0.8f), 128u, 2u);
            const std::array<u8, 4u> right = drawsWindow_(*pixels, drawsScreenX_(0.8f), 128u, 2u);

            // All three instances drew: the image is not a flat clear.
            PPR_TEST_ASSERT(drawsDist_(centre, background) > 20u);
            // The group with payload base 2 rendered ITS payload: green at the
            // centre. A missing g_payload_base binding reads payload 0 here and
            // this window comes out red.
            PPR_TEST_ASSERT(centre[1] > centre[0] + 20u);
            PPR_TEST_ASSERT(centre[1] > centre[2] + 20u);
            // The base-0 group rendered its two red instances at their own
            // transforms, one per column.
            PPR_TEST_ASSERT(left[0] > left[1] + 20u);
            PPR_TEST_ASSERT(left[0] > left[2] + 20u);
            PPR_TEST_ASSERT(right[0] > right[1] + 20u);
            PPR_TEST_ASSERT(right[0] > right[2] + 20u);
        };

        // One bag, one group, N instances: the group binds payload base 0 and
        // the draw issues N instances, so the shader's SV_InstanceID must walk
        // 0..N-1. If it read payload 0 for every instance the image would be a
        // single quad instead of N spread across the frame.
        PPR_UNIT_TEST (instanced_draw_differentiates_instances) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Array<mesh::StaticMeshVertex> quad = drawsQuad_(0.2f);
            const Array<u32> indices = drawsQuadIndices_();
            const Expected<TriangleBagHandle> bag = pass.uploadMesh(quad, indices);
            PPR_TEST_ASSERT(bag.has_value());
            const Expected<MaterialHandle> material = drawsColorMaterial_(pass, float4{0.0f, 0.0f, 1.0f, 1.0f});
            PPR_TEST_ASSERT(material.has_value());

            // SAME bag handle at five different transforms: one group, five
            // instances, payload base 0.
            constexpr float kPlacements[5] = {-1.2f, -0.6f, 0.0f, 0.6f, 1.2f};
            pass.clearInstances();
            for (const float x: kPlacements) {
                PPR_TEST_ASSERT(not pass.submitInstance(*bag, *material, drawsPlace_(x, 0.0f, 0.0f)));
            }

            const float3 eye{0.0f, 0.0f, 3.0f};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, drawsCamera_(eye, float3{0.0f, 0.0f, 0.0f}, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(not drawsTarget_(device, "instanced draw", target));
            PPR_TEST_ASSERT(not p_renderer->renderToTexture(*target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not p_renderer->waitOnHost());

            const Expected<DrawsPixels> pixels = drawsReadback_(device, *target);
            PPR_TEST_ASSERT(pixels.has_value());
            PPR_TEST_ASSERT(not pixels->m_bytes.empty());

            const std::array<u8, 4u> background = drawsWindow_(*pixels, 4u, 4u, 2u);
            u32 drawn_columns = 0u;
            for (const float x: kPlacements) {
                const std::array<u8, 4u> texel = drawsWindow_(*pixels, drawsScreenX_(x), 128u, 2u);
                if (texel[2] > texel[0] + 20u
                    and texel[2] > texel[1] + 20u
                and
                    drawsDist_(texel, background) > 20u)
                {
                    ++drawn_columns;
                }
            }
            // Every instance differentiated: five distinct quads, not one
            // quad drawn five times at the same place.
            PPR_TEST_ASSERT(drawn_columns == 5u);
        };

        // Empty and idle contracts of the single path: a clear-only frame
        // still succeeds, and shutdown is idempotent.
        PPR_UNIT_TEST (empty_scene_draws_and_shutdown_is_idempotent) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));

            const float3 eye{0.0f, 0.0f, 3.0f};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, drawsCamera_(eye, float3{0.0f, 0.0f, 0.0f}, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            rhi::ComPtr<rhi::ITexture> target{};
            PPR_TEST_ASSERT(not drawsTarget_(device, "empty scene", target));
            PPR_TEST_ASSERT(not p_renderer->renderToTexture(*target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not p_renderer->waitOnHost());

            // The frame stayed a uniform clear — no garbage, no partial draw.
            const Expected<DrawsPixels> pixels = drawsReadback_(device, *target);
            PPR_TEST_ASSERT(pixels.has_value());
            PPR_TEST_ASSERT(drawsDist_(drawsWindow_(*pixels, 128u, 128u, 4u), drawsWindow_(*pixels, 8u, 8u, 4u)) <= 2u);

            // Shutdown twice: the second call is a clean no-op, not an error
            // and not a use-after-free on the caches.
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.shutdown());
            // And the pass is usable again after a restart.
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not pass.shutdown());
        };
    } // namespace Draws

    namespace DrawLoss {
        // Private headless rendering application: same shape as the shared
        // fixture but owned by this quarantined leaf alone.
        constexpr ApplicationDomain kLossDomain{
            .m_is_headless = true,
            .m_is_interactive = false,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = false,
        };

        struct LossTestApp : Application {
            LossTestApp()
                : Application(kLossDomain, "AssetDrawLoss", std::span<const char *const>{}) {
            }

            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        [[nodiscard]] CameraSnapshot lossCamera_(
            const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] float4x4 lossPlace_(const float x, const float y, const float z) noexcept {
            return float4x4{
                float4{1.0f, 0.0f, 0.0f, 0.0f},
                float4{0.0f, 1.0f, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{x, y, z, 1.0f},
            };
        }

        [[nodiscard]] mesh::StaticMeshVertex lossVertex_(const float x, const float y) {
            return mesh::StaticMeshVertex{
                .m_position = {x, y, 0.0f},
                .m_normal = {0.0f, 0.0f, 1.0f},
                .m_texcoord = {x + 0.5f, y + 0.5f},
                .m_tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                .m_color = {1.0f, 1.0f, 1.0f, 1.0f},
            };
        }

        struct LossPixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] Expected<LossPixels> lossReadback_(rhi::IDevice &device, rhi::ITexture &target) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err =
                make_error_code(device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) {
                return std::unexpected{err};
            }
            if (blob.get() == nullptr
                or
            blob->getBufferPointer() == nullptr)
            {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            LossPixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::error_code lossTarget_(
            rhi::IDevice &device, const char *label, rhi::ComPtr<rhi::ITexture> &out_target) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {256u, 256u, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = rhi::TextureUsage::RenderTarget;
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = label;
            return make_error_code(device.createTexture(desc, nullptr, out_target.writeRef()));
        }

        [[nodiscard]] u64 lossHash_(const LossPixels &pixels) noexcept {
            u64 hash = 0xcbf29ce484222325ULL;
            for (std::size_t i = 0u; i < pixels.m_bytes.size(); ++i) {
                hash ^= static_cast<u64>(pixels.m_bytes[i]);
                hash *= 0x100000001b3ULL;
            }
            return hash;
        }

        // Device loss through the single draw path, then an explicit
        // shutdown + initialize restart that must reproduce the pre-loss image
        // byte for byte. In-place re-initialize while lost stays closed.
        PPR_UNIT_TEST (device_loss_restart_reproduces_the_frame) {
            LossTestApp loss_app{};
            PPR_TEST_ASSERT(not loss_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not loss_app.teardown()); };
            const auto rhi = loss_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = loss_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            const std::error_code kNotConnected = std::make_error_code(std::errc::not_connected);
            const std::error_code kBusy = std::make_error_code(std::errc::device_or_resource_busy);

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));

            const mesh::StaticMeshVertex quad[] = {
                lossVertex_(-0.5f, -0.5f),
                lossVertex_(0.5f, -0.5f),
                lossVertex_(0.5f, 0.5f),
                lossVertex_(-0.5f, 0.5f),
            };
            const u32 quad_idx[] = {0u, 1u, 2u, 0u, 2u, 3u};
            const Expected<TriangleBagHandle> bag = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag.has_value());
            mesh::MaterialAsset material{};
            material.m_base_color = float4{0.2f, 0.6f, 0.9f, 1.0f};
            const TextureHandle none[4] = {
                TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}
            };
            const Expected<MaterialHandle> packed = pass.packMaterial(material, none);
            PPR_TEST_ASSERT(packed.has_value());

            // Two instances, so the restart compares a real instanced draw.
            PPR_TEST_ASSERT(not pass.submitInstance(*bag, *packed, lossPlace_(-0.6f, 0.0f, 0.0f)));
            PPR_TEST_ASSERT(not pass.submitInstance(*bag, *packed, lossPlace_(0.6f, 0.0f, 0.0f)));

            const float3 eye{0.0f, 0.0f, 3.0f};
            const CameraSnapshot camera = lossCamera_(eye, float3{0.0f, 0.0f, 0.0f}, float2{256.0f, 256.0f});
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, camera));

            Renderer &renderer = loss_app.getRenderer();
            rhi::ComPtr<rhi::ITexture> pre_target{};
            PPR_TEST_ASSERT(not lossTarget_(device, "loss pre-loss", pre_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*pre_target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<LossPixels> pre = lossReadback_(device, *pre_target);
            PPR_TEST_ASSERT(pre.has_value());
            PPR_TEST_ASSERT(not pre->m_bytes.empty());
            const u64 pre_hash = lossHash_(*pre);

            PPR_TEST_ASSERT(not pass.notifyDeviceLost());

            // Parked: residency drops on all three caches, the program
            // survives but cached pipelines do not, and GPU-touching calls
            // fail closed. Re-initialize while lost is closed; restart is
            // shutdown + initialize, never in place.
            PPR_TEST_ASSERT(pass.bagCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.textureCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.materialCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.uploadMesh(quad, quad_idx).error() == kNotConnected);
            PPR_TEST_ASSERT(not pass.notifyDeviceLost());
            PPR_TEST_ASSERT(pass.bagCache().initialize(device) == kBusy);

            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            const Expected<TriangleBagHandle> rebag = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(rebag.has_value());
            const Expected<MaterialHandle> repacked = pass.packMaterial(material, none);
            PPR_TEST_ASSERT(repacked.has_value());
            PPR_TEST_ASSERT(not pass.submitInstance(*rebag, *repacked, lossPlace_(-0.6f, 0.0f, 0.0f)));
            PPR_TEST_ASSERT(not pass.submitInstance(*rebag, *repacked, lossPlace_(0.6f, 0.0f, 0.0f)));
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, camera));

            rhi::ComPtr<rhi::ITexture> post_target{};
            PPR_TEST_ASSERT(not lossTarget_(device, "loss post-restart", post_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*post_target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<LossPixels> post = lossReadback_(device, *post_target);
            PPR_TEST_ASSERT(post.has_value());
            PPR_TEST_ASSERT(post->m_bytes.size() == pre->m_bytes.size());
            PPR_TEST_ASSERT(lossHash_(*post) == pre_hash);
            for (std::size_t i = 0u; i < pre->m_bytes.size(); ++i) {
                PPR_TEST_ASSERT(post->m_bytes[i] == pre->m_bytes[i]);
            }

            PPR_TEST_ASSERT(not pass.shutdown());
        };
    } // namespace DrawLoss
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest &gateEditorLeaf() noexcept;

    const UnitTest draws = UnitTest::Named("draws") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Draws::group_binds_its_own_payload_base,
            detail::Draws::instanced_draw_differentiates_instances,
            detail::Draws::empty_scene_draws_and_shutdown_is_idempotent,
        });
    };

    const UnitTest quarantine = UnitTest::Named("quarantine") / [](UnitTest::IRun &_) -> void {
        // No SharedGpu acquire: this leaf boots its own private app after the
        // shared session is torn down.
        _.recurse({
            gateEditorLeaf(),
            detail::DrawLoss::device_loss_restart_reproduces_the_frame,
        });
    };

    const UnitTest &renderDrawsTests() noexcept {
        return draws;
    }

    const UnitTest &renderQuarantineTests() noexcept {
        return quarantine;
    }
} // namespace pP::tests
