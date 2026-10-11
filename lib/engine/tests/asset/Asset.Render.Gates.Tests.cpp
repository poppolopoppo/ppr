module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>
#include <mango/import3d/mesh.hpp>

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
    [[nodiscard]] safe_ptr<IRhiService> rhiService();

    [[nodiscard]] safe_ptr<IShaderService> shaderService();

    [[nodiscard]] Renderer *renderer();
} // namespace pP::tests::detail::SharedGpu

namespace pP::tests::detail {
    namespace Gate {
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
                .m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u, .m_material_id = 1u};
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{&cached_tile, 1u}));
            GridTilePayload stale_geometry{};
            stale_geometry.m_rect[0] = 2048.0f;
            stale_geometry.m_rect[1] = 2048.0f;
            stale_geometry.m_rect[2] = 2176.0f;
            stale_geometry.m_rect[3] = 2176.0f;
            stale_geometry.m_material = 1u;
            PPR_TEST_ASSERT(grid.requestUpload(GridUploadRequest{0u, std::span{&stale_geometry, 1u}}).has_value());
            const GridTileSubmission offscreen_cells{
                .m_chunk_id = 528u, .m_tile_range = {2048, 2048, 2176, 2176}, .m_dirty_mask = 1u};
            PPR_TEST_ASSERT(not grid.submitTiles(std::span{&offscreen_cells, 1u}));
            GridTilePayload cell_payload{};
            Array<u16> cell_materials;
            cell_materials.resize(GridPass::kCellsPerChunk, 1u);
            PPR_TEST_ASSERT(
                grid.requestUpload(
                        GridUploadRequest{528u, std::span{&cell_payload, 1u}, std::span<const u16>{cell_materials}})
                    .has_value());
            const std::array<GridTileSubmission, 3u> tiles{
                GridTileSubmission{.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_material_id = 1u},
                GridTileSubmission{.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_material_id = 2u},
                GridTileSubmission{.m_chunk_id = 528u, .m_tile_range = {2048, 2048, 2176, 2176}, .m_material_id = 3u},
            };
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            Array<GridTileSubmission> full_capacity;
            for (u32 chunk = 0u; chunk < GridPass::kGridTileCacheCapacity; ++chunk) {
                full_capacity.push_back(
                    GridTileSubmission{.m_chunk_id = chunk, .m_tile_range = {0, 0, 128, 128}, .m_material_id = 1u});
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
                        .m_chunk_id = chunk, .m_tile_range = {x, y, x + 128, y + 128}, .m_dirty_mask = 1u});
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

        PPR_UNIT_TEST (grid_diagnostics_cover_empty_biomes_and_preserve_terrain_modes) {
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
                PPR_TEST_ASSERT(not renderer->retireCompletedFrames());
                const auto pixels = readback_(device, targetRef_(target), 128u);
                PPR_TEST_ASSERT(pixels.has_value());
                return *pixels;
            };
            GridPass::GridStagedFrame source{};
            source.m_content_fingerprint = 77u;
            source.m_has_catalog = true;
            source.m_terrain_visuals.push_back({1u, 0x0000ffffu, 0xffffffffu, 1.0f});
            source.m_tiles.push_back(
                GridTileSubmission{.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u});
            GridPass::GridStagedUpload upload{};
            upload.m_cell_materials.resize(GridPass::kCellsPerChunk, 0u);
            upload.m_diagnostic_indices.resize(GridPass::kCellsPerChunk, 1u);
            source.m_uploads.push_back(std::move(upload));
            source.m_diagnostic_mode = EGridDiagnosticMode::biome;
            source.m_diagnostic_palette = {{}, {0xff0000ffu}};
            source.m_diagnostic_revision = 10u;
            auto invalid = source;
            invalid.m_diagnostic_revision = 11u;
            PPR_TEST_ASSERT(grid.stageWorldCandidate(10u, 20u, invalid) == std::errc::invalid_argument);
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(10u, 20u, source));
            auto ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
            PPR_TEST_ASSERT(ready and not *ready);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(10u, 20u, 77u) == std::errc::operation_not_permitted);
            for (u32 batch = 1u; batch < 128u; ++batch) {
                ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
                PPR_TEST_ASSERT(ready and (*ready == (batch == 127u)));
            }
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(10u, 20u, 77u));
            auto pixels = render();
            auto color = gateTexel_(pixels, 64u, 64u);
            PPR_TEST_ASSERT(color[0] > 240u and color[1] < 10u);

            // Temperature follows the sampled solid cell, while the same
            // material can have a different diagnostic color.
            source.m_diagnostic_mode = EGridDiagnosticMode::temperature;
            source.m_diagnostic_revision = 11u;
            source.m_diagnostic_palette[1].m_color_rgba = 0x00ff00ffu;
            std::ranges::fill(source.m_uploads[0].m_cell_materials, u16{1u});
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(11u, 20u, source));
            for (u32 batch = 0u; batch < 128u; ++batch) {
                ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
                PPR_TEST_ASSERT(ready.has_value());
            }
            PPR_TEST_ASSERT(*ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(11u, 20u, 77u));
            pixels = render();
            color = gateTexel_(pixels, 64u, 64u);
            PPR_TEST_ASSERT(color[1] > 240u and color[0] < 10u);

            source.m_diagnostic_mode = EGridDiagnosticMode::material;
            source.m_diagnostic_revision = 12u;
            source.m_diagnostic_palette.resize(65536u);
            source.m_diagnostic_palette[1].m_color_rgba = 0x0000ffffu;
            source.m_uploads[0].m_diagnostic_indices.clear();
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(12u, 20u, source));
            ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
            PPR_TEST_ASSERT(ready and *ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(12u, 20u, 77u));
            pixels = render();
            color = gateTexel_(pixels, 64u, 64u);
            PPR_TEST_ASSERT(color[2] > 240u and color[0] < 10u);

            // Initial matter occupies nominal empty cells below the authored
            // fill height. Invalidation publishes with the complete edit cohort.
            source.m_diagnostic_mode = EGridDiagnosticMode::initial_matter;
            source.m_diagnostic_revision = 13u;
            source.m_diagnostic_palette = {{}, {0xff0000ffu, 64.0f}};
            std::ranges::fill(source.m_uploads[0].m_cell_materials, u16{0u});
            source.m_uploads[0].m_diagnostic_indices.resize(GridPass::kCellsPerChunk, 1u);
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(13u, 20u, source));
            for (u32 batch = 0u; batch < 128u; ++batch) {
                ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
                PPR_TEST_ASSERT(ready.has_value());
            }
            PPR_TEST_ASSERT(*ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(13u, 20u, 77u));
            pixels = render();
            const auto below = gateTexel_(pixels, 64u, 96u);
            const auto above = gateTexel_(pixels, 64u, 32u);
            PPR_TEST_ASSERT(below[0] > 240u and below[1] < 10u);
            PPR_TEST_ASSERT(above[0] < 80u);
            GridPass::GridStagedFrame edited{};
            edited.m_tiles = source.m_tiles;
            edited.m_has_catalog = source.m_has_catalog;
            edited.m_content_fingerprint = source.m_content_fingerprint;
            edited.m_terrain_visuals = source.m_terrain_visuals;
            edited.m_initial_matter_valid = false;
            edited.m_tiles.push_back(
                GridTileSubmission{.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u});
            auto changed = source.m_uploads[0];
            changed.m_diagnostic_indices.clear();
            changed.m_cell_materials[0] = 1u;
            edited.m_uploads.push_back(changed);
            changed.m_chunk_id = 1u;
            edited.m_uploads.push_back(std::move(changed));
            PPR_TEST_ASSERT(not grid.applyStagedFrame(edited));
            const auto partial = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(partial and not *partial);
            pixels = render();
            color = gateTexel_(pixels, 64u, 96u);
            PPR_TEST_ASSERT(color[0] > 240u and color[1] < 10u);
            const auto published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 1u);
            PPR_TEST_ASSERT(published and *published);
            pixels = render();
            color = gateTexel_(pixels, 64u, 96u);
            PPR_TEST_ASSERT(color[0] < 80u);
            source.m_diagnostic_revision = 14u;
            source.m_diagnostic_palette[1].m_fill_height_fine = 4096.0f;
            source.m_uploads[0].m_cell_materials[64u * 128u + 64u] = 1u;
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(14u, 20u, source));
            for (u32 batch = 0u; batch < 128u; ++batch) {
                ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 8u);
                PPR_TEST_ASSERT(ready.has_value());
            }
            PPR_TEST_ASSERT(*ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(14u, 20u, 77u));
            model.m_origin = float3{63.5f, 63.5f, 0.0f};
            model.m_ortho_scale = 1.0f / 128.0f;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
            pixels = render();
            bool mixed = false;
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 0u; x < 128u; ++x) {
                    const auto edge = gateTexel_(pixels, x, y);
                    mixed = mixed or (edge[0] > 40u and edge[2] > 40u and edge[1] < 8u);
                }
            }
            PPR_TEST_ASSERT(mixed);
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == 0u);
            // The reference path also consumes invalidation without new cells.
            edited.m_tiles = source.m_tiles;
            edited.m_uploads.clear();
            PPR_TEST_ASSERT(not grid.applyStagedFrame(edited));
            const auto wait = [&] { return renderer->waitOnHost(); };
            PPR_TEST_ASSERT(not grid.prepareCellUploads(device, wait, true));
            pixels = render();
            for (u32 y = 0u; y < 128u; ++y) {
                for (u32 x = 0u; x < 128u; ++x) {
                    PPR_TEST_ASSERT(gateTexel_(pixels, x, y)[0] < 80u);
                }
            }
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
                GridTileSubmission{.m_chunk_id = 0u, .m_tile_range = {0, 0, 128, 128}, .m_dirty_mask = 1u});
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
                GridTileSubmission{.m_chunk_id = 1u, .m_tile_range = {128, 0, 256, 128}, .m_dirty_mask = 1u});
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
                    GridTileSubmission{.m_chunk_id = chunk, .m_tile_range = {x, 0, x + 128, 128}, .m_dirty_mask = 1u});
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

        PPR_UNIT_TEST (grid_shared_contours_match_cpu_at_four_chunk_seams) {
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
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            const std::array<u32, 4u> chunks{0u, 1u, 33u, 32u};
            const std::array<u16, 4u> materials{1u, 2u, 4u, 3u};
            GridPass::GridStagedFrame stage{};
            stage.m_has_catalog = true;
            for (const u32 chunk: chunks) {
                const i32 x = static_cast<i32>(chunk % 32u) * 128;
                const i32 y = static_cast<i32>(chunk / 32u) * 128;
                stage.m_tiles.push_back(
                    GridTileSubmission{
                        .m_chunk_id = chunk, .m_tile_range = {x, y, x + 128, y + 128}, .m_dirty_mask = 1u});
            }
            const auto points = math::contourPoints();
            const auto cases = math::contourCases();
            const auto cross = [](const math::ContourPoint a, const math::ContourPoint b,
                                   const math::ContourPoint point) {
                return (b.m_x - a.m_x) * (point.m_y - a.m_y) - (b.m_y - a.m_y) * (point.m_x - a.m_x);
            };
            for (const float metric: {1.0f, 1.0f / 3.0f}) {
                stage.m_cell_size_m = metric;
                PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_origin = float3{127.5f * metric, 127.5f * metric, 0.0f};
                model.m_ortho_scale = metric / 128.0f;
                Camera camera;
                camera.updateModel(
                    TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
                for (u32 mask = 0u; mask < 16u; ++mask) {
                    for (u32 corner = 0u; corner < 4u; ++corner) {
                        Array<u16> cells;
                        cells.resize(GridPass::kCellsPerChunk, 0u);
                        const u32 x = corner == 0u or corner == 3u ? 127u : 0u;
                        const u32 y = corner < 2u ? 127u : 0u;
                        cells[y * 128u + x] = (mask & (1u << corner)) != 0u ? materials[corner] : 0u;
                        GridTilePayload payload{};
                        const auto uploaded = grid.requestUpload(
                            GridUploadRequest{chunks[corner], std::span{&payload, 1u}, std::span<const u16>{cells}});
                        PPR_TEST_ASSERT(uploaded and *uploaded == 1u);
                    }
                    const auto published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 4u);
                    PPR_TEST_ASSERT(published and *published);
                    PPR_TEST_ASSERT(not renderer->renderToTexture(
                        targetRef_(target), {DrawSubmission{"shared contour seam", draw}}, {},
                        PreRenderCallback{prepare}));
                    grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                    PPR_TEST_ASSERT(not renderer->waitOnHost());
                    const auto pixels = readback_(device, targetRef_(target), 128u);
                    PPR_TEST_ASSERT(pixels.has_value());
                    if (mask == 1u and metric == 1.0f) {
                        const auto edge = gateTexel_(*pixels, 2u, 66u);
                        PPR_TEST_ASSERT(edge[0u] > 90u and edge[0u] < 150u);
                    }
                    if (metric == 1.0f / 3.0f and (mask == 1u or mask == 5u or mask == 10u or mask == 15u)) {
                        const auto directory = std::filesystem::current_path() / "temp_gate_fixtures";
                        std::error_code error;
                        std::filesystem::create_directories(directory, error);
                        PPR_TEST_ASSERT(not error);
                        const mango::image::Surface surface{
                            128, 128,
                            mango::image::Format(
                                32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                            static_cast<u32>(pixels->m_row_pitch), pixels->m_bytes.data()};
                        const auto path = directory / std::format("grid-contour-case-{}.png", mask);
                        PPR_TEST_ASSERT(static_cast<bool>(surface.save(path.string())));
                    }
                    const auto &shape = cases[mask];
                    for (u32 y = 2u; y < 128u; y += 5u) {
                        for (u32 x = 2u; x < 128u; x += 5u) {
                            const math::ContourPoint point{
                                (static_cast<float>(x) + 0.5f) / 128.0f,
                                1.0f - (static_cast<float>(y) + 0.5f) / 128.0f};
                            bool near_edge = false;
                            for (u32 edge = 0u; edge < shape.m_segment_count; ++edge) {
                                const auto segment = shape.m_segments[edge];
                                const auto a = points[segment.m_begin];
                                const auto b = points[segment.m_end];
                                const float dx = b.m_x - a.m_x;
                                const float dy = b.m_y - a.m_y;
                                const float t = std::clamp(
                                    ((point.m_x - a.m_x) * dx + (point.m_y - a.m_y) * dy) / (dx * dx + dy * dy), 0.0f,
                                    1.0f);
                                const float ex = point.m_x - a.m_x - t * dx;
                                const float ey = point.m_y - a.m_y - t * dy;
                                near_edge = near_edge or ex * ex + ey * ey < 0.001f;
                            }
                            if (near_edge) {
                                continue;
                            }
                            u32 component{};
                            for (u32 triangle = 0u; triangle < shape.m_triangle_count; ++triangle) {
                                const auto face = shape.m_triangles[triangle];
                                if (cross(points[face.m_a], points[face.m_b], point) >= 0.0f and
                                    cross(points[face.m_b], points[face.m_c], point) >= 0.0f and
                                    cross(points[face.m_c], points[face.m_a], point) >= 0.0f) {
                                    component = face.m_solid_corners;
                                    break;
                                }
                            }
                            u32 dominant{};
                            for (const u32 corner: {0u, 1u, 3u, 2u}) {
                                if ((component & (1u << corner)) != 0u) {
                                    dominant = corner;
                                    break;
                                }
                            }
                            u32 material = materials[dominant];
                            // Distinct labels use midpoint-cut corner islands;
                            // the spatially first label fills the remaining junction.
                            for (u32 corner = 0u; corner < 4u; ++corner) {
                                if ((component & (1u << corner)) == 0u or corner == dominant) {
                                    continue;
                                }
                                const float distance = std::abs(point.m_x - points[corner].m_x) +
                                                       std::abs(point.m_y - points[corner].m_y);
                                near_edge = near_edge or std::abs(distance - 0.5f) < 0.03f;
                                if (distance < 0.5f) {
                                    material = materials[corner];
                                }
                            }
                            if (near_edge) {
                                continue;
                            }
                            const auto texel = gateTexel_(*pixels, x, y);
                            for (u32 channel = 0u; channel < 3u; ++channel) {
                                const i32 expected = component != 0u ? ((material & (1u << channel)) != 0u ? 217 : 38)
                                                     : channel == 2u ? 51
                                                                     : 25;
                                PPR_TEST_ASSERT(std::abs(static_cast<i32>(texel[channel]) - expected) <= 2);
                            }
                        }
                    }
                }
            }
            // Protected samples beyond each world corner must not generate
            // atlas addresses. Nominal rectangles still clip the actual draw.
            for (const u32 chunk: {0u, 31u, 1023u, 992u}) {
                GridPass::GridStagedFrame boundary_stage{};
                boundary_stage.m_has_catalog = true;
                const i32 min_x = static_cast<i32>(chunk % 32u) * 128;
                const i32 min_y = static_cast<i32>(chunk / 32u) * 128;
                boundary_stage.m_tiles.push_back(
                    GridTileSubmission{
                        .m_chunk_id = chunk,
                        .m_tile_range = {min_x, min_y, min_x + 128, min_y + 128},
                        .m_dirty_mask = 1u});
                PPR_TEST_ASSERT(not grid.applyStagedFrame(boundary_stage));
                Array<u16> cells;
                cells.resize(GridPass::kCellsPerChunk, 1u);
                GridTilePayload payload{};
                const auto uploaded = grid.requestUpload(
                    GridUploadRequest{chunk, std::span{&payload, 1u}, std::span<const u16>{cells}});
                PPR_TEST_ASSERT(uploaded and *uploaded == 1u);
                const auto published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 4u);
                PPR_TEST_ASSERT(published and *published);
                const bool left = chunk % 32u == 0u;
                const bool bottom = chunk / 32u == 0u;
                CameraModel model{};
                model.m_camera_mode = ECameraProjection::orthographic;
                model.m_origin = float3{left ? -0.5f : 4095.5f, bottom ? -0.5f : 4095.5f, 0.0f};
                model.m_ortho_scale = 1.0f / 128.0f;
                Camera camera;
                camera.updateModel(
                    TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"protected world corner", draw}}, {},
                    PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                const auto pixels = readback_(device, targetRef_(target), 128u);
                PPR_TEST_ASSERT(pixels.has_value());
                const auto inside = gateTexel_(*pixels, left ? 96u : 32u, bottom ? 32u : 96u);
                const auto outside = gateTexel_(*pixels, left ? 32u : 96u, bottom ? 96u : 32u);
                PPR_TEST_ASSERT(inside[0u] > 160u and inside[1u] < 80u);
                PPR_TEST_ASSERT(outside[0u] < 80u and outside[1u] < 80u);
            }
            PPR_TEST_ASSERT(grid.atlasSerializationCount() == 0u);
        };

        PPR_UNIT_TEST (grid_material_contours_partition_solid_chunk_seams) {
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
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            const std::array<u32, 4u> chunks{0u, 1u, 33u, 32u};
            GridPass::GridStagedFrame stage{};
            stage.m_has_catalog = true;
            stage.m_cell_size_m = 1.0f / 3.0f;
            for (const u32 chunk: chunks) {
                const i32 x = static_cast<i32>(chunk % 32u) * 128;
                const i32 y = static_cast<i32>(chunk / 32u) * 128;
                stage.m_tiles.push_back(
                    GridTileSubmission{
                        .m_chunk_id = chunk, .m_tile_range = {x, y, x + 128, y + 128}, .m_dirty_mask = 1u});
            }
            PPR_TEST_ASSERT(not grid.applyStagedFrame(stage));
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{127.5f / 3.0f, 127.5f / 3.0f, 0.0f};
            model.m_ortho_scale = 1.0f / (3.0f * 128.0f);
            Camera camera;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));

            struct MaterialCase {
                std::array<u16, 4u> m_ids;
                u32 m_dominant_corner;
            };
            Array<MaterialCase> fixtures;
            for (u32 mask = 0u; mask < 16u; ++mask) {
                MaterialCase fixture{};
                u32 count{};
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    fixture.m_ids[corner] = (mask & (1u << corner)) != 0u ? 1u : 2u;
                    count += (mask >> corner) & 1u;
                }
                // A strict majority wins; a two/two tie uses the bottom-left label.
                const u16 dominant = count > 2u ? 1u : count < 2u ? 2u : fixture.m_ids[0u];
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    if (fixture.m_ids[corner] == dominant) {
                        fixture.m_dominant_corner = corner;
                        break;
                    }
                }
                fixtures.push_back(fixture);
            }
            fixtures.push_back({{1u, 1u, 2u, 4u}, 0u});
            fixtures.push_back({{1u, 2u, 1u, 4u}, 0u});
            fixtures.push_back({{1u, 2u, 4u, 3u}, 0u});
            fixtures.push_back({{1u, 2u, 3u, 2u}, 1u});
            fixtures.push_back({{60001u, 65530u, 60004u, 60003u}, 0u});
            fixtures.push_back({{65530u, 60001u, 65530u, 60001u}, 0u});

            // Independent analytic polygons: midpoint cuts, no shader/table oracle.
            const auto contains = [](const u32 mask, const float x, const float y) {
                if (mask == 3u) {
                    return y <= 0.5f;
                }
                if (mask == 6u) {
                    return x >= 0.5f;
                }
                if (mask == 12u) {
                    return y >= 0.5f;
                }
                if (mask == 9u) {
                    return x <= 0.5f;
                }
                return ((mask & 1u) != 0u and x + y <= 0.5f) or
                       ((mask & 2u) != 0u and 1.0f - x + y <= 0.5f) or
                       ((mask & 4u) != 0u and 2.0f - x - y <= 0.5f) or
                       ((mask & 8u) != 0u and x + 1.0f - y <= 0.5f);
            };
            for (const auto &fixture: fixtures) {
                for (u32 corner = 0u; corner < 4u; ++corner) {
                    Array<u16> cells;
                    cells.resize(GridPass::kCellsPerChunk, 0u);
                    const u32 x = corner == 0u or corner == 3u ? 127u : 0u;
                    const u32 y = corner < 2u ? 127u : 0u;
                    cells[y * 128u + x] = fixture.m_ids[corner];
                    GridTilePayload payload{};
                    const auto uploaded = grid.requestUpload(
                        GridUploadRequest{chunks[corner], std::span{&payload, 1u}, std::span<const u16>{cells}});
                    PPR_TEST_ASSERT(uploaded and *uploaded == 1u);
                }
                const auto published = grid.tryPrepareCellUploads(device, renderer->completedFenceValue(), 4u);
                PPR_TEST_ASSERT(published and *published);
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"material contour seam", draw}}, {},
                    PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                const auto pixels = readback_(device, targetRef_(target), 128u);
                PPR_TEST_ASSERT(pixels.has_value());
                for (u32 y = 2u; y < 128u; y += 5u) {
                    for (u32 x = 2u; x < 128u; x += 5u) {
                        const float px = (static_cast<float>(x) + 0.5f) / 128.0f;
                        const float py = 1.0f - (static_cast<float>(y) + 0.5f) / 128.0f;
                        // Avoid exact raster/contour ties, including metric roundoff.
                        if (std::min(
                                {std::abs(px - 0.5f), std::abs(py - 0.5f), std::abs(px + py - 0.5f),
                                    std::abs(px + py - 1.5f), std::abs(px - py - 0.5f), std::abs(py - px - 0.5f)}) <
                            0.02f) {
                            continue;
                        }
                        u16 material = fixture.m_ids[fixture.m_dominant_corner];
                        for (u32 corner = 0u; corner < 4u; ++corner) {
                            const u16 candidate = fixture.m_ids[corner];
                            if (candidate == fixture.m_ids[fixture.m_dominant_corner]) {
                                continue;
                            }
                            u32 mask{};
                            for (u32 other = 0u; other < 4u; ++other) {
                                mask |= fixture.m_ids[other] == candidate ? 1u << other : 0u;
                            }
                            if (contains(mask, px, py)) {
                                material = candidate;
                            }
                        }
                        const auto texel = gateTexel_(*pixels, x, y);
                        for (u32 channel = 0u; channel < 3u; ++channel) {
                            const i32 expected = (material & (1u << channel)) != 0u ? 217 : 38;
                            PPR_TEST_ASSERT(std::abs(static_cast<i32>(texel[channel]) - expected) <= 2);
                        }
                    }
                }
            }
        };

        PPR_UNIT_TEST (grid_catalog_textures_use_sparse_ids_and_world_uvs) {
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
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(16u);
            PPR_TEST_ASSERT(not storage.materialize());
            auto bytes = storage.getMutableData();
            PPR_TEST_ASSERT(bytes.has_value());
            for (u32 pixel = 0u; pixel < 4u; ++pixel) {
                (*bytes)[pixel * 4u] = (pixel % 2u == 0u) ? std::byte{255} : std::byte{0};
                (*bytes)[pixel * 4u + 1u] = std::byte{0};
                (*bytes)[pixel * 4u + 2u] = (pixel % 2u == 0u) ? std::byte{0} : std::byte{255};
                (*bytes)[pixel * 4u + 3u] = std::byte{255};
            }
            mem::SharedBuffer frozen;
            PPR_TEST_ASSERT(not storage.moveToShared(&frozen));
            image::ImageAsset image{};
            image.m_width = 2u;
            image.m_height = 2u;
            image.m_storage = frozen;
            image.m_subresources.push_back(
                {.m_view = frozen.subspan(0u, 16u), .m_row_pitch = 8u, .m_slice_pitch = 16u});
            GridPass::GridStagedFrame stage{};
            stage.m_has_catalog = true;
            stage.m_cell_size_m = 1.0f / 3.0f;
            stage.m_boundary_material_id = 16u;
            stage.m_content_fingerprint = 42u;
            stage.m_terrain_visuals.push_back({16u, 0x00ff00ffu, 0xffffffffu, 1.0f});
            stage.m_terrain_visuals.push_back({257u, 0x80ffffffu, 0u, 0.75f});
            stage.m_terrain_images.push_back(image);
            for (const u32 chunk: {0u, 1u, 32u, 33u}) {
                const i32 x = static_cast<i32>(chunk % 32u) * 128;
                const i32 y = static_cast<i32>(chunk / 32u) * 128;
                stage.m_tiles.push_back(
                    {.m_chunk_id = chunk, .m_tile_range = {x, y, x + 128, y + 128}, .m_dirty_mask = 1u});
                GridPass::GridStagedUpload upload{};
                upload.m_chunk_id = chunk;
                upload.m_cell_materials.assign(GridPass::kCellsPerChunk, 257u);
                stage.m_uploads.push_back(std::move(upload));
            }
            auto header = stage;
            header.m_uploads.clear();
            PPR_TEST_ASSERT(not grid.stageWorldCandidate(1u, 2u, header, false));
            auto partial = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 4u);
            PPR_TEST_ASSERT(partial and not *partial);
            PPR_TEST_ASSERT(grid.activateWorldCandidate(1u, 2u, 42u) == std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(
                grid.appendWorldCandidate(9u, 2u, std::span{stage.m_uploads}.first(2u), false) ==
                std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(not grid.appendWorldCandidate(1u, 2u, std::span{stage.m_uploads}.first(2u), false));
            partial = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 4u);
            PPR_TEST_ASSERT(partial and not *partial);
            PPR_TEST_ASSERT(not grid.appendWorldCandidate(1u, 2u, std::span{stage.m_uploads}.subspan(2u), true));
            const auto ready = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 4u);
            PPR_TEST_ASSERT(ready and *ready);
            PPR_TEST_ASSERT(not grid.activateWorldCandidate(1u, 2u, 42u));
            CameraModel model{};
            model.m_camera_mode = ECameraProjection::orthographic;
            model.m_origin = float3{127.5f / 3.0f, 127.5f / 3.0f, 0.0f};
            model.m_ortho_scale = 1.0f / (3.0f * 128.0f);
            Camera camera;
            camera.updateModel(TimeSpan{}, model, Viewport{PixelRect{int2{0, 0}, int2{128, 128}}, ViewportLayout{}});
            PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
            const auto target = makeRenderTarget_(device, 128u);
            PPR_TEST_ASSERT(target.has_value());
            const auto prepare = [&](const PreRenderContext &context) { return grid.prepareGpuVisibility(context); };
            const auto draw = [&](const DrawContext &context) { return grid.renderGpuIndirect(context); };
            PPR_TEST_ASSERT(not renderer->renderToTexture(
                targetRef_(target), {DrawSubmission{"catalog seam", draw}}, {}, PreRenderCallback{prepare}));
            grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
            PPR_TEST_ASSERT(not renderer->waitOnHost());
            const auto pixels = readback_(device, targetRef_(target), 128u);
            PPR_TEST_ASSERT(pixels.has_value());
            const float tint_red = std::pow((128.0f / 255.0f + 0.055f) / 1.055f, 2.4f);
            for (const u32 y: {16u, 48u, 80u, 112u}) {
                for (const u32 x: {16u, 48u, 80u, 112u}) {
                    const float fine_x = 127.5f + (static_cast<float>(x) + 0.5f) / 128.0f;
                    const float uv = fine_x / (3.0f * 0.75f);
                    const float coordinate = (uv - std::floor(uv)) * 2.0f - 0.5f;
                    const float fraction = coordinate - std::floor(coordinate);
                    const bool left_red = static_cast<i32>(std::floor(coordinate)) % 2 == 0;
                    const float red = left_red ? 1.0f - fraction : fraction;
                    const auto texel = gateTexel_(*pixels, x, y);
                    PPR_TEST_ASSERT(
                        std::abs(
                            static_cast<i32>(texel[0u]) - static_cast<i32>(std::lround(255.0f * tint_red * red))) <= 3);
                    PPR_TEST_ASSERT(texel[1u] <= 2u);
                    PPR_TEST_ASSERT(
                        std::abs(static_cast<i32>(texel[2u]) - static_cast<i32>(std::lround(255.0f * (1.0f - red)))) <=
                        3);
                }
            }
            auto changed = stage;
            changed.m_terrain_visuals[1u].m_color_rgba = 0xffffffffu;
            PPR_TEST_ASSERT(grid.applyStagedFrame(changed) == std::errc::invalid_argument);

            // Epoch reset discards cache handles even when decoded images survive.
            // Replace before redrawing the old world to exercise that null-cache seam.
            for (const bool device_loss: {false, true}) {
                PPR_TEST_ASSERT(not (device_loss ? grid.notifyDeviceLost() : grid.shutdown()));
                PPR_TEST_ASSERT(not grid.initialize(*rhi, *shader, std::filesystem::current_path()));
                PPR_TEST_ASSERT(not grid.update(TimeSpan{}, camera.getSnapshot()));
                const u64 job = device_loss ? 31u : 30u;
                PPR_TEST_ASSERT(not grid.stageWorldCandidate(job, job, stage));
                const auto replacement = grid.advanceWorldCandidate(device, renderer->completedFenceValue(), 4u);
                PPR_TEST_ASSERT(replacement and *replacement);
                PPR_TEST_ASSERT(not grid.activateWorldCandidate(job, job, 42u));
                PPR_TEST_ASSERT(grid.worldCandidateAdmissionReady(renderer->completedFenceValue()));
                PPR_TEST_ASSERT(grid.observations().m_texture_entries == 1u);
                PPR_TEST_ASSERT(not renderer->renderToTexture(
                    targetRef_(target), {DrawSubmission{"textured epoch replacement", draw}}, {},
                    PreRenderCallback{prepare}));
                grid.notifyFrameSubmitted(renderer->lastSubmittedFenceValue());
                PPR_TEST_ASSERT(not renderer->waitOnHost());
                const auto restored = readback_(device, targetRef_(target), 128u);
                PPR_TEST_ASSERT(restored.has_value());
                PPR_TEST_ASSERT(gateTexel_(*restored, 64u, 64u) == gateTexel_(*pixels, 64u, 64u));
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
            detail::Gate::pre_render_preparation_orders_and_rejects_submission,
            detail::Gate::grid_compute_visibility_matches_reference_and_draws,
            detail::Gate::grid_shared_contours_match_cpu_at_four_chunk_seams,
            detail::Gate::grid_material_contours_partition_solid_chunk_seams,
            detail::Gate::grid_catalog_textures_use_sparse_ids_and_world_uvs,
            detail::Gate::grid_candidate_world_retains_active_until_matching_activation,
            detail::Gate::grid_diagnostics_cover_empty_biomes_and_preserve_terrain_modes,
        });
    };

    const UnitTest &renderGatesTests() noexcept {
        return gates;
    }

    const UnitTest &gateEditorLeaf() noexcept {
        return detail::Gate::editor_scene_flow;
    }
} // namespace pP::tests
