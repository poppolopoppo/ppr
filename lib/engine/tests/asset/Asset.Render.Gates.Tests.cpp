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

        [[nodiscard]] Expected<rhi::ComPtr<rhi::ITexture>> makeRenderTarget_(rhi::IDevice &device, const u32 size) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {size, size, 1u};
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
            bool *const admission_during_draw = nullptr) {
            const auto rhi = SharedGpu::rhiService();
            Renderer *const renderer = SharedGpu::renderer();
            PPR_TEST_ASSERT(rhi.isValid() and renderer != nullptr);
            auto &device = rhi->getDevice();
            const auto target = makeRenderTarget_(device, edge);
            PPR_TEST_ASSERT(target.has_value());
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{origin_x, origin_y, -0.5f};
            model.m_ortho_scale = width_m / static_cast<float>(edge);
            Camera camera{ECameraProjection::orthographic};
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{0, 0, int(edge), int(edge)}});
            const auto snapshot = camera.getSnapshot();
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
            const auto wait_for_idle = [&]() { return renderer->waitOnHost(); };
            PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle, true));
            const auto viewport = rhi::Viewport::fromSize(float(edge), float(edge));
            const auto scissor = rhi::ScissorRect::fromSize(edge, edge);
            const auto prepare = [&](const PreRenderContext &context) {
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
                    return grid.prepareBackground(stale, snapshot, viewport, scissor);
                }
                return grid.prepareBackground(context, snapshot, viewport, scissor);
            };
            const auto draw = [&](const DrawContext &context) -> std::error_code {
                if (const auto ec = grid.renderBackground(context)) {
                    return ec;
                }
                if (const auto ec = grid.render(context)) {
                    return ec;
                }
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
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"grid optical gate", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
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

        void saveGridQa_(const GatePixels &pixels, const std::string_view name) {
            const char *const directory = std::getenv("PPR_GRID_QA_DIR");
            if (directory == nullptr) {
                return;
            }
            const std::filesystem::path root{directory};
            std::filesystem::create_directories(root);
            const mango::image::Surface surface{
                int(pixels.m_size), int(pixels.m_size),
                mango::image::Format(32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                pixels.m_row_pitch, pixels.m_bytes.data()};
            PPR_TEST_ASSERT(static_cast<bool>(surface.save((root / (std::string{name} + ".png")).string())));
        }

        void addAdjacentGridChunk_(GridPass::GridStagedFrame &stage) {
            stage.m_tiles.push_back(
                GridTileSubmission{.m_dirty_mask = 1u, .m_tile_range = {128, 0, 256, 128}, .m_chunk_id = 1u});
            auto upload = stage.m_uploads[0];
            upload.m_chunk_id = 1u;
            upload.m_payload.m_rect[0] = 128.0f;
            upload.m_payload.m_rect[2] = 256.0f;
            stage.m_uploads.push_back(std::move(upload));
        }

        void fillGridParticles_(GridPass::GridStagedFrame &stage, const u32 phase, const bool offcenter = false) {
            stage.m_particle_batches.clear();
            for (u32 chunk = 0u; chunk < stage.m_uploads.size(); ++chunk) {
                std::ranges::fill(stage.m_uploads[chunk].m_cell_materials, u16{0u});
                auto records = std::make_shared<Array<GridParticleInstance>>();
                records->reserve(GridPass::kCellsPerChunk);
                for (u32 y = 0u; y < 128u; ++y) {
                    for (u32 x = 0u; x < 128u; ++x) {
                        const double offset = offcenter ? (x % 2u == 0u ? 0.01 : 0.99) : 0.5;
                        records->push_back(
                            GridParticleInstance{
                                u64(chunk) * 16384u + y * 128u + x + 1u, double(chunk * 128u + x) + offset,
                                double(y) + offset, 1.0, 1.0, 300.0, 1u, phase});
                    }
                }
                stage.m_particle_batches.push_back(GridParticleBatch{chunk, std::move(records)});
            }
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
                weights = {1.0 - static_cast<double>(fx) - static_cast<double>(fy), static_cast<double>(fx),
                    static_cast<double>(fy)};
            } else {
                vertices = {{{cx + 1, cy + 1}, {cx, cy + 1}, {cx + 1, cy}}};
                weights = {static_cast<double>(fx) + static_cast<double>(fy) - 1.0, 1.0 - static_cast<double>(fx),
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

        PPR_UNIT_TEST (grid_stochastic_world_uvs_mips_and_particle_seams) {
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
            fillGridParticles_(stage, 1u);
            std::ignore = captureOpticalGrid_(grid, stage, 2u);
            const auto liquid = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            saveGridQa_(liquid, "pattern-liquid-chunk-seam");
            // Identical world material has identical sampling across the two
            // representations and the owner/chunk seam at world x=128.
            PPR_TEST_ASSERT(gridPixelDifference_(solid, liquid) < 0.3);
            fillGridParticles_(stage, 2u, true);
            std::ignore = captureOpticalGrid_(grid, stage, 3u);
            const auto gas = drawOpticalGrid_(grid, 120.0f, 56.0f, 16.0f, 256u);
            saveGridQa_(gas, "pattern-gas-offcenter-chunk-seam");
            PPR_TEST_ASSERT(gridPixelDifference_(liquid, gas) < 0.3);
            const auto subpixel = drawOpticalGrid_(grid, 120.015625f, 56.0f, 16.0f, 256u);
            saveGridQa_(subpixel, "pattern-gas-subpixel-pan");
            PPR_TEST_ASSERT(gridPixelDifference_(gas, subpixel) < 8.0);

            // A whole source period must no longer repeat the same patch.
            double repetition_difference = 0.0;
            for (u32 y = 32u; y < 224u; ++y) {
                for (u32 x = 16u; x < 112u; ++x) {
                    repetition_difference += texelDist_(gateTexel_(gas, x, y), gateTexel_(gas, x + 128u, y));
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

        PPR_UNIT_TEST (grid_translucent_solid_borders_and_particle_exteriors) {
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
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 64u; x < 128u; ++x) {
                    stage.m_uploads[0].m_cell_materials[y * 128u + x] = 0u;
                }
            }
            auto records = std::make_shared<Array<GridParticleInstance>>();
            records->reserve(8192u);
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 64u; x < 128u; ++x) {
                    records->push_back(
                        GridParticleInstance{
                            u64(y) * 128u + x + 1u, double(x) + 0.5, double(y) + 0.5, 1.0, 1.0, 300.0, 2u, 1u});
                }
            }
            stage.m_particle_batches.push_back(GridParticleBatch{0u, records});
            std::ignore = captureOpticalGrid_(grid, stage, 5u);
            const auto liquid = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(liquid, "glass-liquid-exterior-border");
            auto gases = std::make_shared<Array<GridParticleInstance>>(*records);
            for (auto &particle: *gases) {
                particle.m_phase = 2u;
            }
            stage.m_particle_batches[0].m_particles = gases;
            std::ignore = captureOpticalGrid_(grid, stage, 6u);
            const auto gas = drawOpticalGrid_(grid, 60.0f, 60.0f, 8.0f, 256u);
            saveGridQa_(gas, "glass-gas-exterior-border");
            PPR_TEST_ASSERT(gridPixelDifference_(liquid, gas) < 0.1);
            // Particle phase cannot turn an exterior into a two-solid material
            // interface. The exterior casts a shadow only on its matter side.
            double exterior_darkening = 0.0;
            for (u32 y = 32u; y < 224u; ++y) {
                for (u32 x = 136u; x < 144u; ++x) {
                    exterior_darkening += int(gateTexel_(internal, x, y)[0]) - int(gateTexel_(gas, x, y)[0]);
                }
            }
            PPR_TEST_ASSERT(exterior_darkening / (192.0 * 8.0) > 3.0);
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
                                    u64(y) * 128u + x + 1u, double(x) + 0.5, double(y) + 0.5, 1.0, 1.0, 300.0, 1u, 1u});
                        }
                    }
                    stage.m_particle_batches.push_back(GridParticleBatch{0u, std::move(records)});
                }
                std::ignore = captureOpticalGrid_(grid, stage, scenario + 1u);
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, snapshot));
                const auto wait_for_idle = [&]() { return renderer->waitOnHost(); };
                PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle, true));
                const auto prepare = [&](const PreRenderContext &context) {
                    return grid.prepareBackground(context, snapshot, viewport, scissor);
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
            records->push_back(GridParticleInstance{1u, 64.5, 64.5, 1.0, 1.0, 300.0, 1u, 1u});
            records->push_back(GridParticleInstance{2u, 65.5, 64.5, 1.0, 1.0, 300.0, 1u, 1u});
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
            PPR_TEST_ASSERT(changedOpticalPixels_(centered, displaced) == 0u);
            auto half = std::make_shared<Array<GridParticleInstance>>(*offcenter);
            (*half)[0].m_volume_m3 = 0.5;
            stage.m_particle_batches[0].m_particles = half;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            const auto partial = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            PPR_TEST_ASSERT(gateTexel_(partial, 32u, 96u)[0] < 250u);
            PPR_TEST_ASSERT(gateTexel_(partial, 64u, 96u)[0] > 250u);
            stage.m_particle_batches[0].m_particles = offcenter;
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            for (u32 frame = 0u; frame < 6u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f);
            }
            const auto stable = grid.observations();
            PPR_TEST_ASSERT(stable.m_particle_instances == 2u and stable.m_particle_buffer_bytes == 64u);
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
            PPR_TEST_ASSERT(grid.observations().m_particle_root_bindings == 0u);
            stage.m_particle_batches[0].m_particles = std::make_shared<const Array<GridParticleInstance>>();
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            for (u32 frame = 0u; frame < 6u; ++frame) {
                std::ignore = drawOpticalGrid_(grid, 63.5f, 63.5f, 4.0f, 64u);
            }
            const auto empty = grid.observations();
            PPR_TEST_ASSERT(empty.m_particle_instances == 0u and empty.m_particle_buffer_bytes == 0u);
            PPR_TEST_ASSERT(empty.m_particle_root_bindings == 0u);
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
                PPR_TEST_ASSERT(settled.m_particle_instances == 2u and settled.m_particle_buffer_bytes == 64u);
                PPR_TEST_ASSERT(settled.m_texture_entries == stable.m_texture_entries);
                PPR_TEST_ASSERT(settled.m_texture_decoded_bytes == stable.m_texture_decoded_bytes);
                PPR_TEST_ASSERT(settled.m_background_target_bytes == empty.m_background_target_bytes);
                PPR_TEST_ASSERT(grid.retainedAtlasCountForTest() == 1u);
            }
        };

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
                const auto wait_for_idle = [&]() { return renderer->waitOnHost(); };
                PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle, true));
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
            const auto wait_for_idle = [&]() { return renderer->waitOnHost(); };
            PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait_for_idle, true));
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
            GateEditorApp test_app{"AssetEditorFlow", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER {
                // Best-effort: never throw during unwind (an assert here
                // while unwinding from a body failure terminates the process
                // and hides both messages). Happy path asserts below.
                std::ignore = test_app.teardown();
            };

            PPR_TEST_ASSERT(not test_app.loadScene(gateMeshDir(), "textured_box.gltf"));
            PPR_TEST_ASSERT(not test_app.tick(TimeSpan{}));

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
            detail::Gate::grid_candidate_world_retains_active_until_matching_activation,
            detail::Gate::grid_optical_controls_and_four_layer_parallax,
            detail::Gate::grid_particle_support_and_resource_repeat,
            detail::Gate::grid_material_shading_cost_samples,
            detail::Gate::grid_stochastic_world_uvs_mips_and_particle_seams,
            detail::Gate::grid_translucent_solid_borders_and_particle_exteriors,
        });
    };

    const UnitTest &renderGatesTests() noexcept {
        return gates;
    }

    const UnitTest &gateEditorLeaf() noexcept {
        return detail::Gate::editor_scene_flow;
    }
} // namespace pP::tests
