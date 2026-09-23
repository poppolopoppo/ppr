module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

#include <mango/image/image.hpp>

module engine.tests.asset;

import engine.core;
import engine.math;
import engine.app;
import engine.rhi;
import engine.shader;
import engine.image;
import engine.mesh;
import std;

namespace pP::tests::detail {
    namespace Gpu {
        // GPU rows are plain float arrays (Gate 3 C2); mango float4 is the
        // expected side only (float4 == float4 yields a simd mask, not bool).
        [[nodiscard]] bool float4Equal_(const float (&lhs)[4], const float4 &rhs) noexcept {
            return lhs[0] == rhs.x
            and lhs[1] == rhs.y
            and lhs[2] == rhs.z
            and lhs[3] == rhs.w;
        }

        PPR_UNIT_TEST (handles_default_invalid) {
            PPR_TEST_ASSERT(not pP::isValid(TextureHandle{}));
            PPR_TEST_ASSERT(not pP::isValid(MaterialHandle{}));
            PPR_TEST_ASSERT(not pP::isValid(TriangleBagHandle{}));
            PPR_TEST_ASSERT(*kNoTexture == 0xFFFFFFFFu);
            PPR_TEST_ASSERT(sizeof(GpuMaterial) == 80u);
            PPR_TEST_ASSERT(PPR_OFFSETOF(GpuMaterial, m_textures) == 48u);
            PPR_TEST_ASSERT(sizeof(GpuTextureRefs) == 16u);
            PPR_TEST_ASSERT(sizeof(GpuMaterialFlags) == 4u);
        };

