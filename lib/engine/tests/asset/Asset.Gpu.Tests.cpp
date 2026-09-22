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
            return lhs[0] == rhs.x and lhs[1] == rhs.y and lhs[2] == rhs.z and lhs[3] == rhs.w;
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
                        if (not decoded.has_value() or decoded->m_width != 4u or decoded->m_height != 4u)
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        const char *const file = (t + round) % 2 == 0 ? "textured_quad.glb" : "textured_box.gltf";
                        const Expected<mesh::SceneAsset> scene = mesh::importAndConvert(hammerMeshDir(), file);
                        if (not scene.has_value() or scene->m_meshes.empty())
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
            detail::Gpu::hammer_8way_import_decode,
            detail::Gpu::gpu_caches_upload_resolve_release,
        });
    };

    const UnitTest &gpuTests() noexcept {
        return gpu;
    }
} // namespace pP::tests
