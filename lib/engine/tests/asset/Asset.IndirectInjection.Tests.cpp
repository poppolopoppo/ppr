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

// Failure injection (indirect path): RHI allocation/map failure
// points must leave no leaked or half-published entry, and the over-count /
// empty-scene behavior extends the publish guards to the draw level (the
// publish contract proves the clamped publish still draws and the CPU-staged
// path draws clear-only when empty). Vulkan CI stance lives on the loss lane
// (Asset.IndirectLoss.Tests.cpp). One focused test per TU.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();

    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

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
                if (mesh_index >= scene.m_meshes.size() or node_index >= scene.m_nodes.size())
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

namespace pP::tests {
    const UnitTest indirect_injection = UnitTest::Named("indirect_injection") / [](UnitTest::IRun &_) -> void {
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        PPR_DEFER{PPR_TEST_ASSERT(not detail::SharedGpu::release()); };
        _.recurse({
            detail::IndirectInjection::indirect_injection_no_half_publish,
        });
    };

    const UnitTest &indirectInjectionTests() noexcept {
        return indirect_injection;
    }
} // namespace pP::tests