        PPR_UNIT_TEST (build_material_maps_factors) {
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mesh::MaterialAsset{}, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT(float4Equal_(gpu->m_base_color, float4{1.0f, 1.0f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_emissive_metallic, float4{0.0f, 0.0f, 0.0f, 1.0f}));
            PPR_TEST_ASSERT(float4Equal_(gpu->m_rough_alpha_occl_nscale, float4{1.0f, 0.5f, 1.0f, 1.0f}));
            PPR_TEST_ASSERT(gpu->m_textures.m_albedo == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_metallic_roughness == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_normal == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_textures.m_emissive == kNoTexture);
            PPR_TEST_ASSERT(gpu->m_flags.m_bits == 0u);
        };

        PPR_UNIT_TEST (build_material_texcoord_agreement) {
            mesh::MaterialAsset textured{};
            textured.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            textured.m_base_color_map.m_texcoord = mesh::UvSetId{1u};
            textured.m_normal_map.m_image = mesh::ImageAssetId{2u};
            textured.m_normal_map.m_texcoord = mesh::UvSetId{1u};
            const TextureBindlessIndex albedo{7u};
            const TextureBindlessIndex normal{9u};
            const Expected<GpuMaterial> agreed = buildGpuMaterial(
                textured, {albedo, kNoTexture, normal, kNoTexture});
            PPR_TEST_ASSERT(agreed.has_value());
            PPR_TEST_ASSERT(agreed->m_textures.m_albedo == albedo);
            PPR_TEST_ASSERT(*agreed->m_texcoord == 1u);

            textured.m_normal_map.m_texcoord = mesh::UvSetId{0u};
            const Expected<GpuMaterial> split = buildGpuMaterial(
                textured, {albedo, kNoTexture, normal, kNoTexture});
            PPR_TEST_ASSERT(not split.has_value());
            PPR_TEST_ASSERT(split.error() == std::make_error_code(std::errc::invalid_argument));
        };

        PPR_UNIT_TEST (build_material_alpha_flags) {
            mesh::MaterialAsset mask{};
            mask.m_alpha_mode = mesh::AlphaMode::mask;
            mask.m_alpha_cutoff = 0.25f;
            mask.m_twosided = true;
            const Expected<GpuMaterial> gpu = buildGpuMaterial(
                mask, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(gpu.has_value());
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialAlphaModeMask) == enumOrd(mesh::AlphaMode::mask));
            PPR_TEST_ASSERT((gpu->m_flags.m_bits & kGpuMaterialDoubleSidedBit) != 0u);
            PPR_TEST_ASSERT(gpu->m_rough_alpha_occl_nscale[1] == 0.25f);

            mesh::MaterialAsset blend{};
            blend.m_alpha_mode = mesh::AlphaMode::blend;
            const Expected<GpuMaterial> rejected = buildGpuMaterial(
                blend, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
            PPR_TEST_ASSERT(not rejected.has_value());
            PPR_TEST_ASSERT(rejected.error() == std::make_error_code(std::errc::function_not_supported));
        };

        PPR_UNIT_TEST (pipeline_variant_key) {
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_twosided = false, .m_alpha = mesh::AlphaMode::opaque}));
            PPR_TEST_ASSERT(not checkPipelineVariant({.m_twosided = true, .m_alpha = mesh::AlphaMode::mask}));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_twosided = false, .m_alpha = mesh::AlphaMode::blend}) ==
                            std::make_error_code(std::errc::function_not_supported));
            PPR_TEST_ASSERT(checkPipelineVariant({.m_twosided = true, .m_alpha = mesh::AlphaMode::blend}) ==
                            std::make_error_code(std::errc::function_not_supported));
            const TrianglePipelineVariant opaque{};
            const TrianglePipelineVariant masked{.m_twosided = false, .m_alpha = mesh::AlphaMode::mask};
            const TrianglePipelineVariant sided{.m_twosided = true, .m_alpha = mesh::AlphaMode::opaque};
            PPR_TEST_ASSERT(opaque != masked);
            PPR_TEST_ASSERT(opaque != sided);
            PPR_TEST_ASSERT(masked != sided);
        };

        PPR_UNIT_TEST (bindless_budget_constants) {
            PPR_TEST_ASSERT(rhi::kBindlessTextureBudget == 4096u);
            PPR_TEST_ASSERT(rhi::kBindlessCombinedBudget == 4096u);
            PPR_TEST_ASSERT(rhi::kBindlessSamplerBudget == 128u);
            PPR_TEST_ASSERT(rhi::kBindlessBufferBudget == 1024u);
        };

        // P2-entry tangent-w probe (Gate 2 C3): file-tangent vs MikkTSpace
        // lighting compare. P0c mirror math + P1 w=-w stand; this probe proves
        // the w sign is lighting-observable (mirrored bitangent flips the
        // perturbation response), so an inversion cannot hide. Final
        // arbitration is the P3 distinctive-texel render gate.
        PPR_UNIT_TEST (tangent_w_sign_is_lighting_observable) {
            // Plain lane math (no mango vector operators): bitangent = w * (n × t).
            const float normal[3]{0.0f, 0.0f, 1.0f};
            const float tangent[3]{1.0f, 0.0f, 0.0f};
            // Nonzero y: the w flip mirrors the bitangent y response, so the
            // light must observe y or the inversion hides (prior y=0 bug).
            const float light[3]{0.5f, 0.5f, 0.7071068f};
            float lighting[2]{};
            for (int sign = 0; sign < 2; ++sign) {
                const float w = sign == 0 ? 1.0f : -1.0f;
                const float bitangent[3] = {
                    w * (normal[1] * tangent[2] - normal[2] * tangent[1]),
                    w * (normal[2] * tangent[0] - normal[0] * tangent[2]),
                    w * (normal[0] * tangent[1] - normal[1] * tangent[0]),
                };
                float perturbed[3] = {
                    normal[0] + (tangent[0] + bitangent[0]) * 0.25f,
                    normal[1] + (tangent[1] + bitangent[1]) * 0.25f,
                    normal[2] + (tangent[2] + bitangent[2]) * 0.25f,
                };
                const float length =
                        std::sqrt(perturbed[0] * perturbed[0] + perturbed[1] * perturbed[1] + perturbed[2] * perturbed[2]);
                perturbed[0] /= length;
                perturbed[1] /= length;
                perturbed[2] /= length;
                lighting[sign] = perturbed[0] * light[0] + perturbed[1] * light[1] + perturbed[2] * light[2];
            }
            PPR_TEST_ASSERT(lighting[0] != lighting[1]);
            // Converted fixtures keep unit tangents with |w| == 1 (P1 w=-w applied).
            for (const char *const file: {"textured_quad.glb", "textured_box.gltf"}) {
                const Expected<mesh::SceneAsset> scene =
                        mesh::importAndConvert(std::filesystem::current_path() / "meshes" / "", file);
                PPR_TEST_ASSERT(scene.has_value());
                for (const mesh::StaticMeshAsset &mesh_asset: scene->m_meshes) {
                    for (const mesh::StaticMeshVertex &vert: mesh_asset.m_verts) {
                        const float3 tangent_v{vert.m_tangent[0], vert.m_tangent[1], vert.m_tangent[2]};
                        const float length = std::sqrt(dot(tangent_v, tangent_v));
                        if (length > 1e-6f) {
                            PPR_TEST_ASSERT(std::abs(length - 1.0f) < 1e-3f);
                            PPR_TEST_ASSERT(std::abs(std::abs(vert.m_tangent[3]) - 1.0f) < 1e-6f);
                        }
                    }
                }
            }
        };

        // P2-entry hammer (Gate 2 C3): 8-way parallel import/decode after
        // re-verifying the Mango claims on disk — ImageServer has no mutex
        // (read-only post-init: image.cpp:177-280, plain std::map), KTX2
        // single-slot transcode is guarded by our static mutex + immediate
        // clone (Image.Decode.cpp), decode is per-job (no shared decoder).
        // P0c trap honored: Mango concatenates dir+file verbatim, so the dir
        // carries a trailing separator.
        [[nodiscard]] std::filesystem::path hammerMeshDir() {
            return std::filesystem::current_path() / "meshes" / "";
        }

        [[nodiscard]] mem::SharedBuffer hammerPngBytes(const int tag) {
            const std::filesystem::path dir = std::filesystem::current_path() / "temp_hammer_fixtures";
            std::error_code ec{};
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path path = dir / std::format("hammer_{}.png", tag);
            std::array<std::byte, 4u * 4u * 4u> rgba{};
            for (std::size_t i = 0u; i < rgba.size(); i += 4u) {
                rgba[i] = static_cast<std::byte>(tag * 31 + i);
                rgba[i + 1u] = std::byte{128};
                rgba[i + 2u] = std::byte{64};
                rgba[i + 3u] = std::byte{255};
            }
            const mango::image::Surface surface{
                4, 4, mango::image::Format(32, mango::image::Format::UNORM, mango::image::Format::RGBA, 8, 8, 8, 8),
                16u, rgba.data()
            };
            if (not
                static_cast<bool>(surface.save(path.string())))
            {
                return {};
            }
            if (Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(path); mapped.has_value()) {
                return *mapped;
            }
            return {};
        }

        PPR_UNIT_TEST (hammer_8way_import_decode) {
            std::atomic<int> failures{0};
            std::vector<std::thread> workers{};
            for (int t = 0; t < 8; ++t) {
                workers.emplace_back([t, &failures] {
                    const mem::SharedBuffer png = hammerPngBytes(t);
                    if (not
                        png.isValid())
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    for (int round = 0; round < 5; ++round) {
                        const Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                            png.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
                        if (not
                            decoded.has_value()
                        or
                        decoded->m_width != 4u
                        or
                        decoded->m_height != 4u)
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        const char *const file = (t + round) % 2 == 0 ? "textured_quad.glb" : "textured_box.gltf";
                        const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(hammerMeshDir(), file);
                        if (not
                            scene.has_value()
                        or
                        scene->m_meshes.empty())
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                    }
                });
            }
            for (std::thread &worker: workers) {
                worker.join();
            }
            PPR_TEST_ASSERT(failures.load(std::memory_order_relaxed) == 0);
        };

        constexpr ApplicationDomain kGpuDomain{
            .m_is_headless = false,
            .m_is_interactive = true,
            .m_needs_presence = false,
            .m_needs_rendering = true,
            .m_needs_user_interface = true,
        };

        struct GpuTestApp : Application {
            explicit GpuTestApp(const std::string_view name, const std::span<const char *const> argv)
                : Application(kGpuDomain, name, argv) {
            }

            [[nodiscard]] std::error_code boot() { return Application::initialize(); }
            [[nodiscard]] std::error_code teardown() { return Application::shutdown(); }
        };

        [[nodiscard]] mesh::StaticMeshVertex quadVert(const float x, const float y) {
            mesh::StaticMeshVertex vert{};
            vert.m_position[0] = x;
            vert.m_position[1] = y;
            vert.m_position[2] = 0.0f;
            vert.m_normal[0] = 0.0f;
            vert.m_normal[1] = 0.0f;
            vert.m_normal[2] = 1.0f;
            vert.m_texcoord[0] = x + 0.5f;
            vert.m_texcoord[1] = y + 0.5f;
            vert.m_tangent[0] = 1.0f;
            vert.m_tangent[3] = -1.0f;
            vert.m_color[0] = 1.0f;
            vert.m_color[1] = 1.0f;
            vert.m_color[2] = 1.0f;
            vert.m_color[3] = 1.0f;
            return vert;
        }

        // Phase 6 A2 synthetic scenes: one quad mesh, one prim, one
        // base-color-textured material, one image ref. Distinct solid colors
        // give distinct dedup bytes; equal colors share one cache entry.
        [[nodiscard]] Expected<image::ImageAsset> solidImage_(const u8 r, const u8 g, const u8 b) {
            mem::UniqueBuffer storage = mem::UniqueBuffer::allocate(16u);
            if (const std::error_code err = storage.materialize()) [[unlikely]] {
                return std::unexpected{err};
            }
            Expected<mem::MutableBufferView> destination = storage.getMutableData();
            if (not
                destination.has_value())
            [[unlikely]] {
                return std::unexpected{destination.error()};
            }
            for (std::size_t i = 0u; i < 16u; i += 4u) {
                (*destination)[i] = static_cast<std::byte>(r);
                (*destination)[i + 1u] = static_cast<std::byte>(g);
                (*destination)[i + 2u] = static_cast<std::byte>(b);
                (*destination)[i + 3u] = std::byte{255};
            }
            mem::SharedBuffer frozen{};
            if (const std::error_code err = storage.moveToShared(&frozen)) [[unlikely]] {
                return std::unexpected{err};
            }
            image::ImageAsset picture{};
            picture.m_width = 2u;
            picture.m_height = 2u;
            picture.m_storage = frozen;
            picture.m_subresources.push_back(image::ImageSubresource{
                .m_view = frozen.subspan(0u, 16u),
                .m_row_pitch = 8u,
                .m_slice_pitch = 16u,
            });
            return picture;
        }

        struct SyntheticScene {
            mesh::SceneAsset m_scene;
            Array<image::ImageAsset> m_images;
        };

        [[nodiscard]] Expected<SyntheticScene> singleQuadScene_(const u8 r, const u8 g, const u8 b) {
            Expected<image::ImageAsset> image = solidImage_(r, g, b);
            if (not
                image.has_value())
            [[unlikely]] {
                return std::unexpected{image.error()};
            }
            SyntheticScene out{};
            mesh::StaticMeshAsset mesh_asset{};
            mesh_asset.m_verts.push_back(quadVert(-0.5f, -0.5f));
            mesh_asset.m_verts.push_back(quadVert(0.5f, -0.5f));
            mesh_asset.m_verts.push_back(quadVert(0.5f, 0.5f));
            mesh_asset.m_verts.push_back(quadVert(-0.5f, 0.5f));
            for (const u32 index: {0u, 1u, 2u, 0u, 2u, 3u}) {
                mesh_asset.m_indices.push_back(index);
            }
            mesh_asset.m_prims.push_back(mesh::MeshPrimitiveRange{
                .m_start = 0u, .m_count = 6u, .m_base = 0, .m_material = mesh::MaterialAssetId{0u}
            });
            mesh_asset.m_flags = mesh::EMeshAttribute::position | mesh::EMeshAttribute::normal |
                                 mesh::EMeshAttribute::texcoord | mesh::EMeshAttribute::tangent | mesh::EMeshAttribute::color;
            mesh::MaterialAsset material{};
            material.m_base_color_map.m_image = mesh::ImageAssetId{0u};
            material.m_base_color_map.m_texcoord = mesh::UvSetId{0u};
            out.m_scene.m_meshes.push_back(std::move(mesh_asset));
            out.m_scene.m_mats.push_back(material);
            out.m_scene.m_images.push_back(mesh::ImageRef{});
            out.m_images.push_back(*image);
            return out;
        }

        PPR_UNIT_TEST (gpu_caches_upload_resolve_release) {
            GpuTestApp test_app{"AssetGpuCaches", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();
            PPR_TEST_ASSERT(device.hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const mesh::StaticMeshVertex quad[] = {
                quadVert(-0.5f, -0.5f), quadVert(0.5f, -0.5f), quadVert(0.5f, 0.5f), quadVert(-0.5f, 0.5f)
            };
            const u32 quad_idx[] = {0u, 1u, 2u, 0u, 2u, 3u};
            const Expected<TriangleBagHandle> bag0 = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag0.has_value());
            PPR_TEST_ASSERT(isValid(*bag0));
            const Expected<TriangleBagRange> range0 = pass.bagCache().resolve(*bag0);
            PPR_TEST_ASSERT(range0.has_value());
            PPR_TEST_ASSERT(range0->m_count == 6u);
            PPR_TEST_ASSERT(range0->m_vb_offset == 0u);
            PPR_TEST_ASSERT(pass.bagCache().vertexBuffer(*pass.bagCache().bucketOf(*bag0)) != nullptr);

            const Expected<TriangleBagHandle> bag1 = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag1.has_value());
            PPR_TEST_ASSERT(*bag1 != *bag0);
            PPR_TEST_ASSERT(pass.bagCache().resolve(*bag1)->m_vb_offset == 4u);

            PPR_TEST_ASSERT(not pass.bagCache().resolve(TriangleBagHandle{}).has_value());
            PPR_TEST_ASSERT(pass.bagCache().resolve(TriangleBagHandle{}).error() ==
                            std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(not pass.bagCache().release(*bag0));
            PPR_TEST_ASSERT(pass.bagCache().release(*bag0) == std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(not pass.bagCache().resolve(*bag0).has_value());

            const mem::SharedBuffer png = hammerPngBytes(42);
            PPR_TEST_ASSERT(png.isValid());
            const Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                png.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());

            const Expected<TextureHandle> tex0 = pass.uploadTexture(*decoded);
            PPR_TEST_ASSERT(tex0.has_value());
            const Expected<TextureHandle> tex1 = pass.uploadTexture(*decoded);
            PPR_TEST_ASSERT(tex1.has_value());
            PPR_TEST_ASSERT(*tex1 == *tex0);
            PPR_TEST_ASSERT(pass.textureCache().residentIndex(*tex0) == pass.textureCache().residentIndex(*tex1));
            PPR_TEST_ASSERT(pass.textureCache().view(*tex0) != nullptr);
            PPR_TEST_ASSERT(pass.textureCache().view(TextureHandle{}) == nullptr);
            PPR_TEST_ASSERT(not pass.textureCache().release(*tex0));
            PPR_TEST_ASSERT(pass.textureCache().view(*tex0) != nullptr);
            PPR_TEST_ASSERT(not pass.textureCache().release(*tex0));
            PPR_TEST_ASSERT(pass.textureCache().view(*tex0) == nullptr);
            PPR_TEST_ASSERT(pass.textureCache().release(*tex0) == std::make_error_code(std::errc::invalid_argument));

            const Expected<TextureHandle> tex2 = pass.uploadTexture(*decoded);
            PPR_TEST_ASSERT(tex2.has_value());
            PPR_TEST_ASSERT(pass.textureCache().descriptorBuffer() != nullptr);
            PPR_TEST_ASSERT(pass.textureCache().descriptorBuffer()->getDesc().elementSize == sizeof(u64));
            PPR_TEST_ASSERT(*pass.textureCache().residentIndex(*tex2) >= 1u);
            const TextureHandle invalid_tex{};
            const TextureHandle resolved_slots[] = {*tex2, invalid_tex, invalid_tex, invalid_tex};
            const Expected<MaterialHandle> mat0 =
                    pass.packMaterial(mesh::MaterialAsset{}, resolved_slots);
            PPR_TEST_ASSERT(mat0.has_value());
            const Expected<u32> slot0 = pass.materialCache().materialIndex(*mat0);
            PPR_TEST_ASSERT(slot0.has_value());
            PPR_TEST_ASSERT(pass.materialCache().materialBuffer() != nullptr);

            mesh::MaterialAsset blend{};
            blend.m_alpha_mode = mesh::AlphaMode::blend;
            PPR_TEST_ASSERT(pass.packMaterial(blend, resolved_slots).error() ==
                            std::make_error_code(std::errc::function_not_supported));
            const TextureHandle short_slots[] = {*tex2, invalid_tex, invalid_tex};
            PPR_TEST_ASSERT(pass.packMaterial(mesh::MaterialAsset{}, short_slots).error() ==
                            std::make_error_code(std::errc::invalid_argument));

            PPR_TEST_ASSERT(not pass.submitInstance(*bag1, *mat0, float4x4::identity()));
            PPR_TEST_ASSERT(pass.submitInstance(TriangleBagHandle{}, *mat0, float4x4::identity()) ==
                            std::make_error_code(std::errc::invalid_argument));
            PPR_TEST_ASSERT(pass.submitInstance(*bag1, MaterialHandle{}, float4x4::identity()) ==
                            std::make_error_code(std::errc::invalid_argument));
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.textureCache().release(*tex2));
            PPR_TEST_ASSERT(not pass.materialCache().release(*mat0));
            PPR_TEST_ASSERT(not pass.bagCache().release(*bag1));
        };

        PPR_UNIT_TEST (gpu_caches_capacity_telemetry) {
            GpuTestApp test_app{"AssetGpuTelemetry", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();
            PPR_TEST_ASSERT(device.hasFeature(rhi::Feature::Bindless));

            PPR_TEST_ASSERT(kTriangleBagVertexCapacity == 4u * 1024u * 1024u);
            PPR_TEST_ASSERT(kTriangleBagIndexCapacity == 1u * 1024u * 1024u);
            PPR_TEST_ASSERT(kBindlessMaterialCapacity == 512u);
            PPR_TEST_ASSERT(rhi::kBindlessTextureBudget == 4096u);

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            PPR_TEST_ASSERT(pass.bagCache().vertexUsed() == 0u);
            PPR_TEST_ASSERT(pass.bagCache().vertexCapacity() == 0u);
            PPR_TEST_ASSERT(pass.bagCache().indexUsed() == 0u);
            PPR_TEST_ASSERT(pass.bagCache().indexCapacity() == 0u);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);
            PPR_TEST_ASSERT(pass.textureCache().textureUsed() == 0u);
            PPR_TEST_ASSERT(pass.textureCache().textureBudget() == rhi::kBindlessTextureBudget);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 0u);
            PPR_TEST_ASSERT(pass.materialCache().materialUsed() == 0u);
            PPR_TEST_ASSERT(pass.materialCache().materialCapacity() == kBindlessMaterialCapacity);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == 0u);

            const mesh::StaticMeshVertex quad[] = {
                quadVert(-0.5f, -0.5f), quadVert(0.5f, -0.5f), quadVert(0.5f, 0.5f), quadVert(-0.5f, 0.5f)
            };
            const u32 quad_idx[] = {0u, 1u, 2u, 0u, 2u, 3u};
            const Expected<TriangleBagHandle> bag = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag.has_value());
            PPR_TEST_ASSERT(pass.bagCache().vertexUsed() == sizeof(quad));
            PPR_TEST_ASSERT(pass.bagCache().vertexCapacity() == kTriangleBagVertexCapacity);
            PPR_TEST_ASSERT(pass.bagCache().indexUsed() == sizeof(quad_idx));
            PPR_TEST_ASSERT(pass.bagCache().indexCapacity() == kTriangleBagIndexCapacity);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 1u);

            PPR_TEST_ASSERT(not pass.bagCache().release(*bag));
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);
            // Bump-allocated: release retires the record, bytes stay committed.
            PPR_TEST_ASSERT(pass.bagCache().vertexUsed() == sizeof(quad));
            PPR_TEST_ASSERT(pass.bagCache().indexUsed() == sizeof(quad_idx));
        };

        PPR_UNIT_TEST (gpu_caches_overflow_is_fail_closed) {
            GpuTestApp test_app{"AssetGpuOverflow", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();
            PPR_TEST_ASSERT(device.hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const std::error_code kNoSpace = std::make_error_code(std::errc::no_buffer_space);
            const u32 small_idx[] = {0u, 1u, 2u};

            // One vertex upload past the 4 MiB bucket fails closed.
            const std::size_t big_vert_count = kTriangleBagVertexCapacity / sizeof(mesh::StaticMeshVertex) + 1u;
            const std::vector<mesh::StaticMeshVertex> huge_verts(big_vert_count);
            const Expected<TriangleBagHandle> vert_overflow = pass.uploadMesh(huge_verts, small_idx);
            PPR_TEST_ASSERT(not vert_overflow.has_value());
            PPR_TEST_ASSERT(vert_overflow.error() == kNoSpace);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);

            // Small verts with index bytes past the 1 MiB bucket fail closed.
            const mesh::StaticMeshVertex one = quadVert(0.0f, 0.0f);
            const std::vector<u32> many_idx(kTriangleBagIndexCapacity / sizeof(u32) + 16u, 0u);
            const Expected<TriangleBagHandle> idx_overflow =
                    pass.uploadMesh(std::span<const mesh::StaticMeshVertex>{&one, 1u}, many_idx);
            PPR_TEST_ASSERT(not idx_overflow.has_value());
            PPR_TEST_ASSERT(idx_overflow.error() == kNoSpace);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);

            // All 512 material slots pack; the 513th fails closed.
            const TextureHandle empty_slots[] = {TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
            std::vector<MaterialHandle> packed{};
            for (u32 i = 0u; i < kBindlessMaterialCapacity; ++i) {
                const Expected<MaterialHandle> mat = pass.packMaterial(mesh::MaterialAsset{}, empty_slots);
                PPR_TEST_ASSERT(mat.has_value());
                packed.push_back(*mat);
            }
            PPR_TEST_ASSERT(pass.materialCache().materialUsed() == kBindlessMaterialCapacity);
            const Expected<MaterialHandle> mat_overflow = pass.packMaterial(mesh::MaterialAsset{}, empty_slots);
            PPR_TEST_ASSERT(not mat_overflow.has_value());
            PPR_TEST_ASSERT(mat_overflow.error() == kNoSpace);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == kBindlessMaterialCapacity);
            for (const MaterialHandle handle: packed) {
                PPR_TEST_ASSERT(not pass.materialCache().release(handle));
            }

            // A budget-3 cache proves the texture no_buffer_space path (slot 0
            // stays fallback-reserved, so two real uploads fill it); the
            // production budget is the 4096 constant asserted above.
            constexpr u32 kWhitePixel = 0xFFFFFFFFu;
            const rhi::SubresourceData fallback_init{
                .data = &kWhitePixel,
                .rowPitch = sizeof(kWhitePixel),
                .slicePitch = sizeof(kWhitePixel),
            };
            rhi::TextureDesc fallback_desc{};
            fallback_desc.type = rhi::TextureType::Texture2D;
            fallback_desc.size = {1u, 1u, 1u};
            fallback_desc.arrayLength = 1u;
            fallback_desc.mipCount = 1u;
            fallback_desc.format = rhi::Format::RGBA8Unorm;
            fallback_desc.memoryType = rhi::MemoryType::DeviceLocal;
            fallback_desc.usage = rhi::TextureUsage::ShaderResource;
            fallback_desc.defaultState = rhi::ResourceState::ShaderResource;
            fallback_desc.label = "overflow test fallback";
            rhi::ComPtr<rhi::ITexture> fallback_tex{};
            PPR_TEST_ASSERT(not make_error_code(device.createTexture(fallback_desc, &fallback_init, fallback_tex.writeRef())));
            rhi::ComPtr<rhi::ITextureView> fallback_view{};
            PPR_TEST_ASSERT(not make_error_code(fallback_tex->getDefaultView(fallback_view.writeRef())));
            rhi::DescriptorHandle fallback_dh{};
            PPR_TEST_ASSERT(
                not make_error_code(fallback_view->getDescriptorHandle(rhi::DescriptorHandleAccess::Read, &fallback_dh)));

            BindlessTextureCache small{};
            PPR_TEST_ASSERT(not small.initialize(device, 3u, fallback_dh));
            PPR_TEST_ASSERT(small.textureBudget() == 3u);
            const mem::SharedBuffer png_bytes[] = {hammerPngBytes(101), hammerPngBytes(102), hammerPngBytes(103)};
            Expected<image::ImageAsset> decoded[] = {
                image::decodeToRgba8(png_bytes[0].getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color),
                image::decodeToRgba8(png_bytes[1].getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color),
                image::decodeToRgba8(png_bytes[2].getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color),
            };
            for (const Expected<image::ImageAsset> &image: decoded) {
                PPR_TEST_ASSERT(image.has_value());
            }
            const Expected<TextureHandle> tex_a = small.upload(*decoded[0]);
            PPR_TEST_ASSERT(tex_a.has_value());
            const Expected<TextureHandle> tex_b = small.upload(*decoded[1]);
            PPR_TEST_ASSERT(tex_b.has_value());
            PPR_TEST_ASSERT(small.textureUsed() == 2u);
            const Expected<TextureHandle> tex_overflow = small.upload(*decoded[2]);
            PPR_TEST_ASSERT(not tex_overflow.has_value());
            PPR_TEST_ASSERT(tex_overflow.error() == kNoSpace);
            PPR_TEST_ASSERT(small.entryCount() == 2u);
            PPR_TEST_ASSERT(not small.release(*tex_a));
            PPR_TEST_ASSERT(not small.release(*tex_b));
            PPR_TEST_ASSERT(not small.shutdown());
        };

        PPR_UNIT_TEST (gpu_caches_wrong_thread_fails_closed) {
            GpuTestApp test_app{"AssetGpuAffinity", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const mesh::StaticMeshVertex quad[] = {
                quadVert(-0.5f, -0.5f), quadVert(0.5f, -0.5f), quadVert(0.5f, 0.5f), quadVert(-0.5f, 0.5f)
            };
            const u32 quad_idx[] = {0u, 1u, 2u, 0u, 2u, 3u};
            const Expected<TriangleBagHandle> bag = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag.has_value());

            const mem::SharedBuffer png = hammerPngBytes(77);
            PPR_TEST_ASSERT(png.isValid());
            const Expected<image::ImageAsset> decoded = image::decodeToRgba8(
                png.getBufferData(), ".png", image::ImageDecodeDesc{}, image::ImageUsage::color);
            PPR_TEST_ASSERT(decoded.has_value());
            const Expected<TextureHandle> tex = pass.uploadTexture(*decoded);
            PPR_TEST_ASSERT(tex.has_value());

            struct WrongThreadOutcome {
                std::error_code m_bag_upload{};
                std::error_code m_bag_release{};
                std::error_code m_tex_upload{};
                std::error_code m_tex_release{};
                std::error_code m_mat_pack{};
            };
            WrongThreadOutcome outcome{};
            std::thread worker([&] {
                const Expected<TriangleBagHandle> worker_bag = pass.bagCache().upload(
                    std::span<const mesh::StaticMeshVertex>{quad}, std::span<const u32>{quad_idx});
                outcome.m_bag_upload = worker_bag.has_value() ? std::error_code{} : worker_bag.error();
                outcome.m_bag_release = pass.bagCache().release(*bag);
                const Expected<TextureHandle> worker_tex = pass.textureCache().upload(*decoded);
                outcome.m_tex_upload = worker_tex.has_value() ? std::error_code{} : worker_tex.error();
                outcome.m_tex_release = pass.textureCache().release(*tex);
                const Expected<MaterialHandle> worker_mat = pass.materialCache().pack(
                    mesh::MaterialAsset{}, {kNoTexture, kNoTexture, kNoTexture, kNoTexture});
                outcome.m_mat_pack = worker_mat.has_value() ? std::error_code{} : worker_mat.error();
            });
            worker.join();

            const std::error_code kNotPermitted = std::make_error_code(std::errc::operation_not_permitted);
            PPR_TEST_ASSERT(outcome.m_bag_upload == kNotPermitted);
            PPR_TEST_ASSERT(outcome.m_bag_release == kNotPermitted);
            PPR_TEST_ASSERT(outcome.m_tex_upload == kNotPermitted);
            PPR_TEST_ASSERT(outcome.m_tex_release == kNotPermitted);
            PPR_TEST_ASSERT(outcome.m_mat_pack == kNotPermitted);

            // Fail-closed: the worker retired nothing; owner-thread state intact.
            PPR_TEST_ASSERT(pass.bagCache().resolve(*bag).has_value());
            PPR_TEST_ASSERT(pass.textureCache().view(*tex) != nullptr);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 1u);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 1u);
            PPR_TEST_ASSERT(not pass.textureCache().release(*tex));
            PPR_TEST_ASSERT(not pass.bagCache().release(*bag));
        };

        // Phase 6 A2: two scenes coexist through ONE pass with independent
        // unload — A stays drawable while B loads/unloads beside it, and
        // unloading A leaves B drawable. Caches stay pass-owned throughout.
        PPR_UNIT_TEST (two_scenes_coexist_and_unload_independently) {
            GpuTestApp test_app{"AssetTwoScenes", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<SyntheticScene> scene_a = singleQuadScene_(200u, 30u, 30u);
            PPR_TEST_ASSERT(scene_a.has_value());
            const Expected<SyntheticScene> scene_b = singleQuadScene_(30u, 200u, 30u);
            PPR_TEST_ASSERT(scene_b.has_value());

            const Expected<TrianglePass::UploadedScene> up_a = pass.uploadScene(scene_a->m_scene, scene_a->m_images);
            PPR_TEST_ASSERT(up_a.has_value());
            const Expected<TrianglePass::UploadedScene> up_b = pass.uploadScene(scene_b->m_scene, scene_b->m_images);
            PPR_TEST_ASSERT(up_b.has_value());
            PPR_TEST_ASSERT(up_a->m_receipt != 0u);
            PPR_TEST_ASSERT(up_b->m_receipt != 0u);
            PPR_TEST_ASSERT(up_a->m_receipt != up_b->m_receipt);

            // Scene A renders while scene B sits beside it in the same pass.
            PPR_TEST_ASSERT(not pass.submitInstance(
                up_a->m_prims[0].m_bag, up_a->m_prims[0].m_material, float4x4::identity()));
            PPR_TEST_ASSERT(pass.bagCache().resolve(up_b->m_prims[0].m_bag).has_value());
            PPR_TEST_ASSERT(pass.materialCache().material(up_b->m_prims[0].m_material).has_value());

            // Unloading A leaves B drawable; stale A handles fail closed.
            PPR_TEST_ASSERT(not pass.releaseScene(*up_a));
            PPR_TEST_ASSERT(not pass.bagCache().resolve(up_a->m_prims[0].m_bag).has_value());
            PPR_TEST_ASSERT(pass.bagCache().resolve(up_b->m_prims[0].m_bag).has_value());
            PPR_TEST_ASSERT(pass.textureCache().view(up_b->m_textures[0]) != nullptr);
            PPR_TEST_ASSERT(pass.textureCache().residentIndex(up_b->m_textures[0]).has_value());
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.submitInstance(
                up_b->m_prims[0].m_bag, up_b->m_prims[0].m_material, float4x4::identity()));
            pass.clearInstances();

            PPR_TEST_ASSERT(not pass.releaseScene(*up_b));
        };

        // Phase 6 A2: identical bytes dedup to one refcounted entry — the
        // first unload retires one owner, the survivor keeps its texture.
        PPR_UNIT_TEST (two_scenes_shared_texture_survives_single_unload) {
            GpuTestApp test_app{"AssetSharedTexture", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const Expected<SyntheticScene> scene_a = singleQuadScene_(90u, 90u, 200u);
            PPR_TEST_ASSERT(scene_a.has_value());
            const Expected<SyntheticScene> scene_b = singleQuadScene_(90u, 90u, 200u);
            PPR_TEST_ASSERT(scene_b.has_value());

            const Expected<TrianglePass::UploadedScene> up_a = pass.uploadScene(scene_a->m_scene, scene_a->m_images);
            PPR_TEST_ASSERT(up_a.has_value());
            const Expected<TrianglePass::UploadedScene> up_b = pass.uploadScene(scene_b->m_scene, scene_b->m_images);
            PPR_TEST_ASSERT(up_b.has_value());
            PPR_TEST_ASSERT(*up_a->m_textures[0] == *up_b->m_textures[0]);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 1u);
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 2u);

            PPR_TEST_ASSERT(not pass.releaseScene(*up_a));
            PPR_TEST_ASSERT(pass.textureCache().view(up_b->m_textures[0]) != nullptr);
            PPR_TEST_ASSERT(pass.textureCache().residentIndex(up_b->m_textures[0]).has_value());
            PPR_TEST_ASSERT(pass.bagCache().resolve(up_b->m_prims[0].m_bag).has_value());
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.submitInstance(
                up_b->m_prims[0].m_bag, up_b->m_prims[0].m_material, float4x4::identity()));
            pass.clearInstances();

            PPR_TEST_ASSERT(not pass.releaseScene(*up_b));
            PPR_TEST_ASSERT(pass.textureCache().view(up_b->m_textures[0]) == nullptr);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 0u);
        };

        // Phase 6 A2: scene releases are single-shot — repeats and
        // never-issued scenes fail closed without touching the caches.
        PPR_UNIT_TEST (scene_double_release_fails_closed) {
            GpuTestApp test_app{"AssetSceneRelease", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            PPR_TEST_ASSERT(rhi->getDevice().hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_DEFER{PPR_TEST_ASSERT(not pass.shutdown()); };

            const std::error_code kInvalid = std::make_error_code(std::errc::invalid_argument);
            PPR_TEST_ASSERT(pass.releaseScene(TrianglePass::UploadedScene{}) == kInvalid);

            const Expected<SyntheticScene> scene_a = singleQuadScene_(200u, 200u, 30u);
            PPR_TEST_ASSERT(scene_a.has_value());
            const Expected<TrianglePass::UploadedScene> up_a = pass.uploadScene(scene_a->m_scene, scene_a->m_images);
            PPR_TEST_ASSERT(up_a.has_value());
            PPR_TEST_ASSERT(not pass.releaseScene(*up_a));
            PPR_TEST_ASSERT(pass.releaseScene(*up_a) == kInvalid);

            // Failed repeats retire nothing further: a new scene uploads,
            // draws, and releases cleanly afterwards.
            const Expected<SyntheticScene> scene_b = singleQuadScene_(30u, 30u, 30u);
            PPR_TEST_ASSERT(scene_b.has_value());
            const Expected<TrianglePass::UploadedScene> up_b = pass.uploadScene(scene_b->m_scene, scene_b->m_images);
            PPR_TEST_ASSERT(up_b.has_value());
            PPR_TEST_ASSERT(pass.bagCache().resolve(up_b->m_prims[0].m_bag).has_value());
            PPR_TEST_ASSERT(not pass.submitInstance(
                up_b->m_prims[0].m_bag, up_b->m_prims[0].m_material, float4x4::identity()));
            pass.clearInstances();
            PPR_TEST_ASSERT(not pass.releaseScene(*up_b));
        };

        // Phase 6 A3 (single boot: the tier runs 3 loops under 120 s, so
        // lifecycle + rebase share one device): residency state machine —
        // uninitialized → ready → device_lost → uninitialized (shutdown;
        // restart is shutdown + initialize) — plus the composed-identity ABA
        // proof (handles carry the full 32-bit generation while SparseVector
        // keeps its u32-index/u8-seed shape and GPU fields stay 4 B).
        // notifyDeviceLost is the cheap device-loss injection: GPU objects
        // drop, CPU records stay, GPU-touching ops fail closed with
        // no_such_device while release/telemetry keep working.
        PPR_UNIT_TEST (cache_residency_and_composed_identity) {
            PPR_TEST_ASSERT(sizeof(SparseHandle) == 8u);
            PPR_TEST_ASSERT(sizeof(TriangleBagHandle) == 8u);
            PPR_TEST_ASSERT(sizeof(TextureHandle) == 8u);
            PPR_TEST_ASSERT(sizeof(MaterialHandle) == 8u);
            PPR_TEST_ASSERT(sizeof(BagBucketId) == 4u);
            PPR_TEST_ASSERT(sizeof(TextureBindlessIndex) == 4u);

            const std::error_code kNotConnected = std::make_error_code(std::errc::not_connected);
            const std::error_code kNoDevice = std::make_error_code(std::errc::no_such_device);
            const std::error_code kBusy = std::make_error_code(std::errc::device_or_resource_busy);
            const std::error_code kInvalid = std::make_error_code(std::errc::invalid_argument);

            // Partial init: a never-initialized cache is uninitialized —
            // notify is a no-op success, GPU ops fail not_connected.
            TriangleBagCache fresh_bag{};
            PPR_TEST_ASSERT(fresh_bag.residency() == CacheResidency::uninitialized);
            PPR_TEST_ASSERT(not fresh_bag.notifyDeviceLost());
            PPR_TEST_ASSERT(fresh_bag.residency() == CacheResidency::uninitialized);
            const mesh::StaticMeshVertex quad[] = {
                quadVert(-0.5f, -0.5f), quadVert(0.5f, -0.5f), quadVert(0.5f, 0.5f), quadVert(-0.5f, 0.5f)
            };
            const u32 quad_idx[] = {0u, 1u, 2u, 0u, 2u, 3u};
            PPR_TEST_ASSERT(fresh_bag.upload(std::span<const mesh::StaticMeshVertex>{quad},
                                std::span<const u32>{quad_idx}).error() == kNotConnected);
            PPR_TEST_ASSERT(not fresh_bag.shutdown());

            GpuTestApp test_app{"AssetResidency", std::span<const char *const>{}};
            PPR_TEST_ASSERT(not test_app.boot());
            PPR_DEFER{PPR_TEST_ASSERT(not test_app.teardown()); };
            const auto rhi = test_app.getServices().get<IRhiService>();
            PPR_TEST_ASSERT(rhi.isValid());
            const auto shader = test_app.getServices().get<IShaderService>();
            PPR_TEST_ASSERT(shader.isValid());
            rhi::IDevice &device = rhi->getDevice();
            PPR_TEST_ASSERT(device.hasFeature(rhi::Feature::Bindless));

            TrianglePass pass{};
            PPR_TEST_ASSERT(not pass.initialize(*rhi, *shader, std::filesystem::current_path()));
            PPR_TEST_ASSERT(pass.bagCache().residency() == CacheResidency::ready);
            PPR_TEST_ASSERT(pass.textureCache().residency() == CacheResidency::ready);
            PPR_TEST_ASSERT(pass.materialCache().residency() == CacheResidency::ready);

            // Rebase proof first (empty cache → clean slot reuse): 300
            // same-slot upload/release cycles wrap the 8-bit seed; the
            // released first handle must never revalidate.
            const Expected<TriangleBagHandle> first = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(first.has_value());
            const TriangleBagHandle stale = *first;
            PPR_TEST_ASSERT(not pass.bagCache().release(*first));
            u32 last_generation = 0u;
            for (u32 round = 0u; round < 300u; ++round) {
                const Expected<TriangleBagHandle> cycled = pass.uploadMesh(quad, quad_idx);
                PPR_TEST_ASSERT(cycled.has_value());
                const SparseHandle identity = **cycled;
                PPR_TEST_ASSERT(identity.isValid());
                PPR_TEST_ASSERT(identity != *stale);
                PPR_TEST_ASSERT(identity.m_generation != (*stale).m_generation);
                PPR_TEST_ASSERT(identity.m_generation > last_generation);
                last_generation = identity.m_generation;
                // The stale handle never revalidates, even past the 255-cycle
                // seed wrap — generations are 32-bit monotonic, not 8-bit.
                PPR_TEST_ASSERT(pass.bagCache().resolve(stale).error() == kInvalid);
                PPR_TEST_ASSERT(pass.bagCache().release(stale) == kInvalid);
                PPR_TEST_ASSERT(not pass.bagCache().release(*cycled));
            }
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);

            // Residency flow on the same pass (in-memory image: no fixture
            // I/O on the timed path; upload validation is identical).
            const Expected<image::ImageAsset> picture = solidImage_(91u, 140u, 200u);
            PPR_TEST_ASSERT(picture.has_value());
            const Expected<TriangleBagHandle> bag0 = pass.uploadMesh(quad, quad_idx);
            PPR_TEST_ASSERT(bag0.has_value());
            const Expected<TextureHandle> tex0 = pass.uploadTexture(*picture);
            PPR_TEST_ASSERT(tex0.has_value());
            const TextureHandle no_slots[] = {TextureHandle{}, TextureHandle{}, TextureHandle{}, TextureHandle{}};
            const Expected<MaterialHandle> mat0 = pass.packMaterial(mesh::MaterialAsset{}, no_slots);
            PPR_TEST_ASSERT(mat0.has_value());
            PPR_TEST_ASSERT(pass.bagCache().resolve(*bag0).has_value());
            PPR_TEST_ASSERT(pass.textureCache().view(*tex0) != nullptr);
            PPR_TEST_ASSERT(pass.materialCache().materialIndex(*mat0).has_value());
            const Expected<BagBucketId> bag0_bucket = pass.bagCache().bucketOf(*bag0);
            PPR_TEST_ASSERT(bag0_bucket.has_value());
            PPR_TEST_ASSERT(pass.bagCache().vertexBuffer(*bag0_bucket) != nullptr);

            PPR_TEST_ASSERT(not pass.notifyDeviceLost());
            PPR_TEST_ASSERT(pass.bagCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.textureCache().residency() == CacheResidency::device_lost);
            PPR_TEST_ASSERT(pass.materialCache().residency() == CacheResidency::device_lost);

            // Parked: pass-level uploads fail not_connected, cache-level GPU
            // ops fail no_such_device, GPU views drop to null.
            PPR_TEST_ASSERT(pass.uploadMesh(quad, quad_idx).error() == kNotConnected);
            PPR_TEST_ASSERT(pass.bagCache().upload(
                                std::span<const mesh::StaticMeshVertex>{quad}, std::span<const u32>{quad_idx}).error() == kNoDevice);
            PPR_TEST_ASSERT(pass.textureCache().upload(*picture).error() == kNoDevice);
            PPR_TEST_ASSERT(pass.materialCache().pack(mesh::MaterialAsset{},
                                {kNoTexture, kNoTexture, kNoTexture, kNoTexture}).error() == kNoDevice);
            PPR_TEST_ASSERT(pass.bagCache().resolve(*bag0).error() == kInvalid);
            PPR_TEST_ASSERT(pass.textureCache().residentIndex(*tex0).error() == kInvalid);
            PPR_TEST_ASSERT(pass.textureCache().view(*tex0) == nullptr);
            PPR_TEST_ASSERT(pass.materialCache().materialIndex(*mat0).error() == kInvalid);
            PPR_TEST_ASSERT(pass.bagCache().vertexBuffer(*bag0_bucket) == nullptr);
            PPR_TEST_ASSERT(pass.textureCache().descriptorBuffer() == nullptr);
            PPR_TEST_ASSERT(pass.materialCache().materialBuffer() == nullptr);

            // Retained: CPU records, counters, and release() keep working.
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 1u);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 1u);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == 1u);
            // Bump-allocated bytes never reclaim: first + 300 rebase cycles
            // plus the live upload stay committed after records retire.
            PPR_TEST_ASSERT(pass.bagCache().vertexUsed() == 302u * sizeof(quad));
            PPR_TEST_ASSERT(not pass.bagCache().release(*bag0));
            PPR_TEST_ASSERT(not pass.textureCache().release(*tex0));
            PPR_TEST_ASSERT(not pass.materialCache().release(*mat0));
            PPR_TEST_ASSERT(pass.bagCache().rangeCount() == 0u);
            PPR_TEST_ASSERT(pass.textureCache().entryCount() == 0u);
            PPR_TEST_ASSERT(pass.materialCache().entryCount() == 0u);

            // Idempotent notify; re-initialize while lost fails closed —
            // restart is shutdown + initialize, never in place.
            PPR_TEST_ASSERT(not pass.notifyDeviceLost());
            PPR_TEST_ASSERT(pass.bagCache().initialize(device) == kBusy);
            PPR_TEST_ASSERT(pass.materialCache().initialize(device, nullptr) == kBusy);

            // Shutdown plan clears everything; the restart plan re-initializes
            // (cache-level here — no second shader compile on the timed path;
            // the pass-level shutdown + initialize order is covered by the
            // initialize/shutdown pairs every gpu test already runs).
            PPR_TEST_ASSERT(not pass.shutdown());
            PPR_TEST_ASSERT(pass.bagCache().residency() == CacheResidency::uninitialized);
            PPR_TEST_ASSERT(pass.textureCache().residency() == CacheResidency::uninitialized);
            PPR_TEST_ASSERT(pass.materialCache().residency() == CacheResidency::uninitialized);

            TriangleBagCache restarted{};
            PPR_TEST_ASSERT(not restarted.initialize(device));
            PPR_TEST_ASSERT(restarted.residency() == CacheResidency::ready);
            const Expected<TriangleBagHandle> bag1 = restarted.upload(
                std::span<const mesh::StaticMeshVertex>{quad}, std::span<const u32>{quad_idx});
            PPR_TEST_ASSERT(bag1.has_value());
            PPR_TEST_ASSERT(restarted.resolve(*bag1).has_value());
            PPR_TEST_ASSERT(not restarted.release(*bag1));
            PPR_TEST_ASSERT(not restarted.shutdown());
            PPR_TEST_ASSERT(restarted.residency() == CacheResidency::uninitialized);
        };
    } // namespace Gpu
} // namespace pP::tests::detail

