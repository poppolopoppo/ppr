module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>
#include <mango/import3d/mesh.hpp>
#include <slang.h>
#include <slang-com-ptr.h>

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import std;

// Indirect draw proofs under the shared render scope (the asset/render
// parent in Asset.Tests.cpp owns the single SharedGpu acquire): compute
// publish, injection fail-closed, direct-vs-indirect parity � plus the
// quarantined device-loss restart leaf, which boots its private LossTestApp
// and never touches shared refs. Groups: asset/render/indirect (shared 3),
// asset/render/quarantine (loss + the gate editor flow).
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace IndirectCompute {
        // DrawSubmission adapter: encodes renderIndirectCompute() for the
        // last published slot (same staged instance list as the direct path).
        struct ComputeProbe {
            TrianglePass *m_pass = nullptr;

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) {
                return m_pass->renderIndirectCompute(draw_context);
            }
        };

        [[nodiscard]] std::filesystem::path computeMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> computeDecodePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(computeMeshDir() / std::string{name});
            if (not
                mapped.has_value())
            {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        [[nodiscard]] CameraSnapshot computeCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] Expected<u32> computeSubmit_(
            TrianglePass &pass,
            const mesh::SceneAsset &scene,
            const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            u32 submitted = 0u;
            std::size_t prim_cursor = 0u;
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size()
                    or node_index >= scene.m_nodes.size())
                {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                const float4x4 &world = scene.m_nodes[node_index].m_world;
                for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim: scene.m_meshes[mesh_index].m_prims) {
                    if (prim_cursor >= uploaded.m_prims.size()) {
                        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                    }
                    const TrianglePass::UploadedPrimitive &up = uploaded.m_prims[prim_cursor++];
                    if (const std::error_code submit_err = pass.submitInstance(up.m_bag, up.m_material, world)) {
                        return std::unexpected{submit_err};
                    }
                    ++submitted;
                }
            }
            return submitted;
        }

        struct ComputePixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] Expected<ComputePixels> computeReadback_(rhi::IDevice &device, rhi::ITexture &target) {
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
            ComputePixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::array<u8, 4u> computeTexel_(const ComputePixels &pixels, const u32 x, const u32 y) {
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

        [[nodiscard]] u32 computeSpread_(const std::array<u8, 4u> &texel) noexcept {
            const u8 hi = std::max({texel[0], texel[1], texel[2]});
            const u8 lo = std::min({texel[0], texel[1], texel[2]});
            return static_cast<u32>(hi - lo);
        }

        [[nodiscard]] u32 computeDist_(const std::array<u8, 4u> &a, const std::array<u8, 4u> &b) noexcept {
            u32 dist = 0u;
            for (u32 i = 0u; i < 4u; ++i) {
                const u32 delta = a[i] > b[i] ? static_cast<u32>(a[i] - b[i]) : static_cast<u32>(b[i] - a[i]);
                dist = std::max(dist, delta);
            }
            return dist;
        }

        [[nodiscard]] std::error_code computeTarget_(
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

        PPR_UNIT_TEST (compute_publish_count_parity_guards) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(computeMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(not scene->m_instances.empty());
            const Expected<image::ImageAsset> decoded = computeDecodePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());

            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            const Expected<u32> submitted = computeSubmit_(pass, *scene, *uploaded);
            PPR_TEST_ASSERT(submitted.has_value());
            PPR_TEST_ASSERT(*submitted > 0u);

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, computeCamera_(eye, center, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
            ComputeProbe probe{.m_pass = &pass};

            // (a) GPU-written count matches the CPU staged count, plus the
            // heap slot-0 pin (first real texture lands at slot >= 1).
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == *submitted);
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(pass.ringSlotVersion(pass.publishedSlot()) == 1u);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<u32> gpu_count = pass.readPublishedCount(device);
            PPR_TEST_ASSERT(gpu_count.has_value());
            PPR_TEST_ASSERT(*gpu_count == *submitted);
            PPR_TEST_ASSERT(not uploaded->m_textures.empty());
            const Expected<TextureBindlessIndex> heap_slot =
                    pass.textureCache().residentIndex(uploaded->m_textures.front());
            PPR_TEST_ASSERT(heap_slot.has_value());
            PPR_TEST_ASSERT(static_cast<u32>(*heap_slot) >= 1u);

            // (b) Compute-path parity: direct vs kernel-published draws stay
            // pixel-exact (fresh publish → fresh ring slot, same instances).
            rhi::ComPtr<rhi::ITexture> direct_target{};
            PPR_TEST_ASSERT(not computeTarget_(device, "compute parity direct", direct_target));
            rhi::ComPtr<rhi::ITexture> compute_target{};
            PPR_TEST_ASSERT(not computeTarget_(device, "compute parity compute", compute_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*direct_target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*compute_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<ComputePixels> direct = computeReadback_(device, *direct_target);
            PPR_TEST_ASSERT(direct.has_value());
            const Expected<ComputePixels> through_compute = computeReadback_(device, *compute_target);
            PPR_TEST_ASSERT(through_compute.has_value());
            PPR_TEST_ASSERT(direct->m_bytes.size() == through_compute->m_bytes.size());
            PPR_TEST_ASSERT(not direct->m_bytes.empty());
            PPR_TEST_ASSERT(computeSpread_(computeTexel_(*direct, 128u, 128u)) > 12u);
            PPR_TEST_ASSERT(computeSpread_(computeTexel_(*through_compute, 128u, 128u)) > 12u);
            for (std::size_t i = 0u; i < direct->m_bytes.size(); ++i) {
                PPR_TEST_ASSERT(direct->m_bytes[i] == through_compute->m_bytes[i]);
            }
            const std::array<u8, 4u> background = computeTexel_(*direct, 8u, 8u);

            // (c) Ring retirement: a 3-publish burst without waits always
            // succeeds (the ring absorbs 3 in flight — pigeonhole); after
            // the drain the completed fence reclaims slot 0 first-fit with
            // a bumped version and a fence value past every prior signal.
            // (Exact slot identities mid-burst are timing-dependent — the
            // micro-dispatch can complete before the next publish — so only
            // the post-drain reuse is pinned.)
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const u32 version_before = pass.ringSlotVersion(0u);
            PPR_TEST_ASSERT(version_before >= 1u);
            const u64 fence_before = std::max({
                pass.ringSlotFence(0u), pass.ringSlotFence(1u), pass.ringSlotFence(2u)
            });
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() == 0u);
            PPR_TEST_ASSERT(pass.ringSlotVersion(0u) == version_before + 1u);
            PPR_TEST_ASSERT(pass.ringSlotFence(0u) > fence_before);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<u32> rotated_count = pass.readPublishedCount(device);
            PPR_TEST_ASSERT(rotated_count.has_value());
            PPR_TEST_ASSERT(*rotated_count == *submitted);

            // (d) Empty scene: publish succeeds with no dispatch, draw is a
            // clear-only no-op, and no count is readable.
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() == TrianglePass::kInvalidIndirectSlot);
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == 0u);
            PPR_TEST_ASSERT(not pass.readPublishedCount(device).has_value());
            PPR_TEST_ASSERT(
                pass.readPublishedCount(device).error() == std::make_error_code(std::errc::invalid_argument));
            rhi::ComPtr<rhi::ITexture> empty_target{};
            PPR_TEST_ASSERT(not computeTarget_(device, "compute parity empty", empty_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*empty_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<ComputePixels> empty = computeReadback_(device, *empty_target);
            PPR_TEST_ASSERT(empty.has_value());
            PPR_TEST_ASSERT(computeDist_(computeTexel_(*empty, 128u, 128u), background) <= 6u);

            // (e) Over-count: ring capacity + 1 instances clamp to capacity
            // and fail closed (invalid_argument) — the GPU counter proves
            // the clamp, never an overrun.
            PPR_TEST_ASSERT(not uploaded->m_prims.empty());
            for (u32 i = 0u; i <= TrianglePass::kIndirectRingCapacity; ++i) {
                PPR_TEST_ASSERT(not pass.submitInstance(
                    uploaded->m_prims.front().m_bag, uploaded->m_prims.front().m_material, float4x4::identity()));
            }
            const std::error_code over_err = pass.publishIndirectCompute(device);
            PPR_TEST_ASSERT(over_err == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == TrianglePass::kIndirectRingCapacity);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<u32> clamped_count = pass.readPublishedCount(device);
            PPR_TEST_ASSERT(clamped_count.has_value());
            PPR_TEST_ASSERT(*clamped_count == TrianglePass::kIndirectRingCapacity);

            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
        };
    } // namespace IndirectCompute
} // namespace pP::tests::detail

namespace pP::tests::detail {
    namespace IndirectInjection {
        // DrawSubmission adapter: encodes renderIndirectCompute() for the
        // last published slot (same staged instance list as the direct path).
        struct InjectionProbe {
            TrianglePass *m_pass = nullptr;

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) {
                return m_pass->renderIndirectCompute(draw_context);
            }
        };

        [[nodiscard]] std::filesystem::path injectionMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> injectionDecodePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(injectionMeshDir() / std::string{name});
            if (not
                mapped.has_value())
            {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        [[nodiscard]] CameraSnapshot injectionCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] Expected<u32> injectionSubmit_(
            TrianglePass &pass,
            const mesh::SceneAsset &scene,
            const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            u32 submitted = 0u;
            std::size_t prim_cursor = 0u;
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size()
                    or node_index >= scene.m_nodes.size())
                {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                const float4x4 &world = scene.m_nodes[node_index].m_world;
                for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim: scene.m_meshes[mesh_index].m_prims) {
                    if (prim_cursor >= uploaded.m_prims.size()) {
                        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                    }
                    const TrianglePass::UploadedPrimitive &up = uploaded.m_prims[prim_cursor++];
                    if (const std::error_code submit_err = pass.submitInstance(up.m_bag, up.m_material, world)) {
                        return std::unexpected{submit_err};
                    }
                    ++submitted;
                }
            }
            return submitted;
        }

        struct InjectionPixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] Expected<InjectionPixels> injectionReadback_(rhi::IDevice &device, rhi::ITexture &target) {
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
            InjectionPixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::array<u8, 4u> injectionTexel_(const InjectionPixels &pixels, const u32 x, const u32 y) {
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

        [[nodiscard]] u32 injectionSpread_(const std::array<u8, 4u> &texel) noexcept {
            const u8 hi = std::max({texel[0], texel[1], texel[2]});
            const u8 lo = std::min({texel[0], texel[1], texel[2]});
            return static_cast<u32>(hi - lo);
        }

        [[nodiscard]] u32 injectionDist_(const std::array<u8, 4u> &a, const std::array<u8, 4u> &b) noexcept {
            u32 dist = 0u;
            for (u32 i = 0u; i < 4u; ++i) {
                const u32 delta = a[i] > b[i] ? static_cast<u32>(a[i] - b[i]) : static_cast<u32>(b[i] - a[i]);
                dist = std::max(dist, delta);
            }
            return dist;
        }

        [[nodiscard]] std::error_code injectionTarget_(
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

        PPR_UNIT_TEST (indirect_injection_no_half_publish) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            const std::error_code kInvalid = std::make_error_code(std::errc::invalid_argument);

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(injectionMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(not scene->m_instances.empty());
            const Expected<image::ImageAsset> decoded = injectionDecodePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());

            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded_a = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded_a.has_value());
            const u32 bags_after_a = pass.bagCache().rangeCount();
            const u32 textures_after_a = pass.textureCache().entryCount();
            const u32 materials_after_a = pass.materialCache().entryCount();

            // (a) Upload failure points roll back with no half-published entry:
            // image-span mismatch rejects before any acquisition, and a
            // mid-upload texture rejection (zero-size image) unwinds the bags
            // it already acquired — telemetry returns to the post-A levels.
            PPR_TEST_ASSERT(
                pass.uploadScene(*scene, std::span<const image::ImageAsset>{}).error() == kInvalid);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == bags_after_a);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == textures_after_a);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == materials_after_a);

            Array<image::ImageAsset> bad_images{};
            bad_images.push_back(image::ImageAsset{});
            PPR_TEST_ASSERT(pass.uploadScene(*scene, bad_images).error() == kInvalid);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == bags_after_a);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == textures_after_a);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == materials_after_a);

            // No receipt leaked by either failure: a fresh upload succeeds and
            // drains cleanly (double release still fails closed).
            const Expected<TrianglePass::UploadedScene> uploaded_b = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded_b.has_value());
            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded_b));
            PPR_TEST_ASSERT(pass.releaseScene(*uploaded_b) == kInvalid);

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, injectionCamera_(eye, center, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
            InjectionProbe probe{.m_pass = &pass};

            // (b) Over-count past the publish contract: L2b proves the clamp at
            // publish; this extends it to the draw — the clamped publish stays
            // drawable and the GPU counter proves the clamp, never an overrun.
            PPR_TEST_ASSERT(not uploaded_a->m_prims.empty());
            for (u32 i = 0u; i <= TrianglePass::kIndirectRingCapacity; ++i) {
                PPR_TEST_ASSERT(not pass.submitInstance(
                    uploaded_a->m_prims.front().m_bag, uploaded_a->m_prims.front().m_material, float4x4::identity()));
            }
            PPR_TEST_ASSERT(pass.publishIndirectCompute(device) == kInvalid);
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == TrianglePass::kIndirectRingCapacity);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<u32> clamped_count = pass.readPublishedCount(device);
            PPR_TEST_ASSERT(clamped_count.has_value());
            PPR_TEST_ASSERT(*clamped_count == TrianglePass::kIndirectRingCapacity);
            rhi::ComPtr<rhi::ITexture> clamped_target{};
            PPR_TEST_ASSERT(not injectionTarget_(device, "injection clamped draw", clamped_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*clamped_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<InjectionPixels> clamped = injectionReadback_(device, *clamped_target);
            PPR_TEST_ASSERT(clamped.has_value());
            PPR_TEST_ASSERT(injectionSpread_(injectionTexel_(*clamped, 128u, 128u)) > 12u);

            // (c) Empty scene on the L1 CPU-staged path: publish succeeds with
            // nothing published (L2b covers the compute path); renderIndirect
            // draws clear-only — the frame stays uniform, never garbage.
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() == TrianglePass::kInvalidIndirectSlot);
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == 0u);
            rhi::ComPtr<rhi::ITexture> empty_target{};
            PPR_TEST_ASSERT(not injectionTarget_(device, "injection empty staged", empty_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*empty_target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<InjectionPixels> empty = injectionReadback_(device, *empty_target);
            PPR_TEST_ASSERT(empty.has_value());
            PPR_TEST_ASSERT(
                injectionDist_(injectionTexel_(*empty, 128u, 128u), injectionTexel_(*empty, 8u, 8u)) <= 2u);

            // Healthy tail: the ring machine still publishes after the
            // injections (no leaked slot), then both scenes drain to zero.
            const Expected<u32> submitted = injectionSubmit_(pass, *scene, *uploaded_a);
            PPR_TEST_ASSERT(submitted.has_value());
            PPR_TEST_ASSERT(*submitted > 0u);
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<u32> tail_count = pass.readPublishedCount(device);
            PPR_TEST_ASSERT(tail_count.has_value());
            PPR_TEST_ASSERT(*tail_count == *submitted);

            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded_a));
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 0u);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == 0u);
        };
    } // namespace IndirectInjection
} // namespace pP::tests::detail

