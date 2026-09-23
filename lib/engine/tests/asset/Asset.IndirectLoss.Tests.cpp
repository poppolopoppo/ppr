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

// Device-loss injection (indirect path): a loss landing mid-frame
// (published slot live, draw not yet issued) parks every GPU-touching call
// and discards ring retirement outright; the documented restart (shutdown +
// initialize, never in place) then reproduces the pre-loss readback hash.
// One focused test per TU.
// Vulkan CI + tier policy lives in lib/engine/tests/asset/codemap.md.
// CUDA stays out of scope (bindless buffers unsupported, Slang-RHI builds
// with SLANG_RHI_ENABLE_CUDA OFF) and Metal stays out of scope
// (SLANG_E_NOT_AVAILABLE on this path) — both stances already recorded and
// unchanged by this lane.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();

    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace IndirectLoss {
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
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
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

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
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
    const UnitTest indirect_loss = UnitTest::Named("indirect_loss") / [](UnitTest::IRun &_) -> void {
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        PPR_DEFER{PPR_TEST_ASSERT(not detail::SharedGpu::release()); };
        _.recurse({
            detail::IndirectLoss::indirect_device_loss_midframe_restart_hash,
        });
    };

    const UnitTest &indirectLossTests() noexcept {
        return indirect_loss;
    }
} // namespace pP::tests