namespace pP::tests {
    const UnitTest gpu = UnitTest::Named("gpu") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Gpu::handles_default_invalid,
            detail::Gpu::build_material_maps_factors,
            detail::Gpu::build_material_texcoord_agreement,
            detail::Gpu::build_material_alpha_flags,
            detail::Gpu::pipeline_variant_key,
            detail::Gpu::bindless_budget_constants,
            detail::Gpu::tangent_w_sign_is_lighting_observable,
            detail::Gpu::gpu_caches_upload_resolve_release,
            detail::Gpu::gpu_caches_capacity_telemetry,
            detail::Gpu::gpu_caches_overflow_is_fail_closed,
            detail::Gpu::gpu_caches_wrong_thread_fails_closed,
            detail::Gpu::two_scenes_coexist_and_unload_independently,
            detail::Gpu::two_scenes_shared_texture_survives_single_unload,
            detail::Gpu::scene_double_release_fails_closed,
            detail::Gpu::cache_residency_and_composed_identity,
        });
    };

    // Stress tier (Phase 5): the 8-way parallel import/decode storm runs
    // alone so the hammer/stress CI tier stays independent of GPU/readback.
    const UnitTest stress = UnitTest::Named("stress") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Gpu::hammer_8way_import_decode,
        });
    };

    const UnitTest &stressTests() noexcept {
        return stress;
    }

    const UnitTest &gpuTests() noexcept {
        return gpu;
    }
} // namespace pP::tests
