module;
#include "pP/Macros.h"
module game.colony.content;
import engine.core;
import engine.image;
import std;

namespace pP::colony {
    PPR_DECLARE_LOG_CATEGORY(ColonyContent)
    namespace {
        using pP::make_error_code;
        struct Fingerprint {
            u64 m_value{14695981039346656037ull};
            void integer(const u64 v, const u32 width) noexcept {
                for (u32 i = 0u; i < width; ++i) {
                    m_value ^= (v >> (i * 8u)) & 255u;
                    m_value *= 1099511628211ull;
                }
            }
            void text(const std::string_view v) noexcept {
                integer(v.size(), 8u);
                for (const unsigned char byte: v) {
                    integer(byte, 1u);
                }
            }
            void value(const std::string &v) noexcept {
                text(v);
            }
            void value(const double v) noexcept {
                integer(std::bit_cast<u64>(v), 8u);
            }
            void value(const bool v) noexcept {
                integer(v ? 1u : 0u, 1u);
            }
            template<typename T>
                requires (std::is_integral_v<T> and not std::is_same_v<T, bool>)
            void value(const T v) noexcept {
                integer(static_cast<u64>(v), sizeof(T));
            }
            template<typename T>
                requires std::is_enum_v<T>
            void value(const T v) noexcept {
                integer(static_cast<u64>(v), 4u);
            }
            template<typename T>
            void value(const std::optional<T> &v) noexcept {
                value(v.has_value());
                if (v) {
                    value(*v);
                }
            }
            template<typename... T>
            void values(const T &...v) noexcept {
                (value(v), ...);
            }
            template<typename T, typename F>
            void sequence(const T &items, F &&write) noexcept {
                integer(items.size(), 8u);
                for (const auto &item: items) {
                    write(item);
                }
            }
            void material(const MaterialDefinition &m, const bool include_path) noexcept {
                text(m.m_key);
                integer(m.m_numeric_id, 2u);
                text(m.m_display_name);
                integer(static_cast<u8>(m.m_phase), 1u);
                integer(static_cast<u8>(m.m_representation), 1u);
                integer(m.m_color_rgba, 4u);
                value(m.m_reference_density_kg_m3);
                values(
                    m.m_metallic, m.m_roughness, m.m_emissive_rgb[0u], m.m_emissive_rgb[1u], m.m_emissive_rgb[2u],
                    m.m_emissive_strength, m.m_alpha_mode, m.m_ior, m.m_normal_scale, m.m_refraction_depth_m,
                    m.m_texture_sampling);
                if (include_path) {
                    value(m.m_normal_texture);
                    if (m.m_representation == EMaterialRepresentation::particle) {
                        value(m.m_albedo_texture);
                    }
                }
                if (m.m_representation == EMaterialRepresentation::particle) {
                    value(m.m_texture_period_m);
                }
                if (m.m_representation == EMaterialRepresentation::terrain) {
                    if (include_path) {
                        text(*m.m_albedo_texture);
                    }
                    value(*m.m_texture_period_m);
                    value(*m.m_diggable);
                    value(m.m_roundness);
                }
            }
            void rule(const WorldCaveRule &v) noexcept {
                values(
                    v.m_biome, v.m_field, v.m_depth_min_normalized, v.m_depth_max_normalized, v.m_threshold_min,
                    v.m_threshold_max);
            }
            void rule(const WorldWormRule &v) noexcept {
                values(
                    v.m_biome, v.m_count, v.m_depth_min_normalized, v.m_depth_max_normalized, v.m_length_min_m,
                    v.m_length_max_m, v.m_radius_min_m, v.m_radius_max_m, v.m_turn_probability, v.m_material_id,
                    v.m_host_material_id, v.m_turn_smoothing, v.m_radius_variation, v.m_chamber_probability,
                    v.m_chamber_scale);
            }
            void rule(const WorldConnectionRule &v) noexcept {
                values(v.m_radius_m, v.m_extra_links, v.m_natural_anchors, v.m_wells, v.m_branches, v.m_roughness_m);
            }
            void rule(const WorldFeatureRule &v) noexcept {
                values(
                    v.m_feature, v.m_biome, v.m_depth_min_normalized, v.m_depth_max_normalized, v.m_spacing_m,
                    v.m_min_count, v.m_max_count, v.m_attempts, v.m_priority, v.m_reflect);
            }
            void rule(const WorldMatterRule &v) noexcept {
                values(
                    v.m_feature_rule, v.m_marker, v.m_material_id, v.m_temperature_k, v.m_bulk_density_kg_m3,
                    v.m_fill_height_m);
            }
        };
        std::error_code textureFailure(
            config::Diagnostics &diagnostics, const std::filesystem::path &path, const MaterialDefinition &material,
            const std::string_view message, const std::error_code error, const bool normal_map = false) {
            diagnostics.add(
                normal_map ? material.m_normal_source_location : material.m_albedo_source_location,
                material.m_key.starts_with("backgrounds.")
                    ? material.m_key + ".texture"
                    : "materials." + material.m_key + (normal_map ? ".normal_texture" : ".albedo_texture"),
                std::string{message} + ": " + path.string(), error);
            return error;
        }
        Expected<ContentTexture> loadTexture(
            const std::filesystem::path &root, const MaterialDefinition &material, const u64 remaining_input,
            const u64 remaining_decoded, config::Diagnostics &diagnostics, const bool normal_map = false) {
            const auto &relative = normal_map ? *material.m_normal_texture : *material.m_albedo_texture;
            const auto failTexture = [&](config::Diagnostics &target, const std::filesystem::path &failed_path,
                                         const MaterialDefinition &source, const std::string_view message,
                                         const std::error_code failure) {
                return textureFailure(target, failed_path, source, message, failure, normal_map);
            };
            std::error_code error{};
            const auto path = std::filesystem::canonical(root / relative, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(
                        diagnostics, root / relative, material, "Cannot resolve required terrain texture", error));
            }
            const auto within = path.lexically_relative(root);
            if (within.empty() or within.is_absolute() or *within.begin() == "..") {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Texture escapes the pack root",
                                       std::make_error_code(std::errc::invalid_argument)));
            }
            const bool regular = std::filesystem::is_regular_file(path, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot inspect required terrain texture", error));
            }
            if (not regular) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Texture must be a regular file",
                                       std::make_error_code(std::errc::invalid_argument)));
            }
            const auto size = std::filesystem::file_size(path, error);
            if (error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot size required terrain texture", error));
            }
            if (size == 0u or size > kMaxTextureInputBytes or size > remaining_input or remaining_decoded == 0u) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(
                        diagnostics, path, material, "Texture exceeds per-image or aggregate pack byte limits",
                        std::make_error_code(std::errc::file_too_large)));
            }
            auto mapped = io::mapFile(path);
            if (not mapped and mapped.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not mapped) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot map required terrain texture", mapped.error()));
            }
            if (mapped->size() != size) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material, "Required terrain texture changed while loading",
                                       std::make_error_code(std::errc::resource_unavailable_try_again)));
            }
            const mem::SharedBufferView bytes{reinterpret_cast<const std::byte *>(mapped->c_str()), mapped->size()};
            const image::ImageDecodeDesc desc{
                .m_limits = {
                    .m_max_width = kMaxContentTextureDimension,
                    .m_max_height = kMaxContentTextureDimension,
                    .m_max_decoded_bytes = (std::min) (remaining_decoded,
                        u64{kMaxContentTextureDimension} * kMaxContentTextureDimension * 4u),
                    .m_max_input_bytes = kMaxTextureInputBytes}};
            auto decoded = image::decodeToRgba8(
                bytes, path.extension().string(), desc,
                normal_map ? image::EImageUsage::data : image::EImageUsage::color);
            if (not decoded and decoded.error() == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (not decoded) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent, failTexture(
                                       diagnostics, path, material,
                                       "Cannot decode required terrain texture within pack limits", decoded.error()));
            }
            // Build immutable, sRGB-aware levels on the content coordinator,
            // before render-lane upload. Count the whole chain against the pack budget.
            const image::MipGenDesc mip_desc{
                .m_limits = {.m_max_decoded_bytes = remaining_decoded},
                .m_full_chain = true,
                .m_normal_map = normal_map};
            const auto mip_error = image::generateMipChain(*decoded, mip_desc);
            if (mip_error == std::errc::not_enough_memory) {
                throw std::bad_alloc{};
            }
            if (mip_error) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ColonyContent,
                    failTexture(diagnostics, path, material, "Cannot generate terrain mip chain", mip_error));
            }

            Fingerprint source{};
            source.text("ppr.texture.source.v1");
            source.integer(bytes.size_bytes(), 8u);
            for (const auto byte: bytes) {
                source.integer(std::to_integer<u8>(byte), 1u);
            }
            return ContentTexture{relative, size, source.m_value, std::move(*decoded), normal_map};
        }
    }

    u64 ContentCatalog::materialFingerprint() const noexcept {
        Fingerprint hash{};
        hash.text("ppr.materials.v2");
        hash.sequence(m_materials, [&](const auto &v) { hash.material(v, true); });
        return hash.m_value;
    }
    u64 ContentCatalog::computeContentFingerprint() const noexcept {
        Fingerprint hash{};
        hash.text("ppr.content.v2");
        hash.value(kContentSchemaVersion);
        hash.text(m_pack_id);
        hash.sequence(m_materials, [&](const auto &v) {
            hash.material(v, false);
            for (const bool normal_map: {false, true}) {
                const auto &path = normal_map ? v.m_normal_texture : v.m_albedo_texture;
                hash.value(path.has_value());
                if (path) {
                    const auto texture = std::ranges::find_if(m_textures, [&](const ContentTexture &item) {
                        return item.m_path == *path and item.m_normal_map == normal_map;
                    });
                    hash.values(normal_map, texture->m_source_size, texture->m_source_fingerprint);
                }
            }
        });
        hash.sequence(m_fields, [&](const auto &field) {
            const auto &v = field.m_noise;
            hash.values(
                field.m_key, v.m_algorithm, v.m_fractal, v.m_frequency_per_metre, v.m_octaves, v.m_lacunarity, v.m_gain,
                v.m_warp_amplitude_m, v.m_warp_frequency_per_metre, v.m_axis_scale_x, v.m_axis_scale_y,
                v.m_rotation_degrees);
        });
        hash.sequence(m_biomes, [&](const auto &v) {
            hash.values(
                v.m_key, v.m_display_name, v.m_preview_color_rgba, v.m_temperature_base_k, v.m_temperature_variation_k,
                v.m_temperature_field, v.m_lining_material_id, v.m_lining_thickness_fine, v.m_relaxation_passes,
                v.m_relaxation_adoption, v.m_sprinkle_probability);
            hash.values(
                v.m_pore_probability, v.m_pore_radius_min_m, v.m_pore_radius_max_m, v.m_pore_affinity_min,
                v.m_pore_passes, v.m_finish_passes, v.m_erosion_passes, v.m_finish_adoption, v.m_soil_material_id,
                v.m_weathered_material_id, v.m_soil_depth_m, v.m_weathered_depth_m, v.m_formation_material_id,
                v.m_rind_material_id, v.m_formation_probability, v.m_formation_length_m);
            hash.value(v.m_pore_field.has_value());
            if (v.m_pore_field) {
                hash.value(*v.m_pore_field);
            }
            hash.sequence(v.m_composition, [&](const auto &b) {
                hash.values(
                    b.m_key, b.m_order, b.m_depth_min_normalized, b.m_depth_max_normalized, b.m_field, b.m_bed_period_m,
                    b.m_bed_warp_m, b.m_bed_dip);
                hash.sequence(b.m_materials, [&](const auto &m) { hash.values(m.m_key, m.m_material_id, m.m_weight); });
            });
            hash.sequence(v.m_deposits, [&](const auto &b) {
                hash.values(
                    b.m_key, b.m_order, b.m_field, b.m_material_id, b.m_host_material_id, b.m_depth_min_normalized,
                    b.m_depth_max_normalized, b.m_threshold_min, b.m_threshold_max, b.m_shape, b.m_stage,
                    b.m_blob_count, b.m_radius_min_m, b.m_radius_max_m, b.m_stretch_min, b.m_stretch_max,
                    b.m_distortion, b.m_core_coverage, b.m_fringe_coverage, b.m_sprinkle_probability,
                    b.m_orientation_min_degrees, b.m_orientation_max_degrees);
            });
        });
        hash.sequence(m_features, [&](const auto &v) {
            const auto &b = v.m_reservation;
            hash.values(
                v.m_key, v.m_display_name, v.m_allow_horizontal_reflection, b.m_min_x_fine, b.m_min_y_fine,
                b.m_max_x_fine, b.m_max_y_fine);
            hash.sequence(v.m_operations, [&](const auto &b) {
                hash.values(
                    b.m_key, b.m_order, b.m_action, b.m_shape, b.m_x_fine, b.m_y_fine, b.m_width_fine, b.m_height_fine,
                    b.m_end_x_fine, b.m_end_y_fine, b.m_radius_m, b.m_material_id);
            });
            hash.sequence(v.m_markers, [&](const auto &b) { hash.values(b.m_key, b.m_role, b.m_x_fine, b.m_y_fine); });
        });
        hash.sequence(m_worlds, [&](const auto &v) {
            hash.values(
                v.m_key, v.m_display_name, v.m_default_wall_material_id, v.m_boundary_fine, v.m_min_sites,
                v.m_max_sites, v.m_site_attempts, v.m_site_spacing_m, v.m_boundary_warp_amplitude_m,
                v.m_boundary_warp_x_field, v.m_boundary_warp_y_field, v.m_start_biome, v.m_start_feature,
                v.m_start_marker, v.m_start_x_fine, v.m_start_y_fine, v.m_start_clearance_m, v.m_start_route_width_m);
            hash.value(v.m_background_stack);
            hash.values(
                v.m_surface_field.has_value(), v.m_bedrock_field.has_value(), v.m_surface_elevation_m,
                v.m_surface_amplitude_m, v.m_bedrock_thickness_m, v.m_bedrock_variation_m, v.m_bedrock_material_id,
                v.m_start_depth_m.has_value());
            if (v.m_surface_field) {
                hash.value(*v.m_surface_field);
            }
            if (v.m_bedrock_field) {
                hash.value(*v.m_bedrock_field);
            }
            if (v.m_start_depth_m) {
                hash.value(*v.m_start_depth_m);
            }
            hash.sequence(v.m_start_resources, [&](const auto &b) {
                hash.values(b.m_key, b.m_marker, b.m_material_id, b.m_minimum_fine_cells, b.m_max_distance_m);
            });
            hash.sequence(v.m_biomes, [&](const auto &b) {
                hash.values(
                    b.m_key, b.m_biome, b.m_weight, b.m_required_count, b.m_depth_min_normalized,
                    b.m_depth_max_normalized, b.m_distance_min_m, b.m_distance_max_m);
            });
            hash.sequence(v.m_rules, [&](const auto &b) {
                hash.values(b.m_key, b.m_order, b.m_kind, b.m_phase);
                std::visit([&](const auto &parameters) { hash.rule(parameters); }, b.m_parameters);
            });
        });
        hash.sequence(m_backgrounds, [&](const auto &stack) {
            hash.value(stack.m_key);
            hash.sequence(stack.m_layers, [&](const auto &layer) {
                hash.values(
                    layer.m_key, layer.m_order, layer.m_color_rgba, layer.m_repeat_period_m, layer.m_parallax_factor);
                const auto texture = std::ranges::find_if(m_textures, [&](const ContentTexture &item) {
                    return item.m_path == layer.m_texture and not item.m_normal_map;
                });
                hash.values(texture->m_source_size, texture->m_source_fingerprint);
            });
        });
        return hash.m_value;
    }
    std::string_view ContentCatalog::packId() const noexcept {
        return m_pack_id;
    }
    std::span<const FieldDefinition> ContentCatalog::fields() const noexcept {
        return {m_fields.data(), m_fields.size()};
    }
    std::span<const BiomeDefinition> ContentCatalog::biomes() const noexcept {
        return {m_biomes.data(), m_biomes.size()};
    }
    std::span<const FeatureDefinition> ContentCatalog::features() const noexcept {
        return {m_features.data(), m_features.size()};
    }
    std::span<const WorldProfileDefinition> ContentCatalog::worlds() const noexcept {
        return {m_worlds.data(), m_worlds.size()};
    }
    std::span<const ContentTexture> ContentCatalog::textures() const noexcept {
        return {m_textures.data(), m_textures.size()};
    }
    std::span<const BackgroundStackDefinition> ContentCatalog::backgrounds() const noexcept {
        return {m_backgrounds.data(), m_backgrounds.size()};
    }
    std::optional<u64> ContentCatalog::contentFingerprint() const noexcept {
        return m_content_fingerprint;
    }
    const WorldProfileDefinition *ContentCatalog::findWorld(const std::string_view key) const noexcept {
        const auto found = std::ranges::lower_bound(m_worlds, key, {}, &WorldProfileDefinition::m_key);
        return found == m_worlds.end() or found->m_key != key ? nullptr : std::addressof(*found);
    }
    Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, config::Diagnostics &diagnostics, const IContext *const context) {
        auto source = loadContentPackSource(root, diagnostics, context);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, source);
        return loadContentCatalog(root, *source, diagnostics, context);
    }
    Expected<ContentCatalog> loadContentCatalog(
        const std::filesystem::path &root, const ContentPackSource &source, config::Diagnostics &diagnostics,
        const IContext *const context) try {
        const auto cancelled = [&]() -> std::error_code { return context ? context->error() : std::error_code{}; };
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        std::error_code error{};
        const auto canonical_root = std::filesystem::canonical(root, error);
        if (error) {
            diagnostics.add(root.string(), "pack", "Cannot resolve pack root", error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, error);
        }
        auto manifest = decodePackManifest(
            source.m_manifest_source.m_bytes, source.m_manifest_source.m_path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, manifest);
        if (manifest->m_files != source.m_manifest.m_files or manifest->m_pack_id != source.m_manifest.m_pack_id) {
            diagnostics.add(
                root.string(), "pack", "Source manifest metadata does not match its bytes",
                std::make_error_code(std::errc::invalid_argument));
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
        }
        const auto &documents = source.m_documents;
        const auto &materials = documents[static_cast<std::size_t>(EContentDocument::materials)];
        auto catalog = decodeMaterialCatalog(materials.m_bytes, materials.m_path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, catalog);
        catalog->m_pack_id = source.m_manifest.m_pack_id;
        const auto &fields = documents[static_cast<std::size_t>(EContentDocument::fields)];
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        auto decoded_fields = decodeFieldDefinitions(fields.m_bytes, fields.m_path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, decoded_fields);
        catalog->m_fields = std::move(*decoded_fields);
        const auto &biomes = documents[static_cast<std::size_t>(EContentDocument::biomes)];
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        auto decoded_biomes = decodeBiomeDefinitions(
            biomes.m_bytes, biomes.m_path.string(), *catalog, catalog->fields(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, decoded_biomes);
        catalog->m_biomes = std::move(*decoded_biomes);
        const auto &features = documents[static_cast<std::size_t>(EContentDocument::features)];
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        auto decoded_features = decodeFeatureDefinitions(
            features.m_bytes, features.m_path.string(), *catalog, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, decoded_features);
        catalog->m_features = std::move(*decoded_features);
        const auto &worlds = documents[static_cast<std::size_t>(EContentDocument::worlds)];
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        auto decoded_worlds = decodeWorldProfiles(
            worlds.m_bytes, worlds.m_path.string(), *catalog, catalog->fields(), catalog->biomes(), catalog->features(),
            diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, decoded_worlds);
        catalog->m_worlds = std::move(*decoded_worlds);
        const auto &backgrounds = documents[static_cast<std::size_t>(EContentDocument::backgrounds)];
        auto decoded_backgrounds = decodeBackgroundDefinitions(
            backgrounds.m_bytes, backgrounds.m_path.string(), diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, decoded_backgrounds);
        catalog->m_backgrounds = std::move(*decoded_backgrounds);
        for (const auto &world: catalog->worlds()) {
            if (std::ranges::none_of(catalog->m_backgrounds, [&](const auto &stack) {
                    return stack.m_key == world.m_background_stack;
                })) {
                diagnostics.add(
                    worlds.m_path.string(), "worlds." + world.m_key + ".background_stack", "Unknown background stack",
                    std::make_error_code(std::errc::invalid_argument));
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::invalid_argument);
            }
        }

        u64 input_bytes = 0u, decoded_bytes = 0u;
        for (const auto &material: catalog->materials()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
            for (const bool normal_map: {false, true}) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
                const auto &path = normal_map ? material.m_normal_texture : material.m_albedo_texture;
                if (not path or std::ranges::any_of(catalog->m_textures, [&](const ContentTexture &item) {
                        return item.m_path == *path and item.m_normal_map == normal_map;
                    })) {
                    continue;
                }
                if (catalog->m_textures.size() >= kMaxContentTextures) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        ColonyContent, textureFailure(
                                           diagnostics, *path, material, "Too many distinct texture usages",
                                           std::make_error_code(std::errc::file_too_large), normal_map));
                }
                auto texture = loadTexture(
                    canonical_root, material, kMaxPackTextureInputBytes - input_bytes,
                    kMaxPackTextureDecodedBytes - decoded_bytes, diagnostics, normal_map);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, texture);
                input_bytes += texture->m_source_size;
                decoded_bytes += texture->m_image.m_storage.getBufferData().size_bytes();
                catalog->m_textures.push_back(std::move(*texture));
            }
        }
        for (const auto &stack: catalog->m_backgrounds) {
            for (const auto &layer: stack.m_layers) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
                if (std::ranges::any_of(catalog->m_textures, [&](const ContentTexture &item) {
                        return item.m_path == layer.m_texture and not item.m_normal_map;
                    })) {
                    continue;
                }
                MaterialDefinition source_material{
                    .m_key = "backgrounds." + stack.m_key + ".layers." + layer.m_key,
                    .m_albedo_texture = layer.m_texture,
                    .m_albedo_source_location = layer.m_source_location};
                if (catalog->m_textures.size() >= kMaxContentTextures) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::file_too_large);
                }
                auto texture = loadTexture(
                    canonical_root, source_material, kMaxPackTextureInputBytes - input_bytes,
                    kMaxPackTextureDecodedBytes - decoded_bytes, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, texture);
                input_bytes += texture->m_source_size;
                decoded_bytes += texture->m_image.m_storage.getBufferData().size_bytes();
                catalog->m_textures.push_back(std::move(*texture));
            }
        }
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, cancelled());
        catalog->m_content_fingerprint = catalog->computeContentFingerprint();
        return std::move(*catalog);
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::system_error &error) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(
            ColonyContent, error.code() ? error.code() : std::make_error_code(std::errc::state_not_recoverable));
        std::unreachable();
    } catch (...) {
        PPR_RETURN_UNEXPECTED_ON_FAIL(ColonyContent, std::errc::state_not_recoverable);
        std::unreachable();
    }
}