namespace pP::tests::detail {
    namespace IndirectParity {
        // DrawSubmission adapter: DrawSubmission{pass} encodes render(); this
        // encodes renderIndirect() for the same staged instance list.
        struct IndirectProbe {
            TrianglePass *m_pass = nullptr;

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) {
                return m_pass->renderIndirect(draw_context);
            }
        };

        [[nodiscard]] std::filesystem::path parityMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> parityDecodePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(parityMeshDir() / std::string{name});
            if (not
                mapped.has_value())
            {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        [[nodiscard]] CameraSnapshot parityCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] std::error_code paritySubmit_(
            TrianglePass &pass,
            const mesh::SceneAsset &scene,
            const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            std::size_t prim_cursor = 0u;
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size()
                    or node_index >= scene.m_nodes.size())
                {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                const float4x4 &world = scene.m_nodes[node_index].m_world;
                for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim: scene.m_meshes[mesh_index].m_prims) {
                    if (prim_cursor >= uploaded.m_prims.size()) {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const TrianglePass::UploadedPrimitive &up = uploaded.m_prims[prim_cursor++];
                    if (const std::error_code submit_err = pass.submitInstance(up.m_bag, up.m_material, world)) {
                        return submit_err;
                    }
                }
            }
            return default_value_v;
        }

        struct ParityPixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
        };

        [[nodiscard]] Expected<ParityPixels> parityReadback_(rhi::IDevice &device, rhi::ITexture &target) {
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
            ParityPixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] u8 paritySpread_(const std::array<u8, 4u> &texel) noexcept {
            const u8 hi = std::max({texel[0], texel[1], texel[2]});
            const u8 lo = std::min({texel[0], texel[1], texel[2]});
            return static_cast<u8>(hi - lo);
        }

        PPR_UNIT_TEST (direct_vs_indirect_pixel_exact) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(parityMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(not scene->m_instances.empty());
            const Expected<image::ImageAsset> decoded = parityDecodePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());

            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            PPR_TEST_ASSERT(not paritySubmit_(pass, *scene, *uploaded));

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, parityCamera_(eye, center, float2{256.0f, 256.0f})));

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;

            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {256u, 256u, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = rhi::TextureUsage::RenderTarget;
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = "indirect parity target";
            rhi::ComPtr<rhi::ITexture> direct_target{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, direct_target.writeRef())));
            rhi::ComPtr<rhi::ITexture> indirect_target{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, indirect_target.writeRef())));

            IndirectProbe probe{.m_pass = &pass};
            PPR_TEST_ASSERT(not renderer.renderToTexture(*direct_target, {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not renderer.renderToTexture(*indirect_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());

            const Expected<ParityPixels> direct = parityReadback_(device, *direct_target);
            PPR_TEST_ASSERT(direct.has_value());
            const Expected<ParityPixels> indirect = parityReadback_(device, *indirect_target);
            PPR_TEST_ASSERT(indirect.has_value());

            // Both paths drew the box (chromatic center, not a flat clear).
            PPR_TEST_ASSERT(direct->m_bytes.size() == indirect->m_bytes.size());
            PPR_TEST_ASSERT(not direct->m_bytes.empty());
            const auto texel_at = [](const ParityPixels &pixels, const u32 x, const u32 y) -> std::array<u8, 4u> {
                const std::size_t off =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.m_row_pitch) +
                        static_cast<std::size_t>(x) * 4u;
                return {
                    static_cast<u8>(pixels.m_bytes[off]),
                    static_cast<u8>(pixels.m_bytes[off + 1u]),
                    static_cast<u8>(pixels.m_bytes[off + 2u]),
                    static_cast<u8>(pixels.m_bytes[off + 3u]),
                };
            };
            PPR_TEST_ASSERT(paritySpread_(texel_at(*direct, 128u, 128u)) > 12u);
            PPR_TEST_ASSERT(paritySpread_(texel_at(*indirect, 128u, 128u)) > 12u);

            // Pixel-exact parity: same math, same inputs — any divergence is
            // a payload/args staging bug, never acceptable drift.
            for (std::size_t i = 0u; i < direct->m_bytes.size(); ++i) {
                PPR_TEST_ASSERT(direct->m_bytes[i] == indirect->m_bytes[i]);
            }

            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
        };
    } // namespace IndirectParity
} // namespace pP::tests::detail

