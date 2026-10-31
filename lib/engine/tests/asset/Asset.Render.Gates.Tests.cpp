module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>
#include <mango/import3d/mesh.hpp>
#include <slang.h>

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import imgui_internal;
import std;

namespace m3d = mango::import3d;

namespace pP::tests::detail::SharedGpu {
    // ------------------------------------------------------------------
    // render gates
    // ------------------------------------------------------------------

    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace Gate {
        PPR_DEFINE_LOG_CATEGORY(GridMaterialGate, info, none)

        struct GateEditorApp : ApplicationEditor {
            FenceCompletionValue m_last_submission{};
            u64 m_last_ui_version{};
            std::error_code m_last_render_error{};
            bool m_last_presented{}, m_last_dropped{};

            explicit GateEditorApp(const std::string_view name, const std::span<const char *const> argv)
                : ApplicationEditor(name, argv) {
                // Inline renderer posture: the editor flow drives
                // Renderer::renderToTexture / waitOnHost from the test
                // thread, not the render lane.
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

            void onLaneFrameConsumed_(const RenderFrameResult &result) override {
                ApplicationEditor::onLaneFrameConsumed_(result);

                m_last_submission = result.m_submit_value;
                m_last_ui_version = result.m_ui_font_version_applied;
                m_last_render_error = result.m_error;
                m_last_presented = result.m_presented;
                m_last_dropped = result.m_dropped;
            }
        };

        [[nodiscard]] std::filesystem::path gateMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> decodeFilePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(gateMeshDir() / std::string{name});
            if (not mapped.has_value()) {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
        }

        [[nodiscard]] mem::SharedBuffer gatePngBytes(
            const std::string_view name, const std::array<std::byte, 64u> &rgba) {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_gate_fixtures";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path path = dir / name;
            const mango::image::Surface surface{
                4, 4, mango::image::Format(32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                16u, rgba.data()};
            if (not static_cast<bool>(surface.save(path.string()))) {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        [[nodiscard]] Expected<image::ImageAsset> goldenOrmFixture_(
            const std::string_view name, const std::array<u8, 3u> rgb) {
            std::array<std::byte, 64u> rgba{};
            for (std::size_t pixel = 0u; pixel < 16u; ++pixel) {
                const std::size_t offset = pixel * 4u;
                rgba[offset] = std::byte{rgb[0]};
                rgba[offset + 1u] = std::byte{rgb[1]};
                rgba[offset + 2u] = std::byte{rgb[2]};
                rgba[offset + 3u] = std::byte{255u};
            }
            const mem::SharedBuffer png = gatePngBytes(name, rgba);
            if (not png.isValid()) [[unlikely]] {
                return std::unexpected{std::make_error_code(std::errc::io_error)};
            }
            return image::decodeToRgba8(
                png.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::data);
        }

        [[nodiscard]] CameraSnapshot gateCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            // Mango lookat takes (target, viewer, up) — not the GL order.
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] rhi::ITexture &targetRef_(const Expected<rhi::ComPtr<rhi::ITexture>> &target) {
            return *(*target).get();
        }

        [[nodiscard]] Expected<rhi::ComPtr<rhi::ITexture>> makeRenderTarget_(
            rhi::IDevice &device, const u32 size, const u32 height = 0u) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {size, height != 0u ? height : size, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = "gate render target";
            rhi::ComPtr<rhi::ITexture> target{};
            if (const std::error_code err = make_error_code(device.createTexture(desc, nullptr, target.writeRef()))) {
                return std::unexpected{err};
            }
            return target;
        }

        // MSAA variant of makeRenderTarget_: sampleCount (and the matching
        // Texture2DMS type — a non-MS type with sampleCount > 1 is rejected as
        // invalid by the RHI's texture validation) is the only difference, so
        // a multisampled pass with a single-sample resolve target can be built
        // without touching the pipeline path.
        [[nodiscard]] Expected<rhi::ComPtr<rhi::ITexture>> makeMsaaTarget_(
            rhi::IDevice &device, const u32 size, const u32 sample_count, const char *const label) {
            rhi::TextureDesc desc{};
            desc.type = sample_count > 1u ? rhi::TextureType::Texture2DMS : rhi::TextureType::Texture2D;
            desc.size = {size, size, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.sampleCount = sample_count;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = static_cast<rhi::TextureUsage>(
                static_cast<u32>(rhi::TextureUsage::RenderTarget) | static_cast<u32>(rhi::TextureUsage::CopySource));
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = label;
            rhi::ComPtr<rhi::ITexture> target{};
            if (const std::error_code err = make_error_code(device.createTexture(desc, nullptr, target.writeRef()))) {
                return std::unexpected{err};
            }
            return target;
        }

        // Axis-aligned quad facing +Z for the MSAA accept-path gate below —
        // the same geometry shape Draws::drawsQuad_ draws (that helper lives
        // in a different translation unit of this module).
        [[nodiscard]] Array<mesh::StaticMeshVertex> msaaQuad_(const float half) {
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

        // Wound CLOCKWISE as seen from the camera — the front face this
        // pipeline's CullMode::Back accepts (see Draws::drawsQuadIndices_).
        [[nodiscard]] Array<u32> msaaQuadIndices_() {
            return {0u, 3u, 2u, 0u, 2u, 1u};
        }

        // Untextured flat-colour material: the shader multiplies the albedo by
        // the fallback white texture, so the drawn colour is this colour under
        // the fixed light rig (see Draws::drawsColorMaterial_).
        [[nodiscard]] Expected<MaterialHandle> msaaColorMaterial_(TrianglePass &pass, const float4 &rgb) {
            mesh::MaterialAsset material{};
            material.m_base_color = float4{rgb.x, rgb.y, rgb.z, 1.0f};
            material.m_roughness = 1.0f;
            material.m_metallic = 0.0f;
            const TextureHandle none[4] = {TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
            return pass.packMaterial(material, none);
        }

        [[nodiscard]] Expected<rhi::ComPtr<rhi::ITexture>> makeDepthTarget_(rhi::IDevice &device, const u32 size) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {size, size, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::D32Float;
            desc.sampleCount = 1u;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = rhi::TextureUsage::DepthStencil;
            desc.defaultState = rhi::ResourceState::DepthWrite;
            desc.label = "depth gate target";
            rhi::ComPtr<rhi::ITexture> target{};
            if (const std::error_code err = make_error_code(device.createTexture(desc, nullptr, target.writeRef()))) {
                return std::unexpected{err};
            }
            return target;
        }

        struct GatePixels {
            Array<std::byte> m_bytes{};
            u64 m_row_pitch = 0u;
            u32 m_size = 0u;
        };

        [[nodiscard]] Expected<GatePixels> readback_(rhi::IDevice &device, rhi::ITexture &target, const u32 size) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err = make_error_code(
                    device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) {
                return std::unexpected{err};
            }
            if (blob.get() == nullptr or blob->getBufferPointer() == nullptr) {
                return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
            }
            GatePixels pixels{};
            pixels.m_row_pitch = layout.rowPitch;
            pixels.m_size = size;
            const auto *src = static_cast<const std::byte *>(blob->getBufferPointer());
            pixels.m_bytes.assign(src, src + blob->getBufferSize());
            return pixels;
        }

        [[nodiscard]] std::array<u8, 4u> gateTexel_(const GatePixels &pixels, const u32 x, const u32 y) noexcept {
            const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(pixels.m_row_pitch) +
                                    static_cast<std::size_t>(x) * 4u;
            return {
                static_cast<u8>(pixels.m_bytes[off]),
                static_cast<u8>(pixels.m_bytes[off + 1u]),
                static_cast<u8>(pixels.m_bytes[off + 2u]),
                static_cast<u8>(pixels.m_bytes[off + 3u]),
            };
        }

        // Chromatic spread kills the base-color fallback: fallback white under
        // white light is grayscale, so a spread texel proves texture binding.
        [[nodiscard]] u8 spread_(const std::array<u8, 4u> &texel) noexcept {
            const u8 hi = std::max({texel[0], texel[1], texel[2]});
            const u8 lo = std::min({texel[0], texel[1], texel[2]});
            return static_cast<u8>(hi - lo);
        }

        [[nodiscard]] u8 texelDist_(const std::array<u8, 4u> &lhs, const std::array<u8, 4u> &rhs) noexcept {
            u8 dist = 0u;
            for (u32 i = 0u; i < 3u; ++i) {
                const u8 d = lhs[i] > rhs[i] ? static_cast<u8>(lhs[i] - rhs[i]) : static_cast<u8>(rhs[i] - lhs[i]);
                dist = std::max(dist, d);
            }
            return dist;
        }

        // §7 submission mirror for tests that drive the pass directly: one
        // instance per (scene instance, prim) with the node world matrix,
        // joined through the shared mesh-major table.
        [[nodiscard]] std::error_code submitScene_(
            TrianglePass &pass, const mesh::SceneAsset &scene, const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size() or node_index >= scene.m_nodes.size()) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                const float4x4 world = scene.m_nodes[node_index].m_world.toMatrix();
                for (std::size_t prim_slot = 0u; prim_slot < scene.m_meshes[mesh_index].m_primitives.size();
                    ++prim_slot) {
                    Expected<TrianglePass::JoinedInstancePrim> joined = TrianglePass::joinInstancePrim(
                        scene, uploaded, instance, prim_slot);
                    if (not joined.has_value()) {
                        return joined.error();
                    }
                    if (const std::error_code submit_err = pass.submitInstance(
                            joined->m_bag, joined->m_material, world)) {
                        return submit_err;
                    }
                }
            }
            return default_value_v;
        }

        // Final gate: textured glTF through the bindless pass with
        // MULTI-pixel DISTINCTIVE texels — the gradient texture is chromatic
        // everywhere a base-color fallback renders grayscale.
        PPR_UNIT_TEST (bindless_textured_box_gate) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.shutdown());
            };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(gateMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            PPR_TEST_ASSERT(scene->m_meshes.size() == 1u);
            PPR_TEST_ASSERT(not scene->m_instances.empty());
            const Expected<image::ImageAsset> decoded = decodeFilePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());

            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            PPR_TEST_ASSERT(pass.textureCache().descriptorBuffer() != nullptr);
            PPR_TEST_ASSERT(not submitScene_(pass, *scene, *uploaded));

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, center, float2{256.0f, 256.0f})));

            const Expected<rhi::ComPtr<rhi::ITexture>> target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(target.has_value());
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
            PPR_TEST_ASSERT(
                not renderer.renderToTexture(targetRef_(target), {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());

            const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 256u);
            PPR_TEST_ASSERT(pixels.has_value());
            const std::array<u8, 4u> center_texel = gateTexel_(*pixels, 128u, 128u);
            PPR_TEST_ASSERT(spread_(center_texel) > 12u);
            // Dark-sage center (debugger-measured 38,56,46) is mid-gradient,
            // never a saturated primary: calibrate against the read-back
            // background corner (25,25,51 live) instead of a hardcoded clear
            // color. Observed dist is 31; the bar sits between readback noise
            // (<5) and signal.
            const std::array<u8, 4u> background_texel = gateTexel_(*pixels, 8u, 8u);
            PPR_TEST_ASSERT(texelDist_(center_texel, background_texel) > 20u);

            u32 chromatic = 0u;
            const u32 sample_at[] = {64u, 128u, 192u};
            for (const u32 y: sample_at) {
                for (const u32 x: sample_at) {
                    if (spread_(gateTexel_(*pixels, x, y)) > 10u) {
                        ++chromatic;
                    }
                }
            }
            PPR_TEST_ASSERT(chromatic >= 5u);
            PPR_TEST_ASSERT(
                texelDist_(gateTexel_(*pixels, 64u, 64u), gateTexel_(*pixels, 192u, 192u)) > 12u or
                texelDist_(gateTexel_(*pixels, 64u, 192u), gateTexel_(*pixels, 192u, 64u)) > 12u);

            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
        };

        PPR_UNIT_TEST (triangle_depth_occlusion_gate) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.shutdown());
            };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(gateMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(scene.has_value());
            const Expected<image::ImageAsset> decoded = decodeFilePng("textured_box.png");
            PPR_TEST_ASSERT(decoded.has_value());
            Array<image::ImageAsset> images{};
            images.push_back(*decoded);
            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(*scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            PPR_TEST_ASSERT(not uploaded->m_primitives.empty());
            PPR_TEST_ASSERT(not scene->m_materials.empty());

            mesh::MaterialAsset green_material = scene->m_materials.front();
            green_material.m_base_color = float4{0.0f, 1.0f, 0.0f, 1.0f};
            green_material.m_metallic = 0.0f;
            green_material.m_roughness = 1.0f;
            green_material.m_base_color_map.m_image = none_v;
            mesh::MaterialAsset red_material = green_material;
            red_material.m_base_color = float4{1.0f, 0.0f, 0.0f, 1.0f};
            const TextureHandle no_texture[] = {
                TextureHandle{},
                TextureHandle{},
                TextureHandle{},
                TextureHandle{},
            };
            const Expected<MaterialHandle> green_gpu_material = pass.packMaterial(green_material, no_texture);
            PPR_TEST_ASSERT(green_gpu_material.has_value());
            const Expected<MaterialHandle> red_gpu_material = pass.packMaterial(red_material, no_texture);
            PPR_TEST_ASSERT(red_gpu_material.has_value());

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float max_dim = std::max(
                {mesh_asset.m_bounds.size().x, mesh_asset.m_bounds.size().y, mesh_asset.m_bounds.size().z});
            const float3 eye{center.x, center.y, center.z + 2.5f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, center, float2{256.0f, 256.0f})));

            pass.clearInstances();
            const TrianglePass::UploadedPrimitive &primitive = uploaded->m_primitives.front();
            const float4x4 green_transform{
                float4{1.0f, 0.0f, 0.0f, 0.0f},
                float4{0.0f, 1.0f, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{0.0f, 0.0f, -0.6f * max_dim, 1.0f},
            };
            const float4x4 red_transform{
                float4{1.0f, 0.0f, 0.0f, 0.0f},
                float4{0.0f, 1.0f, 0.0f, 0.0f},
                float4{0.0f, 0.0f, 1.0f, 0.0f},
                float4{0.0f, 0.0f, 0.6f * max_dim, 1.0f},
            };
            PPR_TEST_ASSERT(not pass.submitInstance(primitive.m_bag, *red_gpu_material, red_transform));
            PPR_TEST_ASSERT(not pass.submitInstance(primitive.m_bag, *green_gpu_material, green_transform));

            const Expected<rhi::ComPtr<rhi::ITexture>> color_target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(color_target.has_value());
            const Expected<rhi::ComPtr<rhi::ITexture>> depth_target = makeDepthTarget_(device, 256u);
            PPR_TEST_ASSERT(depth_target.has_value());
            const rhi::ComPtr<rhi::ITextureView> color_view = targetRef_(*color_target).getDefaultView();
            const rhi::ComPtr<rhi::ITextureView> depth_view = targetRef_(*depth_target).getDefaultView();
            rhi::RenderPassColorAttachment color_attachment{};
            color_attachment.view = color_view.get();
            rhi::RenderPassDepthStencilAttachment depth_attachment{};
            depth_attachment.view = depth_view.get();

            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            PPR_TEST_ASSERT(not p_renderer->render(
                string_literal{std::in_place, "triangle depth gate"},
                rhi::RenderPassDesc{
                    .colorAttachments = &color_attachment,
                    .colorAttachmentCount = 1u,
                    .depthStencilAttachment = &depth_attachment,
                },
                {DrawSubmission{pass}}));
            PPR_TEST_ASSERT(not p_renderer->waitOnHost());

            const Expected<GatePixels> pixels = readback_(device, targetRef_(*color_target), 256u);
            PPR_TEST_ASSERT(pixels.has_value());
            const std::array<u8, 4u> center_texel = gateTexel_(*pixels, 128u, 128u);
            PPR_TEST_ASSERT(center_texel[0u] > center_texel[1u] + 20u);
            PPR_TEST_ASSERT(center_texel[0u] > center_texel[2u] + 20u);

            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
            PPR_TEST_ASSERT(not pass.materialCache().release(*green_gpu_material));
            PPR_TEST_ASSERT(not pass.materialCache().release(*red_gpu_material));
        };

        PPR_UNIT_TEST (orm_golden_distinct_channels) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.shutdown());
            };

            Expected<mesh::SceneAsset> imported = mesh::importAndConvert(gateMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(imported.has_value());
            mesh::SceneAsset scene = std::move(*imported);
            PPR_TEST_ASSERT(scene.m_materials.size() == 1u);
            scene.m_images.clear();
            scene.m_images.push_back({});
            scene.m_images.push_back({});
            scene.m_images.push_back({});

            const Expected<image::ImageAsset> occlusion = goldenOrmFixture_("orm_occlusion_golden.png", {255u, 1u, 2u});
            const Expected<image::ImageAsset> roughness = goldenOrmFixture_("orm_roughness_golden.png", {3u, 255u, 4u});
            const Expected<image::ImageAsset> metallic = goldenOrmFixture_("orm_metallic_golden.png", {255u, 255u, 0u});
            PPR_TEST_ASSERT(occlusion.has_value());
            PPR_TEST_ASSERT(roughness.has_value());
            PPR_TEST_ASSERT(metallic.has_value());
            Array<image::ImageAsset> images{};
            images.push_back(*occlusion);
            images.push_back(*roughness);
            images.push_back(*metallic);

            mesh::MaterialAsset &material = scene.m_materials.front();
            material.m_base_color = float4{1.0f, 0.5f, 0.25f, 1.0f};
            material.m_metallic = 1.0f;
            material.m_roughness = 1.0f;
            material.m_occlusion_strength = 1.0f;
            material.m_base_color_map = {};
            material.m_normal_map = {};
            material.m_emissive_map = {};
            material.m_occlusion_map.m_image = mesh::ImageAssetId{0u};
            material.m_roughness_map.m_image = mesh::ImageAssetId{1u};
            material.m_metallic_map.m_image = mesh::ImageAssetId{2u};

            const Expected<TrianglePass::UploadedScene> uploaded = pass.uploadScene(scene, images);
            PPR_TEST_ASSERT(uploaded.has_value());
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.releaseScene(*uploaded));
            };
            PPR_TEST_ASSERT(not submitScene_(pass, scene, *uploaded));

            const mesh::StaticMeshAsset &box = scene.m_meshes.front();
            const float3 center = box.m_bounds.center();
            const float3 size = box.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, center, float2{256.0f, 256.0f})));

            rhi::IDevice &device = rhi->getDevice();
            const Expected<rhi::ComPtr<rhi::ITexture>> target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(target.has_value());
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
            PPR_TEST_ASSERT(
                not renderer.renderToTexture(targetRef_(target), {DrawSubmission{pass}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());
            const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 256u);
            PPR_TEST_ASSERT(pixels.has_value());

            // Analytic prediction: composite (1,1,0) gives occlusion=1,
            // roughness=1 (specular killed), metallic=0, so lit =
            // albedo*(0.35+0.65*d) with d≈0.76 → (215,108,54) (debugger-read).
            // Bounds bracket that point: occlusion-wrong collapses to black,
            // metallic-wrong halves every channel, roughness-wrong adds a
            // specular lift — each breaks at least one bound below.
            const std::array<u8, 4u> center_texel = gateTexel_(*pixels, 128u, 128u);
            PPR_TEST_ASSERT(center_texel[0] > 190u and center_texel[0] < 235u);
            PPR_TEST_ASSERT(center_texel[1] > 90u and center_texel[1] < 130u);
            PPR_TEST_ASSERT(center_texel[2] > 40u and center_texel[2] < 70u);
            PPR_TEST_ASSERT(center_texel[1] + 80u < center_texel[0]);
            PPR_TEST_ASSERT(center_texel[2] + 30u < center_texel[1]);
        };

        // Tangent-w arbitration on the render gate: fixtures carry no file
        // tangents (all converted tangents are MikkTSpace-regenerated, correct
        // by construction), so the gate arbitrates with an independent
        // in-fork MikkTSpace reference + analytic prediction. Debugger verdict
        // (live): ref_tangent == (1,0,0,1) — w is 1, NOT 0, so the MikkTSpace
        // w==0 suspect is refuted. The first tilt (191,191,218) kept the
        // shaded normal facing away from the key light for BOTH w signs
        // (diffuse 0 both sides, 0.349 gray) — insensitive by construction,
        // not a render bug. The (128,255,128) tilt is bitangent-dominant, so
        // the w sign flips the lit component and ref/flip visibly differ;
        // the flat map matches the unmapped path (TBN sanity). Outcome: NO
        // inversion — file-tangent w=-w is kept, negate at convert.
        PPR_UNIT_TEST (tangent_w_render_proof) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.shutdown());
            };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(gateMeshDir(), "textured_quad.glb");
            PPR_TEST_ASSERT(scene.has_value());
            const mesh::StaticMeshAsset &quad = scene->m_meshes.front();
            PPR_TEST_ASSERT(quad.m_vertices.size() == 4u);
            PPR_TEST_ASSERT(quad.m_indices.size() == 6u);
            PPR_TEST_ASSERT(not scene->m_materials.empty());
            const mesh::MaterialAsset &quad_mat = scene->m_materials.front();

            // Independent MikkTSpace reference, straight from the fork (not
            // through the converter): soup from converted positions/normals/
            // uvs, tangents read back per corner.
            m3d::Mesh soup{};
            soup.flags = m3d::Vertex::Position | m3d::Vertex::Normal | m3d::Vertex::Texcoord;
            for (std::size_t t = 0u; t < quad.m_indices.size(); t += 3u) {
                m3d::Triangle tri{};
                for (u32 c = 0u; c < 3u; ++c) {
                    const mesh::StaticMeshVertex &src = quad.m_vertices[quad.m_indices[t + c]];
                    m3d::Vertex corner{};
                    corner.position = m3d::float32x3{src.m_position[0], src.m_position[1], src.m_position[2]};
                    corner.normal = m3d::float32x3{src.m_normal[0], src.m_normal[1], src.m_normal[2]};
                    corner.texcoord = m3d::float32x2{src.m_texcoord[0], src.m_texcoord[1]};
                    tri.vertex[c] = corner;
                }
                soup.triangles.push_back(tri);
            }
            soup.computeTangents();
            PPR_TEST_ASSERT((soup.flags & m3d::Vertex::Tangent) != 0u);
            const m3d::float32x4 ref_tangent = soup.triangles.front().vertex[0].tangent;

            // Constant bitangent-dominant normal map: every texel identical,
            // so bilinear filtering is exact and the analytic prediction is
            // tight. The tilt points along +bitangent so the w sign flips the
            // lit component (a normal-dominant tilt stays unlit for both w).
            std::array<std::byte, 64u> tilt_bytes{};
            for (std::size_t i = 0u; i < tilt_bytes.size(); i += 4u) {
                tilt_bytes[i] = static_cast<std::byte>(128);
                tilt_bytes[i + 1u] = static_cast<std::byte>(255);
                tilt_bytes[i + 2u] = static_cast<std::byte>(128);
                tilt_bytes[i + 3u] = static_cast<std::byte>(255);
            }
            const mem::SharedBuffer tilt_mapped = gatePngBytes("tangent_tilt.png", tilt_bytes);
            PPR_TEST_ASSERT(tilt_mapped.isValid());
            const Expected<image::ImageAsset> tilt_decoded = image::decodeToRgba8(
                tilt_mapped.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::data);
            PPR_TEST_ASSERT(tilt_decoded.has_value());
            const Expected<TextureHandle> tilt_tex = pass.uploadTexture(*tilt_decoded);
            PPR_TEST_ASSERT(tilt_tex.has_value());
            const Expected<TextureBindlessIndex> tilt_slot = pass.textureCache().residentIndex(*tilt_tex);
            PPR_TEST_ASSERT(tilt_slot.has_value());

            mesh::MaterialAsset normal_mat = quad_mat;
            normal_mat.m_is_two_sided = true;
            normal_mat.m_normal_map.m_image = mesh::ImageAssetId{0u};
            normal_mat.m_normal_map.m_texcoord = quad_mat.m_base_color_map.m_texcoord;
            const TextureHandle invalid_tex{};
            const TextureHandle tilt_resolved[] = {invalid_tex, invalid_tex, *tilt_tex, invalid_tex};
            const Expected<MaterialHandle> tilt_mat = pass.packMaterial(normal_mat, tilt_resolved);
            PPR_TEST_ASSERT(tilt_mat.has_value());
            const TextureHandle flat_resolved[] = {invalid_tex, invalid_tex, invalid_tex, invalid_tex};
            const Expected<MaterialHandle> flat_mat = pass.packMaterial(normal_mat, flat_resolved);
            PPR_TEST_ASSERT(flat_mat.has_value());

            // Camera faces the quad along its geometric normal.
            const float3 center = quad.m_bounds.center();
            const float3 size = quad.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            float3 face_normal{
                quad.m_vertices.front().m_normal[0], quad.m_vertices.front().m_normal[1],
                quad.m_vertices.front().m_normal[2]};
            const float normal_len = std::sqrt(
                face_normal.x * face_normal.x + face_normal.y * face_normal.y + face_normal.z * face_normal.z);
            face_normal.x /= normal_len;
            face_normal.y /= normal_len;
            face_normal.z /= normal_len;
            const float3 eye{
                center.x + face_normal.x * 2.5f * max_dim,
                center.y + face_normal.y * 2.5f * max_dim,
                center.z + face_normal.z * 2.5f * max_dim,
            };
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, center, float2{128.0f, 128.0f})));

            const auto render_center = [&](const TriangleBagHandle bag, const MaterialHandle mat) -> Expected<float3> {
                if (const std::error_code submit_err = pass.submitInstance(bag, mat, float4x4::identity())) {
                    return std::unexpected{submit_err};
                }
                const Expected<rhi::ComPtr<rhi::ITexture>> target = makeRenderTarget_(device, 128u);
                if (not target.has_value()) {
                    pass.clearInstances();
                    return std::unexpected{target.error()};
                }
                Renderer *const p_renderer = SharedGpu::renderer();
                PPR_TEST_ASSERT(p_renderer != nullptr);
                Renderer &renderer = *p_renderer;
                const std::error_code render_err = renderer.renderToTexture(
                    targetRef_(target), {DrawSubmission{pass}}, ColorAttachmentOps{});
                pass.clearInstances();
                if (render_err) {
                    return std::unexpected{render_err};
                }
                if (const std::error_code wait_err = renderer.waitOnHost()) {
                    return std::unexpected{wait_err};
                }
                const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 128u);
                if (not pixels.has_value()) {
                    return std::unexpected{pixels.error()};
                }
                const std::array<u8, 4u> texel = gateTexel_(*pixels, 64u, 64u);
                return float3{
                    static_cast<float>(texel[0]) / 255.0f,
                    static_cast<float>(texel[1]) / 255.0f,
                    static_cast<float>(texel[2]) / 255.0f,
                };
            };

            // Analytic shader twin (ambient + lambert + blinn from the .slang):
            // constant map ⇒ center texel is exact; occlusion 1, metal/rough
            // from quad factors, emissive black.
            const auto analytic = [&](const float t[3], const float w) -> float3 {
                const float3 n{
                    quad.m_vertices.front().m_normal[0], quad.m_vertices.front().m_normal[1],
                    quad.m_vertices.front().m_normal[2]};
                const float b[3] = {
                    w * (n.y * t[2] - n.z * t[1]),
                    w * (n.z * t[0] - n.x * t[2]),
                    w * (n.x * t[1] - n.y * t[0]),
                };
                constexpr float kTex[3] = {
                    2.0f * 128.0f / 255.0f - 1.0f, 2.0f * 255.0f / 255.0f - 1.0f, 2.0f * 128.0f / 255.0f - 1.0f};
                float shaded[3] = {
                    t[0] * kTex[0] + b[0] * kTex[1] + n.x * kTex[2],
                    t[1] * kTex[0] + b[1] * kTex[1] + n.y * kTex[2],
                    t[2] * kTex[0] + b[2] * kTex[1] + n.z * kTex[2],
                };
                const float len = std::sqrt(shaded[0] * shaded[0] + shaded[1] * shaded[1] + shaded[2] * shaded[2]);
                shaded[0] /= len;
                shaded[1] /= len;
                shaded[2] /= len;
                constexpr float kLight[3] = {-0.35f, 0.55f, 0.76f};
                const float light_len = std::sqrt(
                    kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
                const float diff = std::max(
                    0.0f, (shaded[0] * kLight[0] + shaded[1] * kLight[1] + shaded[2] * kLight[2]) / light_len);
                float view[3] = {eye.x - center.x, eye.y - center.y, eye.z - center.z};
                const float view_len = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                view[0] /= view_len;
                view[1] /= view_len;
                view[2] /= view_len;
                float half_v[3] = {
                    kLight[0] / light_len + view[0], kLight[1] / light_len + view[1], kLight[2] / light_len + view[2]};
                const float half_len = std::sqrt(half_v[0] * half_v[0] + half_v[1] * half_v[1] + half_v[2] * half_v[2]);
                const float spec = std::pow(
                    std::max(0.0f, (shaded[0] * half_v[0] + shaded[1] * half_v[1] + shaded[2] * half_v[2]) / half_len),
                    64.0f + (8.0f - 64.0f) * quad_mat.m_roughness);
                const float gain = (0.35f + 0.65f * diff) * (1.0f - 0.5f * quad_mat.m_metallic);
                const float spec_gain = spec * (1.0f - quad_mat.m_roughness) * 0.25f;
                return float3{gain + spec_gain, gain + spec_gain, gain + spec_gain};
            };

            const auto closeEnough = [](const float3 &lhs, const float3 &rhs, const float tol) noexcept {
                return std::abs(lhs.x - rhs.x) < tol and
                       std::abs(lhs.y - rhs.y) < tol and
                       std::abs(lhs.z - rhs.z) < tol;
            };

            Array<mesh::StaticMeshVertex> ref_verts(quad.m_vertices.begin(), quad.m_vertices.end());
            for (mesh::StaticMeshVertex &vert: ref_verts) {
                vert.m_tangent[0] = ref_tangent.x;
                vert.m_tangent[1] = ref_tangent.y;
                vert.m_tangent[2] = ref_tangent.z;
                vert.m_tangent[3] = ref_tangent.w;
            }
            const std::span<const u32> quad_idx(quad.m_indices.data(), quad.m_indices.size());
            const std::span<const mesh::StaticMeshVertex> ref_span(ref_verts.data(), ref_verts.size());
            const Expected<TriangleBagHandle> ref_bag = pass.uploadMesh(ref_span, quad_idx);
            PPR_TEST_ASSERT(ref_bag.has_value());
            const Expected<float3> ref_lit = render_center(*ref_bag, *tilt_mat);
            PPR_TEST_ASSERT(ref_lit.has_value());

            Array<mesh::StaticMeshVertex> flip_verts(quad.m_vertices.begin(), quad.m_vertices.end());
            for (mesh::StaticMeshVertex &vert: flip_verts) {
                vert.m_tangent[0] = ref_tangent.x;
                vert.m_tangent[1] = ref_tangent.y;
                vert.m_tangent[2] = ref_tangent.z;
                vert.m_tangent[3] = -ref_tangent.w;
            }
            const std::span<const mesh::StaticMeshVertex> flip_span(flip_verts.data(), flip_verts.size());
            const Expected<TriangleBagHandle> flip_bag = pass.uploadMesh(flip_span, quad_idx);
            PPR_TEST_ASSERT(flip_bag.has_value());
            const Expected<float3> flip_lit = render_center(*flip_bag, *tilt_mat);
            PPR_TEST_ASSERT(flip_lit.has_value());

            const float ref_t[3] = {ref_tangent.x, ref_tangent.y, ref_tangent.z};
            PPR_TEST_ASSERT(closeEnough(*ref_lit, analytic(ref_t, ref_tangent.w), 0.05f));
            PPR_TEST_ASSERT(closeEnough(*flip_lit, analytic(ref_t, -ref_tangent.w), 0.05f));
            PPR_TEST_ASSERT(
                std::abs(ref_lit->x - flip_lit->x) > 0.05f or
                std::abs(ref_lit->y - flip_lit->y) > 0.05f or
                std::abs(ref_lit->z - flip_lit->z) > 0.05f);

            const Expected<float3> flat_lit = render_center(*ref_bag, *flat_mat);
            PPR_TEST_ASSERT(flat_lit.has_value());
            const TextureHandle plain_resolved[] = {invalid_tex, invalid_tex, invalid_tex, invalid_tex};
            const Expected<MaterialHandle> plain_mat = pass.packMaterial(normal_mat, plain_resolved);
            PPR_TEST_ASSERT(plain_mat.has_value());
            const Expected<float3> plain_lit = render_center(*ref_bag, *plain_mat);
            PPR_TEST_ASSERT(plain_lit.has_value());
            PPR_TEST_ASSERT(closeEnough(*flat_lit, *plain_lit, 0.03f));

            // releaseScene only releases uploadScene products
            // (receipt-checked); piecemeal handles release via their caches.
            PPR_TEST_ASSERT(not pass.materialCache().release(*tilt_mat));
            PPR_TEST_ASSERT(not pass.materialCache().release(*flat_mat));
            PPR_TEST_ASSERT(not pass.materialCache().release(*plain_mat));
            PPR_TEST_ASSERT(not pass.textureCache().release(*tilt_tex));
            PPR_TEST_ASSERT(not pass.bagCache().release(*ref_bag));
            PPR_TEST_ASSERT(not pass.bagCache().release(*flip_bag));
        };

        // RenderPipelineSignature is count-sensitive but tail-insensitive:
        // only the first m_color_format_count entries participate in
        // equality and hashing, and colorFormats() clamps a stale count so
        // the public u8 can never build an out-of-bounds span.
        PPR_UNIT_TEST (pipeline_signature_count_sensitivity) {
            RenderPipelineSignature one{};
            one.m_color_formats[0] = rhi::Format::RGBA8Unorm;
            one.m_color_format_count = 1u;
            RenderPipelineSignature two = one;
            two.m_color_formats[1] = rhi::Format::RGBA8Unorm;
            two.m_color_format_count = 2u;
            PPR_TEST_ASSERT(one.colorFormats().size() == 1u);
            PPR_TEST_ASSERT(two.colorFormats().size() == 2u);
            PPR_TEST_ASSERT(not (one == two));
            PPR_TEST_ASSERT(not (two == one));
        };

        PPR_UNIT_TEST (pipeline_signature_tail_insensitivity) {
            RenderPipelineSignature lhs{};
            lhs.m_color_formats[0] = rhi::Format::RGBA8Unorm;
            lhs.m_color_format_count = 1u;
            RenderPipelineSignature rhs = lhs;
            rhs.m_color_formats[1] = rhi::Format::D32Float;
            rhs.m_color_formats[2] = rhi::Format::RG32Float;
            rhs.m_color_formats[3] = rhi::Format::RGBA8UnormSrgb;
            PPR_TEST_ASSERT(lhs == rhs);
            PPR_TEST_ASSERT(rhs == lhs);
            PPR_TEST_ASSERT(hashValue(lhs) == hashValue(rhs));
        };

        PPR_UNIT_TEST (pipeline_signature_hash_consistent_with_equality) {
            RenderPipelineSignature base{};
            base.m_color_formats[0] = rhi::Format::RGBA8Unorm;
            base.m_color_format_count = 1u;
            base.m_depth_stencil_format = rhi::Format::D32Float;
            base.m_sample_count = SampleCount::x1;
            const RenderPipelineSignature copy = base;
            PPR_TEST_ASSERT(base == copy);
            PPR_TEST_ASSERT(hashValue(base) == hashValue(copy));
            RenderPipelineSignature resampled = base;
            resampled.m_sample_count = SampleCount::x4;
            PPR_TEST_ASSERT(not (base == resampled));
            // The sample count is part of the key: a resampled signature must
            // not collide with the single-sampled one it came from.
            PPR_TEST_ASSERT(hashValue(base) != hashValue(resampled));
        };

        // Fail closed: more color attachments than kMaxColorFormats is
        // rejected before any attachment is inspected — never truncated.
        PPR_UNIT_TEST (render_rejects_color_attachments_beyond_capacity) {
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;
            std::array<rhi::RenderPassColorAttachment, 5u> attachments{};
            const std::error_code err = renderer.render(
                string_literal{std::in_place, "over-capacity color attachments"},
                rhi::RenderPassDesc{
                    .colorAttachments = attachments.data(),
                    .colorAttachmentCount = 5u,
                },
                {});
            PPR_TEST_ASSERT(err == std::make_error_code(std::errc::invalid_argument));
        };

        void discardExpectedFailureLog_(const Log::Entry &) noexcept {
        }

        class ExpectedFailureLogGuard final {
            Log::Policy m_previous;

        public:
            ExpectedFailureLogGuard() noexcept : m_previous(Log::setWriterPolicy(discardExpectedFailureLog_)) {
            }

            ~ExpectedFailureLogGuard() noexcept {
                // Drain under the discard policy (see Asset.Search.Tests.cpp):
                // the logger is async, and an unrestored queue would fail a
                // later test nondeterministically.
                std::ignore = Log::flush(true);
                Log::setWriterPolicy(m_previous);
            }
        };

        // Fail closed: default views spanning multiple layers or mips are
        // rejected as color attachments — never silently narrowed to one slice.
        PPR_UNIT_TEST (render_rejects_multilayer_multimip_color_attachment) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;

            {
                rhi::TextureDesc desc{};
                desc.type = rhi::TextureType::Texture2DArray;
                desc.size = {64u, 64u, 1u};
                desc.arrayLength = 2u;
                desc.mipCount = 1u;
                desc.format = rhi::Format::RGBA8Unorm;
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = rhi::TextureUsage::RenderTarget;
                desc.defaultState = rhi::ResourceState::RenderTarget;
                desc.label = "gate array render target";
                rhi::ComPtr<rhi::ITexture> target{};
                PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, target.writeRef())));
                const rhi::ComPtr<rhi::ITextureView> view = target->getDefaultView();
                PPR_TEST_ASSERT(view.get() != nullptr);
                rhi::RenderPassColorAttachment attachment{};
                attachment.view = view.get();
                ExpectedFailureLogGuard expected_failure_log{};
                const std::error_code err = renderer.render(
                    string_literal{std::in_place, "array-spanning color attachment"},
                    rhi::RenderPassDesc{
                        .colorAttachments = &attachment,
                        .colorAttachmentCount = 1u,
                    },
                    {});
                PPR_TEST_ASSERT(err == std::make_error_code(std::errc::invalid_argument));
            }

            {
                rhi::TextureDesc desc{};
                desc.type = rhi::TextureType::Texture2D;
                desc.size = {64u, 64u, 1u};
                desc.arrayLength = 1u;
                desc.mipCount = 2u;
                desc.format = rhi::Format::RGBA8Unorm;
                desc.memoryType = rhi::MemoryType::DeviceLocal;
                desc.usage = rhi::TextureUsage::RenderTarget;
                desc.defaultState = rhi::ResourceState::RenderTarget;
                desc.label = "gate mipped render target";
                rhi::ComPtr<rhi::ITexture> target{};
                PPR_TEST_ASSERT(not make_error_code(device.createTexture(desc, nullptr, target.writeRef())));
                const rhi::ComPtr<rhi::ITextureView> view = target->getDefaultView();
                PPR_TEST_ASSERT(view.get() != nullptr);
                rhi::RenderPassColorAttachment attachment{};
                attachment.view = view.get();
                ExpectedFailureLogGuard expected_failure_log{};
                const std::error_code err = renderer.render(
                    string_literal{std::in_place, "mip-spanning color attachment"},
                    rhi::RenderPassDesc{
                        .colorAttachments = &attachment,
                        .colorAttachmentCount = 1u,
                    },
                    {});
                PPR_TEST_ASSERT(err == std::make_error_code(std::errc::invalid_argument));
            }
        };

        // Acceptance: a multisampled color attachment with a single-sample
        // resolve target is a valid render-pass shape. There are no draws —
        // the clear alone must survive the multisampled pass and land in the
        // 1x target, which is what the pixel readback checks.
        PPR_UNIT_TEST (render_resolves_msaa_color_attachment) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;

            constexpr u32 kSize = 64u;
            const Expected<rhi::ComPtr<rhi::ITexture>> msaa_target = makeMsaaTarget_(
                device, kSize, 4u, "gate msaa target");
            PPR_TEST_ASSERT(msaa_target.has_value());
            const Expected<rhi::ComPtr<rhi::ITexture>> resolve_target = makeMsaaTarget_(
                device, kSize, 1u, "gate resolve target");
            PPR_TEST_ASSERT(resolve_target.has_value());

            const rhi::ComPtr<rhi::ITextureView> msaa_view = targetRef_(msaa_target).getDefaultView();
            const rhi::ComPtr<rhi::ITextureView> resolve_view = targetRef_(resolve_target).getDefaultView();
            PPR_TEST_ASSERT(msaa_view.get() != nullptr);
            PPR_TEST_ASSERT(resolve_view.get() != nullptr);

            rhi::RenderPassColorAttachment attachment{};
            attachment.view = msaa_view.get();
            attachment.resolveTarget = resolve_view.get();
            attachment.loadOp = rhi::LoadOp::Clear;
            attachment.storeOp = rhi::StoreOp::Store;
            attachment.clearValue[0] = 0.0f;
            attachment.clearValue[1] = 1.0f;
            attachment.clearValue[2] = 0.0f;
            attachment.clearValue[3] = 1.0f;

            PPR_TEST_ASSERT(not renderer.render(
                string_literal{std::in_place, "msaa resolve"},
                rhi::RenderPassDesc{
                    .colorAttachments = &attachment,
                    .colorAttachmentCount = 1u,
                },
                {}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());

            const Expected<GatePixels> pixels = readback_(device, targetRef_(resolve_target), kSize);
            PPR_TEST_ASSERT(pixels.has_value());
            const std::array<u8, 4u> clear_texel{0u, 255u, 0u, 255u};
            PPR_TEST_ASSERT(texelDist_(gateTexel_(*pixels, 32u, 32u), clear_texel) <= 1u);
        };

        // Accept path for the closed SampleCount set: 2x and 4x are the two
        // non-1x enumerators a multisampled pass can carry, so both must build
        // a pipeline (the pass rejects nothing by count any more) and land the
        // drawn quad in the 1x resolve target. Enum exhaustiveness is by
        // construction — a signature cannot hold a count outside {1,2,4,8} —
        // so there is no invalid sample count left to build a negative test
        // against; the fail-closed mapping from the raw u32 is unreachable per
        // RHI validation and therefore covered by the mapping itself, not here.
        PPR_UNIT_TEST (render_accepts_msaa_sample_counts) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();
            Renderer *const p_renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(p_renderer != nullptr);
            Renderer &renderer = *p_renderer;

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not pass.shutdown());
            };

            constexpr u32 kSize = 256u;

            const Array<mesh::StaticMeshVertex> quad = msaaQuad_(0.5f);
            const Array<u32> indices = msaaQuadIndices_();
            const Expected<TriangleBagHandle> bag = pass.uploadMesh(quad, indices);
            PPR_TEST_ASSERT(bag.has_value());
            const Expected<MaterialHandle> material = msaaColorMaterial_(pass, float4{0.0f, 0.0f, 1.0f, 1.0f});
            PPR_TEST_ASSERT(material.has_value());

            // Camera faces the quad head-on: its centre lands mid-frame.
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.submitInstance(*bag, *material, float4x4::identity()));
            const float3 eye{0.0f, 0.0f, 3.0f};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, float3{zero_v}, float2{256.0f, 256.0f})));

            for (const u32 sample_count: {2u, 4u}) {
                const Expected<rhi::ComPtr<rhi::ITexture>> msaa_target = makeMsaaTarget_(
                    device, kSize, sample_count, "gate msaa draw target");
                PPR_TEST_ASSERT(msaa_target.has_value());
                const Expected<rhi::ComPtr<rhi::ITexture>> resolve_target = makeMsaaTarget_(
                    device, kSize, 1u, "gate msaa draw resolve");
                PPR_TEST_ASSERT(resolve_target.has_value());

                const rhi::ComPtr<rhi::ITextureView> msaa_view = targetRef_(msaa_target).getDefaultView();
                const rhi::ComPtr<rhi::ITextureView> resolve_view = targetRef_(resolve_target).getDefaultView();
                PPR_TEST_ASSERT(msaa_view.get() != nullptr);
                PPR_TEST_ASSERT(resolve_view.get() != nullptr);

                rhi::RenderPassColorAttachment attachment{};
                attachment.view = msaa_view.get();
                attachment.resolveTarget = resolve_view.get();
                attachment.loadOp = rhi::LoadOp::Clear;
                attachment.storeOp = rhi::StoreOp::Store;
                attachment.clearValue[0] = 0.0f;
                attachment.clearValue[1] = 0.0f;
                attachment.clearValue[2] = 0.0f;
                attachment.clearValue[3] = 1.0f;

                PPR_TEST_ASSERT(not renderer.render(
                    string_literal{std::in_place, "msaa quad resolve"},
                    rhi::RenderPassDesc{
                        .colorAttachments = &attachment,
                        .colorAttachmentCount = 1u,
                    },
                    {DrawSubmission{pass}}));
                PPR_TEST_ASSERT(not renderer.waitOnHost());

                const Expected<GatePixels> pixels = readback_(device, targetRef_(resolve_target), kSize);
                PPR_TEST_ASSERT(pixels.has_value());

                // The quad drew and resolved: the centre is no longer the flat
                // black clear, and the flat blue material keeps blue dominant
                // over red and green (the clear is neutral, so it cannot).
                const std::array<u8, 4u> background = gateTexel_(*pixels, 4u, 4u);
                const std::array<u8, 4u> centre = gateTexel_(*pixels, kSize / 2u, kSize / 2u);
                PPR_TEST_ASSERT(texelDist_(centre, background) > 20u);
                PPR_TEST_ASSERT(centre[2] > centre[0] + 20u);
                PPR_TEST_ASSERT(centre[2] > centre[1] + 20u);
            }

            pass.clearInstances();
        };

        [[nodiscard]] GridPass::GridStagedFrame opticalGridFixture_(
            const GridTerrainVisual &visual, const u16 terrain, const bool layered = false) {
            GridPass::GridStagedFrame stage{};
            stage.m_has_background = true;
            stage.m_terrain_visuals.push_back(visual);
            for (u32 layer = 0u; layer < 4u; ++layer) {
                std::array<std::byte, 64u> rgba{};
                for (u32 pixel = 0u; pixel < 16u; ++pixel) {
                    const u32 x = pixel % 4u;
                    const u32 y = pixel / 4u;
                    rgba[pixel * 4u] = std::byte{static_cast<u8>(20u + x * 70u)};
                    rgba[pixel * 4u + 1u] = std::byte{static_cast<u8>(20u + y * 70u)};
                    rgba[pixel * 4u + 2u] = std::byte{static_cast<u8>((x + y) % 2u == 0u ? 30u : 230u)};
                    rgba[pixel * 4u + 3u] = std::byte{static_cast<u8>(layer == 0u ? 255u : layered ? 96u : 0u)};
                }
                const auto png = gatePngBytes(std::format("grid_optics_layer_{}.png", layer), rgba);
                PPR_TEST_ASSERT(png.isValid());
                const auto decoded = image::decodeToRgba8(
                    png.getBufferData(), ".png", image::ImageDecodeDesc{}, image::EImageUsage::color);
                PPR_TEST_ASSERT(decoded.has_value());
                stage.m_terrain_images.push_back(*decoded);
                const float factors[] = {0.08f, 0.22f, 0.45f, 0.70f};
                const u32 colors[] = {0xffffffffu, 0xff8080ffu, 0x80ff80ffu, 0x8080ffffu};
                stage.m_background_layers[layer] = GridBackgroundLayer{
                    .m_texture_index = layer,
                    .m_order = layer,
                    .m_color_rgba = colors[layer],
                    .m_repeat_period_m = 8.0f + 3.0f * layer,
                    .m_parallax_factor = factors[layer],
                };
            }
            const auto flat = goldenOrmFixture_("grid_normal_flat.png", {128u, 128u, 255u});
            const auto tilt = goldenOrmFixture_("grid_normal_tilt.png", {220u, 128u, 220u});
            PPR_TEST_ASSERT(flat.has_value() and tilt.has_value());
            stage.m_terrain_images.push_back(*flat);
            stage.m_terrain_images.push_back(*tilt);
            stage.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridPass::GridStagedUpload upload{};
            upload.m_payload.m_rect[2] = 128.0f;
            upload.m_payload.m_rect[3] = 128.0f;
            upload.m_cell_materials.assign(GridPass::kCellsPerChunk, terrain);
            stage.m_uploads.push_back(std::move(upload));
            return stage;
        }

        [[nodiscard]] GatePixels drawOpticalGrid_(
            GridPass &grid, const float origin_x = 56.0f, const float origin_y = 56.0f, const float width_m = 16.0f,
            const u32 edge = 128u, u32 *const forced_background_waits = nullptr,
            bool *const admission_during_draw = nullptr, const bool retro = false, const u32 viewport_height = 0u,
            const float roll = 0.0f, const RetroSettings *const picture = nullptr,
            RetroPass *const retained_pass = nullptr, const bool camera_cut = false, const u64 world_job = 0u,
            const u64 world_content = 0u) {
            const auto rhi = SharedGpu::rhiService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            const auto target = makeRenderTarget_(device, edge);
            PPR_TEST_ASSERT(target.has_value());
            const bool offscreen = retro or picture != nullptr;
            RetroPass owned_pass;
            RetroPass &retro_pass = retained_pass != nullptr ? *retained_pass : owned_pass;
            if (offscreen and retained_pass == nullptr)
                PPR_TEST_ASSERT(
                    not retro_pass.initialize(*rhi, *SharedGpu::shaderService(), std::filesystem::current_path()));

            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                if (retained_pass == nullptr)
                    PPR_TEST_ASSERT(not retro_pass.shutdown());
            };
            const RetroSettings settings =
                picture != nullptr ? *picture
                                   : RetroSettings{.m_enabled = retro, .m_palette_enabled = retro, .m_pixel_scale = 4u};
            const u32 scene_edge = settings.m_enabled ? edge / settings.m_pixel_scale : edge;
            const u32 scene_height = viewport_height != 0u ? viewport_height : scene_edge;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{origin_x, origin_y, -0.5f};
            model.m_ortho_scale = width_m / static_cast<float>(scene_edge);
            model.m_basis = Quaternion::rotateZ(roll);
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(scene_edge), int(scene_height)}});
            const float height_m = model.m_ortho_scale * static_cast<float>(scene_height);
            const float3 centre{origin_x + width_m * 0.5f, origin_y + height_m * 0.5f, -0.5f};
            model.m_origin = centre - camera.getRight() * (width_m * 0.5f) - camera.getUp() * (height_m * 0.5f);
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(scene_edge), int(scene_height)}});
            auto snapshot = camera.getSnapshot();
            snapshot.m_has_camera_cut = camera_cut;
            const CameraSnapshot background_snapshot = snapshot;
            if (offscreen)
                snapshot = retro_pass.beginFrame(
                    settings, snapshot, int2{int(scene_edge), int(scene_height)}, grid.worldGeneration(), world_job,
                    world_content);

            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
            for (;;) {
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                PPR_TEST_ASSERT(prepared.has_value());

                if (*prepared)
                    break;

                PPR_TEST_ASSERT(not renderer->waitOnHost());
            }

            const auto viewport = rhi::Viewport::fromSize(float(scene_edge), float(scene_height));
            const auto scissor = rhi::ScissorRect::fromSize(scene_edge, scene_height);
            std::array<rhi::IBuffer *, 3u> prepared_particles{};
            const auto prepare_content = [&](const PreRenderContext &context) {
                if (forced_background_waits != nullptr) {
                    // Deterministically reproduce a pre-wait completion snapshot
                    // after all three backdrop slots have submitted readers.
                    PPR_TEST_ASSERT(static_cast<bool>(context.m_wait_for_fence));
                    const auto wait = [&](const FenceCompletionValue value) {
                        ++*forced_background_waits;
                        return context.m_wait_for_fence(value);
                    };
                    const PreRenderContext stale{
                        context.m_device, context.m_encoder, kFenceValueNone, FenceWaitHook{wait}};
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridMaterialGate, grid.prepareBackground(stale, background_snapshot, viewport, scissor));
                } else {
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridMaterialGate, grid.prepareBackground(context, background_snapshot, viewport, scissor));
                }

                PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.prepareGpuParticles(context));
                prepared_particles = grid.particleBuffersForTest();
                return std::error_code{};
            };
            const auto draw_content = [&](const DrawContext &context) -> std::error_code {
                if (const auto ec = grid.renderBackground(context)) {
                    return ec;
                }
                if (const auto ec = grid.render(context)) {
                    return ec;
                }

                PPR_TEST_ASSERT(grid.particleBuffersForTest() == prepared_particles);
                if (const auto ec = grid.renderParticles(context)) {
                    return ec;
                }
                if (admission_during_draw != nullptr) {
                    // Current roots are encoded and have no submitted completion yet.
                    // They must not be mistaken for a retiring preview's roots.
                    *admission_during_draw = false;
                    for (u32 step = 0u; step < 32u and not *admission_during_draw; ++step) {
                        *admission_during_draw = grid.worldCandidateAdmissionReady(context.m_completed_fence_value);
                    }
                }
                return std::error_code{};
            };
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, prepare_content(context));
                if (offscreen) {
                    return retro_pass.prepareScene(
                        context, RetroPass::scenePipelineSignature(retro_pass.temporalActive()),
                        int2{int(scene_edge), int(scene_height)}, DrawCallback{draw_content});
                }

                return {};
            };
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                return offscreen ? retro_pass.render(context) : draw_content(context);
            };
            DrawSubmission submission{"grid optical gate", draw};
            if (not offscreen) {
                submission.m_viewport = viewport;
                submission.m_scissor = scissor;
            }

            PPR_TEST_ASSERT(
                not renderer->renderToTexture(targetRef_(target), {submission}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            if (offscreen)
                retro_pass.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
            const auto pixels = readback_(device, targetRef_(target), edge);
            PPR_TEST_ASSERT(pixels.has_value());
            return *pixels;
        }

        [[nodiscard]] GatePixels captureOpticalGrid_(
            GridPass &grid, const GridPass::GridStagedFrame &stage, const u64 generation) {
            const auto rhi = SharedGpu::rhiService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and renderer != nullptr);
            bool admitted = false;
            for (u32 step = 0u; step < 32u and not admitted; ++step) {
                admitted = grid.worldCandidateAdmissionReady(renderer->completedFenceValue());
            }
            PPR_TEST_ASSERT(admitted);
            // Each control owns an independent world/catalog; only immutable
            // shader programs and signature-compatible pipelines are reused.
            auto candidate = stage;
            candidate.m_content_fingerprint = generation;
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(generation, generation, candidate));
            bool prepared = false;
            for (u32 step = 0u; step < 32u and not prepared; ++step) {
                const auto progress = grid.advanceWorldCandidate(rhi->getDevice(), renderer->completedFenceValue(), 8u);
                PPR_TEST_ASSERT(progress.has_value());
                prepared = *progress;
            }
            PPR_TEST_ASSERT(prepared);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(generation, generation, generation));
            return drawOpticalGrid_(grid);
        }

        [[nodiscard]] u32 changedOpticalPixels_(const GatePixels &a, const GatePixels &b) {
            PPR_TEST_ASSERT(a.m_size == b.m_size);
            u32 changed = 0u;
            for (u32 y = 8u; y + 8u < a.m_size; y += 3u) {
                for (u32 x = 8u; x + 8u < a.m_size; x += 3u) {
                    changed += texelDist_(gateTexel_(a, x, y), gateTexel_(b, x, y)) > 2u ? 1u : 0u;
                }
            }
            return changed;
        }

        PPR_UNIT_TEST (grid_optical_controls_and_four_layer_parallax) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not grid.shutdown());
            };
            u64 generation = 0u;
            const auto capture = [&](const GridPass::GridStagedFrame &stage) {
                return captureOpticalGrid_(grid, stage, ++generation);
            };
            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0x7090b0ffu;
            visual.m_surface_flags = 1u;
            visual.m_opacity = 0.0f;
            visual.m_normal_texture_index = 5u;
            visual.m_refraction_depth_m = 0.5f;
            const auto background = capture(opticalGridFixture_(visual, 0u));
            const auto ior_one = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(background, ior_one) == 0u);
            visual.m_ior = 1.5f;
            visual.m_normal_texture_index = 4u;
            const auto flat = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(background, flat) == 0u);
            visual.m_normal_texture_index = 5u;
            visual.m_refraction_depth_m = 0.0f;
            const auto zero_depth = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(background, zero_depth) == 0u);
            visual.m_refraction_depth_m = 0.5f;
            const auto refracted = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(background, refracted) > 100u);
            visual.m_surface_flags = kGridSurfaceOpticalBlend | kGridSurfaceTextureRepeat;
            visual.m_ior = 1.0f;
            const auto repeat_one = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(background, repeat_one) == 0u);
            visual.m_ior = 1.5f;
            const auto repeat_refracted = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(repeat_one, repeat_refracted) > 100u);

            visual.m_surface_flags = 0u;
            const auto opaque = capture(opticalGridFixture_(visual, 1u));
            visual.m_ior = 1.0f;
            const auto opaque_one = capture(opticalGridFixture_(visual, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(opaque, opaque_one) == 0u);
            auto relief = visual;
            relief.m_normal_texture_index = 4u;
            const auto flat_relief = capture(opticalGridFixture_(relief, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(opaque_one, flat_relief) > 100u);
            relief.m_metallic = 1.0f;
            relief.m_roughness = 1.0f;
            const auto rough_metal = capture(opticalGridFixture_(relief, 1u));
            relief.m_roughness = 0.0f;
            const auto smooth_metal = capture(opticalGridFixture_(relief, 1u));
            PPR_TEST_ASSERT(changedOpticalPixels_(rough_metal, smooth_metal) > 100u);
            relief.m_emissive_rgb[0] = 1.0f;
            relief.m_emissive_strength = 1.0f;
            const auto emitting = capture(opticalGridFixture_(relief, 1u));
            PPR_TEST_ASSERT(gateTexel_(emitting, 63u, 63u)[0] > gateTexel_(smooth_metal, 63u, 63u)[0] + 20u);
            visual.m_surface_flags = 1u;
            visual.m_opacity = 0.25f;
            const auto quarter = capture(opticalGridFixture_(visual, 1u));
            for (const u32 x: {17u, 53u, 91u}) {
                const auto b = gateTexel_(background, x, 63u);
                const auto m = gateTexel_(opaque_one, x, 63u);
                const auto q = gateTexel_(quarter, x, 63u);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    const float expected = 0.25f * m[channel] + 0.75f * b[channel];
                    PPR_TEST_ASSERT(std::abs(float(q[channel]) - expected) <= 2.0f);
                }
            }

            const auto complete_stage = opticalGridFixture_(visual, 0u, true);
            const auto complete = capture(complete_stage);
            const auto pan = drawOpticalGrid_(grid, 59.0f, 57.0f);
            const auto zoom = drawOpticalGrid_(grid, 56.0f, 56.0f, 8.0f);
            PPR_TEST_ASSERT(changedOpticalPixels_(complete, pan) > 100u);
            PPR_TEST_ASSERT(changedOpticalPixels_(complete, zoom) > 100u);
            for (u32 layer = 0u; layer < 4u; ++layer) {
                auto masked = complete_stage;
                masked.m_background_layers[layer].m_color_rgba = layer == 0u ? 0x000000ffu : 0xffffff00u;
                PPR_TEST_ASSERT(changedOpticalPixels_(complete, capture(masked)) > 100u);
            }

            // The centre row lies above the left surface and below the right
            // surface. Both samples use the real reflected height buffer.
            auto surface_stage = complete_stage;
            auto surface = std::make_shared<GridSurfaceProfile>();
            for (u32 column = 0u; column < kGridSurfaceColumns; ++column) {
                surface->m_heights_m[column / 4u][column % 4u] = column < 64u ? 63.0f : 65.0f;
            }
            surface_stage.m_surface_profile = std::move(surface);
            const auto surfaced = capture(surface_stage);
            const auto sky = gateTexel_(surfaced, 32u, 64u);
            PPR_TEST_ASSERT(sky != gateTexel_(complete, 32u, 64u));
            PPR_TEST_ASSERT(sky[2] > sky[1] and sky[1] > sky[0]);
            PPR_TEST_ASSERT(gateTexel_(surfaced, 96u, 64u) == gateTexel_(complete, 96u, 64u));
            surface_stage.m_terrain_visuals[0u].m_opacity = 0.0f;
            surface_stage.m_terrain_visuals[0u].m_ior = 1.0f;
            surface_stage.m_uploads[0u].m_cell_materials.assign(GridPass::kCellsPerChunk, 1u);
            PPR_TEST_ASSERT(changedOpticalPixels_(surfaced, capture(surface_stage)) == 0u);
        };

        [[nodiscard]] image::ImageAsset gridPattern_(const bool normal, const bool constant = false) {
            constexpr u32 size = 128u;
            std::vector<std::byte> pixels(size * size * 4u);
            for (u32 y = 0u; y < size; ++y) {
                for (u32 x = 0u; x < size; ++x) {
                    const float a = std::sin((float(x) + 2.0f * float(y)) * 2.0f * pi_v<float> / size);
                    const float b = std::cos((3.0f * float(x) - float(y)) * 2.0f * pi_v<float> / size);
                    const auto byte = [](const float value) {
                        return std::byte{u8(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f))};
                    };
                    const auto offset = (y * size + x) * 4u;
                    if (normal) {
                        const float nx = constant ? 0.0f : 0.45f * a;
                        const float ny = constant ? 0.0f : 0.35f * b;
                        pixels[offset] = byte(0.5f + 0.5f * nx);
                        pixels[offset + 1u] = byte(0.5f + 0.5f * ny);
                        pixels[offset + 2u] = byte(0.5f + 0.5f * std::sqrt(1.0f - nx * nx - ny * ny));
                    } else {
                        pixels[offset] = byte(constant ? 0.5f : 0.5f + 0.25f * a + 0.125f * b);
                        pixels[offset + 1u] = byte(constant ? 0.5f : 0.5f - 0.25f * a);
                        pixels[offset + 2u] = byte(constant ? 0.5f : 0.5f + 0.25f * b);
                    }
                    pixels[offset + 3u] = std::byte{255};
                }
            }
            image::ImageAsset image{};
            image.m_width = size;
            image.m_height = size;
            image.m_storage = mem::SharedBuffer::clone(mem::SharedBufferView{pixels.data(), pixels.size()});
            image.m_subresources.push_back(
                image::ImageSubresource{
                    .m_view = image.m_storage, .m_row_pitch = size * 4u, .m_slice_pitch = pixels.size()});
            // Linear fixtures make the public readback's expected values direct;
            // production sRGB decoding is covered by the image/cache gates.
            PPR_TEST_ASSERT(
                not image::generateMipChain(image, image::MipGenDesc{.m_full_chain = true, .m_normal_map = normal}));
            PPR_TEST_ASSERT(image.m_mip_count == 8u);
            return image;
        }

        void saveGridQa_(const GatePixels &pixels, const std::string_view name, const u32 height = 0u) {
            const char *const directory = std::getenv("PPR_GRID_QA_DIR");
            if (directory == nullptr) {
                return;
            }
            const std::filesystem::path root{directory};
            std::filesystem::create_directories(root);
            const mango::image::Surface surface{
                int(pixels.m_size), int(height != 0u ? height : pixels.m_size),
                mango::image::Format(32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                pixels.m_row_pitch, pixels.m_bytes.data()};
            PPR_TEST_ASSERT(static_cast<bool>(surface.save((root / (std::string{name} + ".png")).string())));
        }

        PPR_UNIT_TEST (picture_controls_preserve_independent_color_and_temporal_history) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            RetroPass picture_pass;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture_pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            RetroPass fresh_reference;
            PPR_TEST_ASSERT(not fresh_reference.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not fresh_reference.shutdown());
                PPR_TEST_ASSERT(not picture_pass.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0x7090b0ffu;
            std::ignore = captureOpticalGrid_(grid, opticalGridFixture_(visual, 0u), 1u);
            const auto capture = [&](const RetroSettings &settings, const u32 edge = 128u, const bool cut = false,
                                     const u64 job = 0u, const u64 content = 0u) {
                return drawOpticalGrid_(
                    grid, 56.0f, 56.0f, 16.0f, edge, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture_pass, cut,
                    job, content);
            };

            const auto differing_pixels = [](const GatePixels &a, const GatePixels &b) {
                PPR_TEST_ASSERT(a.m_size == b.m_size);
                u32 changed = 0u;
                for (u32 y = 0u; y < a.m_size; ++y) {
                    for (u32 x = 0u; x < a.m_size; ++x) {
                        changed += gateTexel_(a, x, y) != gateTexel_(b, x, y) ? 1u : 0u;
                    }
                }

                return changed;
            };

            RetroSettings settings{};
            const auto native = drawOpticalGrid_(grid);
            const auto bypass = capture(settings);
            PPR_TEST_ASSERT(changedOpticalPixels_(native, bypass) == 0u);
            settings.m_palette_enabled = true;
            const auto native_palette = capture(settings);
            PPR_TEST_ASSERT(changedOpticalPixels_(native, native_palette) > 100u);

            settings.m_enabled = true;
            settings.m_pixel_scale = 4u;
            settings.m_palette_enabled = false;
            const auto pixelized = capture(settings);
            PPR_TEST_ASSERT(gateTexel_(pixelized, 17u, 17u) == gateTexel_(pixelized, 18u, 18u));
            settings.m_palette_enabled = true;
            const auto pixelized_palette = capture(settings);
            PPR_TEST_ASSERT(changedOpticalPixels_(pixelized, pixelized_palette) > 100u);
            saveGridQa_(pixelized, "picture-pixelized-full-color");
            saveGridQa_(pixelized_palette, "picture-pixelized-palette");

            // Four accepted submits exercise every jitter phase and retain a
            // previous image while the unjittered composite background stays stable.
            for (const bool pixelization: {false, true}) {
                settings.m_enabled = pixelization;
                settings.m_palette_enabled = false;
                settings.m_temporal_enabled = true;
                const auto first = capture(settings, 128u, true);
                PPR_TEST_ASSERT(picture_pass.temporalActive());
                GatePixels converged{};
                for (u32 frame = 1u; frame < 4u; ++frame) {
                    converged = capture(settings);
                }

                // The composite background stays spatial and unjittered; its
                // appearance must remain stable across the temporal cycle.
                const auto repeated_phase = capture(settings);
                PPR_TEST_ASSERT(differing_pixels(first, repeated_phase) == 0u);
                PPR_TEST_ASSERT(texelDist_(gateTexel_(converged, 64u, 64u), {0u, 0u, 0u, 255u}) > 20u);
                saveGridQa_(converged, pixelization ? "picture-temporal-pixelized" : "picture-temporal-native");
                settings.m_palette_enabled = true;
                const auto temporal_palette = capture(settings);
                PPR_TEST_ASSERT(changedOpticalPixels_(first, temporal_palette) > 100u);
            }

            // Occupied cells form a diagonal silhouette. Unlike the background
            // checks above, this path writes scene depth and reprojects it.
            auto diagonal = opticalGridFixture_(visual, 0u);
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 0u; x < 128u; ++x) {
                    diagonal.m_uploads[0u].m_cell_materials[y * 128u + x] = x < y ? 1u : 0u;
                }
            }

            const auto geometry_native = captureOpticalGrid_(grid, diagonal, 2u);
            PPR_TEST_ASSERT(changedOpticalPixels_(geometry_native, native) > 100u);
            saveGridQa_(geometry_native, "picture-diagonal-native");
            for (const bool pixelization: {false, true}) {
                settings.m_enabled = pixelization;
                settings.m_palette_enabled = false;
                settings.m_temporal_enabled = true;
                GatePixels geometry{};
                for (u32 frame = 0u; frame < 4u; ++frame) {
                    geometry = capture(settings, 128u, frame == 0u);
                }

                saveGridQa_(
                    geometry,
                    pixelization ? "picture-diagonal-temporal-pixelized" : "picture-diagonal-temporal-native");
                const auto moving = drawOpticalGrid_(
                    grid, 56.125f, 56.0f, 16.0f, 128u, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture_pass);
                const auto moving_cut = drawOpticalGrid_(
                    grid, 56.125f, 56.0f, 16.0f, 128u, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture_pass,
                    true);
                PPR_TEST_ASSERT(changedOpticalPixels_(geometry, moving) > 0u);
                PPR_TEST_ASSERT(texelDist_(gateTexel_(moving, 32u, 32u), gateTexel_(moving_cut, 32u, 32u)) <= 3u);
                PPR_TEST_ASSERT(texelDist_(gateTexel_(moving, 96u, 96u), gateTexel_(moving_cut, 96u, 96u)) <= 3u);
                saveGridQa_(moving, pixelization ? "picture-diagonal-pan-pixelized" : "picture-diagonal-pan-native");
            }

            // Remove textured-background variation from the coverage proof.
            // Half an internal pixel places cell boundaries on sample centers,
            // so the four phases visit both sides of the fixed silhouette.
            diagonal.m_has_background = false;
            diagonal.m_terrain_visuals[0u].m_color_rgba = 0xff0000ffu;
            std::ignore = captureOpticalGrid_(grid, diagonal, 3u);
            for (const bool pixelization: {false, true}) {
                settings.m_enabled = pixelization;
                const float origin = pixelization ? 56.25f : 56.0625f;
                const auto coverage = [&](const bool cut = false) {
                    return drawOpticalGrid_(
                        grid, origin, origin, 16.0f, 128u, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture_pass,
                        cut);
                };

                const auto first = coverage(true);
                for (u32 phase = 1u; phase < 4u; ++phase) {
                    std::ignore = coverage();
                }

                const auto accumulated = coverage();
                const auto foreground = gateTexel_(first, 16u, 16u);
                const auto background = gateTexel_(first, 112u, 112u);
                PPR_TEST_ASSERT(texelDist_(foreground, background) > 30u);
                PPR_TEST_ASSERT(texelDist_(gateTexel_(accumulated, 16u, 16u), foreground) <= 1u);
                PPR_TEST_ASSERT(texelDist_(gateTexel_(accumulated, 112u, 112u), background) <= 1u);

                u32 intermediate_edges = 0u;
                for (u32 y = 8u; y < 120u; ++y) {
                    for (u32 x = 8u; x < 120u; ++x) {
                        if (std::abs(int(x + y) - 127) > 8)
                            continue;

                        const auto fresh = gateTexel_(first, x, y);
                        const auto temporal = gateTexel_(accumulated, x, y);
                        const bool fresh_endpoint = texelDist_(fresh, foreground) <= 1u or
                                                    texelDist_(fresh, background) <= 1u;
                        intermediate_edges += fresh_endpoint and
                                                      texelDist_(temporal, foreground) > 3u and
                                                      texelDist_(temporal, background) > 3u
                                                  ? 1u
                                                  : 0u;
                    }
                }

                PPR_TEST_ASSERT(intermediate_edges > 0u);
                std::println(
                    "TEMPORAL-COVERAGE pixelized={} intermediate_edge_pixels={}", pixelization, intermediate_edges);
                saveGridQa_(
                    first,
                    pixelization ? "picture-flat-diagonal-first-pixelized" : "picture-flat-diagonal-first-native");
                saveGridQa_(
                    accumulated, pixelization ? "picture-flat-diagonal-accumulated-pixelized"
                                              : "picture-flat-diagonal-accumulated-native");
            }

            // Reset checks use the flat diagonal so phase accumulation changes
            // geometry pixels before each reset is compared with fresh phase zero.
            for (const bool pixelization: {false, true}) {
                settings.m_enabled = pixelization;
                settings.m_palette_enabled = false;
                settings.m_temporal_enabled = true;
                const u32 edge = 128u;
                const float origin = pixelization ? 56.25f : 56.0625f;
                const auto render = [&](RetroPass &pass, const RetroSettings &active, const u32 extent, const bool cut,
                                        const u64 job, const u64 content) {
                    return drawOpticalGrid_(
                        grid, origin, origin, 16.0f, extent, nullptr, nullptr, false, 0u, 0.0f, &active, &pass, cut,
                        job, content);
                };
                const auto prove_accumulation = [&](const u32 extent, const u64 job, const u64 content) {
                    GatePixels accumulated{};
                    for (u32 phase = 0u; phase < 5u; ++phase) {
                        accumulated = render(picture_pass, settings, extent, phase == 0u, job, content);
                    }

                    const auto fresh = render(fresh_reference, settings, extent, true, job, content);
                    PPR_TEST_ASSERT(differing_pixels(accumulated, fresh) > 0u);
                };

                prove_accumulation(edge, 0u, 0u);
                const auto cut = render(picture_pass, settings, edge, true, 0u, 0u);
                const auto cut_reference = render(fresh_reference, settings, edge, true, 0u, 0u);
                PPR_TEST_ASSERT(differing_pixels(cut, cut_reference) == 0u);

                // Job and content identity changes must restart at phase zero
                // after the same diagonal has visibly accumulated prior history.
                prove_accumulation(edge, 17u, 23u);
                const auto new_job = render(picture_pass, settings, edge, false, 18u, 23u);
                const auto new_job_reference = render(fresh_reference, settings, edge, true, 18u, 23u);
                PPR_TEST_ASSERT(differing_pixels(new_job, new_job_reference) == 0u);

                prove_accumulation(edge, 18u, 23u);
                const auto new_content = render(picture_pass, settings, edge, false, 18u, 24u);
                const auto new_content_reference = render(fresh_reference, settings, edge, true, 18u, 24u);
                PPR_TEST_ASSERT(differing_pixels(new_content, new_content_reference) == 0u);

                prove_accumulation(edge, 0u, 0u);
                const auto resized = render(picture_pass, settings, 64u, false, 0u, 0u);
                const auto resized_reference = render(fresh_reference, settings, 64u, true, 0u, 0u);
                PPR_TEST_ASSERT(differing_pixels(resized, resized_reference) == 0u);

                prove_accumulation(edge, 0u, 0u);
                RetroSettings spatial = settings;
                spatial.m_temporal_enabled = false;
                std::ignore = render(picture_pass, spatial, edge, false, 0u, 0u);
                PPR_TEST_ASSERT(not picture_pass.temporalActive());
                const auto reenabled = render(picture_pass, settings, edge, false, 0u, 0u);
                const auto reenabled_reference = render(fresh_reference, settings, edge, true, 0u, 0u);
                PPR_TEST_ASSERT(differing_pixels(reenabled, reenabled_reference) == 0u);
            }
        };

        PPR_UNIT_TEST (background_magnification_matches_wrapped_cubic_reference) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            auto stage = opticalGridFixture_(visual, 0u);
            stage.m_terrain_images[0u].m_format = image::ENativeImageFormat::rgba8_linear;
            stage.m_background_layers[0u].m_parallax_factor = 1.0f;
            std::ignore = captureOpticalGrid_(grid, stage, 1u);
            const auto pixels = drawOpticalGrid_(grid, 7.8f, -0.2f, 0.8f);
            const auto &image = stage.m_terrain_images[0u];
            const auto &level = image.m_subresources.front();
            const auto bytes = level.m_view.getBufferData();
            const auto basis = [](const double distance) {
                const double d = std::abs(distance);
                if (d < 1.0)
                    return (4.0 - 6.0 * d * d + 3.0 * d * d * d) / 6.0;

                return d < 2.0 ? std::pow(2.0 - d, 3.0) / 6.0 : 0.0;
            };

            // This is a sixteen-point cardinal-basis convolution, independent
            // of the shader's four bilinear taps. The view crosses both repeat
            // seams and magnifies each source texel to hundreds of pixels.
            for (const u32 y: {4u, 31u, 63u, 95u, 123u}) {
                for (const u32 x: {4u, 31u, 63u, 95u, 123u}) {
                    const double world_x = 7.8 + (double(x) + 0.5) * 0.8 / 128.0;
                    const double world_y = -0.2 + (127.5 - double(y)) * 0.8 / 128.0;
                    const double tx = world_x * 0.5 - 0.5;
                    const double ty = world_y * 0.5 - 0.5;
                    const int base_x = int(std::floor(tx)), base_y = int(std::floor(ty));
                    std::array<double, 3u> expected{};
                    for (int dy = -1; dy <= 2; ++dy) {
                        for (int dx = -1; dx <= 2; ++dx) {
                            const int ix = base_x + dx, iy = base_y + dy;
                            const u32 sx = u32((ix % 4 + 4) % 4), sy = u32((iy % 4 + 4) % 4);
                            const double weight = basis(tx - ix) * basis(ty - iy);
                            for (u32 channel = 0u; channel < 3u; ++channel) {
                                expected[channel] += weight * std::to_integer<u8>(
                                                                  bytes[sy * level.m_row_pitch + sx * 4u + channel]);
                            }
                        }
                    }

                    const auto actual = gateTexel_(pixels, x, y);
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        PPR_TEST_ASSERT(std::abs(int(actual[channel]) - int(std::lround(expected[channel]))) <= 2);
                    }
                }
            }

            saveGridQa_(pixels, "background-cubic-repeat-seams");
        };

        // ------------------------------------------------------------------
        // temporal motion quality against an analytic spatial reference
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (temporal_layered_background_pan_uses_stable_spatial_fallback) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            RetroPass picture;
            RetroPass spatial_reference;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not spatial_reference.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not spatial_reference.shutdown());
                PPR_TEST_ASSERT(not picture.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            std::ignore = captureOpticalGrid_(grid, opticalGridFixture_(visual, 0u, true), 1u);
            for (const bool pixelized: {false, true}) {
                RetroSettings settings{.m_enabled = pixelized, .m_temporal_enabled = true, .m_pixel_scale = 4u};
                const auto capture = [&](const float pan, const RetroSettings &policy, RetroPass *const retained) {
                    return drawOpticalGrid_(
                        grid, pan, 56.0f, 16.0f, 128u, nullptr, nullptr, false, 0u, 0.0f, &policy, retained);
                };

                GatePixels last{};
                for (u32 frame = 0u; frame < 8u; ++frame) {
                    const float pan = 56.0f + float(frame) * 0.03125f;
                    last = capture(pan, settings, &picture);
                    RetroSettings spatial = settings;
                    spatial.m_temporal_enabled = false;
                    const auto reference = capture(pan, spatial, &spatial_reference);
                    PPR_TEST_ASSERT(changedOpticalPixels_(last, reference) == 0u);
                }

                for (u32 frame = 0u; frame < 4u; ++frame) {
                    const auto stopped = capture(56.21875f, settings, &picture);
                    PPR_TEST_ASSERT(changedOpticalPixels_(last, stopped) == 0u);
                }

                saveGridQa_(
                    last,
                    pixelized ? "background-pan-spatial-fallback-pixelized" : "background-pan-spatial-fallback-native");
            }
        };

        PPR_UNIT_TEST (temporal_camera_pan_preserves_pattern_contrast_and_phase) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader_service = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader_service.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            constexpr string_literal source = R"(
