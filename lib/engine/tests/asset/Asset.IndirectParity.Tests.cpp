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

// Parity gate: the identical scene rendered through the direct
// path (render) and the CPU-staged indirect path (renderIndirect) must be
// pixel-exact. This gate is the adopted proof that drawIndirect executes
// through the engine.rhi seam.
// One focused test per TU.
namespace pP::tests::detail::SharedGpu {
    [[nodiscard]] std::error_code acquire();

    [[nodiscard]] std::error_code release();

    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

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
                if (mesh_index >= scene.m_meshes.size() or node_index >= scene.m_nodes.size())
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

namespace pP::tests {
    const UnitTest indirect_parity = UnitTest::Named("indirect_parity") / [](UnitTest::IRun &_) -> void {
        PPR_TEST_ASSERT(not detail::SharedGpu::acquire());
        PPR_DEFER{PPR_TEST_ASSERT(not detail::SharedGpu::release()); };
        _.recurse({
            detail::IndirectParity::direct_vs_indirect_pixel_exact,
        });
    };

    const UnitTest &indirectParityTests() noexcept {
        return indirect_parity;
    }
} // namespace pP::tests