namespace pP::tests::detail {
    namespace IndirectLoss {
        // Private headless rendering application: same shape as the shared
        // fixture (Asset.GpuFixture.cpp) but owned by this leaf alone.
        constexpr ApplicationDomain kLossDomain{
            .m_is_headless = true,
            .m_is_interactive = false,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = false,
        };

        struct LossTestApp : Application {
            LossTestApp()
                : Application(kLossDomain, "AssetIndirectLoss", std::span<const char *const>{}) {
            }

            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        // DrawSubmission adapter: encodes renderIndirectCompute() for the
        // last published slot (same staged instance list as the direct path).
        struct LossProbe {
            TrianglePass *m_pass = nullptr;

            [[nodiscard]] std::error_code render(const DrawContext &draw_context) {
                return m_pass->renderIndirectCompute(draw_context);
            }
        };

        [[nodiscard]] std::filesystem::path lossMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> lossDecodePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(lossMeshDir() / std::string{name});
            if (not
                mapped.has_value())
            {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        [[nodiscard]] CameraSnapshot lossCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] float3 lossEye_(const mesh::SceneAsset &scene) {
            const mesh::StaticMeshAsset &mesh_asset = scene.m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            return float3{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
        }

        [[nodiscard]] Expected<u32> lossSubmit_(
            TrianglePass &pass,
            const mesh::SceneAsset &scene,
            const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            u32 submitted = 0u;
            std::size_t prim_cursor = 0u;
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size()
                    or node_index >= scene.m_nodes.size())
                {
                    return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                }
                const float4x4 &world = scene.m_nodes[node_index].m_world;
                for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim: scene.m_meshes[mesh_index].m_prims) {
                    if (prim_cursor >= uploaded.m_prims.size()) {
                        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
                    }
                    const TrianglePass::UploadedPrimitive &up = uploaded.m_prims[prim_cursor++];
                    if (const std::error_code submit_err = pass.submitInstance(up.m_bag, up.m_material, world)) {
                        return std::unexpected{submit_err};
                    }
                    ++submitted;
                }
            }
            return submitted;
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