struct PatternFrame { float4x4 view_projection; float4 foreground; };
ConstantBuffer<PatternFrame> g_pattern;
struct VertexOutput { float4 position : SV_Position; float2 world : TEXCOORD0; nointerpolation uint layer : TEXCOORD1; };
[shader("vertex")]
VertexOutput vertexPattern(uint vertex : SV_VertexID) {
    static const float2 corners[3] = {
        float2(-512.0, -512.0), float2(2048.0, -512.0), float2(-512.0, 2048.0)};
    float2 xy;
    float z;
    uint layer = vertex >= 3u ? 1u : 0u;
    if (layer != 0u) {
        static const float2 quad[6] = {
            float2(0,0), float2(1,0), float2(1,1), float2(0,0), float2(1,1), float2(0,1)};
        float2 uv = quad[vertex - 3u];
        xy = float2(g_pattern.foreground.y + uv.x * 8.0, -512.0 + uv.y * 2560.0);
        z = g_pattern.foreground.x;
    } else {
        xy = corners[vertex];
        z = 0.2 + g_pattern.foreground.z * xy.x;
    }
    VertexOutput output;
    output.position = mul(float4(xy, z, 1.0), g_pattern.view_projection);
    output.world = xy;
    output.layer = layer;
    return output;
}
[shader("fragment")]
float4 fragmentPattern(VertexOutput input) : SV_Target {
    if (input.layer != 0u) return float4(0.9, 0.1, 0.2, 1.0);
    float value = 0.5 + 0.4 * sin(input.world.x * 6.283185307179586 / g_pattern.foreground.w);
    return float4(value, value, value, 1.0);
}
)";
            struct alignas(16) PatternFrame {
                float4x4 m_view_projection{identity_v};
                float m_foreground[4]{};
            };
            static_assert(sizeof(PatternFrame) == 80u);
            static_assert(alignof(PatternFrame) == 16u);
            static_assert(PPR_OFFSETOF(PatternFrame, m_foreground) == 64u);

            shader::SharedModule module{};
            PPR_TEST_ASSERT(not shader_service->loadModuleFromSource(
                "temporal_motion_pattern", "temporal_motion_pattern.slang", source, module.writeRef()));
            shader::ComPtr<shader::IEntryPoint> vertex;
            shader::ComPtr<shader::IEntryPoint> fragment;
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("vertexPattern", vertex.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(module->findEntryPointByName("fragmentPattern", fragment.writeRef())));
            shader::IComponentType *entries[] = {vertex.get(), fragment.get()};
            rhi::ShaderProgramDesc program_desc{};
            program_desc.linkingStyle = rhi::LinkingStyle::SingleProgram;
            program_desc.slangGlobalScope = module.get();
            program_desc.slangEntryPoints = entries;
            program_desc.slangEntryPointCount = 2u;
            rhi::ComPtr<rhi::IShaderProgram> program;
            shader::Diagnose diagnostics;
            PPR_TEST_ASSERT(not make_error_code(
                device.createShaderProgram(program_desc, program.writeRef(), diagnostics.writeRef())));

            rhi::ColorTargetDesc color{};
            color.format = rhi::Format::RGBA16Float;
            rhi::RenderPipelineDesc pipeline_desc{};
            pipeline_desc.program = program;
            pipeline_desc.primitiveTopology = rhi::PrimitiveTopology::TriangleList;
            pipeline_desc.targets = &color;
            pipeline_desc.targetCount = 1u;
            pipeline_desc.multisample.sampleCount = 4u;
            pipeline_desc.depthStencil.format = rhi::Format::D32Float;
            pipeline_desc.depthStencil.depthTestEnable = true;
            pipeline_desc.depthStencil.depthWriteEnable = true;
            pipeline_desc.depthStencil.depthFunc = rhi::ComparisonFunc::LessEqual;
            pipeline_desc.rasterizer.cullMode = rhi::CullMode::None;
            rhi::ComPtr<rhi::IRenderPipeline> pipeline;
            rhi::ComPtr<rhi::IShaderObject> root;
            PPR_TEST_ASSERT(not make_error_code(device.createRenderPipeline(pipeline_desc, pipeline.writeRef())));
            PPR_TEST_ASSERT(not make_error_code(device.createRootShaderObject(program, root.writeRef())));

            constexpr u32 output_edge = 128u;
            const auto target = makeRenderTarget_(device, output_edge);
            PPR_TEST_ASSERT(target.has_value());
            RetroPass moving;
            RetroPass fresh;
            PPR_TEST_ASSERT(not moving.initialize(*rhi, *shader_service, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not fresh.initialize(*rhi, *shader_service, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not fresh.shutdown());
                PPR_TEST_ASSERT(not moving.shutdown());
            };

            struct Quality {
                double m_contrast{};
                double m_phase_lag_pixels{};
                double m_mse{};
            };
            bool motion_quality_passed = true;
            bool stopped_quality_passed = true;
            bool transformed_quality_passed = true;

            constexpr float orthographic_slope = 0.0005f;
            constexpr float perspective_slope = 0.25f;
            constexpr float representable_period = 8.0f;
            constexpr float stress_period = 4.0f;
            for (const bool pixelized: {false, true}) {
                const u32 scale = pixelized ? 4u : 1u;
                const u32 scene_edge = output_edge / scale;
                const RetroSettings settings{.m_enabled = pixelized, .m_temporal_enabled = true, .m_pixel_scale = 4u};
                const auto snapshot_for = [&](const float pan, const float roll, const float zoom,
                                              const bool perspective) {
                    CameraModel model{};
                    model.m_camera_mode = perspective ? ECameraProjection::perspective
                                                      : ECameraProjection::orthographic;
                    model.m_origin =
                        perspective
                            ? float3{pan + float(scene_edge) * 0.5f, float(scene_edge) * 0.5f, -float(scene_edge)}
                            : float3{pan, 0.0f, -0.5f};
                    model.m_basis = Quaternion::rotateZ(roll);
                    model.m_ortho_scale = zoom;

                    Camera camera{model.m_camera_mode};
                    camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(scene_edge), int(scene_edge)}});
                    return camera.getSnapshot();
                };

                const auto render = [&](RetroPass &picture, const float pan, const bool cut, const bool read_pixels,
                                        const float roll = 0.0f, const float zoom = 1.0f,
                                        const bool perspective = false, const bool occluder = false,
                                        const float world_period = stress_period) {
                    auto snapshot = snapshot_for(pan, roll, zoom, perspective);
                    snapshot.m_has_camera_cut = cut;
                    const auto raster = picture.beginFrame(
                        settings, snapshot, int2{int(scene_edge), int(scene_edge)}, 1u);
                    PPR_TEST_ASSERT(picture.temporalActive());

                    const auto draw_pattern = [&](const DrawContext &context) -> std::error_code {
                        PatternFrame frame{};
                        frame.m_view_projection = raster.m_view_projection;
                        frame.m_foreground[0u] = -float(scene_edge) * 0.5f;
                        frame.m_foreground[1u] = 17.2f + float(scene_edge) * 0.5f;
                        frame.m_foreground[2u] = perspective ? perspective_slope : orthographic_slope;
                        frame.m_foreground[3u] = world_period;

                        rhi::ShaderCursor constants{};
                        PPR_RETURN_ERROR_ON_FAIL(
                            GridMaterialGate, rhi::ShaderCursor{root}["g_pattern"].getDereferenced(constants));
                        PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, constants.setData(&frame, sizeof(frame)));

                        context.m_pass.setRenderState(
                            {.viewports = {context.m_viewport},
                                .viewportCount = 1u,
                                .scissorRects = {context.m_scissor},
                                .scissorRectCount = 1u,
                                .vertexBuffers = {},
                                .indexBuffer = {}});
                        context.m_pass.bindPipeline(pipeline, root);
                        context.m_pass.draw({.vertexCount = occluder ? 9u : 3u});
                        return {};
                    };
                    const auto prepare = [&](const PreRenderContext &context) {
                        return picture.prepareScene(
                            context, RetroPass::scenePipelineSignature(true), int2{int(scene_edge), int(scene_edge)},
                            DrawCallback{draw_pattern});
                    };
                    const auto present = [&](const DrawContext &context) { return picture.render(context); };
                    PPR_TEST_ASSERT(not renderer->renderToTexture(
                        targetRef_(target), {DrawSubmission{"temporal motion pattern", present}}, {},
                        PreRenderCallback{prepare}));
                    picture.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                    if (not read_pixels)
                        return GatePixels{};

                    const auto pixels = readback_(device, targetRef_(target), output_edge);
                    PPR_TEST_ASSERT(pixels.has_value());
                    return *pixels;
                };

                const auto quality = [&](const GatePixels &pixels, const double pan,
                                         const double world_period = double(stress_period)) {
                    // Integrate the world sinusoid over the unjittered pixel
                    // footprint. This consumed reference is independent of
                    // temporal filtering and has the correct box-filter MTF.
                    const double frequency = 2.0 * std::numbers::pi_v<double> / world_period;
                    const double amplitude = 0.4 * std::sin(frequency * 0.5) / (frequency * 0.5);
                    double sine = 0.0, cosine = 0.0, error = 0.0;
                    u32 count = 0u;
                    for (u32 x = 4u; x + 4u < scene_edge; ++x) {
                        const double phase = frequency * (pan + double(x) + 0.5);
                        const double value = double(gateTexel_(pixels, x * scale + scale / 2u, output_edge / 2u)[0]) /
                                             255.0;
                        const double centered = value - 0.5;
                        sine += centered * std::sin(phase);
                        cosine += centered * std::cos(phase);
                        error += std::pow(centered - amplitude * std::sin(phase), 2.0);
                        ++count;
                    }

                    sine *= 2.0 / double(count);
                    cosine *= 2.0 / double(count);
                    return Quality{
                        std::hypot(sine, cosine) / amplitude, std::atan2(cosine, sine) / frequency,
                        error / double(count)};
                };

                const auto reference_pixel = [&](const CameraSnapshot &snapshot, const double pixel_x,
                                                 const double pixel_y, const double world_period) {
                    const double slope = snapshot.m_camera_mode == ECameraProjection::perspective
                                             ? double(perspective_slope)
                                             : double(orthographic_slope);
                    const double frequency = 2.0 * std::numbers::pi_v<double> / world_period;

                    double result = 0.0;
                    for (u32 sy = 0u; sy < 8u; ++sy) {
                        for (u32 sx = 0u; sx < 8u; ++sx) {
                            const double ndc_x = (pixel_x + (double(sx) + 0.5) / 8.0) * 2.0 / double(scene_edge) - 1.0;
                            const double ndc_y = 1.0 - (pixel_y + (double(sy) + 0.5) / 8.0) * 2.0 / double(scene_edge);
                            const auto unproject = [&](const double depth) {
                                const std::array<double, 4u> clip{ndc_x, ndc_y, depth, 1.0};
                                std::array<double, 4u> world{};
                                for (u32 column = 0u; column < 4u; ++column) {
                                    for (u32 row = 0u; row < 4u; ++row) {
                                        world[column] += clip[row] *
                                                         double(snapshot.m_invert_view_projection[row][column]);
                                    }
                                }

                                for (u32 axis = 0u; axis < 3u; ++axis)
                                    world[axis] /= world[3u];

                                return world;
                            };

                            const auto near_point = unproject(0.0);
                            const auto far_point = unproject(1.0);
                            const double dx = far_point[0u] - near_point[0u];
                            const double dz = far_point[2u] - near_point[2u];
                            const double t = (0.2 + slope * near_point[0u] - near_point[2u]) / (dz - slope * dx);
                            const double world_x = near_point[0u] + dx * t;
                            result += 0.5 + 0.4 * std::sin(world_x * frequency);
                        }
                    }

                    return result / 64.0;
                };

                const auto transformed_quality = [&](const GatePixels &pixels, const CameraSnapshot &snapshot,
                                                     const double world_period) {
                    double reference_energy = 0.0, observed_reference = 0.0, gradient_energy = 0.0;
                    double residual_gradient = 0.0, error = 0.0;
                    u32 count = 0u;
                    for (const u32 y: {scene_edge / 4u, scene_edge / 2u, scene_edge * 3u / 4u}) {
                        for (u32 x = 4u; x + 4u < scene_edge; ++x) {
                            const double expected = reference_pixel(snapshot, double(x), double(y), world_period);
                            const double gradient =
                                (reference_pixel(snapshot, double(x) + 0.1, double(y), world_period) -
                                    reference_pixel(snapshot, double(x) - 0.1, double(y), world_period)) /
                                0.2;
                            const double observed = double(gateTexel_(
                                                        pixels, x * scale + scale / 2u, y * scale + scale / 2u)[0]) /
                                                    255.0;
                            reference_energy += std::pow(expected - 0.5, 2.0);
                            observed_reference += (observed - 0.5) * (expected - 0.5);
                            gradient_energy += gradient * gradient;
                            residual_gradient += (observed - expected) * gradient;
                            error += std::pow(observed - expected, 2.0);
                            ++count;
                        }
                    }

                    return Quality{
                        observed_reference / reference_energy, residual_gradient / gradient_energy,
                        error / double(count)};
                };

                const auto max_projected_frequency = [&](const CameraSnapshot &snapshot, const double world_period) {
                    const double q = std::tan(double(snapshot.m_fov) * 0.5) * double(snapshot.m_aspect_ratio);
                    const double depth = 0.2 -
                                         double(snapshot.m_origin.z) +
                                         double(perspective_slope) * double(snapshot.m_origin.x);
                    const double slope_q = std::abs(double(perspective_slope) * q);
                    const double max_world_dx = 2.0 *
                                                q *
                                                depth /
                                                (double(scene_edge) * (1.0 - slope_q) * (1.0 - slope_q));
                    return max_world_dx / world_period;
                };

                for (const float speed: {0.125f, 0.5f, 2.25f}) {
                    constexpr float initial_pan = 17.2f;
                    for (u32 frame = 0u; frame < 8u; ++frame) {
                        std::ignore = render(moving, initial_pan, frame == 0u, false);
                    }

                    GatePixels moving_pixels{};
                    for (u32 frame = 1u; frame <= 16u; ++frame) {
                        moving_pixels = render(moving, initial_pan + speed * float(frame), false, frame == 16u);
                    }

                    const float final_pan = initial_pan + speed * 16.0f;
                    const Quality motion = quality(moving_pixels, double(final_pan));
                    const auto current_only = render(fresh, final_pan, true, true);
                    const Quality current = quality(current_only, double(final_pan));
                    GatePixels stopped_pixels{};
                    for (u32 frame = 0u; frame < 32u; ++frame) {
                        stopped_pixels = render(moving, final_pan, false, frame == 31u);
                    }

                    const Quality stopped = quality(stopped_pixels, double(final_pan));
                    std::println(
                        "TEMPORAL-MOTION pixelized={} speed_internal_pixels={} contrast={} lag_pixels={} mse={} "
                        "stopped_contrast={} stopped_lag={} stopped_mse={} fresh_mse={}",
                        pixelized, speed, motion.m_contrast, motion.m_phase_lag_pixels, motion.m_mse,
                        stopped.m_contrast, stopped.m_phase_lag_pixels, stopped.m_mse, current.m_mse);
                    motion_quality_passed = motion_quality_passed and
                                            motion.m_contrast >= 0.85 and
                                            std::abs(motion.m_phase_lag_pixels) <= 0.15 and
                                            motion.m_mse <= 0.006;
                    stopped_quality_passed = stopped_quality_passed and
                                             stopped.m_contrast >= 0.85 and
                                             std::abs(stopped.m_phase_lag_pixels) <= 0.15 and
                                             stopped.m_mse < current.m_mse * 0.6;
                    saveGridQa_(moving_pixels, std::format("motion-pattern-moving-{}-{}", pixelized, speed));
                    saveGridQa_(stopped_pixels, std::format("motion-pattern-stopped-{}-{}", pixelized, speed));
                }

                const std::array cases{"roll", "zoom", "reversal", "perspective-slow", "perspective-fast"};
                for (u32 scenario = 0u; scenario < cases.size(); ++scenario) {
                    constexpr float initial_pan = 17.2f;
                    const bool perspective = scenario >= 3u;
                    const float world_period = perspective ? representable_period : stress_period;
                    for (u32 frame = 0u; frame < 8u; ++frame) {
                        std::ignore = render(
                            moving, initial_pan, frame == 0u, false, 0.0f, 1.0f, perspective, false, world_period);
                    }

                    float pan = initial_pan, roll = 0.0f, zoom = 1.0f;
                    GatePixels pixels{};
                    for (u32 frame = 1u; frame <= 16u; ++frame) {
                        if (scenario == 0u)
                            roll = float(frame) * 0.002f;
                        else if (scenario == 1u)
                            zoom = 1.0f - float(frame) * 0.00125f;
                        else if (scenario == 2u)
                            pan = initial_pan + float(frame <= 8u ? frame : 16u - frame) * 0.5f;
                        else
                            pan = initial_pan + float(frame) * (scenario == 3u ? 0.125f : 2.25f);

                        pixels = render(moving, pan, false, frame == 16u, roll, zoom, perspective, false, world_period);
                    }

                    const auto snapshot = snapshot_for(pan, roll, zoom, perspective);
                    const Quality motion = transformed_quality(pixels, snapshot, double(world_period));
                    const Quality current = transformed_quality(
                        render(fresh, pan, true, true, roll, zoom, perspective, false, world_period), snapshot,
                        double(world_period));
                    GatePixels stopped{};
                    for (u32 frame = 0u; frame < 32u; ++frame) {
                        stopped = render(
                            moving, pan, false, frame == 31u, roll, zoom, perspective, false, world_period);
                    }

                    const Quality settled = transformed_quality(stopped, snapshot, double(world_period));
                    const double max_frequency = perspective ? max_projected_frequency(snapshot, double(world_period))
                                                             : 0.0;
                    if (perspective)
                        PPR_TEST_ASSERT(max_frequency < 0.5);

                    std::println(
                        "TEMPORAL-TRANSFORM pixelized={} case={} period={} max_cycles_per_pixel={} "
                        "contrast={} lag_pixels={} mse={} "
                        "stopped_contrast={} stopped_lag={} stopped_mse={} fresh_mse={}",
                        pixelized, cases[scenario], world_period, max_frequency, motion.m_contrast,
                        motion.m_phase_lag_pixels, motion.m_mse, settled.m_contrast, settled.m_phase_lag_pixels,
                        settled.m_mse, current.m_mse);
                    transformed_quality_passed = transformed_quality_passed and
                                                 motion.m_contrast >= 0.85 and
                                                 std::abs(motion.m_phase_lag_pixels) <= 0.2 and
                                                 motion.m_mse <= 0.008;
                    stopped_quality_passed = stopped_quality_passed and
                                             settled.m_contrast >= 0.85 and
                                             std::abs(settled.m_phase_lag_pixels) <= 0.2 and
                                             settled.m_mse < current.m_mse * 0.6;
                    saveGridQa_(pixels, std::format("motion-pattern-transform-{}-{}", pixelized, cases[scenario]));
                }

                // Period-four perspective stress approaches or exceeds Nyquist
                // across the cases; report it without applying acceptance gates.
                for (const float speed: {0.125f, 2.25f}) {
                    constexpr float initial_pan = 17.2f;
                    for (u32 frame = 0u; frame < 8u; ++frame) {
                        std::ignore = render(
                            moving, initial_pan, frame == 0u, false, 0.0f, 1.0f, true, false, stress_period);
                    }

                    float pan = initial_pan;
                    GatePixels pixels{};
                    for (u32 frame = 1u; frame <= 16u; ++frame) {
                        pan = initial_pan + speed * float(frame);
                        pixels = render(moving, pan, false, frame == 16u, 0.0f, 1.0f, true, false, stress_period);
                    }

                    const auto snapshot = snapshot_for(pan, 0.0f, 1.0f, true);
                    const Quality motion = transformed_quality(pixels, snapshot, double(stress_period));
                    const auto fresh_pixels = render(fresh, pan, true, true, 0.0f, 1.0f, true, false, stress_period);
                    const Quality current = transformed_quality(fresh_pixels, snapshot, double(stress_period));
                    GatePixels stopped{};
                    for (u32 frame = 0u; frame < 32u; ++frame) {
                        stopped = render(moving, pan, false, frame == 31u, 0.0f, 1.0f, true, false, stress_period);
                    }

                    const Quality settled = transformed_quality(stopped, snapshot, double(stress_period));
                    const double max_frequency = max_projected_frequency(snapshot, double(stress_period));
                    std::println(
                        "TEMPORAL-PERSPECTIVE-STRESS pixelized={} case={} period={} "
                        "max_cycles_per_pixel={} moving_contrast={} moving_lag_pixels={} moving_mse={} "
                        "fresh_contrast={} fresh_lag_pixels={} fresh_mse={} "
                        "stopped_contrast={} stopped_lag_pixels={} stopped_mse={}",
                        pixelized, speed == 0.125f ? "slow" : "fast", stress_period, max_frequency, motion.m_contrast,
                        motion.m_phase_lag_pixels, motion.m_mse, current.m_contrast, current.m_phase_lag_pixels,
                        current.m_mse, settled.m_contrast, settled.m_phase_lag_pixels, settled.m_mse);
                    saveGridQa_(
                        pixels,
                        std::format("motion-pattern-perspective-period4-stress-moving-{}-{}", pixelized, speed));
                    saveGridQa_(
                        fresh_pixels,
                        std::format("motion-pattern-perspective-period4-stress-fresh-{}-{}", pixelized, speed));
                    saveGridQa_(
                        stopped,
                        std::format("motion-pattern-perspective-period4-stress-stopped-{}-{}", pixelized, speed));
                }

                // Two actual rasterized depth planes produce newly revealed
                // background under perspective translation. Its gray pattern
                // must not retain the foreground's distinct red/green colors.
                for (u32 frame = 0u; frame < 8u; ++frame) {
                    std::ignore = render(moving, 17.2f, frame == 0u, false, 0.0f, 1.0f, true, true);
                }

                GatePixels revealed{};
                for (u32 frame = 1u; frame <= 16u; ++frame) {
                    revealed = render(moving, 17.2f + float(frame) * 0.5f, false, frame == 16u, 0.0f, 1.0f, true, true);
                }

                const auto revealed_reference = render(fresh, 25.2f, true, true, 0.0f, 1.0f, true, true);
                u32 revealed_samples = 0u;
                for (u32 x = scene_edge / 2u + 2u; x <= scene_edge / 2u + 6u; ++x) {
                    const auto actual = gateTexel_(revealed, x * scale + scale / 2u, output_edge / 2u);
                    const auto reference = gateTexel_(revealed_reference, x * scale + scale / 2u, output_edge / 2u);
                    PPR_TEST_ASSERT(std::abs(int(reference[0u]) - int(reference[1u])) <= 1);
                    PPR_TEST_ASSERT(std::abs(int(actual[0u]) - int(actual[1u])) <= 2);
                    PPR_TEST_ASSERT(std::abs(int(actual[1u]) - int(actual[2u])) <= 2);
                    ++revealed_samples;
                }

                const auto foreground = gateTexel_(
                    revealed, (scene_edge / 2u - 3u) * scale + scale / 2u, output_edge / 2u);
                PPR_TEST_ASSERT(foreground[0u] > 220u and foreground[1u] < 30u);
                std::println("TEMPORAL-DISOCCLUSION pixelized={} revealed_samples={}", pixelized, revealed_samples);
                saveGridQa_(revealed, std::format("motion-pattern-disocclusion-{}", pixelized));
            }

            PPR_TEST_ASSERT(motion_quality_passed);
            PPR_TEST_ASSERT(stopped_quality_passed);
            PPR_TEST_ASSERT(transformed_quality_passed);
        };

        void addAdjacentGridChunk_(GridPass::GridStagedFrame &stage) {
            stage.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 1u});
            auto upload = stage.m_uploads[0];
            upload.m_chunk_id = 1u;
            upload.m_payload.m_rect[0] = 128.0f;
            upload.m_payload.m_rect[2] = 256.0f;
            stage.m_uploads.push_back(std::move(upload));
        }

        [[nodiscard]] double gridPixelDifference_(const GatePixels &a, const GatePixels &b, const u32 shift_x = 0u) {
            PPR_TEST_ASSERT(a.m_size == b.m_size);
            double sum = 0.0;
            u64 count = 0u;
            for (u32 y = 16u; y + 16u < a.m_size; ++y) {
                for (u32 x = 16u; x + shift_x + 16u < a.m_size; ++x) {
                    const auto pa = gateTexel_(a, x + shift_x, y);
                    const auto pb = gateTexel_(b, x, y);
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        sum += std::abs(int(pa[channel]) - int(pb[channel]));
                        ++count;
                    }
                }
            }
            return sum / double(count);
        }

        [[nodiscard]] std::array<double, 4u> gridBilinear_(
            const image::ImageAsset &image, const double u, const double v) {
            const double x = (u - std::floor(u)) * image.m_width - 0.5;
            const double y = (v - std::floor(v)) * image.m_height - 0.5;
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            const auto &level = image.m_subresources.front();
            const auto bytes = level.m_view.getBufferData();
            std::array<double, 4u> result{};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const u32 sx = u32((ix + dx + int(image.m_width)) % int(image.m_width));
                    const u32 sy = u32((iy + dy + int(image.m_height)) % int(image.m_height));
                    const double weight = (dx == 0 ? 1.0 - (x - ix) : x - ix) * (dy == 0 ? 1.0 - (y - iy) : y - iy);
                    for (u32 channel = 0u; channel < 4u; ++channel) {
                        result[channel] += weight *
                                           std::to_integer<u8>(bytes[sy * level.m_row_pitch + sx * 4u + channel]) /
                                           255.0;
                    }
                }
            }
            return result;
        }

        [[nodiscard]] std::array<double, 3u> gridPatternReference_(
            const image::ImageAsset &albedo, const image::ImageAsset &normals, const float u, const float v,
            const bool stochastic) {
            const float lx = u - v * 0.577350269f, ly = v * 1.154700538f;
            const int cx = int(std::floor(lx)), cy = int(std::floor(ly));
            const float fx = lx - static_cast<float>(cx), fy = ly - static_cast<float>(cy);
            std::array<std::array<int, 2u>, 3u> vertices{};
            std::array<double, 3u> weights{};
            if (fx + fy <= 1.0f) {
                vertices = {{{cx, cy}, {cx + 1, cy}, {cx, cy + 1}}};
                weights = {
                    1.0 - static_cast<double>(fx) - static_cast<double>(fy), static_cast<double>(fx),
                    static_cast<double>(fy)};
            } else {
                vertices = {{{cx + 1, cy + 1}, {cx, cy + 1}, {cx + 1, cy}}};
                weights = {
                    static_cast<double>(fx) + static_cast<double>(fy) - 1.0, 1.0 - static_cast<double>(fx),
                    1.0 - static_cast<double>(fy)};
            }
            if (not stochastic) {
                weights = {1.0, 0.0, 0.0};
            }
            std::array<double, 3u> color{}, normal{};
            for (u32 patch = 0u; patch < 3u; ++patch) {
                u32 hash = (u32(vertices[patch][0]) & 65535u) * 0x9e3779b9u ^
                           (u32(vertices[patch][1]) & 65535u) * 0x85ebca6bu;
                hash ^= hash >> 16u;
                hash *= 0x7feb352du;
                hash ^= hash >> 15u;
                hash *= 0x846ca68bu;
                hash ^= hash >> 16u;
                const double pu = static_cast<double>(u) + (stochastic ? double(hash & 65535u) / 65536.0 : 0.0);
                const double pv = static_cast<double>(v) + (stochastic ? double(hash >> 16u) / 65536.0 : 0.0);
                const auto a = gridBilinear_(albedo, pu, pv);
                const auto n = gridBilinear_(normals, pu, pv);
                for (u32 channel = 0u; channel < 3u; ++channel) {
                    color[channel] += a[channel] * weights[patch];
                    double decoded = n[channel] * 2.0 - 1.0;
                    if (channel < 2u and std::abs(decoded) <= 1.0 / 255.0 + 1.0e-6) {
                        decoded = 0.0;
                    }
                    normal[channel] += decoded * weights[patch];
                }
            }
            const double inverse_length = 1.0 /
                                          std::sqrt(
                                              std::inner_product(normal.begin(), normal.end(), normal.begin(), 0.0));
            const double facing = std::clamp(
                (normal[0] * -0.35 + normal[1] * 0.45 + normal[2]) * inverse_length / std::sqrt(1.325), 0.0, 1.0);
            const auto tail = albedo.m_subresources.back().m_view.getBufferData();
            const double correction = stochastic ? 1.0 / std::sqrt(
                                                             std::inner_product(
                                                                 weights.begin(), weights.end(), weights.begin(), 0.0))
                                                 : 1.0;
            for (u32 channel = 0u; channel < 3u; ++channel) {
                const double mean = std::to_integer<u8>(tail[channel]) / 255.0;
                color[channel] = std::clamp(mean + (color[channel] - mean) * correction, 0.0, 1.0);
                color[channel] = (color[channel] * (0.72 + 0.28 * facing) + 0.04 * facing * facing * 0.12) * 255.0;
            }
            return color;
        }

        PPR_UNIT_TEST (grid_stochastic_world_uvs_mips_and_chunk_seams) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid());
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not grid.shutdown());
            };
            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xffffff80u;
            visual.m_surface_flags = 1u;
            visual.m_texture_index = 6u;
            visual.m_normal_texture_index = 7u;
            visual.m_texture_period_m = 8.0f;
            auto stage = opticalGridFixture_(visual, 1u);
            stage.m_has_catalog = true;
            stage.m_terrain_images.push_back(gridPattern_(false));
            stage.m_terrain_images.push_back(gridPattern_(true));
            addAdjacentGridChunk_(stage);
            std::ignore = captureOpticalGrid_(grid, stage, 1u);
            const auto solid = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            saveGridQa_(solid, "pattern-solid-chunk-seam");
            const auto subpixel = drawOpticalGrid_(grid, 120.015625f, 56.0f, 16.0f, 256u);
            saveGridQa_(subpixel, "pattern-terrain-subpixel-pan");
            PPR_TEST_ASSERT(gridPixelDifference_(solid, subpixel) < 8.0);

            // A whole source period must no longer repeat the same patch.
            double repetition_difference = 0.0;
            for (u32 y = 32u; y < 224u; ++y) {
                for (u32 x = 16u; x < 112u; ++x) {
                    repetition_difference += texelDist_(gateTexel_(solid, x, y), gateTexel_(solid, x + 128u, y));
                }
            }
            PPR_TEST_ASSERT(repetition_difference / (192.0 * 96.0) > 12.0);

            auto repeated = stage;
            repeated.m_has_background = false;
            repeated.m_terrain_visuals[0].m_surface_flags = kGridSurfaceTextureRepeat;
            std::ignore = captureOpticalGrid_(grid, repeated, 7u);
            const auto regular = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            saveGridQa_(regular, "pattern-authored-repeat");
            double regular_difference = 0.0;
            for (u32 y = 32u; y < 224u; ++y) {
                for (u32 x = 16u; x < 112u; ++x) {
                    regular_difference += texelDist_(gateTexel_(regular, x, y), gateTexel_(regular, x + 128u, y));
                }
            }
            PPR_TEST_ASSERT(regular_difference / (192.0 * 96.0) < 0.3);
            repeated.m_terrain_visuals[0].m_surface_flags = 0u;
            std::ignore = captureOpticalGrid_(grid, repeated, 8u);
            const auto coherent = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            // Isolate world sampling from the backdrop's deliberate parallax.
            const auto panned = drawOpticalGrid_(grid, 120.0625f, 56.0f, 16.0f, 256u);
            PPR_TEST_ASSERT(gridPixelDifference_(coherent, panned, 1u) < 0.3);
            // At exactly one source texel per pixel the base-level bilinear
            // reference pins patch translations, weights and signed normal
            // decoding together, rather than merely checking image variation.
            for (u32 y = 32u; y < 224u; y += 31u) {
                for (u32 x = 32u; x < 224u; x += 31u) {
                    const float u = (120.0f + (float(x) + 0.5f) / 16.0f) / 8.0f;
                    const float v = (56.0f + (255.5f - float(y)) / 16.0f) / 8.0f;
                    for (const bool stochastic: {false, true}) {
                        const auto expected = gridPatternReference_(
                            stage.m_terrain_images[6], stage.m_terrain_images[7], u, v, stochastic);
                        const auto actual = gateTexel_(stochastic ? coherent : regular, x, y);
                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            PPR_TEST_ASSERT(std::abs(expected[channel] - actual[channel]) <= 3.0);
                        }
                    }
                }
            }

            // Positive world coordinates can still produce a negative skewed
            // lattice coordinate. Preserve its fractional precision and hash.
            const auto negative_skew = drawOpticalGrid_(grid, 56.0f, 112.0f, 16.0f, 256u);
            for (u32 y = 32u; y < 224u; y += 31u) {
                for (u32 x = 32u; x < 224u; x += 31u) {
                    const float u = (56.0f + (float(x) + 0.5f) / 16.0f) / 8.0f;
                    const float v = (112.0f + (255.5f - float(y)) / 16.0f) / 8.0f;
                    const auto expected = gridPatternReference_(
                        stage.m_terrain_images[6], stage.m_terrain_images[7], u, v, true);
                    const auto actual = gateTexel_(negative_skew, x, y);
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        PPR_TEST_ASSERT(std::abs(expected[channel] - actual[channel]) <= 3.0);
                    }
                }
            }

            auto coarse = stage;
            coarse.m_particle_batches.clear();
            coarse.m_has_background = false;
            coarse.m_terrain_visuals[0].m_surface_flags = 0u;
            coarse.m_terrain_visuals[0].m_normal_texture_index = 0xffffffffu;
            coarse.m_terrain_visuals[0].m_texture_period_m = 0.001f;
            for (auto &upload: coarse.m_uploads) {
                std::ranges::fill(upload.m_cell_materials, u16{1u});
            }
            std::ignore = captureOpticalGrid_(grid, coarse, 4u);
            const auto minified = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            saveGridQa_(minified, "pattern-coarsest-mip");
            const auto tail = coarse.m_terrain_images[6].m_subresources.back().m_view.getBufferData();
            for (u32 y = 16u; y < 240u; y += 7u) {
                for (u32 x = 16u; x < 240u; x += 7u) {
                    const auto pixel = gateTexel_(minified, x, y);
                    for (u32 channel = 0u; channel < 3u; ++channel) {
                        PPR_TEST_ASSERT(std::abs(int(pixel[channel]) - std::to_integer<int>(tail[channel])) <= 1);
                    }
                }
            }
            coarse.m_terrain_visuals[0].m_texture_period_m = 4096.0f / (std::numeric_limits<float>::max)();
            std::ignore = captureOpticalGrid_(grid, coarse, 5u);
            const auto extreme = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            PPR_TEST_ASSERT(gridPixelDifference_(minified, extreme) < 0.1);
            coarse.m_terrain_images[6] = gridPattern_(false, true);
            // GPU format is authoritative even if caller metadata disagrees.
            coarse.m_terrain_images[6].m_is_srgb = true;
            coarse.m_terrain_visuals[0].m_texture_period_m = 8.0f;
            std::ignore = captureOpticalGrid_(grid, coarse, 6u);
            const auto constant = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            for (u32 y = 16u; y < 240u; y += 7u) {
                for (u32 x = 16u; x < 240u; x += 7u) {
                    const auto pixel = gateTexel_(constant, x, y);
                    PPR_TEST_ASSERT(pixel[0] == 128u and pixel[1] == 128u and pixel[2] == 128u);
                }
            }
        };

        PPR_UNIT_TEST (grid_translucent_solid_borders_and_internal_boundaries) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid());
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not grid.shutdown());
            };
            GridTerrainVisual glass{};
            glass.m_material_id = 1u;
            glass.m_color_rgba = 0xffffffffu;
            glass.m_surface_flags = 1u;
            auto stage = opticalGridFixture_(glass, 1u);
            stage.m_has_catalog = true;
            stage.m_has_background = false;
            auto solid = glass;
            solid.m_material_id = 2u;
            solid.m_surface_flags = 0u;
            stage.m_terrain_visuals.push_back(solid);
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 64u; x < 128u; ++x) {
                    stage.m_uploads[0].m_cell_materials[y * 128u + x] = 2u;
                }
            }
            std::ignore = captureOpticalGrid_(grid, stage, 1u);
            const auto opaque = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(opaque, "solid-solid-opaque-border");
            stage.m_terrain_visuals[0].m_color_rgba = 0xffffff00u;
            std::ignore = captureOpticalGrid_(grid, stage, 2u);
            const auto clear = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(clear, "solid-solid-zero-opacity-border-no-backdrop");
            stage.m_terrain_visuals[0].m_color_rgba = 0xffffff80u;
            std::ignore = captureOpticalGrid_(grid, stage, 3u);
            const auto translucent = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(translucent, "solid-solid-half-opacity-border-no-backdrop");
            u32 dark_border = 0u;
            for (u32 y = 24u; y < 232u; ++y) {
                for (u32 x = 108u; x < 148u; ++x) {
                    const auto reference = gateTexel_(opaque, x, y);
                    if (reference[0] >= 5u and reference[0] <= 8u) {
                        const auto a = gateTexel_(clear, x, y);
                        const auto b = gateTexel_(translucent, x, y);
                        PPR_TEST_ASSERT(std::abs(int(a[0]) - int(reference[0])) <= 2);
                        PPR_TEST_ASSERT(std::abs(int(b[0]) - int(reference[0])) <= 2);
                        ++dark_border;
                    }
                }
            }
            PPR_TEST_ASSERT(dark_border > 100u);
            // The optical interior remains transparent; contour opacity does
            // not turn the entire zero-opacity solid into opaque dark fill.
            PPR_TEST_ASSERT(texelDist_(gateTexel_(opaque, 32u, 128u), gateTexel_(clear, 32u, 128u)) > 100u);

            stage.m_has_background = true;
            std::ignore = captureOpticalGrid_(grid, stage, 4u);
            const auto internal = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(internal, "solid-glass-internal-border");
        };

        PPR_UNIT_TEST (grid_material_shading_cost_samples) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            constexpr u32 edge = 1024u;
            constexpr u32 warm_frames = 3u;
            constexpr u32 sample_frames = 5u;
            std::println(
                "GRID-COST device_type={} core_validation={} gpu_assisted_validation={}",
                static_cast<int>(device.getDeviceType()), rhi->isCoreValidationEnabled(),
                rhi->isGpuAssistedValidationEnabled());
            const auto target = makeRenderTarget_(device, edge);
            PPR_TEST_ASSERT(target.has_value());
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, -0.5f};
            model.m_ortho_scale = 128.0f / static_cast<float>(edge);
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(edge), int(edge)}});
            const auto snapshot = camera.getSnapshot();
            const auto viewport = rhi::Viewport::fromSize(float(edge), float(edge));
            const auto scissor = rhi::ScissorRect::fromSize(edge, edge);
            const std::array names{
                "homogeneous-solid", "dense-solid-interfaces", "translucent-exterior", "dense-particles"};
            for (u32 scenario = 0u; scenario < names.size(); ++scenario) {
                GridTerrainVisual visual{};
                visual.m_material_id = 1u;
                visual.m_color_rgba = scenario >= 2u ? 0xffffff80u : 0xffffffffu;
                visual.m_texture_index = 0u;
                visual.m_texture_period_m = 8.0f;
                visual.m_normal_texture_index = 5u;
                visual.m_roughness = 0.5f;
                visual.m_surface_flags = scenario >= 2u ? 1u : 0u;
                visual.m_ior = 1.5f;
                visual.m_refraction_depth_m = 0.05f;
                auto stage = opticalGridFixture_(visual, scenario == 3u ? 0u : 1u, true);
                stage.m_has_catalog = true;
                if (scenario == 1u) {
                    auto other = visual;
                    other.m_material_id = 2u;
                    other.m_color_rgba = 0x80c0ffffu;
                    other.m_normal_texture_index = 4u;
                    stage.m_terrain_visuals.push_back(other);
                }
                if (scenario == 1u or scenario == 2u) {
                    for (u32 y = 0u; y < 128u; ++y) {
                        for (u32 x = 0u; x < 128u; ++x) {
                            stage.m_uploads[0].m_cell_materials[y * 128u + x] = (x + y) % 2u == 0u ? 1u
                                                                                : scenario == 1u   ? 2u
                                                                                                   : 0u;
                        }
                    }
                }
                if (scenario == 3u) {
                    auto records = std::make_shared<Array<GridParticleInstance>>();
                    records->reserve(GridPass::kCellsPerChunk);
                    for (u32 y = 0u; y < 128u; ++y) {
                        for (u32 x = 0u; x < 128u; ++x) {
                            records->push_back(
                                GridParticleInstance{
                                    u64(y) * 128u + x + 1u, double(x) + 0.5, double(y) + 0.5, 1.0, 1.0, 300.0, 1u});
                        }
                    }
                    stage.m_particle_batches.push_back(GridParticleBatch{0u, std::move(records)});
                }
                std::ignore = captureOpticalGrid_(grid, stage, scenario + 1u);
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                for (;;) {
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                    const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                    PPR_TEST_ASSERT(prepared.has_value());

                    if (*prepared)
                        break;

                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                }

                const auto prepare = [&](const PreRenderContext &context) {
                    PPR_RETURN_ERROR_ON_FAIL(
                        GridMaterialGate, grid.prepareBackground(context, snapshot, viewport, scissor));
                    return grid.prepareGpuParticles(context);
                };
                const auto draw = [&](const DrawContext &context) -> std::error_code {
                    PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.renderBackground(context));
                    PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.render(context));
                    PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.renderParticles(context));
                    return {};
                };

                // Reuse the target, camera, world and resident resources. These
                // submission-matched timestamps include backdrop preparation and
                // all production grid draws, without readback in the sample loop.
                std::array<float, sample_frames> samples{};
                for (u32 frame = 0u; frame < warm_frames + sample_frames; ++frame) {
                    PPR_TEST_ASSERT(not renderer->renderToTexture(
                        targetRef_(target), {DrawSubmission{"grid material cost", draw}}, {},
                        PreRenderCallback{prepare}));
                    const auto submitted = renderer->lastSubmittedFenceValue();
                    grid.notifyFrameSubmitted(submitted);
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                    const auto timing = renderer->lastGpuFrameTiming();
                    PPR_TEST_ASSERT(timing.has_value() and timing->m_submission == submitted);
                    PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
                    if (frame >= warm_frames) {
                        samples[frame - warm_frames] = timing->m_milliseconds;
                    }
                }
                const auto observations = grid.observations();
                PPR_TEST_ASSERT(observations.m_particle_instances == (scenario == 3u ? 16384u : 0u));
                PPR_TEST_ASSERT(observations.m_background_targets == 3u);
                std::ranges::sort(samples);
                const float mean = std::accumulate(samples.begin(), samples.end(), 0.0f) / float(sample_frames);
                std::println(
                    "GRID-COST scenario={} edge={} samples={} gpu_ms_min={} median={} max={} mean={} "
                    "decoded_bytes={} particle_bytes={} backdrop_bytes={}",
                    names[scenario], edge, sample_frames, samples.front(), samples[sample_frames / 2u], samples.back(),
                    mean, observations.m_texture_decoded_bytes, observations.m_particle_buffer_bytes,
                    observations.m_background_target_bytes);
            }
        };

        PPR_UNIT_TEST (picture_temporal_cost_samples) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            constexpr u32 edge = 1024u;
            constexpr u32 warm_frames = 5u;
            constexpr u32 sample_frames = 5u;
            const auto target = makeRenderTarget_(device, edge);
            PPR_TEST_ASSERT(target.has_value());
            GridPass grid;
            RetroPass picture;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                PPR_TEST_ASSERT(not picture.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0x7090b080u;
            visual.m_texture_index = 0u;
            visual.m_texture_period_m = 8.0f;
            visual.m_surface_flags = kGridSurfaceOpticalBlend | kGridSurfaceTextureRepeat;
            visual.m_opacity = 0.5f;
            visual.m_ior = 1.0f;
            std::ignore = captureOpticalGrid_(grid, opticalGridFixture_(visual, 1u, true), 1u);
            std::println(
                "TEMPORAL-COST device_type={} core_validation={} gpu_assisted_validation={} output_edge={}",
                static_cast<int>(device.getDeviceType()), rhi->isCoreValidationEnabled(),
                rhi->isGpuAssistedValidationEnabled(), edge);

            for (const bool pixelized: {false, true}) {
                const u32 scene_edge = pixelized ? edge / 4u : edge;
                const auto viewport = rhi::Viewport::fromSize(float(scene_edge), float(scene_edge));
                const auto scissor = rhi::ScissorRect::fromSize(scene_edge, scene_edge);
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_origin = float3{56.0f, 56.0f, -0.5f};
                model.m_ortho_scale = 16.0f / static_cast<float>(scene_edge);
                Camera camera{ECameraProjection::orthographic};
                camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(scene_edge), int(scene_edge)}});
                auto snapshot = camera.getSnapshot();
                snapshot.m_has_camera_cut = false;

                for (const bool temporal: {false, true}) {
                    const RetroSettings settings{
                        .m_enabled = pixelized,
                        .m_palette_enabled = pixelized,
                        .m_temporal_enabled = temporal,
                        .m_pixel_scale = 4u};
                    const bool offscreen = pixelized or temporal;
                    std::array<float, sample_frames> samples{};
                    for (u32 frame = 0u; frame < warm_frames + sample_frames; ++frame) {
                        const CameraSnapshot raster = picture.beginFrame(
                            settings, snapshot, int2{int(scene_edge), int(scene_edge)}, grid.worldGeneration());
                        PPR_TEST_ASSERT(picture.temporalActive() == temporal);
                        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, raster));
                        const auto draw_content = [&](const DrawContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.renderBackground(context));
                            return grid.render(context);
                        };
                        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(
                                GridMaterialGate, grid.prepareBackground(context, snapshot, viewport, scissor));
                            if (offscreen) {
                                return picture.prepareScene(
                                    context, RetroPass::scenePipelineSignature(temporal),
                                    int2{int(scene_edge), int(scene_edge)}, DrawCallback{draw_content});
                            }

                            return {};
                        };
                        const auto present = [&](const DrawContext &context) {
                            return offscreen ? picture.render(context) : draw_content(context);
                        };

                        // Keep targets, world, and pass resources alive across
                        // warmup/samples; readback is outside this measured loop.
                        PPR_TEST_ASSERT(not renderer->renderToTexture(
                            targetRef_(target), {DrawSubmission{"temporal picture cost", present}}, {},
                            PreRenderCallback{prepare}));
                        const auto submitted = renderer->lastSubmittedFenceValue();
                        grid.notifyFrameSubmitted(submitted);
                        if (offscreen)
                            picture.notifyFrameSubmitted(submitted);
                        PPR_TEST_ASSERT(not renderer->waitOnHost());
                        PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                        const auto timing = renderer->lastGpuFrameTiming();
                        PPR_TEST_ASSERT(timing.has_value() and timing->m_submission == submitted);
                        PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
                        if (frame >= warm_frames) {
                            samples[frame - warm_frames] = timing->m_milliseconds;
                        }
                    }

                    std::ranges::sort(samples);
                    const float mean = std::accumulate(samples.begin(), samples.end(), 0.0f) / float(sample_frames);
                    std::println(
                        "TEMPORAL-COST pixelized={} palette={} temporal={} scene_edge={} samples={} "
                        "gpu_ms_min={} median={} max={} mean={}",
                        pixelized, settings.m_palette_enabled, temporal, scene_edge, sample_frames, samples.front(),
                        samples[sample_frames / 2u], samples.back(), mean);
                }
            }
        };

        PPR_UNIT_TEST (grid_particle_support_and_resource_repeat) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xff0000ffu;
            auto stage = opticalGridFixture_(visual, 0u);
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->push_back(GridParticleInstance{1u, 64.5, 64.5, 1.0, 1.0, 300.0, 1u});
            records->push_back(GridParticleInstance{2u, 65.5, 64.5, 1.0, 1.0, 300.0, 1u});
            stage.m_particle_batches.push_back(GridParticleBatch{0u, records});
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not grid.shutdown());
            };
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto centered = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            for (u32 x = 17u; x < 79u; ++x) {
                const auto pixel = gateTexel_(centered, x, 96u);
                PPR_TEST_ASSERT(pixel[0] > 250u and pixel[1] < 3u and pixel[2] < 3u);
            }
            stage.m_uploads.clear();
            stage.m_tiles[0].m_dirty_mask = 0u;
            auto offcenter = std::make_shared<Array<GridParticleInstance>>(*records);
            (*offcenter)[0].m_position_x_m = 64.01;
            (*offcenter)[0].m_position_y_m = 64.01;
            (*offcenter)[1].m_position_x_m = 65.99;
            (*offcenter)[1].m_position_y_m = 64.99;
            stage.m_particle_batches[0].m_particles = offcenter;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto displaced = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            PPR_TEST_ASSERT(changedOpticalPixels_(centered, displaced) > 0u);
            auto half = std::make_shared<Array<GridParticleInstance>>(*offcenter);
            (*half)[0].m_volume_m3 = 0.5;
            stage.m_particle_batches[0].m_particles = half;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto partial = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            PPR_TEST_ASSERT(changedOpticalPixels_(displaced, partial) > 0u);
            stage.m_particle_batches[0].m_particles = offcenter;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            for (u32 frame = 0u; frame < 6u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            }
            const auto stable = grid.observations();
            PPR_TEST_ASSERT(stable.m_particle_instances == 2u and stable.m_particle_buffer_bytes == 396u);
            PPR_TEST_ASSERT(stable.m_background_targets == 3u);
            PPR_TEST_ASSERT(stable.m_background_target_bytes == 3u * 128u * 128u * 8u);
            for (u32 frame = 0u; frame < 6u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            }
            PPR_TEST_ASSERT(grid.observations().m_particle_uploads == stable.m_particle_uploads);
            PPR_TEST_ASSERT(grid.observations().m_texture_entries == stable.m_texture_entries);
            PPR_TEST_ASSERT(grid.observations().m_texture_decoded_bytes == stable.m_texture_decoded_bytes);
            u32 forced_background_waits = 0u;
            for (u32 frame = 0u; frame < 3u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 3000.0f, 3000.0f, 4.0f, 64u, &forced_background_waits);
            }
            PPR_TEST_ASSERT(forced_background_waits == 3u);
            // The particles remain alive but offscreen. Resizing every old slot
            // must detach their completed roots without requiring another draw.
            PPR_TEST_ASSERT(grid.observations().m_particle_instances == 2u);
            PPR_TEST_ASSERT(grid.observations().m_particle_root_bindings <= 3u);
            stage.m_particle_batches[0].m_particles = std::make_shared<const Array<GridParticleInstance>>();
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            for (u32 frame = 0u; frame < 6u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f, 64u);
            }
            const auto empty = grid.observations();
            PPR_TEST_ASSERT(empty.m_particle_instances == 0u and empty.m_particle_buffer_bytes == 276u);
            PPR_TEST_ASSERT(empty.m_particle_root_bindings <= 3u);
            PPR_TEST_ASSERT(empty.m_background_targets == 3u);
            PPR_TEST_ASSERT(empty.m_background_target_bytes == 3u * 64u * 64u * 8u);
            std::println(
                "grid resources decoded={} particle={} backdrop={} draws={} uploads={}", empty.m_texture_decoded_bytes,
                stable.m_particle_buffer_bytes, empty.m_background_target_bytes, empty.m_draw_commands,
                empty.m_particle_uploads);
            const auto timing = renderer->lastGpuFrameTiming();
            PPR_TEST_ASSERT(timing.has_value());
            PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
            std::println("grid completed GPU frame ms={}", timing->m_milliseconds);
            const auto empty_pixels = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f, 64u);
            for (u64 cycle = 0u; cycle < 3u; ++cycle) {
                auto replacement = opticalGridFixture_(visual, 0u);
                replacement.m_content_fingerprint = 100u + cycle;
                replacement.m_particle_batches.push_back(GridParticleBatch{0u, records});
                const u64 job = 20u + cycle * 2u;
                // Diagnostic previews replace graphics while retaining one simulation generation.
                constexpr u64 generation = 41u;
                PPR_TEST_ASSERT(grid.worldCandidateAdmissionReady(renderer->completedFenceValue()));
                PPR_TEST_ASSERT(not grid.stageWorldCandidate(job, generation, replacement));
                const auto partial_candidate = grid.advanceWorldCandidate(
                    rhi->getDevice(), renderer->completedFenceValue(), 1u);
                PPR_TEST_ASSERT(partial_candidate.has_value());
                grid.cancelWorldCandidate(job);
                // Settle initialization uploads through the next owned submit
                // before asking retirement to release their resources.
                const auto retained = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f, 64u);
                bool admitted = false;
                for (u32 step = 0u; step < 32u and not admitted; ++step) {
                    admitted = grid.worldCandidateAdmissionReady(renderer->completedFenceValue());
                }
                PPR_TEST_ASSERT(admitted);
                if (cycle == 0u) {
                    PPR_TEST_ASSERT(changedOpticalPixels_(empty_pixels, retained) == 0u);
                }
                PPR_TEST_ASSERT(not grid.stageWorldCandidate(job + 1u, generation, replacement));
                bool prepared = false;
                for (u32 step = 0u; step < 32u and not prepared; ++step) {
                    const auto progress = grid.advanceWorldCandidate(
                        rhi->getDevice(), renderer->completedFenceValue(), 8u);
                    PPR_TEST_ASSERT(progress.has_value());
                    prepared = *progress;
                }
                PPR_TEST_ASSERT(prepared);
                PPR_TEST_ASSERT(
                    not grid.activateWorldCandidate(job + 1u, generation, replacement.m_content_fingerprint));
                PPR_TEST_ASSERT(grid.worldGeneration() == generation);
                admitted = false;
                // Drain inside the first active encode. Waiting another frame
                // could make generation-only retirement accidentally succeed.
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f, 64u, nullptr, &admitted);
                PPR_TEST_ASSERT(admitted);
                const auto settled = grid.observations();
                PPR_TEST_ASSERT(settled.m_particle_instances == 2u and settled.m_particle_buffer_bytes == 396u);
                PPR_TEST_ASSERT(settled.m_texture_entries == stable.m_texture_entries);
                PPR_TEST_ASSERT(settled.m_texture_decoded_bytes == stable.m_texture_decoded_bytes);
                PPR_TEST_ASSERT(settled.m_background_target_bytes == empty.m_background_target_bytes);
                PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 1u);
            }
        };

        // ------------------------------------------------------------------
        // resident circle masks, ordering and source revocation
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (grid_resident_circle_mask_motion_overlap_occlusion_and_leases) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not grid.shutdown());
            };
            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xffffffffu;
            visual.m_opacity = 0.5f;
            auto stage = opticalGridFixture_(visual, 0u);
            stage.m_has_background = false;
            auto terrain_visual = visual;
            terrain_visual.m_material_id = 2u;
            terrain_visual.m_color_rgba = 0x0000ffffu;
            terrain_visual.m_opacity = 1.0f;
            terrain_visual.m_roundness = 0.0f;
            stage.m_terrain_visuals.push_back(terrain_visual);
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            struct Hot {
                std::array<float, 16u> fields{};
            };
            struct Cold {
                std::array<float, 6u> fields{};
            };
            static_assert(sizeof(Hot) == 64u and sizeof(Cold) == 24u);
            std::array<Hot, 2u> hot{};
            std::array<Cold, 2u> cold{};
            for (u32 row = 0u; row < 2u; ++row) {
                hot[row].fields[0u] = 67.0f;
                hot[row].fields[1u] = 67.0f;
                hot[row].fields[15u] = 1.0f;
                cold[row].fields[1u] = std::numbers::pi_v<float>;
                cold[row].fields[3u + row] = 1.0f;
            }

            auto live = std::make_shared<std::atomic<u64>>(0u);
            u64 epoch = 0u;
            GridParticleGpuView selected;
            const auto publish = [&](const u32 count) {
                const auto buffer = [&](const u64 stride, const void *data) {
                    rhi::BufferDesc desc{};
                    desc.size = std::max(1u, count) * stride;
                    desc.elementSize = safe_narrowing(stride);
                    desc.memoryType = rhi::MemoryType::DeviceLocal;
                    desc.usage = rhi::BufferUsage::ShaderResource;
                    desc.defaultState = rhi::ResourceState::ShaderResource;
                    rhi::ComPtr<rhi::IBuffer> result;
                    PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
                    return result;
                };
                Array<GridParticleMetadataRow> rows(count);
                for (u32 row = 0u; row < count; ++row) {
                    rows[row].m_id_low = row + 1u;
                    rows[row].m_source_row = row;
                    rows[row].m_material = 1u;
                    rows[row].m_radius = 1.0f;
                }

                const auto metadata = prepareGridParticleMetadata(device, rows, count, 1.0, ++epoch);
                PPR_TEST_ASSERT(metadata.has_value());
                selected.m_numerical = buffer(sizeof(Hot), hot.data());
                selected.m_readonly = buffer(sizeof(Cold), cold.data());
                selected.m_metadata = *metadata;
                selected.m_live_epoch = live;
                selected.m_world = grid.activeWorldIdentity();
                selected.m_device_epoch = epoch;
                selected.m_publication = epoch;
                live->store(epoch, std::memory_order_release);
                PPR_TEST_ASSERT(not grid.applyGpuParticleView(selected));
            };

            publish(0u);
            const auto background = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
            publish(1u);
            const auto circle = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
            // Reference terrain rotates after particle preparation. Both slots
            // must retain this submission, and the pixel checks must draw its IDs.
            PPR_TEST_ASSERT(std::ranges::count(grid.slotCompletionValues(), renderer->lastSubmittedFenceValue()) == 2);

            const auto centre = gateTexel_(circle, 64u, 64u);
            const auto clear = gateTexel_(background, 64u, 64u);
            // Straight alpha0.5 over the captured clear, allowing two UNORM8
            // roundings. The cold red color must override the white palette.
            PPR_TEST_ASSERT(std::abs(int(centre[0]) - int(std::lround(127.5 + clear[0] * 0.5))) <= 2);
            PPR_TEST_ASSERT(std::abs(int(centre[1]) - int(std::lround(clear[1] * 0.5))) <= 2);
            PPR_TEST_ASSERT(gateTexel_(circle, 78u, 78u) == gateTexel_(background, 78u, 78u));
            PPR_TEST_ASSERT(gateTexel_(circle, 76u, 64u)[0] > gateTexel_(background, 76u, 64u)[0] + 30u);
            saveGridQa_(circle, "particle-circle-native");

            hot[0u].fields[0u] += 0.125f;
            publish(1u);
            const auto moved = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
            PPR_TEST_ASSERT(changedOpticalPixels_(circle, moved) > 0u);
            // 1/8 cell at16 pixels/cell translates exactly two native pixels.
            for (u32 y = 44u; y < 84u; ++y) {
                for (u32 x = 44u; x < 84u; ++x)
                    PPR_TEST_ASSERT(gateTexel_(moved, x + 2u, y) == gateTexel_(circle, x, y));
            }

            hot[0u].fields[0u] = 67.0f;
            publish(2u);
            const auto overlap = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
            const auto mixed = gateTexel_(overlap, 64u, 64u);
            PPR_TEST_ASSERT(std::abs(int(mixed[0]) - int(std::lround(63.75 + clear[0] * 0.25))) <= 2);
            PPR_TEST_ASSERT(std::abs(int(mixed[1]) - int(std::lround(127.5 + clear[1] * 0.25))) <= 2);
            rhi::IndirectDrawArguments args{};
            const auto buffers = grid.particleBuffersForTest();
            PPR_TEST_ASSERT(not make_error_code(device.readBuffer(buffers[1u], 0u, sizeof(args), &args)));
            PPR_TEST_ASSERT(args.vertexCountPerInstance == 6u and args.instanceCount == 2u);
            saveGridQa_(overlap, "particle-overlap-native");

            // The later green disk sits inside an earlier quad's transparent
            // corner. Mask-zero fragments must neither paint nor occlude it.
            hot[1u].fields[0u] = 67.85f;
            hot[1u].fields[1u] = 67.85f;
            cold[1u].fields[1u] = std::numbers::pi_v<float> * 0.0625f;
            publish(2u);
            const auto corner = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
            PPR_TEST_ASSERT(gateTexel_(corner, 77u, 50u)[1] > gateTexel_(background, 77u, 50u)[1] + 30u);

            publish(1u);
            const auto zoomed_out = drawOpticalGrid_(grid, 56.0f, 56.0f, 16.0f);
            PPR_TEST_ASSERT(gateTexel_(zoomed_out, 76u, 64u) == gateTexel_(background, 76u, 64u));
            const auto aspect = drawOpticalGrid_(grid, 60.0f, 62.0f, 8.0f, 128u, nullptr, nullptr, false, 64u);
            // Width8 at128 pixels gives16 pixels/metre on both axes. A radius1
            // disk must contain +/-12 and exclude +/-20 in the128x64 viewport.
            PPR_TEST_ASSERT(gateTexel_(aspect, 64u, 32u)[0] > clear[0] + 30u);
            for (const int direction: {-1, 1}) {
                PPR_TEST_ASSERT(gateTexel_(aspect, u32(64 + direction * 12), 32u)[0] > clear[0] + 30u);
                PPR_TEST_ASSERT(gateTexel_(aspect, 64u, u32(32 + direction * 12))[0] > clear[0] + 30u);
                PPR_TEST_ASSERT(gateTexel_(aspect, u32(64 + direction * 20), 32u) == clear);
                PPR_TEST_ASSERT(gateTexel_(aspect, 64u, u32(32 + direction * 20)) == clear);
            }

            const auto rotated = drawOpticalGrid_(
                grid, 60.0f, 60.0f, 8.0f, 128u, nullptr, nullptr, false, 0u, std::numbers::pi_v<float> * 0.25f);
            PPR_TEST_ASSERT(std::abs(int(gateTexel_(rotated, 64u, 64u)[0]) - int(centre[0])) <= 2);
            PPR_TEST_ASSERT(gateTexel_(rotated, 78u, 78u) == gateTexel_(background, 78u, 78u));
            const auto retro = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 128u, nullptr, nullptr, true);
            PPR_TEST_ASSERT(gateTexel_(retro, 64u, 64u)[0] > gateTexel_(retro, 80u, 80u)[0] + 20u);
            saveGridQa_(retro, "particle-circle-retro4");

            // An isolated solid cell has a rectangular occupied corner when
            // contours are off, and a cut corner when contours are enabled.
            stage.m_uploads[0u].m_cell_materials[64u * 128u + 64u] = 2u;
            for (const bool contours: {false, true}) {
                stage.m_has_catalog = contours;
                PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                publish(0u);
                const auto terrain = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
                publish(1u);
                const auto occupied = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f);
                PPR_TEST_ASSERT(gateTexel_(occupied, 72u, 56u) == gateTexel_(terrain, 72u, 56u));
                if (contours) {
                    PPR_TEST_ASSERT(gateTexel_(occupied, 65u, 62u)[0] > gateTexel_(terrain, 65u, 62u)[0] + 30u);
                } else {
                    PPR_TEST_ASSERT(gateTexel_(occupied, 65u, 62u) == gateTexel_(terrain, 65u, 62u));
                }

                saveGridQa_(occupied, contours ? "particle-terrain-contour" : "particle-terrain-discrete");
            }

            const auto before = grid.observations();
            std::ignore = drawOpticalGrid_(grid, 3000.0f, 3000.0f, 8.0f);
            const auto culled = grid.particleBuffersForTest();
            PPR_TEST_ASSERT(not make_error_code(device.readBuffer(culled[1u], 0u, sizeof(args), &args)));
            PPR_TEST_ASSERT(args.instanceCount == 0u);
            PPR_TEST_ASSERT(grid.observations().m_particle_uploads == before.m_particle_uploads);
            auto invalid = selected;
            invalid.m_readonly_stride = 64u;
            PPR_TEST_ASSERT(grid.applyGpuParticleView(invalid) == std::errc::invalid_argument);
            invalid = selected;
            invalid.m_readonly.setNull();
            PPR_TEST_ASSERT(grid.applyGpuParticleView(invalid) == std::errc::invalid_argument);
            live->fetch_add(1u, std::memory_order_release);
            PPR_TEST_ASSERT(not selected.current());
            PPR_TEST_ASSERT(grid.applyGpuParticleView(selected) == std::errc::invalid_argument);
        };

        // ------------------------------------------------------------------
        // element-textured solids and reconstructed liquid surfaces
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (grid_solid_particles_share_catalog_textures_and_metric_uvs) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass terrain_grid;
            GridPass particle_grid;
            PPR_TEST_ASSERT(not terrain_grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not particle_grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not particle_grid.shutdown());
                PPR_TEST_ASSERT(not terrain_grid.shutdown());
            };

            GridTerrainVisual visual{};
            visual.m_material_id = 1u;
            visual.m_color_rgba = 0xffffffffu;
            visual.m_texture_index = 0u;
            visual.m_texture_period_m = 1.0f;
            visual.m_normal_texture_index = 5u;
            visual.m_metallic = 0.2f;
            visual.m_roughness = 0.4f;
            visual.m_surface_flags = kGridSurfaceParticleSolid | kGridSurfaceTextureRepeat;
            auto terrain = opticalGridFixture_(visual, 1u);
            terrain.m_has_background = false;
            terrain.m_cell_size_m = 1.0f / 3.0f;
            auto second = visual;
            second.m_material_id = 2u;
            second.m_texture_index = 4u;
            terrain.m_terrain_visuals.push_back(second);
            auto particles = terrain;
            std::ranges::fill(particles.m_uploads.front().m_cell_materials, 0u);

            // One fine cell's physical volume at the selected 3-cells/metre
            // scale is 1/9 m^3. The metre-wide view resolves its disk interior.
            GatePixels first_element{};
            for (const u32 material: {1u, 2u}) {
                std::ranges::fill(terrain.m_uploads.front().m_cell_materials, static_cast<u16>(material));
                PPR_TEST_ASSERT(not terrain_grid.applyStagedFrame(terrain));

                auto records = std::make_shared<Array<GridParticleInstance>>();
                records->push_back({1u, 21.5, 21.5, 1.0 / 9.0, 1.0, 300.0, material});
                particles.m_particle_batches = {GridParticleBatch{0u, std::move(records)}};
                PPR_TEST_ASSERT(not particle_grid.applyStagedFrame(particles));

                for (const bool retro: {false, true}) {
                    const auto reference = drawOpticalGrid_(
                        terrain_grid, 21.0f, 21.0f, 1.0f, 128u, nullptr, nullptr, retro);
                    const auto disk = drawOpticalGrid_(
                        particle_grid, 21.0f, 21.0f, 1.0f, 128u, nullptr, nullptr, retro);

                    // Interior samples have full disk coverage. The corresponding
                    // terrain pixels consume the same element, physical UVs,
                    // normal texture and local material response.
                    for (const u32 y: {60u, 64u, 68u}) {
                        for (const u32 x: {60u, 64u, 68u}) {
                            PPR_TEST_ASSERT(texelDist_(gateTexel_(disk, x, y), gateTexel_(reference, x, y)) <= 4u);
                        }
                    }

                    if (not retro) {
                        saveGridQa_(
                            disk, material == 1u ? "particle-solid-textured-element-1-native"
                                                 : "particle-solid-textured-element-2-native");
                        const auto moved = drawOpticalGrid_(particle_grid, 21.015625f, 21.0f, 1.0f);
                        for (const u32 x: {60u, 64u, 68u}) {
                            PPR_TEST_ASSERT(texelDist_(gateTexel_(moved, x - 2u, 64u), gateTexel_(disk, x, 64u)) <= 4u);
                        }

                        if (material == 1u) {
                            PPR_TEST_ASSERT(texelDist_(gateTexel_(disk, 48u, 64u), gateTexel_(disk, 80u, 64u)) > 15u);
                            first_element = disk;
                        } else {
                            PPR_TEST_ASSERT(changedOpticalPixels_(first_element, disk) > 0u);
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (grid_surface_inventory_transitions_preserve_gas_visibility_and_single_blending) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual solid{};
            solid.m_material_id = 1u;
            solid.m_color_rgba = 0x0000ffffu;
            solid.m_surface_flags = kGridSurfaceParticleSolid;
            auto stage = opticalGridFixture_(solid, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;
            auto liquid = solid;
            liquid.m_material_id = 2u;
            liquid.m_surface_flags = kGridSurfaceParticleLiquid;
            stage.m_terrain_visuals.push_back(liquid);
            auto gas = solid;
            gas.m_material_id = 3u;
            gas.m_color_rgba = 0x00ff0080u;
            gas.m_surface_flags = kGridSurfaceOpticalBlend;
            stage.m_terrain_visuals.push_back(gas);
            const auto capture = [&](const std::initializer_list<GridParticleInstance> particles) {
                stage.m_particle_batches.clear();
                if (particles.size() != 0u) {
                    auto rows = std::make_shared<Array<GridParticleInstance>>(particles);
                    stage.m_particle_batches.push_back({0u, std::move(rows)});
                }

                PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                return drawOpticalGrid_(grid, 21.0f, 21.0f, 2.0f);
            };

            const GridParticleInstance solid_row{1u, 21.4, 22.0, 1.0 / 9.0, 1.0, 300.0, 1u};
            const GridParticleInstance liquid_row{2u, 21.8, 22.0, 1.0 / 9.0, 1.0, 300.0, 2u};
            const GridParticleInstance gas_row{3u, 22.6, 22.0, 1.0 / 9.0, 1.0, 300.0, 3u};
            const auto empty = capture({});
            const auto reference = capture({gas_row});
            const auto gas_pixel = gateTexel_(reference, 102u, 64u);
            const auto clear_pixel = gateTexel_(empty, 102u, 64u);
            PPR_TEST_ASSERT(gas_pixel[1] > clear_pixel[1] + 20u);

            std::ignore = capture({solid_row, liquid_row});
            const auto appended = capture({solid_row, liquid_row, gas_row});
            PPR_TEST_ASSERT(texelDist_(gateTexel_(appended, 102u, 64u), gas_pixel) <= 2u);
            auto replacement = gas_row;
            replacement.m_id = 101u;
            const auto replaced = capture({replacement});
            PPR_TEST_ASSERT(texelDist_(gateTexel_(replaced, 102u, 64u), gas_pixel) <= 2u);

            const auto removed = capture({solid_row, liquid_row});
            PPR_TEST_ASSERT(texelDist_(gateTexel_(removed, 102u, 64u), clear_pixel) <= 2u);
            const auto cleared = capture({});
            PPR_TEST_ASSERT(texelDist_(gateTexel_(cleared, 102u, 64u), clear_pixel) <= 2u);
            const auto returned = capture({replacement});
            PPR_TEST_ASSERT(texelDist_(gateTexel_(returned, 102u, 64u), gas_pixel) <= 2u);
        };

        PPR_UNIT_TEST (grid_solid_envelope_closes_nominal_interstices_without_bridging_sparse_grains) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridTerrainVisual blue{};
            blue.m_material_id = 1u;
            blue.m_color_rgba = 0x0000ffffu;
            blue.m_ior = 1.0f;
            blue.m_surface_flags = kGridSurfaceParticleSolid;
            auto stage = opticalGridFixture_(blue, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;
            auto red = blue;
            red.m_material_id = 2u;
            red.m_color_rgba = 0xff0000ffu;
            stage.m_terrain_visuals.push_back(red);
            auto rows = std::make_shared<Array<GridParticleInstance>>();
            for (u32 y = 0u; y < 6u; ++y) {
                for (u32 x = 0u; x < 6u; ++x) {
                    rows->push_back(
                        {u64(y * 6u + x + 1u), 21.0 + (double(x) + 0.5) / 3.0, 21.0 + (double(y) + 0.5) / 3.0,
                            1.0 / 9.0, 1.0, 300.0, x < 3u ? 1u : 2u});
                }
            }

            const double radius = std::sqrt((1.0 / 9.0) / std::numbers::pi);
            rows->push_back({37u, 20.5, 20.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            rows->push_back({38u, 23.0, 20.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            rows->push_back({39u, 23.0 + 4.5 * radius, 20.5, 1.0 / 9.0, 1.0, 300.0, 2u});
            rows->push_back({40u, 20.4, 23.4, 1.0 / 9.0, 1.0, 300.0, 1u});
            rows->push_back({41u, 20.9, 23.9, 1.0 / 36.0, 0.25, 300.0, 2u});
            const std::array<double, 4u> angles{0.0, std::numbers::pi / 8.0, std::numbers::pi / 4.0, std::atan(0.85)};
            for (u32 direction = 0u; direction < angles.size(); ++direction) {
                const double angle = angles[direction];
                const double y = 20.5 + double(direction) * 1.5;
                rows->push_back({u64(42u + direction * 2u), 24.8, y, 1.0 / 9.0, 1.0, 300.0, 1u});
                rows->push_back(
                    {u64(43u + direction * 2u), 24.8 + 3.5 * radius * std::cos(angle),
                        y + 3.5 * radius * std::sin(angle), 1.0 / 9.0, 1.0, 300.0, 1u});
            }

            stage.m_particle_batches = {GridParticleBatch{0u, std::move(rows)}};
            for (u32 policy = 0u; policy < 4u; ++policy) {
                GridPass grid;
                RetroPass picture;
                PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_DEFER {
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    PPR_TEST_ASSERT(not picture.shutdown());
                    PPR_TEST_ASSERT(not grid.shutdown());
                };
                PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                const bool temporal = policy == 1u;
                const bool pixelized = policy == 2u;
                const float roll = policy == 3u ? std::numbers::pi_v<float> * 0.25f : 0.0f;
                const RetroSettings settings{
                    .m_enabled = pixelized,
                    .m_palette_enabled = false,
                    .m_temporal_enabled = temporal,
                    .m_pixel_scale = 4u};
                GatePixels pixels{};
                for (u32 frame = 0u; frame < (temporal ? 4u : 1u); ++frame) {
                    pixels = drawOpticalGrid_(
                        grid, 20.0f, 20.0f, 6.0f, 384u, nullptr, nullptr, false, 0u, roll, &settings, &picture);
                }

                PPR_TEST_ASSERT(picture.temporalActive() == temporal);
                saveGridQa_(pixels, std::format("particle-solid-envelope-policy-{}", policy));
                const auto sample = [&](const double x, const double y) {
                    const double dx = x - 23.0;
                    const double dy = y - 23.0;
                    const double right = dx * std::cos(double(roll)) + dy * std::sin(double(roll));
                    const double up = -dx * std::sin(double(roll)) + dy * std::cos(double(roll));
                    return gateTexel_(pixels, u32((right + 3.0) * 64.0), u32((3.0 - up) * 64.0));
                };
                u32 holes = 0u;
                for (u32 y = 1u; y < 6u; ++y) {
                    for (u32 x = 1u; x < 6u; ++x) {
                        const auto pixel = sample(21.0 + double(x) / 3.0, 21.0 + double(y) / 3.0);
                        // A filtered red/blue interface can split full coverage
                        // between channels; their sum preserves opaque energy.
                        holes += u32(pixel[0]) + u32(pixel[2]) < 200u ? 1u : 0u;
                    }
                }

                std::println("SOLID-ENVELOPE policy={} packed_samples=25 holes={}", policy, holes);
                PPR_TEST_ASSERT(holes == 0u);
                const auto left = sample(21.8, 22.0);
                const auto right = sample(22.2, 22.0);
                PPR_TEST_ASSERT(left[2] > left[0] + 80u and right[0] > right[2] + 80u);
                // Sparse grains and the small/large diagonal pair retain real
                // empty space. These samples are separated from dense material.
                if (roll == 0.0f) {
                    const auto clear = sample(25.8, 25.8);
                    PPR_TEST_ASSERT(sample(20.5, 20.5)[2] > 150u);
                    PPR_TEST_ASSERT(texelDist_(sample(23.0 + 2.25 * radius, 20.5), clear) <= 2u);
                    const auto diagonal_gap = sample(20.7, 23.7);
                    PPR_TEST_ASSERT(texelDist_(diagonal_gap, clear) <= 2u);
                    for (u32 direction = 0u; direction < angles.size(); ++direction) {
                        const double angle = angles[direction];
                        const auto gap = sample(
                            24.8 + 1.75 * radius * std::cos(angle),
                            20.5 + double(direction) * 1.5 + 1.75 * radius * std::sin(angle));
                        std::println(
                            "SOLID-SPARSE-GAP policy={} separation_r=3.5 direction_degrees={} red={} blue={} clear_delta={}",
                            policy, angle * 180.0 / std::numbers::pi, u32(gap[0]), u32(gap[2]), texelDist_(gap, clear));
                        PPR_TEST_ASSERT(texelDist_(gap, clear) <= 2u);
                    }
                }
            }
        };

        PPR_UNIT_TEST (grid_liquid_surface_connects_and_preserves_element_boundaries) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual blue{};
            blue.m_material_id = 1u;
            blue.m_color_rgba = 0x0000ffffu;
            blue.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;
            auto stage = opticalGridFixture_(blue, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;
            auto red = blue;
            red.m_material_id = 2u;
            red.m_color_rgba = 0xff0000ffu;
            stage.m_terrain_visuals.push_back(red);
            GridTerrainVisual wall{};
            wall.m_material_id = 3u;
            wall.m_color_rgba = 0x909090ffu;
            wall.m_surface_flags = kGridSurfaceParticleSolid;
            stage.m_terrain_visuals.push_back(wall);
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto background = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            const auto retro_background = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f, 128u, nullptr, nullptr, true);

            const double radius = std::sqrt((1.0 / 9.0) / std::numbers::pi);
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->push_back({1u, 21.5 - 1.1 * radius, 21.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            records->push_back({2u, 21.5 + 1.1 * radius, 21.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            stage.m_particle_batches = {GridParticleBatch{0u, records}};
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto connected = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            saveGridQa_(connected, "particle-liquid-connected-native");

            // The midpoint lies outside both physical disks. A visible neck
            // therefore proves reconstruction rather than independent sprites.
            for (const u32 x: {63u, 64u, 65u}) {
                PPR_TEST_ASSERT(gateTexel_(connected, x, 64u)[2] > gateTexel_(background, x, 64u)[2] + 30u);
            }
            PPR_TEST_ASSERT(gateTexel_(connected, 64u, 28u) == gateTexel_(background, 64u, 28u));
            const auto retro = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f, 128u, nullptr, nullptr, true);
            saveGridQa_(retro, "particle-liquid-connected-retro4");

            // Palette quantization may darken blue. Compare the same pixel
            // against the empty scene under the identical picture policy.
            PPR_TEST_ASSERT(texelDist_(gateTexel_(retro, 64u, 64u), gateTexel_(retro_background, 64u, 64u)) > 20u);

            auto distinct = std::make_shared<Array<GridParticleInstance>>(*records);
            (*distinct)[0u].m_position_x_m = 21.5 - 0.9 * radius;
            (*distinct)[1u].m_position_x_m = 21.5 + 0.9 * radius;
            (*distinct)[1u].m_material_id = 2u;
            stage.m_particle_batches.front().m_particles = distinct;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto separated = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            const auto left = gateTexel_(separated, 42u, 64u);
            const auto right = gateTexel_(separated, 86u, 64u);
            PPR_TEST_ASSERT(left[2] > left[0] + 40u and right[0] > right[2] + 40u);

            // These samples lie inside both physical disks at their interface.
            // Foreign-material filtering must not open a background-coloured slit.
            for (const u32 x: {62u, 63u, 64u, 65u}) {
                PPR_TEST_ASSERT(texelDist_(gateTexel_(separated, x, 64u), gateTexel_(background, x, 64u)) > 30u);
            }

            saveGridQa_(separated, "particle-liquid-elements-native");

            auto solid_records = std::make_shared<Array<GridParticleInstance>>();
            solid_records->push_back({3u, 21.5, 21.5, 1.0 / 9.0, 1.0, 300.0, 3u});
            stage.m_particle_batches.front().m_particles = solid_records;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto solid_only = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            auto mixed = std::make_shared<Array<GridParticleInstance>>(*distinct);
            mixed->push_back(solid_records->front());
            stage.m_particle_batches.front().m_particles = std::move(mixed);
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            // A mobile opaque solid occupies the same layer as terrain. Liquid
            // reconstruction beneath it must not paint over its textured face.
            const auto mobile_occluded = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            PPR_TEST_ASSERT(gateTexel_(mobile_occluded, 64u, 64u) == gateTexel_(solid_only, 64u, 64u));
            saveGridQa_(mobile_occluded, "particle-liquid-mobile-solid-occlusion");

            stage.m_particle_batches.front().m_particles = distinct;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            for (const u32 edge: {64u, 128u}) {
                for (u32 frame = 0u; frame < 3u; ++frame) {
                    std::ignore = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f, edge);
                }
            }

            stage.m_uploads.front().m_cell_materials[64u * 128u + 64u] = 3u;
            stage.m_particle_batches.front().m_particles = std::make_shared<const Array<GridParticleInstance>>();
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto terrain = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);

            stage.m_particle_batches.front().m_particles = distinct;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto occluded = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
            PPR_TEST_ASSERT(gateTexel_(occluded, 64u, 64u) == gateTexel_(terrain, 64u, 64u));

            stage.m_particle_batches.front().m_particles = std::make_shared<const Array<GridParticleInstance>>();
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            for (u32 frame = 0u; frame < 3u; ++frame) {
                const auto empty = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f);
                PPR_TEST_ASSERT(changedOpticalPixels_(terrain, empty) == 0u);
            }

            stage.m_uploads.front().m_cell_materials[64u * 128u + 64u] = 0u;
            auto dense = std::make_shared<Array<GridParticleInstance>>();
            dense->reserve(70000u);
            for (u64 id = 1u; id <= 70000u; ++id) {
                // The centre aligns with a half-resolution texel centre in the
                // 64-pixel target, making every kernel contribute its full 1.
                dense->push_back({id, 21.515625, 21.484375, 1.0 / 9.0, 1.0, 300.0, 1u});
            }

            stage.m_particle_batches.front().m_particles = std::move(dense);
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto dense_pixels = drawOpticalGrid_(grid, 21.0f, 21.0f, 1.0f, 64u);
            const auto dense_centre = gateTexel_(dense_pixels, 32u, 32u);
            // The admitted overlap exceeds FP16's finite range. Overflow must
            // not turn the normalized blue surface into a black/NaN result.
            PPR_TEST_ASSERT(dense_centre[2] > 150u and dense_centre[2] > dense_centre[0] + 80u);
            saveGridQa_(dense_pixels, "particle-liquid-dense-overlap");
        };

        PPR_UNIT_TEST (grid_liquid_optics_and_viewport_cost) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            GridPass no_optics;
            GridPass rough_control;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not no_optics.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not rough_control.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not rough_control.shutdown());
                PPR_TEST_ASSERT(not no_optics.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            // The shipped water material's authored optical appearance, against
            // an owned patterned backdrop that exposes refracted displacement.
            GridTerrainVisual water{};
            water.m_material_id = 1u;
            water.m_color_rgba = 0x3989c970u;
            water.m_texture_period_m = 2.0f;
            water.m_roughness = 0.12f;
            water.m_ior = 1.333f;
            water.m_normal_scale = 0.8f;
            water.m_refraction_depth_m = 0.2f;
            water.m_emissive_strength = 1.0f;
            water.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;
            auto stage = opticalGridFixture_(water, 0u);
            stage.m_cell_size_m = 1.0f / 3.0f;
            for (auto &layer: stage.m_background_layers) {
                layer.m_repeat_period_m = 2.0f;
            }

            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto background = drawOpticalGrid_(grid, 19.5f, 20.0f, 4.0f, 512u);

            auto records = std::make_shared<Array<GridParticleInstance>>();
            constexpr u32 columns = 12u;
            constexpr u32 rows = 6u;
            records->reserve(columns * rows);
            for (u32 y = 0u; y < rows; ++y) {
                for (u32 x = 0u; x < columns; ++x) {
                    records->push_back(
                        {u64(y * columns + x + 1u), 20.4 + 0.2 * x, 20.8 + 0.2 * y, 1.0 / 9.0, 1.0, 300.0, 1u});
                }
            }

            stage.m_particle_batches = {GridParticleBatch{0u, std::move(records)}};
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const auto optical = drawOpticalGrid_(grid, 19.5f, 20.0f, 4.0f, 512u);
            saveGridQa_(optical, "particle-water-optical-native");
            saveGridQa_(optical, "particle-water-glossy-native");
            PPR_TEST_ASSERT(changedOpticalPixels_(background, optical) > 100u);
            PPR_TEST_ASSERT(grid.observations().m_particle_instances == columns * rows);

            auto unshifted = stage;
            // Preserve Fresnel and highlight parameters; only backdrop
            // displacement changes in this refraction control.
            unshifted.m_terrain_visuals.front().m_refraction_depth_m = 0.0f;
            PPR_TEST_ASSERT(not no_optics.applyStagedFrame(unshifted));

            const auto flat = drawOpticalGrid_(no_optics, 19.5f, 20.0f, 4.0f, 512u);
            saveGridQa_(flat, "particle-water-refraction-off-native");
            PPR_TEST_ASSERT(changedOpticalPixels_(flat, optical) > 0u);

            auto rough = stage;
            rough.m_terrain_visuals.front().m_roughness = 1.0f;
            PPR_TEST_ASSERT(not rough_control.applyStagedFrame(rough));

            const auto matte = drawOpticalGrid_(rough_control, 19.5f, 20.0f, 4.0f, 512u);
            saveGridQa_(matte, "particle-water-roughness-1-native");
            PPR_TEST_ASSERT(changedOpticalPixels_(matte, optical) > 0u);

            constexpr u32 warm_frames = 3u;
            constexpr u32 sample_frames = 5u;
            std::array<float, sample_frames> samples{};
            for (u32 frame = 0u; frame < warm_frames + sample_frames; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 19.5f, 20.0f, 4.0f, 1920u, nullptr, nullptr, false, 1080u);
                const auto submitted = renderer->lastSubmittedFenceValue();
                const auto timing = renderer->lastGpuFrameTiming();
                PPR_TEST_ASSERT(timing.has_value() and timing->m_submission == submitted);
                PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
                if (frame >= warm_frames) {
                    samples[frame - warm_frames] = timing->m_milliseconds;
                }
            }

            std::ranges::sort(samples);
            // ID, depth and two scalar fields use 16 bytes per half-resolution
            // pixel. Three retained slots at this viewport total 23.73 MiB.
            constexpr u64 field_bytes = 16ull * 960u * 540u * 3u;
            std::println(
                "LIQUID-COST particles={} viewport=1920x1080 target_edge=1920 warm_frames={} samples={} "
                "field_bytes={} gpu_ms_min={} median={} max={}",
                columns * rows, warm_frames, sample_frames, field_bytes, samples.front(), samples[sample_frames / 2u],
                samples.back());
        };

        PPR_UNIT_TEST (grid_liquid_nominal_density_deformation_coverage) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            GridPass grid;
            RetroPass picture;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not picture.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual blue{};
            blue.m_material_id = 1u;
            blue.m_color_rgba = 0x0000ffffu;
            blue.m_surface_flags = kGridSurfaceParticleLiquid;
            auto stage = opticalGridFixture_(blue, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;

            constexpr double spacing = 1.0 / 3.0;
            constexpr double volume = spacing * spacing;
            const double radius = std::sqrt(volume / std::numbers::pi);
            constexpr double cavity_radius = 2.0;
            constexpr double cavity_centre = 21.5;
            constexpr double min_x = 13.5;
            constexpr double min_y = 21.5 - 16.0 / 3.0;
            constexpr double max_x = min_x + 48.0 / 3.0;
            constexpr double max_y = min_y + 32.0 / 3.0;
            const double cavity_cut_radius = cavity_radius + 3.0 * radius;
            // The extra radius also excludes the discretized particle edge;
            // bulk checks remain at least five radii from the cavity surface.
            const double cavity_exclusion_radius = cavity_cut_radius + 6.0 * radius;
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->reserve(48u * 32u + 3u);
            for (u32 y = 0u; y < 32u; ++y) {
                for (u32 x = 0u; x < 48u; ++x) {
                    // This procedural stressed counterexample is not a replay:
                    // a frozen 2x2 block expands by only 0.25 spacing per axis.
                    const double px = min_x + (double(x) + 0.5) * spacing + ((x & 1u) == 0u ? -0.25 : 0.25) * spacing;
                    const double py = min_y + (double(y) + 0.5) * spacing + ((y & 1u) == 0u ? -0.25 : 0.25) * spacing;
                    const double dx = px - cavity_centre;
                    const double dy = py - cavity_centre;
                    if (dx * dx + dy * dy < cavity_cut_radius * cavity_cut_radius) {
                        continue;
                    }

                    records->push_back({u64(records->size() + 1u), px, py, volume, 1.0, 300.0, 1u});
                }
            }

            // An isolated disk and a pair separated by 4.5 physical radii must
            // retain empty space; filling bulk interstices cannot fill all gaps.
            records->push_back({u64(records->size() + 1u), 12.5, 13.5, volume, 1.0, 300.0, 1u});
            records->push_back({u64(records->size() + 1u), 15.5, 13.5, volume, 1.0, 300.0, 1u});
            records->push_back({u64(records->size() + 1u), 15.5 + 4.5 * radius, 13.5, volume, 1.0, 300.0, 1u});
            const u32 particle_count = safe_narrowing(records->size());
            stage.m_particle_batches = {GridParticleBatch{0u, std::move(records)}};
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            std::array<u32, 2u> holes{}, interior_samples{}, cavity_painted{}, cavity_samples{};
            std::array<u32, 2u> isolated_blue{}, isolated_gap_blue{}, left_blue{}, right_blue{}, pair_gap_blue{};
            std::array<u32, 2u> isolated_gap_delta{}, pair_gap_delta{};
            const auto sample = [](const GatePixels &pixels, const double x, const double y) {
                const u32 px = static_cast<u32>((x - 11.5) * 512.0 / 20.0);
                const u32 py = static_cast<u32>((31.5 - y) * 512.0 / 20.0);
                return gateTexel_(pixels, px, py);
            };

            for (const bool temporal: {false, true}) {
                const u32 mode = temporal ? 1u : 0u;
                const RetroSettings settings{
                    .m_enabled = false,
                    .m_palette_enabled = false,
                    .m_temporal_enabled = temporal,
                    .m_pixel_scale = 4u};
                GatePixels pixels{};
                for (u32 frame = 0u; frame < 4u; ++frame) {
                    pixels = drawOpticalGrid_(
                        grid, 11.5f, 11.5f, 20.0f, 512u, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture);
                    PPR_TEST_ASSERT(picture.temporalActive() == temporal);
                }

                saveGridQa_(
                    pixels, temporal ? "particle-liquid-nominal-deformation-temporal"
                                     : "particle-liquid-nominal-deformation-native");

                const auto empty_color = gateTexel_(pixels, 0u, 0u);
                for (u32 y = 0u; y < 512u; ++y) {
                    for (u32 x = 0u; x < 512u; ++x) {
                        const double px = 11.5 + (double(x) + 0.5) * 20.0 / 512.0;
                        const double py = 31.5 - (double(y) + 0.5) * 20.0 / 512.0;
                        const double dx = px - cavity_centre;
                        const double dy = py - cavity_centre;
                        const double distance_squared = dx * dx + dy * dy;
                        const auto pixel = gateTexel_(pixels, x, y);
                        if (distance_squared < 1.5 * 1.5) {
                            ++cavity_samples[mode];
                            cavity_painted[mode] += texelDist_(pixel, empty_color) > 2u ? 1u : 0u;
                        }

                        if (px <= min_x + 5.0 * radius or
                            px >= max_x - 5.0 * radius or
                            py <= min_y + 5.0 * radius or
                            py >= max_y - 5.0 * radius or
                            distance_squared <= cavity_exclusion_radius * cavity_exclusion_radius) {
                            continue;
                        }

                        ++interior_samples[mode];
                        holes[mode] += pixel[2] < 200u ? 1u : 0u;
                    }
                }

                isolated_blue[mode] = sample(pixels, 12.5, 13.5)[2];
                isolated_gap_blue[mode] = sample(pixels, 12.5 + 2.5 * radius, 13.5)[2];
                isolated_gap_delta[mode] = texelDist_(sample(pixels, 12.5 + 2.5 * radius, 13.5), empty_color);
                left_blue[mode] = sample(pixels, 15.5, 13.5)[2];
                right_blue[mode] = sample(pixels, 15.5 + 4.5 * radius, 13.5)[2];
                pair_gap_blue[mode] = sample(pixels, 15.5 + 2.25 * radius, 13.5)[2];
                pair_gap_delta[mode] = texelDist_(sample(pixels, 15.5 + 2.25 * radius, 13.5), empty_color);

                std::println(
                    "LIQUID-COVERAGE fixture=frozen-outward-2x2-not-replay nominal_fill=1 spacing_m={} "
                    "jitter_fraction=0.25 particles={} temporal={} blue_threshold=200 interior_samples={} "
                    "holes={} cavity_radius_m={} cavity_checked_radius_m=1.5 cavity_exclusion_radius_m={} "
                    "cavity_samples={} cavity_painted={} isolated_blue={} isolated_gap_blue={} "
                    "isolated_gap_delta={} left_blue={} right_blue={} pair_gap_blue={} pair_gap_delta={}",
                    spacing, particle_count, temporal, interior_samples[mode], holes[mode], cavity_radius,
                    cavity_exclusion_radius, cavity_samples[mode], cavity_painted[mode], isolated_blue[mode],
                    isolated_gap_blue[mode], isolated_gap_delta[mode], left_blue[mode], right_blue[mode],
                    pair_gap_blue[mode], pair_gap_delta[mode]);
            }

            // Both modes emit their complete metrics and QA before a baseline
            // failure, so this diagnostic never hides the second observation.
            for (u32 mode = 0u; mode < 2u; ++mode) {
                PPR_TEST_ASSERT(interior_samples[mode] > 0u and cavity_samples[mode] > 0u);
                PPR_TEST_ASSERT(isolated_blue[mode] >= 200u and left_blue[mode] >= 200u and right_blue[mode] >= 200u);
                PPR_TEST_ASSERT(isolated_gap_delta[mode] <= 2u and pair_gap_delta[mode] <= 2u);
                PPR_TEST_ASSERT(cavity_painted[mode] == 0u);
                PPR_TEST_ASSERT(holes[mode] == 0u);
            }
        };

        PPR_UNIT_TEST (grid_liquid_full_resolution_temporal_cost) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            const int2 native_extent{3445, 1837};
            const auto target = makeRenderTarget_(device, 3445u, 1837u);
            PPR_TEST_ASSERT(target.has_value());
            GridPass grid;
            RetroPass picture;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                const auto submitted = renderer->lastSubmittedFenceValue();
                if (submitted != kFenceValueNone) {
                    PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                }

                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                PPR_TEST_ASSERT(not picture.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            const char *const phase_setting = std::getenv("PPR_PARTICLE_PHASE_TIMINGS");
            PPR_TEST_ASSERT(
                phase_setting == nullptr or
                std::string_view{phase_setting} == "0" or
                std::string_view{phase_setting} == "1");
            const bool instrumented = phase_setting != nullptr and std::string_view{phase_setting} == "1";
            rhi::ComPtr<rhi::IQueryPool> phase_queries;
            const double frequency = static_cast<double>(device.getInfo().timestampFrequency);
            PPR_TEST_ASSERT(std::isfinite(frequency) and frequency > 0.0);
            if (instrumented) {
                rhi::QueryPoolDesc desc{};
                desc.type = rhi::QueryType::Timestamp;
                desc.count = 12u;
                desc.label = "test-only particle phase attribution";
                PPR_TEST_ASSERT(not make_error_code(device.createQueryPool(desc, phase_queries.writeRef())));

                PPR_TEST_ASSERT(
                    not grid.setParticleTimingQueriesForTest(phase_queries, renderer->completedFenceValue()));
            }

            const char *const workload_setting = std::getenv("PPR_PARTICLE_BENCHMARK_WORKLOAD");
            const std::string_view workload = workload_setting != nullptr ? workload_setting : "liquid";
            PPR_TEST_ASSERT(
                workload == "liquid" or workload == "solid" or workload == "mixed" or workload == "solid_materials");
            PPR_TEST_ASSERT(not instrumented or (workload != "solid" and workload != "solid_materials"));
            const char *const gas_setting = std::getenv("PPR_PARTICLE_BENCHMARK_GAS");
            const std::string_view gas_value = gas_setting != nullptr ? gas_setting : "0";
            PPR_TEST_ASSERT(gas_value == "0" or gas_value == "1");
            const bool gas_present = gas_value == "1";

            const char *const ppc_setting = std::getenv("PPR_PARTICLE_BENCHMARK_SOLID_PPC");
            const std::string_view ppc_value = ppc_setting != nullptr ? ppc_setting : "1";
            PPR_TEST_ASSERT(ppc_value == "1" or ppc_value == "4");
            const u32 solid_ppc = ppc_value == "4" ? 4u : 1u;
            constexpr u32 physical_parent_cells = 96u * 48u;
            const u32 solid_parent_cells = workload == "mixed" ? physical_parent_cells / 2u
                                                               : (workload == "solid" or workload == "solid_materials"
                                                                         ? physical_parent_cells
                                                                         : 0u);
            const u32 particle_count = physical_parent_cells + solid_parent_cells * (solid_ppc - 1u) + u32(gas_present);

            GridTerrainVisual water{};
            water.m_material_id = 1u;
            water.m_color_rgba = 0x3989c970u;
            water.m_roughness = 0.12f;
            water.m_ior = 1.333f;
            water.m_refraction_depth_m = 0.2f;
            water.m_surface_flags = kGridSurfaceParticleLiquid | kGridSurfaceOpticalBlend;

            GridTerrainVisual solid = water;
            solid.m_material_id = 2u;
            solid.m_color_rgba = 0xb39164ffu;
            solid.m_ior = 1.0f;
            solid.m_roughness = 0.8f;
            solid.m_refraction_depth_m = 0.0f;
            solid.m_texture_index = 0u;
            solid.m_texture_period_m = 1.0f;
            solid.m_surface_flags = kGridSurfaceParticleSolid | kGridSurfaceTextureRepeat;
            if (workload == "solid" or workload == "solid_materials") {
                water = solid;
                water.m_material_id = 1u;
            }

            if (workload == "solid_materials") {
                std::println(
                    "PARTICLE-MATERIAL-WORKLOAD workload=solid_materials checker_block_grains=4 texture_index=0 matched_frozen_positions=true");
            }

            auto stage = opticalGridFixture_(water, 0u);
            stage.m_has_catalog = true;
            if (workload == "mixed" or workload == "solid_materials") {
                if (workload == "solid_materials") {
                    solid.m_color_rgba = 0x648fb3ffu;
                }
                stage.m_terrain_visuals.push_back(solid);
            }
            if (gas_present) {
                GridTerrainVisual gas{};
                gas.m_material_id = 3u;
                gas.m_color_rgba = 0xffffff80u;
                gas.m_surface_flags = kGridSurfaceOpticalBlend;
                stage.m_terrain_visuals.push_back(gas);
            }

            stage.m_cell_size_m = 1.0f / 3.0f;
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->reserve(particle_count);
            for (u32 y = 0u; y < 48u; ++y) {
                for (u32 x = 0u; x < 96u; ++x) {
                    const double px = 5.0 + (double(x) + 0.5 + ((x & 1u) == 0u ? -0.25 : 0.25)) / 3.0;
                    const double py = 12.0 + (double(y) + 0.5 + ((y & 1u) == 0u ? -0.25 : 0.25)) / 3.0;
                    const bool second_solid = workload == "solid_materials" and ((x / 4u + y / 4u) & 1u) != 0u;
                    const u32 material = (workload == "mixed" and x >= 48u) or second_solid ? 2u : 1u;
                    const bool solid_parent = workload == "solid" or
                                              workload == "solid_materials" or
                                              (workload == "mixed" and x >= 48u);
                    const u32 samples_per_parent = solid_parent ? solid_ppc : 1u;
                    for (u32 sample = 0u; sample < samples_per_parent; ++sample) {
                        const double offset_x = samples_per_parent == 1u ? 0.0
                                                                         : ((sample & 1u) != 0u ? 0.25 : -0.25) *
                                                                               static_cast<double>(stage.m_cell_size_m);
                        const double offset_y = samples_per_parent == 1u ? 0.0
                                                                         : ((sample & 2u) != 0u ? 0.25 : -0.25) *
                                                                               static_cast<double>(stage.m_cell_size_m);
                        records->push_back(
                            {static_cast<u64>(records->size()) + 1u, px + offset_x, py + offset_y,
                                (1.0 / 9.0) / samples_per_parent, 1.0 / samples_per_parent, 300.0, material});
                    }
                }
            }

            if (gas_present) {
                // Admitted inventory contains gas even when the 32m camera
                // cannot see it; the wide view contains this one extra grain.
                records->push_back({static_cast<u64>(records->size()) + 1u, 40.0, 40.0, 1.0 / 9.0, 1.0, 300.0, 3u});
            }

            PPR_TEST_ASSERT(records->size() == particle_count);

            // This fixture's seed adapter packs every admitted row, including
            // its optional gas row, into hot64/cold24 and metadata32 protocols.
            // Report a row-byte model, not device allocation/residency totals.
            std::println(
                "PARTICLE-FULLRES-INVENTORY workload={} gas_present={} physical_parent_cells={} solid_parent_cells={} "
                "solid_ppc={} actual_sample_count={} metadata_rows={} numerical_rows={} readonly_rows={} packed_rows={} "
                "protocol_byte_model=true metadata_bytes={} numerical_bytes={} readonly_bytes={} packed_bytes={}",
                workload, gas_present, physical_parent_cells, solid_parent_cells, solid_ppc, particle_count,
                particle_count, particle_count, particle_count, particle_count,
                u64(particle_count) * sizeof(GridParticleMetadataRow),
                u64(particle_count) * kGridParticleNumericalStride, u64(particle_count) * kGridParticleReadonlyStride,
                u64(particle_count) * 32u);

            stage.m_particle_batches = {GridParticleBatch{0u, std::move(records)}};
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            for (const float width_m: {32.0f, 128.0f}) {
                const char *const occupancy = width_m == 32.0f ? "dense-pool" : "wide-sparse";
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_ortho_scale = width_m / float(native_extent.x);
                const float height_m = model.m_ortho_scale * float(native_extent.y);
                // Preserve the existing 32m camera exactly. The wide view centres
                // on the unchanged pool, exposing empty viewport work around it.
                const float origin_x = width_m == 32.0f ? 5.0f : 21.0f - width_m * 0.5f;
                const float origin_y = width_m == 32.0f ? 12.0f : 20.0f - height_m * 0.5f;
                model.m_origin = float3{origin_x, origin_y, -0.5f};
                Camera camera{ECameraProjection::orthographic};
                camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, native_extent.x, native_extent.y}});
                const float3 centre{origin_x + width_m * 0.5f, origin_y + height_m * 0.5f, -0.5f};
                model.m_origin = centre - camera.getRight() * (width_m * 0.5f) - camera.getUp() * (height_m * 0.5f);
                camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, native_extent.x, native_extent.y}});
                auto snapshot = camera.getSnapshot();
                snapshot.m_has_camera_cut = false;
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                for (;;) {
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                    const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                    PPR_TEST_ASSERT(prepared.has_value());
                    if (*prepared) {
                        break;
                    }

                    const auto submitted = renderer->lastSubmittedFenceValue();
                    if (submitted != kFenceValueNone) {
                        PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                    }
                }

                const std::array<RetroSettings, 3u> policies{
                    RetroSettings{
                        .m_enabled = false,
                        .m_palette_enabled = false,
                        .m_temporal_enabled = false,
                        .m_pixel_scale = 8u},
                    RetroSettings{
                        .m_enabled = false,
                        .m_palette_enabled = false,
                        .m_temporal_enabled = true,
                        .m_pixel_scale = 8u},
                    RetroSettings{
                        .m_enabled = true,
                        .m_palette_enabled = false,
                        .m_temporal_enabled = false,
                        .m_pixel_scale = 8u}};
                constexpr u32 warm_frames = 30u;
                constexpr u32 sample_frames = 15u;
                for (const auto &settings: policies) {
                    const auto scene_extent = settings.m_enabled ? RetroPass::sceneExtent(native_extent, 8u)
                                                                 : native_extent;
                    const auto viewport = rhi::Viewport::fromSize(float(scene_extent.x), float(scene_extent.y));
                    const auto scissor = rhi::ScissorRect::fromSize(u32(scene_extent.x), u32(scene_extent.y));
                    const bool offscreen = settings.m_enabled or settings.m_temporal_enabled;
                    std::array<float, sample_frames> samples{};
                    std::array<double, sample_frames> cpu_samples{};
                    const auto cpu_clock = std::chrono::steady_clock::now;
                    for (u32 frame = 0u; frame < warm_frames + sample_frames; ++frame) {
                        if (instrumented) {
                            // The previous exact submit fence and retirement below
                            // completed before this pool is reset for another frame.
                            PPR_TEST_ASSERT(not make_error_code(phase_queries->reset(0u, 12u)));
                        }

                        const auto raster = picture.beginFrame(
                            settings, snapshot, scene_extent, grid.worldGeneration());
                        PPR_TEST_ASSERT(picture.temporalActive() == settings.m_temporal_enabled);
                        PPR_TEST_ASSERT(not grid.update(TimeSpan{}, raster));

                        const auto draw_content = [&](const DrawContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.renderBackground(context));
                            PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.render(context));
                            return grid.renderParticles(context);
                        };

                        const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                            PPR_RETURN_ERROR_ON_FAIL(
                                GridMaterialGate, grid.prepareBackground(context, snapshot, viewport, scissor));
                            PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.prepareGpuParticles(context));
                            if (offscreen) {
                                return picture.prepareScene(
                                    context, RetroPass::scenePipelineSignature(settings.m_temporal_enabled),
                                    scene_extent, DrawCallback{draw_content});
                            }

                            return {};
                        };

                        const auto present = [&](const DrawContext &context) {
                            return offscreen ? picture.render(context) : draw_content(context);
                        };

                        DrawSubmission submission{"nominal-density full-resolution liquid cost", present};
                        if (not offscreen) {
                            submission.m_viewport = viewport;
                            submission.m_scissor = scissor;
                        }

                        // Target, camera, pool, picture history and pass resources
                        // are retained. No CPU pixel readback occurs in this loop.
                        const auto cpu_begin = cpu_clock();
                        PPR_TEST_ASSERT(not renderer->renderToTexture(
                            targetRef_(target), {submission}, {}, PreRenderCallback{prepare}));
                        const auto cpu_end = cpu_clock();
                        const double cpu_ms = std::chrono::duration<double, std::milli>(cpu_end - cpu_begin).count();
                        PPR_TEST_ASSERT(std::isfinite(cpu_ms) and cpu_ms >= 0.0);
                        const auto submitted = renderer->lastSubmittedFenceValue();
                        grid.notifyFrameSubmitted(submitted);
                        if (offscreen) {
                            picture.notifyFrameSubmitted(submitted);
                        }

                        PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                        PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                        PPR_TEST_ASSERT(renderer->completedFenceValue() >= submitted);
                        const auto timing = renderer->lastGpuFrameTiming();
                        PPR_TEST_ASSERT(timing.has_value() and timing->m_submission == submitted);
                        PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
                        if (frame >= warm_frames) {
                            samples[frame - warm_frames] = timing->m_milliseconds;
                            cpu_samples[frame - warm_frames] = cpu_ms;
                        }

                        if (instrumented) {
                            rhi::QueryResultState state{};
                            PPR_TEST_ASSERT(not make_error_code(phase_queries->getResultState(0u, 12u, &state)));
                            PPR_TEST_ASSERT(state == rhi::QueryResultState::Resolved);

                            std::array<u64, 12u> ticks{};
                            PPR_TEST_ASSERT(not make_error_code(phase_queries->getResult(0u, 12u, ticks.data())));

                            std::array<double, 6u> phase_ms{};
                            for (u32 phase = 0u; phase < phase_ms.size(); ++phase) {
                                PPR_TEST_ASSERT(ticks[phase * 2u + 1u] >= ticks[phase * 2u]);
                                phase_ms[phase] = double(ticks[phase * 2u + 1u] - ticks[phase * 2u]) *
                                                  1000.0 /
                                                  frequency;
                            }

                            std::println(
                                "PARTICLE-PHASE workload={} gas_present={} particles={} physical_parent_cells={} actual_sample_count={} solid_ppc={} fov_width_m={} occupancy={} pixelized={} temporal={} frame={} measured={} submission={} "
                                "pack_cull_ms={} element_ms={} density_ms={} filter_ms={} composite_ms={} ordinary_ms={}",
                                workload, gas_present, particle_count, physical_parent_cells, particle_count, solid_ppc,
                                width_m, occupancy, settings.m_enabled, settings.m_temporal_enabled, frame,
                                frame >= warm_frames, submitted, phase_ms[0u], phase_ms[1u], phase_ms[2u], phase_ms[3u],
                                phase_ms[4u], phase_ms[5u]);
                        }
                    }

                    PPR_TEST_ASSERT(grid.observations().m_particle_instances == particle_count);

                    const auto unsorted = samples;
                    const auto cpu_unsorted = cpu_samples;
                    std::ranges::sort(samples);
                    std::ranges::sort(cpu_samples);

                    // Synchronous encode+submit only: excludes the explicit GPU
                    // fence wait/retirement below each submit, display and scanout.
                    std::println(
                        "PARTICLE-FULLRES-CPU catalog_contours=true workload={} gas_present={} particles={} physical_parent_cells={} actual_sample_count={} solid_ppc={} fov_width_m={} occupancy={} pixelized={} temporal={} "
                        "warm_frames={} samples={} encode_submit_ms_min={} median={} max={} "
                        "sample_ms=[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}]",
                        workload, gas_present, particle_count, physical_parent_cells, particle_count, solid_ppc,
                        width_m, occupancy, settings.m_enabled, settings.m_temporal_enabled, warm_frames, sample_frames,
                        cpu_samples.front(), cpu_samples[sample_frames / 2u], cpu_samples.back(), cpu_unsorted[0u],
                        cpu_unsorted[1u], cpu_unsorted[2u], cpu_unsorted[3u], cpu_unsorted[4u], cpu_unsorted[5u],
                        cpu_unsorted[6u], cpu_unsorted[7u], cpu_unsorted[8u], cpu_unsorted[9u], cpu_unsorted[10u],
                        cpu_unsorted[11u], cpu_unsorted[12u], cpu_unsorted[13u], cpu_unsorted[14u]);

                    std::println(
                        "PARTICLE-FULLRES-COST catalog_contours=true workload={} gas_present={} fixture=frozen-outward-2x2-not-replay nominal_fill=1 particles={} physical_parent_cells={} actual_sample_count={} solid_ppc={} "
                        "fov_width_m={} occupancy={} nominal_pool_fraction={} target=3445x1837 scene={}x{} "
                        "pixelized={} palette=false temporal={} "
                        "instrumented={} warm_frames={} samples={} gpu_frame_ms_min={} median={} max={} "
                        "sample_ms=[{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}]",
                        workload, gas_present, particle_count, physical_parent_cells, particle_count, solid_ppc,
                        width_m, occupancy, 512.0 / (double(width_m) * double(height_m)), scene_extent.x,
                        scene_extent.y, settings.m_enabled, settings.m_temporal_enabled, instrumented, warm_frames,
                        sample_frames, samples.front(), samples[sample_frames / 2u], samples.back(), unsorted[0u],
                        unsorted[1u], unsorted[2u], unsorted[3u], unsorted[4u], unsorted[5u], unsorted[6u],
                        unsorted[7u], unsorted[8u], unsorted[9u], unsorted[10u], unsorted[11u], unsorted[12u],
                        unsorted[13u], unsorted[14u]);
                }
            }
        };

        [[nodiscard]] GridPass::GridStagedFrame slantedSolidInterfaceFixture_() {
            GridTerrainVisual blue{};
            blue.m_material_id = 1u;
            blue.m_color_rgba = 0x0000ffffu;
            blue.m_ior = 1.0f;
            blue.m_surface_flags = kGridSurfaceParticleSolid;
            auto stage = opticalGridFixture_(blue, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;
            auto red = blue;
            red.m_material_id = 2u;
            red.m_color_rgba = 0xff0000ffu;
            stage.m_terrain_visuals.push_back(red);
            auto rows = std::make_shared<Array<GridParticleInstance>>();
            for (u32 y = 0u; y < 9u; ++y) {
                for (u32 x = 0u; x < 9u; ++x) {
                    const double px = 21.5 + (double(x) + 0.5) / 3.0;
                    const double py = 21.5 + (double(y) + 0.5) / 3.0;
                    const u32 material = px < 23.0 + 0.37 * (py - 23.0) ? 1u : 2u;
                    rows->push_back({u64(y * 9u + x + 1u), px, py, 1.0 / 9.0, 1.0, 300.0, material});
                }
            }

            const double radius = std::sqrt((1.0 / 9.0) / std::numbers::pi);
            rows->push_back({82u, 20.5, 20.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            rows->push_back({83u, 20.5 + 4.5 * radius, 20.5, 1.0 / 9.0, 1.0, 300.0, 1u});
            stage.m_particle_batches = {GridParticleBatch{0u, std::move(rows)}};
            return stage;
        }

        void checkSlantedSolidInterfacePixels_(
            const GatePixels &pixels, const u32 offset_x, const u32 offset_y, const u32 extent_x, const u32 extent_y,
            const double origin_x, const double origin_y, const double width_m, const bool temporal) {
            const double metres_per_pixel = width_m / double(extent_x);
            const double top = origin_y + double(extent_y) * metres_per_pixel;
            const auto sample = [&](const double x, const double y) {
                return gateTexel_(
                    pixels, offset_x + u32((x - origin_x) / metres_per_pixel),
                    offset_y + u32((top - y) / metres_per_pixel));
            };
            u32 intermediate = 0u, holes = 0u, pure_samples = 0u, impure_far = 0u, max_row_mixed = 0u;
            u32 mixed_without_both_pure_neighbors = 0u;
            for (u32 y = 0u; y < extent_y; ++y) {
                const double py = top - (double(y) + 0.5) * metres_per_pixel;
                if (py <= 21.9 or py >= 24.1) {
                    continue;
                }

                u32 row_mixed = 0u;
                for (u32 x = 0u; x < extent_x; ++x) {
                    const double px = origin_x + (double(x) + 0.5) * metres_per_pixel;
                    if (px <= 21.9 or px >= 24.1) {
                        continue;
                    }

                    const auto pixel = gateTexel_(pixels, offset_x + x, offset_y + y);
                    // Red/blue mixing preserves their combined contribution;
                    // a valid half mix need not retain either pure channel peak.
                    holes += u32(pixel[0]) + u32(pixel[2]) < 200u ? 1u : 0u;
                    if (pixel[0] > 30u and pixel[2] > 30u) {
                        ++row_mixed;
                        bool pure_blue = false, pure_red = false;
                        for (int dy = -6; dy <= 6; ++dy) {
                            for (int dx = -6; dx <= 6; ++dx) {
                                const int sx = int(x) + dx;
                                const int sy = int(y) + dy;
                                if (dx * dx + dy * dy > 36 or
                                    sx < 0 or
                                    sy < 0 or
                                    sx >= int(extent_x) or
                                    sy >= int(extent_y)) {
                                    continue;
                                }

                                const auto neighbor = gateTexel_(pixels, offset_x + u32(sx), offset_y + u32(sy));
                                pure_blue = pure_blue or (neighbor[2] >= 200u and neighbor[0] < 30u);
                                pure_red = pure_red or (neighbor[0] >= 200u and neighbor[2] < 30u);
                            }
                        }

                        mixed_without_both_pure_neighbors += pure_blue and pure_red ? 0u : 1u;
                    }


                    const double side = px - (23.0 + 0.37 * (py - 23.0));
                    if (std::abs(side) > 0.5) {
                        ++pure_samples;
                        impure_far += side < 0.0 ? (pixel[2] < 200u or pixel[0] >= 30u ? 1u : 0u)
                                                 : (pixel[0] < 200u or pixel[2] >= 30u ? 1u : 0u);
                    }
                }

                intermediate += row_mixed;
                max_row_mixed = std::max(max_row_mixed, row_mixed);
            }

            const double radius = std::sqrt((1.0 / 9.0) / std::numbers::pi);
            const auto clear = sample(25.8, 25.8);
            const u32 gap_delta = texelDist_(sample(20.5 + 2.25 * radius, 20.5), clear);
            std::println(
                "SOLID-INTERFACE temporal={} viewport={}x{} offset={},{} intermediate={} max_mixed_pixels_per_row={} holes={} pure_samples={} impure_far={} sparse_gap_delta={} pure_neighbor_radius_px=6 mixed_without_both_pure_neighbors={}",
                temporal, extent_x, extent_y, offset_x, offset_y, intermediate, max_row_mixed, holes, pure_samples,
                impure_far, gap_delta, mixed_without_both_pure_neighbors);
            PPR_TEST_ASSERT(intermediate > 0u and pure_samples > 0u);
            // A stair segment can be long along a horizontal row while its
            // material band remains thin. Bound thickness in two dimensions:
            // both pure sides must lie within a symmetric six-pixel radius,
            // preserving the twelve-pixel diameter contract at any orientation.
            PPR_TEST_ASSERT(mixed_without_both_pure_neighbors == 0u);
            PPR_TEST_ASSERT(holes == 0u and impure_far == 0u);
            PPR_TEST_ASSERT(sample(20.5, 20.5)[2] >= 200u and gap_delta <= 2u);
        }

        enum class EParticleFeatureFixture {
            isolated,
            slanted_interface,
        };

        void checkFivePixelParticleFeatures_(
            const u32 phase_flag, const EParticleFeatureFixture fixture = EParticleFeatureFixture::isolated) {
            const bool slanted_interface = fixture == EParticleFeatureFixture::slanted_interface;
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            const auto target = makeRenderTarget_(device, 3445u, 1837u);
            PPR_TEST_ASSERT(target.has_value());
            GridPass grid;
            RetroPass picture;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                const auto submitted = renderer->lastSubmittedFenceValue();
                if (submitted != kFenceValueNone) {
                    PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                }

                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                PPR_TEST_ASSERT(not picture.shutdown());
                PPR_TEST_ASSERT(not grid.shutdown());
            };

            GridTerrainVisual blue{};
            blue.m_material_id = 1u;
            blue.m_color_rgba = 0x0000ffffu;
            blue.m_surface_flags = phase_flag;
            auto stage = opticalGridFixture_(blue, 0u);
            stage.m_has_background = false;
            stage.m_cell_size_m = 1.0f / 3.0f;
            auto red = blue;
            red.m_material_id = 2u;
            red.m_color_rgba = 0xff0000ffu;
            stage.m_terrain_visuals.push_back(red);
            const double radius = std::sqrt((1.0 / 9.0) / std::numbers::pi);
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->push_back({1u, 8.0, 20.0, 1.0 / 9.0, 1.0, 300.0, 1u});
            records->push_back({2u, 18.0, 20.0, 1.0 / 9.0, 1.0, 300.0, 1u});
            records->push_back({3u, 18.0 + 4.5 * radius, 20.0, 1.0 / 9.0, 1.0, 300.0, 1u});
            records->push_back({4u, 30.0, 20.0, 1.0 / 9.0, 1.0, 300.0, 1u});
            records->push_back({5u, 30.0 + 1.8 * radius, 20.0, 1.0 / 9.0, 1.0, 300.0, 2u});
            stage.m_particle_batches = {GridParticleBatch{0u, std::move(records)}};
            if (slanted_interface) {
                stage = slantedSolidInterfaceFixture_();
            }

            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));

            const int2 scene_extent{3431, 1815};
            const rhi::Viewport output_viewport{7.0f, 11.0f, 3431.0f, 1815.0f};
            const rhi::ScissorRect output_scissor{7u, 11u, 3438u, 1826u};
            constexpr float width_m = 128.0f;
            const float height_m = width_m * float(scene_extent.y) / float(scene_extent.x);
            const float origin_x = 21.5f - width_m * 0.5f;
            const float origin_y = 20.0f - height_m * 0.5f;
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{origin_x, origin_y, -0.5f};
            model.m_ortho_scale = width_m / float(scene_extent.x);
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, scene_extent}});
            auto snapshot = camera.getSnapshot();
            snapshot.m_has_camera_cut = false;
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
            for (;;) {
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                PPR_TEST_ASSERT(prepared.has_value());
                if (*prepared) {
                    break;
                }

                const auto submitted = renderer->lastSubmittedFenceValue();
                if (submitted != kFenceValueNone) {
                    PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                }
            }

            const auto sample_x = [&](const double world_x) {
                return 7u + static_cast<u32>((world_x - double(origin_x)) * double(scene_extent.x) / double(width_m));
            };
            const u32 sample_y = 11u + static_cast<u32>(
                                           (double(origin_y) + double(height_m) - 20.0) *
                                           double(scene_extent.y) /
                                           double(height_m));
            using Pixel = std::array<u8, 4u>;
            std::array<Pixel, 2u> isolated{}, isolated_gap{}, pair_left{}, pair_right{}, pair_gap{};
            std::array<Pixel, 2u> isolated_interior{}, isolated_exterior{};
            std::array<Pixel, 2u> boundary_left{}, boundary_right{}, boundary_midpoint{}, empty{};
            for (const bool temporal: {false, true}) {
                const u32 mode = temporal ? 1u : 0u;
                const RetroSettings settings{
                    .m_enabled = false,
                    .m_palette_enabled = false,
                    .m_temporal_enabled = temporal,
                    .m_pixel_scale = 8u};
                const auto viewport = temporal ? rhi::Viewport::fromSize(3431.0f, 1815.0f) : output_viewport;
                const auto scissor = temporal ? rhi::ScissorRect::fromSize(3431u, 1815u) : output_scissor;
                for (u32 frame = 0u; frame < (temporal ? 4u : 1u); ++frame) {
                    const auto raster = picture.beginFrame(settings, snapshot, scene_extent, grid.worldGeneration());
                    PPR_TEST_ASSERT(picture.temporalActive() == temporal);
                    PPR_TEST_ASSERT(not grid.update(TimeSpan{}, raster));

                    const auto draw_content = [&](const DrawContext &context) -> std::error_code {
                        PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.renderBackground(context));
                        PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.render(context));
                        return grid.renderParticles(context);
                    };

                    const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                        PPR_RETURN_ERROR_ON_FAIL(
                            GridMaterialGate, grid.prepareBackground(context, snapshot, viewport, scissor));
                        PPR_RETURN_ERROR_ON_FAIL(GridMaterialGate, grid.prepareGpuParticles(context));
                        if (temporal) {
                            return picture.prepareScene(
                                context, RetroPass::scenePipelineSignature(true), scene_extent,
                                DrawCallback{draw_content});
                        }

                        return {};
                    };

                    const auto present = [&](const DrawContext &context) {
                        return temporal ? picture.render(context) : draw_content(context);
                    };

                    DrawSubmission submission{"five-pixel liquid feature quality", present};
                    submission.m_viewport = output_viewport;
                    submission.m_scissor = output_scissor;
                    PPR_TEST_ASSERT(not renderer->renderToTexture(
                        targetRef_(target), {submission}, {}, PreRenderCallback{prepare}));
                    const auto submitted = renderer->lastSubmittedFenceValue();
                    grid.notifyFrameSubmitted(submitted);
                    if (temporal) {
                        picture.notifyFrameSubmitted(submitted);
                    }

                    PPR_TEST_ASSERT(not renderer->waitForFenceValue(submitted));
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                }

                // Only these two final captures read pixels back. Warmup frames
                // retain the target/history and perform no CPU pixel readback.
                const auto pixels = readback_(device, targetRef_(target), 3445u);
                PPR_TEST_ASSERT(pixels.has_value());
                if (fixture == EParticleFeatureFixture::isolated) {
                    saveGridQa_(
                        *pixels,
                        std::format(
                            "particle-{}-five-pixel-offset-{}",
                            phase_flag == kGridSurfaceParticleSolid ? "solid" : "liquid",
                            temporal ? "temporal" : "native"),
                        1837u);
                }
                if (slanted_interface) {
                    saveGridQa_(
                        *pixels,
                        temporal ? "particle-solid-slanted-interface-offset-temporal"
                                 : "particle-solid-slanted-interface-offset-native",
                        1837u);
                    checkSlantedSolidInterfacePixels_(
                        *pixels, 7u, 11u, u32(scene_extent.x), u32(scene_extent.y), double(origin_x), double(origin_y),
                        double(width_m), temporal);
                    continue;
                }

                empty[mode] = gateTexel_(*pixels, 20u, 20u);
                isolated[mode] = gateTexel_(*pixels, sample_x(8.0), sample_y);
                isolated_interior[mode] = gateTexel_(*pixels, sample_x(8.0 + 0.5 * radius), sample_y);
                isolated_exterior[mode] = gateTexel_(*pixels, sample_x(8.0 + 1.8 * radius), sample_y);
                isolated_gap[mode] = gateTexel_(*pixels, sample_x(8.0 + 2.5 * radius), sample_y);
                pair_left[mode] = gateTexel_(*pixels, sample_x(18.0), sample_y);
                pair_right[mode] = gateTexel_(*pixels, sample_x(18.0 + 4.5 * radius), sample_y);
                pair_gap[mode] = gateTexel_(*pixels, sample_x(18.0 + 2.25 * radius), sample_y);
                boundary_left[mode] = gateTexel_(*pixels, sample_x(30.0), sample_y);
                boundary_right[mode] = gateTexel_(*pixels, sample_x(30.0 + 1.8 * radius), sample_y);
                boundary_midpoint[mode] = gateTexel_(*pixels, sample_x(30.0 + 0.9 * radius), sample_y);
                std::println(
                    "PARTICLE-SMALL-FEATURE phase={} temporal={} target=3445x1837 viewport_origin=7,11 extent=3431x1815 "
                    "fov_width_m=128 radius_m={} native_radius_px={} sample_y={} isolated_at_x={} isolated_blue={} "
                    "interior_half_r_delta={} exterior_1_8r_delta={} isolated_gap_delta={} "
                    "pair_left_blue={} pair_right_blue={} pair_gap_delta={} "
                    "boundary_left_blue={} boundary_left_red={} boundary_right_red={} boundary_right_blue={} interface_delta={}",
                    phase_flag == kGridSurfaceParticleSolid ? "solid" : "liquid", temporal, radius,
                    radius * double(scene_extent.x) / double(width_m), sample_y, sample_x(8.0), u32(isolated[mode][2]),
                    texelDist_(isolated_interior[mode], empty[mode]), texelDist_(isolated_exterior[mode], empty[mode]),
                    texelDist_(isolated_gap[mode], empty[mode]), u32(pair_left[mode][2]), u32(pair_right[mode][2]),
                    texelDist_(pair_gap[mode], empty[mode]), u32(boundary_left[mode][2]), u32(boundary_left[mode][0]),
                    u32(boundary_right[mode][0]), u32(boundary_right[mode][2]),
                    texelDist_(boundary_midpoint[mode], empty[mode]));
            }

            if (fixture != EParticleFeatureFixture::isolated) {
                return;
            }

            // These finite-size drops have about five native pixels of radius;
            // this does not promise preservation of arbitrary subtexel droplets.
            for (u32 mode = 0u; mode < 2u; ++mode) {
                PPR_TEST_ASSERT(
                    isolated[mode][2] >= 200u and pair_left[mode][2] >= 200u and pair_right[mode][2] >= 200u);
                PPR_TEST_ASSERT(texelDist_(isolated_interior[mode], empty[mode]) > 30u);
                PPR_TEST_ASSERT(texelDist_(isolated_exterior[mode], empty[mode]) <= 2u);
                PPR_TEST_ASSERT(texelDist_(isolated_gap[mode], empty[mode]) <= 2u);
                PPR_TEST_ASSERT(texelDist_(pair_gap[mode], empty[mode]) <= 2u);
                PPR_TEST_ASSERT(boundary_left[mode][2] >= 200u and boundary_left[mode][0] < 30u);
                PPR_TEST_ASSERT(boundary_right[mode][0] >= 200u and boundary_right[mode][2] < 30u);
                PPR_TEST_ASSERT(texelDist_(boundary_midpoint[mode], empty[mode]) > 30u);
            }
        }

        PPR_UNIT_TEST (grid_liquid_five_pixel_features_survive_large_offset_viewport) {
            checkFivePixelParticleFeatures_(kGridSurfaceParticleLiquid);
        };

        PPR_UNIT_TEST (grid_solid_five_pixel_features_survive_large_offset_viewport) {
            checkFivePixelParticleFeatures_(kGridSurfaceParticleSolid);
        };

        PPR_UNIT_TEST (grid_solid_slanted_material_interface_reconstructs_narrow_color_band) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            for (const bool temporal: {false, true}) {
                GridPass grid;
                RetroPass picture;
                PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_TEST_ASSERT(not picture.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_DEFER {
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    PPR_TEST_ASSERT(not picture.shutdown());
                    PPR_TEST_ASSERT(not grid.shutdown());
                };
                PPR_TEST_ASSERT(not grid.applyStagedFrame(slantedSolidInterfaceFixture_()));
                const RetroSettings settings{
                    .m_enabled = false, .m_palette_enabled = false, .m_temporal_enabled = temporal};
                GatePixels pixels{};
                for (u32 frame = 0u; frame < (temporal ? 4u : 1u); ++frame) {
                    pixels = drawOpticalGrid_(
                        grid, 20.0f, 20.0f, 6.0f, 512u, nullptr, nullptr, false, 0u, 0.0f, &settings, &picture);
                }

                PPR_TEST_ASSERT(picture.temporalActive() == temporal);
                saveGridQa_(
                    pixels,
                    temporal ? "particle-solid-slanted-interface-temporal" : "particle-solid-slanted-interface-native");
                checkSlantedSolidInterfacePixels_(pixels, 0u, 0u, 512u, 512u, 20.0, 20.0, 6.0, temporal);
            }

            checkFivePixelParticleFeatures_(kGridSurfaceParticleSolid, EParticleFeatureFixture::slanted_interface);

            const auto capture_alpha = [&](const GridPass::GridStagedFrame &stage) {
                GridPass grid;
                PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_DEFER {
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    PPR_TEST_ASSERT(not grid.shutdown());
                };
                PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                return drawOpticalGrid_(grid, 20.0f, 20.0f, 6.0f, 512u);
            };

            // No authored backdrop: compare final blending over the same clear
            // color. A fully transparent material's RGB must have no influence.
            auto partial = slantedSolidInterfaceFixture_();
            partial.m_has_background = false;
            partial.m_background_layers = {};
            for (auto &visual: partial.m_terrain_visuals) {
                visual.m_surface_flags |= kGridSurfaceOpticalBlend;
                visual.m_opacity = 0.6f;
                visual.m_color_rgba = 0xff0000ffu;
            }

            partial.m_terrain_visuals[1u].m_opacity = 0.0f;
            auto contrasting = partial;
            contrasting.m_terrain_visuals[1u].m_color_rgba = 0x0000ffffu;
            const auto transparent_red = capture_alpha(partial);
            const auto transparent_blue = capture_alpha(contrasting);
            saveGridQa_(transparent_blue, "particle-solid-slanted-interface-transparent-blue");
            PPR_TEST_ASSERT(changedOpticalPixels_(transparent_red, transparent_blue) == 0u);

            // The right interior is fully transparent, while the left retains
            // visible fractional opacity rather than becoming opaque or empty.
            const auto clear = gateTexel_(transparent_blue, 495u, 17u);
            const auto invisible = gateTexel_(transparent_blue, 341u, 256u);
            const auto visible = gateTexel_(transparent_blue, 170u, 256u);
            PPR_TEST_ASSERT(texelDist_(invisible, clear) <= 2u);
            PPR_TEST_ASSERT(visible[0] > clear[0] + 20u and visible[0] < 240u);

            // Equal fractional appearance under two IDs must match the same
            // physical scene under one ID, including the internal seam. This
            // catches applying opacity twice in the reconstructed material band.
            partial.m_terrain_visuals[1u].m_opacity = 0.6f;
            const auto two_ids = capture_alpha(partial);
            auto one_id = partial;
            auto unified = std::make_shared<Array<GridParticleInstance>>(*one_id.m_particle_batches[0u].m_particles);
            for (auto &particle: *unified) {
                particle.m_material_id = 1u;
            }

            one_id.m_particle_batches[0u].m_particles = std::move(unified);
            const auto uniform = capture_alpha(one_id);
            saveGridQa_(two_ids, "particle-solid-slanted-interface-fractional-opacity");
            PPR_TEST_ASSERT(changedOpticalPixels_(two_ids, uniform) == 0u);
        };

        // ------------------------------------------------------------------
        // terrain visibility and retained world publication
        // ------------------------------------------------------------------

        PPR_UNIT_TEST (grid_compute_visibility_matches_reference_and_draws) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            rhi::IDevice &device = rhi->getDevice();
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                std::ignore = renderer->waitOnHost();
                std::ignore = grid.shutdown();
            };
            const auto target = makeRenderTarget_(device, 128u);
            PPR_TEST_ASSERT(target.has_value());
            const GridTileSubmission cached_tile{
                .m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u, .m_material_id = 1u};
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{&cached_tile, 1u}));
            GridTilePayload stale_geometry{};
            stale_geometry.m_rect[0] = 2048.0f;
            stale_geometry.m_rect[1] = 2048.0f;
            stale_geometry.m_rect[2] = 2176.0f;
            stale_geometry.m_rect[3] = 2176.0f;
            stale_geometry.m_material = 1u;
            PPR_TEST_ASSERT(grid.requestUpload(GridUploadRequest{0u, std::span{&stale_geometry, 1u}}).has_value());
            const GridTileSubmission offscreen_cells{
                .m_dirty_mask = 1u, .m_tile_range = {2048, 2048, 2176, 2176}, .m_chunk_id = 528u};
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{&offscreen_cells, 1u}));
            GridTilePayload cell_payload{};
            Array<u16> cell_materials;
            cell_materials.resize(GridPass::kCellsPerChunk, 1u);
            PPR_TEST_ASSERT(
                grid.requestUpload(
                        GridUploadRequest{528u, std::span{&cell_payload, 1u}, std::span<const u16>{cell_materials}})
                    .has_value());
            const std::array<GridTileSubmission, 3u> tiles{
                GridTileSubmission{.m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u, .m_material_id = 1u},
                GridTileSubmission{.m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 1u, .m_material_id = 2u},
                GridTileSubmission{.m_tile_range = {2048, 2048, 2176, 2176}, .m_chunk_id = 528u, .m_material_id = 3u},
            };
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            Array<GridTileSubmission> full_capacity;
            for (u32 chunk = 0u; chunk < GridPass::kGridTileCacheCapacity; ++chunk) {
                full_capacity.push_back(
                    GridTileSubmission{.m_tile_range = {0, 0, 128, 128}, .m_chunk_id = chunk, .m_material_id = 1u});
            }
            // Mixed visibility, edge touch, all outside, all visible, and empty.
            for (const u32 scenario: {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0u}) {
                const float cell_size_m = scenario == 6u or scenario == 7u ? 1.0f / 3.0f : 1.0f;
                GridPass::GridStagedFrame metric_stage{};
                metric_stage.m_cell_size_m = cell_size_m;
                PPR_TEST_ASSERT(not grid.applyStagedFrame(metric_stage));
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_origin = float3{scenario == 2u ? 3500.0f : scenario == 6u ? 64.0f : 0.0f, 0.0f, 0.0f};
                model.m_ortho_scale = scenario == 3u ? 64.0f : scenario == 0u ? 2.0f : 1.0f;
                Camera camera;
                camera.updateModel(
                    TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
                const auto snapshot = camera.getSnapshot();
                const std::span<const GridTileSubmission> submitted =
                    scenario == 4u   ? std::span<const GridTileSubmission>{}
                    : scenario == 5u ? std::span<const GridTileSubmission>{full_capacity}
                                     : std::span<const GridTileSubmission>{tiles};
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                PPR_TEST_ASSERT(not grid.submitTiles(submitted));
                const auto reference = GridPass::planTiles(snapshot, submitted, cell_size_m);
                PPR_TEST_ASSERT(reference.has_value());
                for (;;) {
                    PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                    const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                    PPR_TEST_ASSERT(prepared.has_value());

                    if (*prepared)
                        break;

                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                }

                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"GPU culled grid", draw}}, {}, PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                const auto [args_buffer, ids_buffer] = grid.visibilityBuffersForTest();
                rhi::IndirectDrawArguments args{};
                PPR_TEST_ASSERT(not make_error_code(device.readBuffer(args_buffer, 0u, sizeof(args), &args)));
                PPR_TEST_ASSERT(args.vertexCountPerInstance == 6u);
                PPR_TEST_ASSERT(args.startVertexLocation == 0u and args.startInstanceLocation == 0u);
                PPR_TEST_ASSERT(args.instanceCount == reference->m_tiles.size());
                Array<u32> actual;
                actual.resize(args.instanceCount);
                if (not actual.empty()) {
                    PPR_TEST_ASSERT(not make_error_code(
                        device.readBuffer(ids_buffer, 0u, actual.size() * sizeof(u32), actual.data())));
                }
                Array<u32> expected;
                for (const auto &tile: reference->m_tiles) {
                    expected.push_back(tile.m_submission_index);
                }
                std::ranges::sort(actual);
                std::ranges::sort(expected);
                PPR_TEST_ASSERT(std::ranges::equal(actual, expected));
                if (scenario == 0u or scenario == 2u or scenario == 4u or scenario == 7u) {
                    const auto pixels = readback_(device, targetRef_(target), 128u);
                    PPR_TEST_ASSERT(pixels.has_value());
                    const auto centre = gateTexel_(*pixels, 32u, 96u);
                    std::println(
                        "grid scenario={} visible={} centre={},{},{},{}", scenario, args.instanceCount, centre[0],
                        centre[1], centre[2], centre[3]);
                    if (scenario == 0u or scenario == 7u) {
                        PPR_TEST_ASSERT(centre[0] > 160u and centre[1] < 80u);
                        if (scenario == 7u) {
                            const auto outside = gateTexel_(*pixels, 96u, 96u);
                            PPR_TEST_ASSERT(outside[0] < 80u and outside[1] < 80u);
                        }
                        PPR_TEST_ASSERT(not renderer->renderToTexture(targetRef_(target), {DrawSubmission{grid}}));
                        grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                        PPR_TEST_ASSERT(not renderer->waitOnHost());
                        const auto direct_pixels = readback_(device, targetRef_(target), 128u);
                        PPR_TEST_ASSERT(direct_pixels.has_value());
                        PPR_TEST_ASSERT(gateTexel_(*direct_pixels, 32u, 96u) == centre);
                    } else {
                        PPR_TEST_ASSERT(centre[0] < 80u and centre[1] < 80u);
                    }
                }
            }

            // Clean camera motion reuses the resident table after each safe
            // ring slot has received it once. Atlas work remains quiescent.
            const u64 table_builds = grid.residentTableBuildCount();
            const u64 atlas_writes = grid.atlasWriteCount();
            const u64 atlas_waits = grid.atlasSerializationCount();
            for (u32 frame = 0u; frame < 6u; ++frame) {
                CameraModel moving_model{};
                moving_model.m_camera_mode = ECameraProjection::orthographic;
                moving_model.m_origin = float3{static_cast<float>(frame), 0.0f, 0.0f};
                Camera moving_camera;
                moving_camera.updateModel(
                    TimeSpan{}, moving_model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, moving_camera.getSnapshot()));
                PPR_TEST_ASSERT(not grid.submitTiles(tiles));
                const u64 uploads = grid.residentTableUploadCount();
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"persistent grid", draw}}, {}, PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(grid.residentTableBuildCount() == table_builds);
                if (frame >= 3u) {
                    PPR_TEST_ASSERT(grid.residentTableUploadCount() == uploads);
                }
            }
            PPR_TEST_ASSERT(grid.atlasWriteCount() == atlas_writes);
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == atlas_waits);

            // A clean submission can change bounds even with a cached payload.
            auto moved_tiles = tiles;
            moved_tiles[0u].m_tile_range = {3072, 0, 3200, 128};
            PPR_TEST_ASSERT(not grid.submitTiles(moved_tiles));
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"moved resident grid", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            PPR_TEST_ASSERT(grid.residentTableBuildCount() == table_builds + 1u);
            const auto moved_pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(moved_pixels.has_value());
            const auto moved_texel = gateTexel_(*moved_pixels, 32u, 96u);
            PPR_TEST_ASSERT(moved_texel[0] < 80u and moved_texel[1] < 80u);

            // A bounded cohort preserves the previous GPU table until every
            // replacement is ready, including identical unacknowledged replays.
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            CameraModel cohort_model{};
            cohort_model.m_camera_mode = ECameraProjection::orthographic;
            Camera cohort_camera;
            cohort_camera.updateModel(
                TimeSpan{}, cohort_model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, cohort_camera.getSnapshot()));
            const auto queue_cells = [&](const u32 chunk, const u16 material) {
                cell_materials.assign(GridPass::kCellsPerChunk, material);
                const auto uploaded = grid.requestUpload(
                    GridUploadRequest{chunk, std::span{&cell_payload, 1u}, std::span<const u16>{cell_materials}});
                PPR_TEST_ASSERT(uploaded and *uploaded == 1u);
                return uploaded;
            };
            auto cohort_tiles = tiles;
            for (auto &tile: cohort_tiles) {
                tile.m_dirty_mask = 1u;
            }
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{cohort_tiles.data(), 2u}));
            for (u32 chunk = 0u; chunk < 2u; ++chunk) {
                PPR_TEST_ASSERT(queue_cells(chunk, 1u).has_value());
            }
            auto published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 2u);
            PPR_TEST_ASSERT(published and *published);
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"old atlas cohort", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const auto old_pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(old_pixels.has_value());
            const auto old_texel = gateTexel_(*old_pixels, 32u, 96u);
            const u64 old_builds = grid.residentTableBuildCount();
            GridPass::GridStagedFrame replacement_stage{};
            replacement_stage.m_has_catalog = true;
            replacement_stage.m_cell_size_m = 1.0f / 3.0f;
            replacement_stage.m_tiles.assign(cohort_tiles.begin(), cohort_tiles.begin() + 2u);
            PPR_TEST_ASSERT(not grid.applyStagedFrame(replacement_stage));
            for (u32 chunk = 0u; chunk < 2u; ++chunk) {
                PPR_TEST_ASSERT(queue_cells(chunk, 2u).has_value());
            }
            const u64 bytes_before = grid.atlasUploadedBytes();
            published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and not *published);
            PPR_TEST_ASSERT(grid.atlasUploadedBytes() == bytes_before + GridPass::kCellWordsPerChunk * sizeof(u32));
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"deferred atlas cohort", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const auto deferred_pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(deferred_pixels.has_value());
            PPR_TEST_ASSERT(gateTexel_(*deferred_pixels, 32u, 96u) == old_texel);
            PPR_TEST_ASSERT(grid.residentTableBuildCount() == old_builds);
            for (u32 chunk = 0u; chunk < 2u; ++chunk) {
                PPR_TEST_ASSERT(queue_cells(chunk, 2u).has_value());
            }
            published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and *published);
            PPR_TEST_ASSERT(
                grid.atlasUploadedBytes() == bytes_before + 2u * GridPass::kCellWordsPerChunk * sizeof(u32));
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"published atlas cohort", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const auto new_pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(new_pixels.has_value());
            PPR_TEST_ASSERT(gateTexel_(*new_pixels, 32u, 96u) != old_texel);
            PPR_TEST_ASSERT(gateTexel_(*new_pixels, 96u, 96u)[0u] < 80u);
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == 0u);

            // Fill both physical generations. A conservative completion frontier
            // must defer a third generation without copying or waiting; an actual
            // completion proof then permits reuse of the retired generation.
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, cohort_camera.getSnapshot()));
            Array<GridTileSubmission> atlas_tiles;
            for (u32 chunk = 0u; chunk < GridPass::kGridTileCacheCapacity; ++chunk) {
                const i32 x = static_cast<i32>(chunk % 32u) * 128;
                const i32 y = static_cast<i32>(chunk / 32u) * 128;
                atlas_tiles.push_back(
                    GridTileSubmission{
                        .m_dirty_mask = 1u, .m_tile_range = {x, y, x + 128, y + 128}, .m_chunk_id = chunk});
            }
            PPR_TEST_ASSERT(not grid.submitTiles(atlas_tiles));
            for (const u16 material: {u16{1u}, u16{2u}}) {
                for (u32 chunk = 0u; chunk < GridPass::kGridTileCacheCapacity; ++chunk) {
                    PPR_TEST_ASSERT(queue_cells(chunk, material).has_value());
                }
                published = grid.tryPrepareCellUploads(device, kFenceValueNone, GridPass::kGridTileCacheCapacity);
                PPR_TEST_ASSERT(published and *published);
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"full atlas generation", draw}}, {},
                    PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                // The host wait proves GPU completion; refresh the renderer's
                // observed fence before testing physical-slot reuse.
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
            }
            for (u32 chunk = 0u; chunk < GridPass::kGridTileCacheCapacity; ++chunk) {
                PPR_TEST_ASSERT(queue_cells(chunk, 3u).has_value());
            }
            const u64 full_bytes = grid.atlasUploadedBytes();
            published = grid.tryPrepareCellUploads(device, kFenceValueNone, GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT(published and not *published);
            PPR_TEST_ASSERT(grid.atlasUploadedBytes() == full_bytes);
            published = grid.tryPrepareCellUploads(
                device, renderer->completedFenceValue(), GridPass::kGridTileCacheCapacity);
            PPR_TEST_ASSERT(published and *published);
            // No render preparation between this full publication and a new
            // edit: retirement of table pins must not depend on frame admission.
            PPR_TEST_ASSERT(queue_cells(0u, 4u).has_value());
            published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and *published);
            // Mixing explicit reference preparation with an async partial cohort
            // must clear the deferred state once the whole cohort is ready.
            PPR_TEST_ASSERT(queue_cells(0u, 5u).has_value());
            PPR_TEST_ASSERT(queue_cells(1u, 5u).has_value());
            published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and not *published);
            for (;;) {
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());

                const auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
                PPR_TEST_ASSERT(prepared.has_value());

                if (*prepared)
                    break;

                PPR_TEST_ASSERT(not renderer->waitOnHost());
            }

            PPR_TEST_ASSERT(queue_cells(2u, 6u).has_value());
            published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and *published);
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"mixed atlas preparation", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());

            // A new legacy pass lifetime must not inherit the prepared metric.
            GridPass::GridStagedFrame metric_stage{};
            metric_stage.m_cell_size_m = 1.0f / 3.0f;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(metric_stage));
            PPR_TEST_ASSERT(not grid.shutdown());
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            CameraModel legacy_model{};
            legacy_model.m_camera_mode = ECameraProjection::orthographic;
            Camera legacy_camera;
            legacy_camera.updateModel(
                TimeSpan{}, legacy_model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, legacy_camera.getSnapshot()));
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{tiles.data(), 1u}));
            PPR_TEST_ASSERT(not renderer->renderToTexture(targetRef_(target), {DrawSubmission{grid}}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const auto legacy_pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(legacy_pixels.has_value());
            const auto legacy_texel = gateTexel_(*legacy_pixels, 96u, 96u);
            PPR_TEST_ASSERT(legacy_texel[0] > 160u and legacy_texel[1] < 80u);
        };

        PPR_UNIT_TEST (grid_candidate_world_retains_active_until_matching_activation) {
            const auto rhi = SharedGpu::rhiService();
            const auto shader = SharedGpu::shaderService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and shader.isValid() and renderer != nullptr);
            rhi::IDevice &device = rhi->getDevice();
            GridPass grid;
            PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER {
                std::ignore = renderer->waitOnHost();
                std::ignore = grid.shutdown();
            };
            const auto target = makeRenderTarget_(device, 128u);
            PPR_TEST_ASSERT(target.has_value());
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{0.0f, 0.0f, 0.0f};
            model.m_ortho_scale = 1.0f;
            Camera camera;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            const auto render = [&] {
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"candidate world", draw}}, {}, PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());

                // Queue idleness precedes the cached completion observation used by retirement.
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                PPR_TEST_ASSERT(renderer->completedFenceValue() >= renderer->lastSubmittedFenceValue());

                const auto pixels = readback_(device, targetRef_(target), 128u);
                PPR_TEST_ASSERT(pixels.has_value());
                return gateTexel_(*pixels, 64u, 64u);
            };
            GridPass::GridStagedFrame active{};
            active.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {0, 0, 128, 128}, .m_chunk_id = 0u});
            GridPass::GridStagedUpload upload{};
            upload.m_cell_materials.resize(GridPass::kCellsPerChunk, 1u);
            active.m_uploads.push_back(std::move(upload));
            PPR_TEST_ASSERT(not grid.applyStagedFrame(active));
            auto prepared = grid.tryPrepareCellUploads(device, renderer->completedFenceValue());
            PPR_TEST_ASSERT(prepared and *prepared);
            const auto before = render();
            PPR_TEST_ASSERT(before[0] > 160u and before[1] < 80u);
            PPR_TEST_ASSERT(render() == before);
            PPR_TEST_ASSERT(render() == before);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 1u);

            auto candidate = active;
            candidate.m_content_fingerprint = 77u;
            std::ranges::fill(candidate.m_uploads[0].m_cell_materials, u16{2u});
            candidate.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 1u});
            auto second = candidate.m_uploads[0];
            second.m_chunk_id = 1u;
            candidate.m_uploads.push_back(std::move(second));
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(10u, 20u, candidate));
            PPR_TEST_ASSERT(grid.stageWorldCandidate(11u, 21u, candidate) == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(10u, 20u, 77u) == std::errc::operation_not_permitted);
            prepared = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(prepared and not *prepared);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 2u);
            PPR_TEST_ASSERT(render() == before);
            prepared = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(prepared and *prepared);
            PPR_TEST_ASSERT(render() == before);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(9u, 20u, 77u) == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(10u, 21u, 77u) == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(10u, 20u, 78u) == std::errc::operation_not_permitted);
            grid.cancelWorldCandidate(9u);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(10u, 20u, 77u));
            PPR_TEST_ASSERT(grid.stageWorldCandidate(11u, 21u, candidate) == std::errc::device_or_resource_busy);
            const auto after = render();
            PPR_TEST_ASSERT(after[1] > 160u and after[0] < 80u);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 1u);
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == 0u);

            // Completed old readers permit another candidate. A stale cancel
            // cannot discard it; a matching cancel preserves the active world.
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(11u, 21u, candidate));
            grid.cancelWorldCandidate(10u);
            prepared = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(prepared and not *prepared);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 2u);
            grid.cancelWorldCandidate(11u);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 2u);
            PPR_TEST_ASSERT(grid.stageWorldCandidate(12u, 22u, candidate) == std::errc::device_or_resource_busy);
            PPR_TEST_ASSERT(render() == after);
            const auto absent = grid.advanceWorldCandidate(device, renderer->completedFenceValue());
            PPR_TEST_ASSERT(not absent and absent.error() == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 1u);
            auto invalid = candidate;
            invalid.m_cell_size_m = 2.0f;
            PPR_TEST_ASSERT(grid.stageWorldCandidate(12u, 22u, invalid) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(render() == after);

            for (const bool device_loss: {false, true}) {
                PPR_TEST_ASSERT(not (device_loss ? grid.notifyDeviceLost() : grid.shutdown()));
                PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_TEST_ASSERT(not grid.stageWorldCandidate(12u, 22u, candidate));
                prepared = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 2u);
                PPR_TEST_ASSERT(prepared and *prepared);
                PPR_TEST_ASSERT(not grid.activateWorldCandidate(12u, 22u, 77u));
                // A fresh fence epoch starts at zero. The restored active
                // bundle has not drawn and must carry no old reader fence.
                const auto no_candidate = grid.advanceWorldCandidate(device, kFenceValueNone);
                PPR_TEST_ASSERT(not no_candidate and no_candidate.error() == std::errc::operation_not_permitted);
                PPR_TEST_ASSERT(not grid.stageWorldCandidate(13u, 23u, candidate));
                grid.cancelWorldCandidate(13u);
                PPR_TEST_ASSERT(render() == after);
            }

            // A cancelled, never-uploaded candidate still owns large CPU blocks.
            // Retirement must yield between bounded batches and keep the old draw live.
            GridPass::GridStagedFrame header{};
            header.m_content_fingerprint = 77u;
            header.m_diagnostic_mode = EGridDiagnosticMode::biome;
            header.m_diagnostic_revision = 14u;
            header.m_diagnostic_palette.push_back({.m_color_rgba = 0xffffffffu});
            for (u32 chunk = 0u; chunk < 32u; ++chunk) {
                const i32 x = static_cast<i32>(chunk * 128u);
                header.m_tiles.push_back(
                    GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {x, 0, x + 128, 128}, .m_chunk_id = chunk});
            }
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(14u, 24u, header, false));
            for (u32 first = 0u; first < 32u; first += 8u) {
                Array<GridPass::GridStagedUpload> batch;
                for (u32 chunk = first; chunk < first + 8u; ++chunk) {
                    GridPass::GridStagedUpload row{};
                    row.m_chunk_id = chunk;
                    row.m_cell_materials.resize(GridPass::kCellsPerChunk, 1u);
                    row.m_diagnostic_indices.resize(GridPass::kCellsPerChunk, 0u);
                    batch.push_back(std::move(row));
                }
                PPR_TEST_ASSERT(not grid.appendWorldCandidate(14u, 24u, batch, first == 24u));
            }
            grid.cancelWorldCandidate(14u);
            const auto retirement_before = grid.observations();
            PPR_TEST_ASSERT(not grid.worldCandidateAdmissionReady(renderer->completedFenceValue()));
            const auto first_step = grid.observations();
            PPR_TEST_ASSERT(first_step.m_retirement_steps == retirement_before.m_retirement_steps + 1u);
            PPR_TEST_ASSERT(first_step.m_retired_chunk_blocks == retirement_before.m_retired_chunk_blocks + 8u);
            PPR_TEST_ASSERT(grid.stageWorldCandidate(15u, 25u, candidate) == std::errc::device_or_resource_busy);
            bool retired = false;
            for (u32 frame = 0u; frame < 32u and not retired; ++frame) {
                PPR_TEST_ASSERT(render() == after);
                retired = grid.worldCandidateAdmissionReady(renderer->completedFenceValue());
                const auto observed = grid.observations();
                PPR_TEST_ASSERT(
                    observed.m_retired_chunk_blocks - retirement_before.m_retired_chunk_blocks <=
                    8u * (observed.m_retirement_steps - retirement_before.m_retirement_steps));
            }
            PPR_TEST_ASSERT(retired);
            PPR_TEST_ASSERT(
                grid.observations().m_retired_chunk_blocks == retirement_before.m_retired_chunk_blocks + 64u);
        };

        PPR_UNIT_TEST (gpu_timestamps_follow_completed_submissions_and_reject_abandoned_encoders) {
            Renderer *const renderer = SharedGpu::renderer();
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(renderer != nullptr and rhi.isValid());
            const auto target = makeRenderTarget_(rhi->getDevice(), 128u);
            PPR_TEST_ASSERT(target.has_value());
            PPR_DEFER {
                std::ignore = renderer->waitOnHost();
            };
            FenceCompletionValue previous{};
            for (u32 frame = 0u; frame < 4u; ++frame) {
                PPR_TEST_ASSERT(not renderer->renderToTexture(targetRef_(target), {}));
                const auto submitted = renderer->lastSubmittedFenceValue();
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                const auto timing = renderer->lastGpuFrameTiming();
                PPR_TEST_ASSERT(timing and timing->m_submission == submitted and timing->m_submission > previous);
                PPR_TEST_ASSERT(std::isfinite(timing->m_milliseconds) and timing->m_milliseconds >= 0.0f);
                previous = submitted;
                const auto reject = [](const DrawContext &) {
                    return std::make_error_code(std::errc::invalid_argument);
                };
                PPR_TEST_ASSERT(
                    renderer->renderToTexture(
                        targetRef_(target), {DrawSubmission{"abandoned timestamp encoder", reject}}) ==
                    std::errc::invalid_argument);
                PPR_TEST_ASSERT(renderer->lastGpuFrameTiming()->m_submission == submitted);
            }
        };

        PPR_UNIT_TEST (pre_render_preparation_orders_and_rejects_submission) {
            const auto rhi = SharedGpu::rhiService();
            PPR_TEST_ASSERT(rhi.isValid());
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(renderer != nullptr);
            const auto target = makeRenderTarget_(rhi->getDevice(), 32u);
            PPR_TEST_ASSERT(target.has_value());

            u32 phase = 0u;
            const auto prepare = [&](const PreRenderContext &context) -> std::error_code {
                PPR_TEST_ASSERT(phase == 0u);
                PPR_TEST_ASSERT(std::addressof(context.m_device) == std::addressof(rhi->getDevice()));
                PPR_TEST_ASSERT(static_cast<bool>(context.m_wait_for_fence));
                context.m_encoder.globalBarrier();
                phase = 1u;
                return {};
            };
            const auto draw = [&](const DrawContext &) -> std::error_code {
                PPR_TEST_ASSERT(phase == 1u);
                phase = 2u;
                return {};
            };
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"prepared draw", draw}}, {}, PreRenderCallback{prepare}));
            PPR_TEST_ASSERT(phase == 2u);
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const FenceCompletionValue accepted = renderer->lastSubmittedFenceValue();

            const auto reject = [&](const PreRenderContext &) -> std::error_code {
                phase = 3u;
                return std::make_error_code(std::errc::invalid_argument);
            };
            PPR_TEST_ASSERT(
                renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"unreachable draw", draw}}, {}, PreRenderCallback{reject}) ==
                std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(phase == 3u);
            PPR_TEST_ASSERT(renderer->lastSubmittedFenceValue() == accepted);
            PPR_TEST_ASSERT(renderer->lastSubmitSettlement() == FrameSettlement::kRejected);

            phase = 0u;
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"retry prepared draw", draw}}, {}, PreRenderCallback{prepare}));
            PPR_TEST_ASSERT(phase == 2u);
            PPR_TEST_ASSERT(not renderer->waitOnHost());
        };

        // §7 editor flow end to end: boot editor, loadScene, per-frame submit
        // via update, render the pass offscreen, release on unload/teardown.
        PPR_UNIT_TEST (editor_scene_flow) {
            constexpr u32 particle_material = 1u;
            constexpr auto frame_delta = std::chrono::milliseconds{16};
            GateEditorApp test_app{"AssetEditorFlow", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                // Best-effort: never throw during unwind (an assert here
                // while unwinding from a body failure terminates the process
                // and hides both messages). Happy path asserts below.
                std::ignore = test_app.teardown();
            };

            PPR_TEST_ASSERT(not test_app.loadScene(gateMeshDir(), "textured_box.gltf"));
            PPR_TEST_ASSERT(not test_app.tick(frame_delta));

            // The editor camera sits at (0,0,-2) facing the box's unlit -Z
            // face straight-on (debugger: center texel 19,18,19, spread 1 —
            // outside the box, but viewing its dark side). Drive the pass with
            // the same calibrated snapshot as the box gate (camera outside at
            // +Z, box framed, lit face visible). Instances submitted by tick()
            // persist — TrianglePass::update only re-snapshots the camera.
            const Expected<mesh::SceneAsset> bounds_scene = mesh::importAndConvert(gateMeshDir(), "textured_box.gltf");
            PPR_TEST_ASSERT(bounds_scene.has_value());
            const float3 box_center = bounds_scene->m_meshes.front().m_bounds.center();
            const float3 box_size = bounds_scene->m_meshes.front().m_bounds.size();
            const float box_max = std::max({box_size.x, box_size.y, box_size.z});
            const float3 box_eye{
                box_center.x + 0.25f * box_max, box_center.y + 0.2f * box_max, box_center.z + 2.2f * box_max};
            PPR_TEST_ASSERT(not test_app.getTrianglePass().update(
                TimeSpan{}, gateCamera_(box_eye, box_center, float2{256.0f, 256.0f})));

            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();
            Expected<rhi::ComPtr<rhi::ITexture>> target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(target.has_value());
            Renderer &renderer = test_app.getRenderer();
            PPR_TEST_ASSERT(not renderer.renderToTexture(
                targetRef_(target), {DrawSubmission{test_app.getTrianglePass()}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());

            const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 256u);
            PPR_TEST_ASSERT(pixels.has_value());
            PPR_TEST_ASSERT(spread_(gateTexel_(*pixels, 128u, 128u)) > 12u);

            {
                // Install the fixture catalog through the first owned frame,
                // before GPU preparation freezes its palette.
                GridTerrainVisual visual{};
                visual.m_material_id = particle_material;
                visual.m_color_rgba = 0xffffffffu;
                GridStagedFrame world{};
                world.m_terrain_visuals.push_back(visual);
                test_app.submitGridStage(std::move(world));
            }

            {
                // Scope the UI service safe_ptr: teardown destroys the service,
                // so no observers may outlive the in-frame draw below.
                const auto ui = test_app.getServices().get<IUIService>();
                PPR_TEST_ASSERT(ui.isValid());
                ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ui->getContext()));
                static bool show_demo_window{true};
                ImGui::ShowDemoWindow(&show_demo_window);
                PPR_TEST_ASSERT(not test_app.draw());
                ImGui::SetCurrentContext(nullptr);
            }

            {
                // A final joint publication can revoke the pass's cached E
                // before a terrain frame carries E+1. Workshop must supply that
                // final strong lease while scene and detached UI keep presenting.
                auto &grid = test_app.getGridPass();

                const auto live = std::make_shared<std::atomic<u64>>(1u);
                const auto viewAt = [&](const u64 epoch) {
                    std::array<float, 16u> hot{};
                    hot[0u] = hot[1u] = 3.0f;
                    hot[15u] = 1.0f;
                    std::array<float, 6u> cold{};
                    cold[0u] = 1.0f;
                    cold[1u] = std::numbers::pi_v<float> * 0.25f;
                    cold[2u] = static_cast<float>(particle_material);
                    cold[3u] = 1.0f;

                    const auto buffer = [&](const u64 bytes, const void *const data) {
                        rhi::BufferDesc desc{};
                        desc.size = bytes;
                        desc.elementSize = safe_narrowing(bytes);
                        desc.memoryType = rhi::MemoryType::DeviceLocal;
                        desc.usage = rhi::BufferUsage::ShaderResource;
                        desc.defaultState = rhi::ResourceState::ShaderResource;
                        rhi::ComPtr<rhi::IBuffer> result;
                        PPR_TEST_ASSERT(not make_error_code(device.createBuffer(desc, data, result.writeRef())));
                        return result;
                    };

                    GridParticleMetadataRow row{};
                    row.m_id_low = 1u;
                    row.m_source_row = 0u;
                    row.m_material = particle_material;
                    row.m_radius = 0.5f;
                    const auto metadata = prepareGridParticleMetadata(device, {&row, 1u}, 1u, 1.0, epoch);
                    PPR_TEST_ASSERT(metadata.has_value());

                    GridParticleGpuView view{};
                    view.m_numerical = buffer(sizeof(hot), hot.data());
                    view.m_readonly = buffer(sizeof(cold), cold.data());
                    view.m_metadata = *metadata;
                    view.m_live_epoch = live;
                    view.m_world = grid.activeWorldIdentity();
                    view.m_device_epoch = epoch;
                    view.m_publication = epoch;
                    return view;
                };

                const auto draw_retained = [&](const std::optional<GridParticleGpuView> &view) {
                    PPR_TEST_ASSERT(not renderer.waitOnHost());
                    PPR_TEST_ASSERT(not renderer.retireCompletedFrames());

                    PPR_TEST_ASSERT(not test_app.tick(frame_delta));
                    const auto ui = test_app.getServices().get<IUIService>();
                    PPR_TEST_ASSERT(ui.isValid());
                    ImGui::SetCurrentContext(static_cast<ImGuiContext *>(ui->getContext()));
                    PPR_DEFER {
                        ImGui::SetCurrentContext(nullptr);
                    };

                    bool show = true;
                    ImGui::ShowDemoWindow(&show);

                    GridStagedFrame retained{};
                    retained.m_retain_content = true;
                    retained.m_gpu_particles = view;
                    test_app.submitGridStage(std::move(retained));
                    PPR_TEST_ASSERT(not test_app.draw());
                    PPR_TEST_ASSERT(not test_app.m_last_render_error);
                };

                const auto previous = viewAt(1u);
                PPR_TEST_ASSERT(not grid.applyGpuParticleView(previous));
                draw_retained(previous);
                PPR_TEST_ASSERT(test_app.m_last_presented and not test_app.m_last_dropped);
                const auto accepted = renderer.lastSubmittedFenceValue();

                const auto final = viewAt(2u);
                live->store(2u, std::memory_order_release);
                PPR_TEST_ASSERT(not previous.current() and final.current());
                PPR_TEST_ASSERT(not grid.gpuParticleFrameCurrent({}));
                draw_retained(std::nullopt);
                PPR_TEST_ASSERT(test_app.m_last_dropped and not test_app.m_last_presented);
                PPR_TEST_ASSERT(test_app.m_last_submission == kFenceValueNone and test_app.m_last_ui_version == 0u);
                PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == accepted);

                draw_retained(previous);
                PPR_TEST_ASSERT(test_app.m_last_dropped and not test_app.m_last_presented);
                PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == accepted);

                draw_retained(final);
                PPR_TEST_ASSERT(test_app.m_last_presented and not test_app.m_last_dropped);
                PPR_TEST_ASSERT(test_app.m_last_submission > accepted and test_app.m_last_ui_version != 0u);
                const auto presented = grid.activeParticlePresentation();
                PPR_TEST_ASSERT(presented and presented->m_publication == 2u);
                PPR_TEST_ASSERT(not renderer.waitOnHost());
            }

            {
                const auto windows = test_app.getServices().get<IWindowService>();
                PPR_TEST_ASSERT(windows.isValid());
                const auto &window = windows->getMainWindow();
                PPR_TEST_ASSERT(window.isValid());
                const SurfaceGeneration initial = renderer.surfaceGenerationFor(window->m_handle);
                const auto before = renderer.appliedSurfaceSettingsFor(window->m_handle);
                PPR_TEST_ASSERT(initial != kSurfaceGenerationNone and before.m_available);

                // Real-backend vsync-only change, no window resize required.
                // A successful host present is not physical display delivery.
                const auto changed = renderer.reconfigureWindowSurface(
                    window->m_handle, initial, SurfaceSettings{.m_vsync = not before.m_vsync});
                PPR_TEST_ASSERT(changed.has_value());
                PPR_TEST_ASSERT(changed->m_extent.x == before.m_extent.x and changed->m_extent.y == before.m_extent.y);
                PPR_TEST_ASSERT(changed->m_vsync != before.m_vsync and changed->m_generation != initial);
                PPR_TEST_ASSERT(not renderer.renderAndPresent(*window, {SurfaceRenderPass{}}));
                const FenceCompletionValue accepted = renderer.lastSubmittedFenceValue();
                PPR_TEST_ASSERT(accepted != kFenceValueNone);

                // Resize and explicit control share one update path. A control
                // captured before the shape change cannot apply afterwards.
                const auto minimized = renderer.reconfigureWindowSurface(
                    window->m_handle, changed->m_generation,
                    SurfaceSettings{.m_vsync = changed->m_vsync, .m_extent = int2{zero_v}});
                PPR_TEST_ASSERT(minimized.has_value() and not minimized->m_available);
                const auto stale = renderer.reconfigureWindowSurface(window->m_handle, initial, SurfaceSettings{});
                PPR_TEST_ASSERT(
                    not stale and stale.error() == std::make_error_code(std::errc::resource_unavailable_try_again));
                PPR_TEST_ASSERT(renderer.lastSubmittedFenceValue() == accepted);
                PPR_TEST_ASSERT(not renderer.renderAndPresent(*window, {SurfaceRenderPass{}}));

                const SurfaceGeneration restored = renderer.surfaceGenerationFor(window->m_handle);
                PPR_TEST_ASSERT(restored != minimized->m_generation);
                PPR_TEST_ASSERT(not renderer.destroyWindowSurface(window->m_handle));
                const auto destroyed = renderer.reconfigureWindowSurface(window->m_handle, restored, SurfaceSettings{});
                PPR_TEST_ASSERT(not destroyed);
                // A stale packet cannot lazily resurrect the destroyed record.
                PPR_TEST_ASSERT(
                    renderer.renderAndPresent(
                        *window, {SurfaceRenderPass{}}, SurfaceIdentity{window->m_handle, restored}) ==
                    std::make_error_code(std::errc::resource_unavailable_try_again));
                PPR_TEST_ASSERT(not renderer.renderAndPresent(*window, {SurfaceRenderPass{}}));
                PPR_TEST_ASSERT(renderer.surfaceGenerationFor(window->m_handle) != restored);
                PPR_TEST_ASSERT(not renderer.reconfigureWindowSurface(window->m_handle, restored, SurfaceSettings{}));
            }

            PPR_TEST_ASSERT(not test_app.unloadScene());
            // Release the device ref BEFORE teardown: the render target holds a
            // DeviceChild ref and RHI destroy asserts on live children (same
            // contract as the frame-fence EditorShutdownHarness).
            target = Expected<rhi::ComPtr<rhi::ITexture>>{};
            PPR_TEST_ASSERT(not test_app.teardown());
        };
    } // namespace Gate
} // namespace pP::tests::detail

