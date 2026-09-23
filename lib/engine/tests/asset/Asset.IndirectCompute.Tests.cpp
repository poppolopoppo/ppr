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

// Phase 7 L2b compute-publish gate: the kernel-published path (dispatch +
// UAV counter + ring + barriers) proves (a) the GPU-written count matches
// the CPU staged count, (b) the compute path stays pixel-exact vs direct,
// (c) count guards fail closed, (d) the 3-frame ring retires by fence value
// with slot 0 of the texture heap still pinned. One focused test per TU.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();

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

namespace pP::tests {
    const UnitTest indirect_compute = UnitTest::Named("indirect_compute") / [](UnitTest::IRun &_) -> void {
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        PPR_DEFER{PPR_TEST_ASSERT(not detail::SharedGpu::release()); };
        _.recurse({
            detail::IndirectCompute::compute_publish_count_parity_guards,
        });
    };

    const UnitTest &indirectComputeTests() noexcept {
        return indirect_compute;
    }
} // namespace pP::tests