        PPR_UNIT_TEST (indirect_device_loss_midframe_restart_hash) {
            LossTestApp loss_app{};
            PPR_TEST_ASSERT(not loss_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not loss_app.teardown()); };
            const auto rhi = loss_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = loss_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            const std::error_code kNotConnected = std::make_error_code(std::errc::not_connected);
            const std::error_code kNoDevice = std::make_error_code(std::errc::no_such_device);
            const std::error_code kInvalid = std::make_error_code(std::errc::invalid_argument);

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(lossMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(not scene->m_instances.empty());
            const Expected<image::ImageAsset> decoded = lossDecodePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());

            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            const Expected<u32> submitted = lossSubmit_(pass, *scene, *uploaded);
            PPR_TEST_ASSERT(submitted.has_value());
            PPR_TEST_ASSERT(*submitted > 0u);
            const float3 center = scene->m_meshes.front().m_bounds.center();
            const float3 eye = lossEye_(*scene);
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, lossCamera_(eye, center, float2{256.0f, 256.0f})));

            Renderer &renderer = loss_app.getRenderer();
            LossProbe probe{.m_pass = &pass};

            // Pre-loss readback hash through the compute path.
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == *submitted);
            rhi::ComPtr<rhi::ITexture> pre_target{};
            PPR_TEST_ASSERT(not lossTarget_(device, "loss pre-loss", pre_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*pre_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<LossPixels> pre = lossReadback_(device, *pre_target);
            PPR_TEST_ASSERT(pre.has_value());
            PPR_TEST_ASSERT(not pre->m_bytes.empty());
            const u64 pre_hash = lossHash_(*pre);

            // Mid-frame loss: a fresh publish is live (slot valid, draw not
            // yet issued) when the device drops.
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedSlot() < TrianglePass::kIndirectRingFrames);
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == *submitted);
            PPR_TEST_ASSERT(not pass.notifyDeviceLost());

            // Parked: residency drops on all three caches, the published slot
            // is discarded (retirement is meaningless — GPU objects are gone),
            // pass-level GPU calls fail not_connected while cache-level GPU
            // ops fail no_such_device; slot versions read back as never
            // acquired. Re-initialize while lost fails closed — restart is
            // shutdown + initialize, never in place.
            PPR_TEST_ASSERT(pass.bagCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.textureCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.materialCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.publishedSlot() == TrianglePass::kInvalidIndirectSlot);
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == 0u);
            PPR_TEST_ASSERT(pass.ringSlotVersion(0u) == 0u);
            PPR_TEST_ASSERT(pass.ringSlotVersion(1u) == 0u);
            PPR_TEST_ASSERT(pass.ringSlotVersion(2u) == 0u);
            PPR_TEST_ASSERT(pass.publishIndirectCompute(device) == kNotConnected);
            PPR_TEST_ASSERT(pass.readPublishedCount(device).error() == kNotConnected);
            PPR_TEST_ASSERT(pass.uploadMesh({}, {}).error() == kNotConnected);
            const mesh::StaticMeshVertex loss_quad[] = {
                {
                    .m_position = {-0.5f, -0.5f, 0.0f},
                    .m_normal = {0.0f, 0.0f, 1.0f},
                    .m_texcoord = {0.0f, 0.0f},
                    .m_tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                    .m_color = {1.0f, 1.0f, 1.0f, 1.0f}
                },
                {
                    .m_position = {0.5f, -0.5f, 0.0f},
                    .m_normal = {0.0f, 0.0f, 1.0f},
                    .m_texcoord = {1.0f, 0.0f},
                    .m_tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                    .m_color = {1.0f, 1.0f, 1.0f, 1.0f}
                },
                {
                    .m_position = {0.5f, 0.5f, 0.0f},
                    .m_normal = {0.0f, 0.0f, 1.0f},
                    .m_texcoord = {1.0f, 1.0f},
                    .m_tangent = {1.0f, 0.0f, 0.0f, 1.0f},
                    .m_color = {1.0f, 1.0f, 1.0f, 1.0f}
                },
            };
            const u32 loss_idx[] = {0u, 1u, 2u};
            PPR_TEST_ASSERT(pass.bagCache().upload(
                                std::span<const mesh::StaticMeshVertex>{loss_quad},
                                std::span<const u32>{loss_idx}).error() == kNoDevice);
            PPR_TEST_ASSERT(pass.textureCache().upload(*decoded).error() == kNoDevice);

            // Restart: shutdown + initialize, then the identical scene flows
            // through publish + draw again and reproduces the pre-loss hash.
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            const Expected<TrianglePass::UploadedScene> reuploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(reuploaded.has_value());
            const Expected<u32> resubmitted = lossSubmit_(pass, *scene, *reuploaded);
            PPR_TEST_ASSERT(resubmitted.has_value());
            PPR_TEST_ASSERT(*resubmitted == *submitted);
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, lossCamera_(eye, center, float2{256.0f, 256.0f})));
            PPR_TEST_ASSERT(not pass.publishIndirectCompute(device));
            PPR_TEST_ASSERT(pass.publishedInstanceCount() == *submitted);
            rhi::ComPtr<rhi::ITexture> post_target{};
            PPR_TEST_ASSERT(not lossTarget_(device, "loss post-restart", post_target));
            PPR_TEST_ASSERT(not renderer.renderToTexture(*post_target, {DrawSubmission{probe}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<LossPixels> post = lossReadback_(device, *post_target);
            PPR_TEST_ASSERT(post.has_value());
            PPR_TEST_ASSERT(post->m_bytes.size() == pre->m_bytes.size());
            PPR_TEST_ASSERT(lossHash_(*post) == pre_hash);
            for (std::size_t i = 0u; i < pre->m_bytes.size(); ++i) {
                PPR_TEST_ASSERT(post->m_bytes[i] == pre->m_bytes[i]);
            }

            PPR_TEST_ASSERT(not renderer.waitOnHost());
            PPR_TEST_ASSERT(not pass.releaseScene(*reuploaded));
            PPR_TEST_ASSERT(not pass.shutdown());
        };
    } // namespace IndirectLoss
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest &gateEditorLeaf() noexcept;

    const UnitTest indirect = UnitTest::Named("indirect") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::IndirectCompute::compute_publish_count_parity_guards,
            detail::IndirectInjection::indirect_injection_no_half_publish,
            detail::IndirectParity::direct_vs_indirect_pixel_exact,
        });
    };

    const UnitTest quarantine = UnitTest::Named("quarantine") / [](UnitTest::IRun &_) -> void {
        // No SharedGpu acquire: both leaves boot private apps (LossTestApp
        // here, GateEditorApp in the gates TU) after the shared session
        // is torn down.
        _.recurse({
            gateEditorLeaf(),
            detail::IndirectLoss::indirect_device_loss_midframe_restart_hash,
        });
    };

    const UnitTest &renderIndirectTests() noexcept {
        return indirect;
    }

    const UnitTest &renderQuarantineTests() noexcept {
        return quarantine;
    }
} // namespace pP::tests