namespace pP::tests {
    // Render gates under the shared render scope (the asset/render parent in
    // Asset.Tests.cpp owns the single SharedGpu acquire). The editor flow is
    // quarantined: it boots its private GateEditorApp and is recursed from
    // asset/render/quarantine, never from here.
    const UnitTest gates = UnitTest::Named("gates") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Gate::bindless_textured_box_gate,
            detail::Gate::triangle_depth_occlusion_gate,
            detail::Gate::orm_golden_distinct_channels,
            detail::Gate::tangent_w_render_proof,
            detail::Gate::pipeline_signature_count_sensitivity,
            detail::Gate::pipeline_signature_tail_insensitivity,
            detail::Gate::pipeline_signature_hash_consistent_with_equality,
            detail::Gate::render_rejects_color_attachments_beyond_capacity,
            detail::Gate::render_rejects_multilayer_multimip_color_attachment,
            detail::Gate::render_resolves_msaa_color_attachment,
            detail::Gate::render_accepts_msaa_sample_counts,
            detail::Gate::gpu_timestamps_follow_completed_submissions_and_reject_abandoned_encoders,
            detail::Gate::pre_render_preparation_orders_and_rejects_submission,
            detail::Gate::grid_compute_visibility_matches_reference_and_draws,
            detail::Gate::grid_resident_circle_mask_motion_overlap_occlusion_and_leases,
            detail::Gate::grid_solid_particles_share_catalog_textures_and_metric_uvs,
            detail::Gate::grid_surface_inventory_transitions_preserve_gas_visibility_and_single_blending,
            detail::Gate::grid_solid_envelope_closes_nominal_interstices_without_bridging_sparse_grains,
            detail::Gate::grid_liquid_surface_connects_and_preserves_element_boundaries,
            detail::Gate::grid_liquid_optics_and_viewport_cost,
            detail::Gate::grid_liquid_nominal_density_deformation_coverage,
            detail::Gate::grid_liquid_full_resolution_temporal_cost,
            detail::Gate::grid_liquid_five_pixel_features_survive_large_offset_viewport,
            detail::Gate::grid_solid_five_pixel_features_survive_large_offset_viewport,
            detail::Gate::grid_solid_slanted_material_interface_reconstructs_narrow_color_band,
            detail::Gate::grid_candidate_world_retains_active_until_matching_activation,
            detail::Gate::grid_optical_controls_and_four_layer_parallax,
            detail::Gate::picture_controls_preserve_independent_color_and_temporal_history,
            detail::Gate::background_magnification_matches_wrapped_cubic_reference,
            detail::Gate::temporal_layered_background_pan_uses_stable_spatial_fallback,
            detail::Gate::temporal_camera_pan_preserves_pattern_contrast_and_phase,
            detail::Gate::grid_particle_support_and_resource_repeat,
            detail::Gate::grid_material_shading_cost_samples,
            detail::Gate::picture_temporal_cost_samples,
            detail::Gate::grid_stochastic_world_uvs_mips_and_chunk_seams,
            detail::Gate::grid_translucent_solid_borders_and_internal_boundaries,
        });
    };

    const UnitTest &renderGatesTests() noexcept {
        return gates;
    }

    const UnitTest &gateEditorLeaf() noexcept {
        return detail::Gate::editor_scene_flow;
    }
} // namespace pP::tests
