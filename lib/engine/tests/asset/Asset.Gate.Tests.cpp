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
import std;

namespace m3d = mango::import3d;

namespace pP::tests::detail {
    namespace Gate {
        constexpr ApplicationDomain kGateDomain{
            .m_is_headless = false,
            .m_is_interactive = true,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = true,
        };

        struct GateTestApp : Application {
            explicit GateTestApp(const std::string_view name, const std::span<const char *const> argv)
                : Application(kGateDomain, name, argv) {
            }

            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        struct GateEditorApp : ApplicationEditor {
            explicit GateEditorApp(const std::string_view name, const std::span<const char *const> argv)
                : ApplicationEditor(name, argv) {
            }

            [[nodiscard]] std::error_code boot() { return ApplicationEditor::initialize(); }
            [[nodiscard]] std::error_code teardown() { return ApplicationEditor::shutdown(); }
            [[nodiscard]] std::error_code tick(const TimeSpan dt) { return ApplicationEditor::update(dt); }
        };

        [[nodiscard]] std::filesystem::path gateMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] Expected<image::ImageAsset> decodeFilePng(const std::string_view name) {
            Expected<mem::SharedBuffer> mapped =
                    mem::SharedBuffer::mapFile(gateMeshDir() / std::string{name});
            if (not
                mapped.has_value()) {
                return std::unexpected{mapped.error()};
            }
            return image::decodeToRgba8(
                mapped->getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
        }

        [[nodiscard]] mem::SharedBuffer gatePngBytes(const std::string_view name, const std::array<std::byte, 64u> &rgba) {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_gate_fixtures";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path path = dir / name;
            const mango::image::Surface surface{
                4, 4, mango::image::Format(32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                16u, rgba.data()
            };
            if (not
                static_cast<bool>(surface.save(path.string()))) {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        [[nodiscard]] CameraSnapshot gateCamera_(const float3 &eye, const float3 &target, const float2 &extent) {
            CameraSnapshot snapshot{};
            // Mango lookat takes (target, viewer, up) — not the GL order.
            snapshot.m_view = float4x4::lookat(target, eye, math::axis_y);
            snapshot.m_projection = rhi::getPerspectiveMatrix(
                pi_v<float> / 3.0f, extent.x / extent.y, 0.05f, 100.0f);
            snapshot.m_view_projection = snapshot.m_view * snapshot.m_projection;
            snapshot.m_origin = eye;
            snapshot.m_viewport_size = extent;
            return snapshot;
        }

        [[nodiscard]] rhi::ITexture &targetRef_(const Expected<rhi::ComPtr<rhi::ITexture> > &target) {
            return *(*target).get();
        }

        [[nodiscard]] Expected<rhi::ComPtr<rhi::ITexture> > makeRenderTarget_(rhi::IDevice &device, const u32 size) {
            rhi::TextureDesc desc{};
            desc.type = rhi::TextureType::Texture2D;
            desc.size = {size, size, 1u};
            desc.arrayLength = 1u;
            desc.mipCount = 1u;
            desc.format = rhi::Format::RGBA8Unorm;
            desc.memoryType = rhi::MemoryType::DeviceLocal;
            desc.usage = rhi::TextureUsage::RenderTarget;
            desc.defaultState = rhi::ResourceState::RenderTarget;
            desc.label = "gate render target";
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

        [[nodiscard]] Expected<GatePixels> readback_(
            rhi::IDevice &device, rhi::ITexture &target, const u32 size) {
            shader::ComPtr<ISlangBlob> blob{};
            rhi::SubresourceLayout layout{};
            if (const std::error_code err =
                    make_error_code(device.readTexture(&target, 0u, 0u, blob.writeRef(), &layout))) {
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
        // instance per (scene instance, prim) with the node world matrix.
        [[nodiscard]] std::error_code submitScene_(
            TrianglePass &pass,
            const mesh::SceneAsset &scene,
            const TrianglePass::UploadedScene &uploaded) {
            pass.clearInstances();
            std::size_t prim_cursor = 0u;
            for (const mesh::SceneInstance &instance: scene.m_instances) {
                const std::size_t mesh_index = static_cast<std::size_t>(*instance.m_mesh);
                const std::size_t node_index = static_cast<std::size_t>(*instance.m_node);
                if (mesh_index >= scene.m_meshes.size() or node_index >= scene.m_nodes.size()) {
                    return std::make_error_code(std::errc::invalid_argument);
                }
                const float4x4 &world = scene.m_nodes[node_index].m_world;
                for ([[maybe_unused]] const mesh::MeshPrimitiveRange &prim: scene.m_meshes[mesh_index].m_prims) {
                    if (prim_cursor >= uploaded.m_prims.size()) {
                        return std::make_error_code(std::errc::invalid_argument);
                    }
                    const TrianglePass::UploadedPrimitive &up = uploaded.m_prims[prim_cursor++];
                    MaterialHandle material = up.m_material;
                    if (instance.m_materialOverride != mesh::kInvalidMaterial) {
                        const std::size_t override_index =
                                static_cast<std::size_t>(*instance.m_materialOverride);
                        if (override_index >= uploaded.m_materials.size()) {
                            return std::make_error_code(std::errc::invalid_argument);
                        }
                        material = uploaded.m_materials[override_index];
                    }
                    if (const std::error_code submit_err = pass.submitInstance(up.m_bag, material, world)) {
                        return submit_err;
                    }
                }
            }
            return default_value_v;
        }

        // Final gate (plan §8): textured glTF through the bindless pass with
        // MULTI-pixel DISTINCTIVE texels — the gradient texture is chromatic
        // everywhere a base-color fallback renders grayscale.
        PPR_UNIT_TEST(bindless_textured_box_gate) {
            GateTestApp test_app{"AssetGate", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER { PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER { PPR_TEST_ASSERT(not pass.shutdown()); };

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
            PPR_TEST_ASSERT(not submitScene_(pass, *scene, *uploaded));

            const mesh::StaticMeshAsset &mesh_asset = scene->m_meshes.front();
            const float3 center = mesh_asset.m_bounds.center();
            const float3 size = mesh_asset.m_bounds.size();
            const float max_dim = std::max({size.x, size.y, size.z});
            const float3 eye{center.x + 0.25f * max_dim, center.y + 0.2f * max_dim, center.z + 2.2f * max_dim};
            PPR_TEST_ASSERT(not pass.update(TimeSpan{}, gateCamera_(eye, center, float2{256.0f, 256.0f})));

            const Expected<rhi::ComPtr<rhi::ITexture> > target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(target.has_value());
            Renderer &renderer = test_app.getRenderer();
            PPR_TEST_ASSERT(not renderer.renderToTexture(targetRef_(target), {DrawSubmission{pass}}, ColorAttachmentOps{}));
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
        // inversion — file-tangent w=-w (P0c/P1) is kept, negate at convert.
        PPR_UNIT_TEST(tangent_w_render_arbitration) {
            GateTestApp test_app{"AssetTangentGate", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER { PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER { PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(gateMeshDir(), "textured_quad.glb");
            PPR_TEST_ASSERT(scene.has_value());
            const mesh::StaticMeshAsset &quad = scene->m_meshes.front();
            PPR_TEST_ASSERT(quad.m_verts.size() == 4u);
            PPR_TEST_ASSERT(quad.m_indices.size() == 6u);
            PPR_TEST_ASSERT(not scene->m_mats.empty());
            const mesh::MaterialAsset &quad_mat = scene->m_mats.front();

            // Independent MikkTSpace reference, straight from the fork (not
            // through the converter): soup from converted positions/normals/
            // uvs, tangents read back per corner.
            m3d::Mesh soup{};
            soup.flags = m3d::Vertex::Position | m3d::Vertex::Normal | m3d::Vertex::Texcoord;
            for (std::size_t t = 0u; t < quad.m_indices.size(); t += 3u) {
                m3d::Triangle tri{};
                for (u32 c = 0u; c < 3u; ++c) {
                    const mesh::StaticMeshVertex &src = quad.m_verts[quad.m_indices[t + c]];
                    m3d::Vertex corner{};
                    corner.position =
                            m3d::float32x3{src.m_position[0], src.m_position[1], src.m_position[2]};
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
                tilt_mapped.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::data);
            PPR_TEST_ASSERT(tilt_decoded.has_value());
            const Expected<TextureHandle> tilt_tex = pass.uploadTexture(*tilt_decoded);
            PPR_TEST_ASSERT(tilt_tex.has_value());
            const Expected<TextureBindlessIndex> tilt_slot = pass.textureCache().residentIndex(*tilt_tex);
            PPR_TEST_ASSERT(tilt_slot.has_value());

            mesh::MaterialAsset normal_mat = quad_mat;
            normal_mat.m_twosided = true;
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
                quad.m_verts.front().m_normal[0],
                quad.m_verts.front().m_normal[1],
                quad.m_verts.front().m_normal[2]
            };
            const float normal_len =
                    std::sqrt(face_normal.x * face_normal.x + face_normal.y * face_normal.y + face_normal.z * face_normal.z);
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
                const Expected<rhi::ComPtr<rhi::ITexture> > target = makeRenderTarget_(device, 128u);
                if (not
                    target.has_value()) {
                    pass.clearInstances();
                    return std::unexpected{target.error()};
                }
                Renderer &renderer = test_app.getRenderer();
                const std::error_code render_err =
                        renderer.renderToTexture(targetRef_(target), {DrawSubmission{pass}}, ColorAttachmentOps{});
                pass.clearInstances();
                if (render_err) {
                    return std::unexpected{render_err};
                }
                if (const std::error_code wait_err = renderer.waitOnHost()) {
                    return std::unexpected{wait_err};
                }
                const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 128u);
                if (not
                    pixels.has_value()) {
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
                    quad.m_verts.front().m_normal[0],
                    quad.m_verts.front().m_normal[1],
                    quad.m_verts.front().m_normal[2]
                };
                const float b[3] = {
                    w * (n.y * t[2] - n.z * t[1]),
                    w * (n.z * t[0] - n.x * t[2]),
                    w * (n.x * t[1] - n.y * t[0]),
                };
                constexpr float kTex[3] = {
                    2.0f * 128.0f / 255.0f - 1.0f, 2.0f * 255.0f / 255.0f - 1.0f,
                    2.0f * 128.0f / 255.0f - 1.0f
                };
                float shaded[3] = {
                    t[0] * kTex[0] + b[0] * kTex[1] + n.x * kTex[2],
                    t[1] * kTex[0] + b[1] * kTex[1] + n.y * kTex[2],
                    t[2] * kTex[0] + b[2] * kTex[1] + n.z * kTex[2],
                };
                const float len = std::sqrt(
                    shaded[0] * shaded[0] + shaded[1] * shaded[1] + shaded[2] * shaded[2]);
                shaded[0] /= len;
                shaded[1] /= len;
                shaded[2] /= len;
                constexpr float kLight[3] = {-0.35f, 0.55f, 0.76f};
                const float light_len =
                        std::sqrt(kLight[0] * kLight[0] + kLight[1] * kLight[1] + kLight[2] * kLight[2]);
                const float diff = std::max(0.0f,
                    (shaded[0] * kLight[0] + shaded[1] * kLight[1] + shaded[2] * kLight[2]) / light_len);
                float view[3] = {eye.x - center.x, eye.y - center.y, eye.z - center.z};
                const float view_len = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                view[0] /= view_len;
                view[1] /= view_len;
                view[2] /= view_len;
                float half_v[3] = {
                    kLight[0] / light_len + view[0], kLight[1] / light_len + view[1], kLight[2] / light_len + view[2]
                };
                const float half_len =
                        std::sqrt(half_v[0] * half_v[0] + half_v[1] * half_v[1] + half_v[2] * half_v[2]);
                const float spec = std::pow(
                    std::max(0.0f, (shaded[0] * half_v[0] + shaded[1] * half_v[1] + shaded[2] * half_v[2]) / half_len),
                    64.0f + (8.0f - 64.0f) * quad_mat.m_roughness);
                const float gain = (0.35f + 0.65f * diff) * (1.0f - 0.5f * quad_mat.m_metallic);
                const float spec_gain = spec * (1.0f - quad_mat.m_roughness) * 0.25f;
                return float3{gain + spec_gain, gain + spec_gain, gain + spec_gain};
            };

            const auto closeEnough = [](const float3 &lhs, const float3 &rhs, const float tol) noexcept {
                return std::abs(lhs.x - rhs.x) < tol and std::abs(lhs.y - rhs.y) < tol and
                       std::abs(lhs.z - rhs.z) < tol;
            };

            Array<mesh::StaticMeshVertex> ref_verts(quad.m_verts.begin(), quad.m_verts.end());
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

            Array<mesh::StaticMeshVertex> flip_verts(quad.m_verts.begin(), quad.m_verts.end());
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
                std::abs(ref_lit->x - flip_lit->x) > 0.05f or std::abs(ref_lit->y - flip_lit->y) > 0.05f or
                std::abs(ref_lit->z - flip_lit->z) > 0.05f);

            const Expected<float3> flat_lit = render_center(*ref_bag, *flat_mat);
            PPR_TEST_ASSERT(flat_lit.has_value());
            const TextureHandle plain_resolved[] = {invalid_tex, invalid_tex, invalid_tex, invalid_tex};
            const Expected<MaterialHandle> plain_mat = pass.packMaterial(normal_mat, plain_resolved);
            PPR_TEST_ASSERT(plain_mat.has_value());
            const Expected<float3> plain_lit = render_center(*ref_bag, *plain_mat);
            PPR_TEST_ASSERT(plain_lit.has_value());
            PPR_TEST_ASSERT(closeEnough(*flat_lit, *plain_lit, 0.03f));

            PPR_TEST_ASSERT(not pass.releaseScene(
                TrianglePass::UploadedScene{.m_prims = {}, .m_textures = {*tilt_tex}, .m_materials = {*tilt_mat, *flat_mat, *plain_mat}}));
            PPR_TEST_ASSERT(not pass.bagCache().release(*ref_bag));
            PPR_TEST_ASSERT(not pass.bagCache().release(*flip_bag));
        };

        // §7 editor flow end to end: boot editor, loadScene, per-frame submit
        // via update, render the pass offscreen, release on unload/teardown.
        PPR_UNIT_TEST(editor_scene_flow) {
            GateEditorApp test_app{"AssetEditorFlow", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER { PPR_TEST_ASSERT(not test_app.teardown()); };

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
                box_center.x + 0.25f * box_max, box_center.y + 0.2f * box_max,
                box_center.z + 2.2f * box_max
            };
            PPR_TEST_ASSERT(not test_app.trianglePass().update(TimeSpan{}, gateCamera_(box_eye, box_center, float2{256.0f, 256.0f})));

            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            rhi::IDevice &device = rhi->getDevice();
            const Expected<rhi::ComPtr<rhi::ITexture> > target = makeRenderTarget_(device, 256u);
            PPR_TEST_ASSERT(target.has_value());
            Renderer &renderer = test_app.getRenderer();
            PPR_TEST_ASSERT(not renderer.renderToTexture(
                targetRef_(target), {DrawSubmission{test_app.trianglePass()}}, ColorAttachmentOps{}));
            PPR_TEST_ASSERT(not renderer.waitOnHost());

            const Expected<GatePixels> pixels = readback_(device, targetRef_(target), 256u);
            PPR_TEST_ASSERT(pixels.has_value());
            PPR_TEST_ASSERT(spread_(gateTexel_(*pixels, 128u, 128u)) > 12u);

            PPR_TEST_ASSERT(not test_app.unloadScene());
        };
    } // namespace Gate
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest gate = UnitTest::Named("gate") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Gate::bindless_textured_box_gate,
            detail::Gate::tangent_w_render_arbitration,
            detail::Gate::editor_scene_flow,
        });
    };

    const UnitTest &gateTests() noexcept {
        return gate;
    }
} // namespace pP::tests
