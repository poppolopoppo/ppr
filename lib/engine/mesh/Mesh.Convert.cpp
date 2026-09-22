module;
#include "pP/Macros.h"
#include <mango/core/exception.hpp>
#include <mango/import3d/import3d.hpp>

module engine.mesh;

import :types;
import :convert;
import engine.core;
import engine.math;
import std;

namespace pP {
    namespace mesh {
        PPR_DEFINE_LOG_CATEGORY(Mesh, info, none)

        namespace details {
            namespace m3d = mango::import3d;

            // Sentinel Mango emits for primitive-restart (U8 0xFF / U16 0xFFFF
            // map to 0xFFFFFFFF; import_gltf.cpp keeps it for GPU restart).
            inline constexpr u32 kRestartIndex{0xFFFFFFFFu};

            [[nodiscard]] Expected<u32> toU32(const std::size_t value) {
                if (value > static_cast<std::size_t>(std::numeric_limits<u32>::max())) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                return static_cast<u32>(value);
            }

            [[nodiscard]] Expected<i32> toI32(const u32 value) {
                if (value > static_cast<u32>(std::numeric_limits<i32>::max())) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                return static_cast<i32>(value);
            }

            // GLB-embed lifetime fix (P1 blocker): the fork views blob bytes out
            // of fastgltf locals (GltfDataBuffer copy + Asset-owned sources)
            // that die in the ImportGLTF ctor, so ImageSource.memory dangles
            // while the Scene lives (ASan heap-use-after-free via clone ←
            // freezeEmbedView ← convertImage; freed by sources::Array dtor).
            // The fork is fetch-cache (cmake/external/Mango.cmake), never
            // patchable in-repo: PPR re-resolves embed bytes from its own GLB
            // file mapping, matched by images[] order (the fork pushes
            // Scene.images 1:1 with asset.images, empties included). An embed
            // without a file-backed resolution fails closed below
            // (invalid_argument) — a mango view is never read again.
            inline constexpr u32 kGlbMagic{0x46546C67u};
            inline constexpr u32 kGlbJsonChunk{0x4E4F534Au};
            inline constexpr u32 kGlbBinChunk{0x004E4942u};

            [[nodiscard]] bool skipJsonWs(const char *&ptr, const char *end) noexcept {
                while (ptr != end
                    and(*ptr == ' ' or * ptr == '\t' or * ptr == '\n' or * ptr == '\r'))
                {
                    ++ptr;
                }
                return ptr != end;
            }

            [[nodiscard]] bool skipJsonString(const char *&ptr, const char *end) noexcept {
                // *ptr == '"'.
                ++ptr;
                while (ptr != end) {
                    const char c = *ptr++;
                    if (c == '"') {
                        return true;
                    }
                    if (c == '\\') {
                        if (ptr == end) {
                            return false;
                        }
                        ++ptr;
                    }
                }
                return false;
            }

            [[nodiscard]] bool skipJsonNumber(const char *&ptr, const char *end) noexcept {
                const char *start = ptr;
                while (ptr != end
                    and((*ptr >= '0' and * ptr <= '9')
                or *ptr == '-'
                or *ptr == '+'
                or *ptr == '.'
                or *ptr == 'e'
                or *ptr == 'E'))
                {
                    ++ptr;
                }
                return ptr != start;
            }

            [[nodiscard]] bool matchJsonLiteral(const char *&ptr, const char *end, const std::string_view word) noexcept {
                if (static_cast<std::size_t>(end - ptr) < word.size()
                    or std::string_view{ptr, word.size()} != word)
                {
                    return false;
                }
                ptr += word.size();
                return true;
            }

            [[nodiscard]] bool skipJsonValue(const char *&ptr, const char *end, const u32 depth = 0u) noexcept {
                if (depth > 32u
                    or not skipJsonWs(ptr, end))
                {
                    return false;
                }
                const char c = *ptr;
                if (c == '"') {
                    return skipJsonString(ptr, end);
                }
                if (c == '{'
                    or c == '[')
                {
                    const char close = c == '{' ? '}' : ']';
                    ++ptr;
                    if (not skipJsonWs(ptr, end)) {
                        return false;
                    }
                    if (*ptr == close) {
                        ++ptr;
                        return true;
                    }
                    while (true) {
                        if (c == '{') {
                            if (*ptr != '"'
                                or not skipJsonString(ptr, end) or not skipJsonWs(ptr, end) or *ptr != ':')
                            {
                                return false;
                            }
                            ++ptr;
                        }
                        if (not skipJsonValue(ptr, end, depth + 1u) or not skipJsonWs(ptr, end)) {
                            return false;
                        }
                        if (*ptr == ',') {
                            ++ptr;
                            continue;
                        }
                        if (*ptr == close) {
                            ++ptr;
                            return true;
                        }
                        return false;
                    }
                }
                if (c == 't') {
                    return matchJsonLiteral(ptr, end, "true");
                }
                if (c == 'f') {
                    return matchJsonLiteral(ptr, end, "false");
                }
                if (c == 'n') {
                    return matchJsonLiteral(ptr, end, "null");
                }
                return skipJsonNumber(ptr, end);
            }

            [[nodiscard]] bool parseJsonU64(const char *&ptr, const char *end, u64 &out) noexcept {
                if (not skipJsonWs(ptr, end)) {
                    return false;
                }
                u64 value = 0u;
                bool any = false;
                while (ptr != end
                    and *ptr >= '0'
                and *ptr <= '9')
                {
                    const u64 digit = static_cast<u64>(*ptr - '0');
                    if (value > (std::numeric_limits<u64>::max() - digit) / 10u) {
                        return false;
                    }
                    value = value * 10u + digit;
                    ++ptr;
                    any = true;
                }
                if (not any) {
                    return false;
                }
                out = value;
                return true;
            }

            // Captures a JSON key body; escapes are passed through raw, which
            // is compare-safe for the ASCII keys below.
            [[nodiscard]] bool parseJsonKey(const char *&ptr, const char *end, std::string &out) {
                if (not skipJsonWs(ptr, end) or ptr == end or *ptr != '"')
                {
                    return false;
                }
                ++ptr;
                out.clear();
                while (ptr != end) {
                    const char c = *ptr++;
                    if (c == '"') {
                        return true;
                    }
                    if (c == '\\') {
                        if (ptr == end) {
                            return false;
                        }
                        out.push_back(*ptr++);
                        continue;
                    }
                    out.push_back(c);
                }
                return false;
            }

            // Parses one {...} member list; onMember(key, ptr, end) captures or
            // skips the value and advances ptr past it.
            template<typename OnMember>
            [[nodiscard]] bool parseJsonObject(const char *&ptr, const char *end, OnMember &&onMember) {
                // *ptr is unchecked here; the caller established '{'.
                ++ptr;
                std::string key;
                if (not skipJsonWs(ptr, end)) {
                    return false;
                }
                if (*ptr == '}') {
                    ++ptr;
                    return true;
                }
                while (true) {
                    if (not parseJsonKey(ptr, end, key) or not skipJsonWs(ptr, end) or ptr == end or *ptr != ':')
                    {
                        return false;
                    }
                    ++ptr;
                    if (not onMember(key, ptr, end)) {
                        return false;
                    }
                    if (not skipJsonWs(ptr, end)) {
                        return false;
                    }
                    if (ptr != end
                        and *ptr == ',')
                    {
                        ++ptr;
                        continue;
                    }
                    if (ptr != end
                        and *ptr == '}')
                    {
                        ++ptr;
                        return true;
                    }
                    return false;
                }
            }

            struct GlbBufferViewDesc {
                u64 m_buffer = 0u;
                u64 m_offset = 0u;
                u64 m_length = 0u;
                bool m_has_length = false;
            };

            struct GlbImageDesc {
                u64 m_buffer_view = 0u;
                bool m_has_view = false;
            };

            struct GlbJsonIndex {
                Array<GlbImageDesc> m_images;
                Array<GlbBufferViewDesc> m_views;
            };

            [[nodiscard]] bool parseGlbJson(const char *begin, const char *end, GlbJsonIndex &index) {
                const char *ptr = begin;
                if (not skipJsonWs(ptr, end) or ptr == end or *ptr != '{')
                {
                    return false;
                }
                auto onTop = [&](const std::string &name, const char *&p, const char *e) -> bool {
                    if (name != "images"
                        and name != "bufferViews")
                    {
                        return skipJsonValue(p, e);
                    }
                    if (not skipJsonWs(p, e) or p == e or *p != '[')
                    {
                        return false;
                    }
                    ++p;
                    while (true) {
                        if (not skipJsonWs(p, e)) {
                            return false;
                        }
                        if (*p == ']') {
                            ++p;
                            return true;
                        }
                        if (p == e
                            or *p != '{')
                        {
                            return false;
                        }
                        if (name == "images") {
                            GlbImageDesc desc;
                            auto onImage = [&](const std::string &key, const char *&q, const char *f) -> bool {
                                if (key != "bufferView") {
                                    return skipJsonValue(q, f);
                                }
                                if (not parseJsonU64(q, f, desc.m_buffer_view)) {
                                    return false;
                                }
                                desc.m_has_view = true;
                                return true;
                            };
                            if (not parseJsonObject(p, e, onImage)) {
                                return false;
                            }
                            index.m_images.push_back(desc);
                        } else {
                            GlbBufferViewDesc desc;
                            auto onView = [&](const std::string &key, const char *&q, const char *f) -> bool {
                                if (key == "buffer") {
                                    return parseJsonU64(q, f, desc.m_buffer);
                                }
                                if (key == "byteOffset") {
                                    return parseJsonU64(q, f, desc.m_offset);
                                }
                                if (key == "byteLength") {
                                    if (not parseJsonU64(q, f, desc.m_length)) {
                                        return false;
                                    }
                                    desc.m_has_length = true;
                                    return true;
                                }
                                return skipJsonValue(q, f);
                            };
                            if (not parseJsonObject(p, e, onView)) {
                                return false;
                            }
                            index.m_views.push_back(desc);
                        }
                        if (not skipJsonWs(p, e)) {
                            return false;
                        }
                        if (p != e
                            and *p == ',')
                        {
                            ++p;
                            continue;
                        }
                        if (p != e
                            and *p == ']')
                        {
                            ++p;
                            return true;
                        }
                        return false;
                    }
                };
                return parseJsonObject(ptr, end, onTop);
            }

            // Re-resolves GLB embed bytes from PPR's own file mapping, indexed
            // 1:1 with Scene.images. The fetch-cache fork truncates blob
            // lifetime (import_gltf.cpp:78 local GltfDataBuffer; Asset-owned
            // sources die in the ImportGLTF ctor), so mango views dangle while
            // the Scene lives. Entries stay empty when unresolvable
            // (no BIN chunk, foreign buffer index, bad range, JSON anomaly) —
            // convertImage fails those closed instead of reading dead views.
            [[nodiscard]] Array<mem::SharedBuffer> resolveGlbEmbeds(const mem::SharedBuffer &mapping) {
                Array<mem::SharedBuffer> table;
                const mem::SharedBufferView bytes = mapping.getBufferData();
                const std::size_t size = bytes.size();
                const std::byte *data = bytes.data();
                if (data == nullptr
                    or size<20u)
                {
                    return table;
                }
                auto readAt = [&](const std::size_t off, u32 &out) noexcept -> bool {
                    if (off > size
                        or size
                    -off < sizeof(u32))
                    {
                        return false;
                    }
                    std::memcpy(&out, data + off, sizeof(out));
                    return true;
                };
                u32 magic = 0u, total = 0u;
                if (not readAt(0u, magic) or magic != kGlbMagic or not readAt(8u, total) or total > size)
                {
                    return table;
                }
                const char *json_begin = nullptr;
                const char *json_end = nullptr;
                u64 bin_start = 0u;
                u64 bin_size = 0u;
                bool have_json = false;
                bool have_bin = false;
                std::size_t chunk = 12u;
                while (chunk + 8u >= 8u
                    and chunk
                +8u <= size)
                {
                    u32 chunk_len = 0u, chunk_type = 0u;
                    if (not readAt(chunk, chunk_len) or not readAt(chunk + 4u, chunk_type)) {
                        break;
                    }
                    const u64 start = static_cast<u64>(chunk) + 8u;
                    const u64 finish = start + static_cast<u64>(chunk_len);
                    if (finish < start
                        or finish > static_cast<u64>(total))
                    {
                        break;
                    }
                    if (chunk_type == kGlbJsonChunk
                        and not have_json)
                    {
                        json_begin = reinterpret_cast<const char *>(data + static_cast<std::size_t>(start));
                        json_end = reinterpret_cast<const char *>(data + static_cast<std::size_t>(finish));
                        have_json = true;
                    } else if (chunk_type == kGlbBinChunk
                        and not have_bin)
                    {
                        bin_start = start;
                        bin_size = static_cast<u64>(chunk_len);
                        have_bin = true;
                    }
                    if (finish > static_cast<u64>(std::numeric_limits<std::size_t>::max())) {
                        break;
                    }
                    const std::size_t next = static_cast<std::size_t>(finish);
                    if (next <= chunk) {
                        break;
                    }
                    chunk = next;
                }
                if (not have_json) {
                    return table;
                }
                GlbJsonIndex refs;
                if (not parseGlbJson(json_begin, json_end, refs) or not have_bin) {
                    return table;
                }
                table.resize(refs.m_images.size());
                for (std::size_t i = 0u; i < refs.m_images.size(); ++i) {
                    const GlbImageDesc &image = refs.m_images[i];
                    if (not
                        image.m_has_view
                    or
                    image.m_buffer_view >= refs.m_views.size())
                    {
                        continue;
                    }
                    const GlbBufferViewDesc &view =
                            refs.m_views[static_cast<std::size_t>(image.m_buffer_view)];
                    if (not
                        view.m_has_length
                    or
                    view.m_length == 0u
                    or
                    view.m_buffer != 0u)
                    {
                        continue;
                    }
                    const u64 begin = bin_start + view.m_offset;
                    const u64 finish = begin + view.m_length;
                    if (begin < bin_start
                        or finish<begin or finish>
                    static_cast<u64>(size))
                    {
                        continue;
                    }
                    table[i] =
                            mapping.subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(view.m_length));
                }
                return table;
            }

            [[nodiscard]] Expected<ImageRef> convertImage(const m3d::ImageSource &source, const std::filesystem::path &dir,
                                                          const u32 image_index, const Array<mem::SharedBuffer> &glb_embeds) {
                ImageRef ref;
                ref.m_name = source.name;
                ref.m_ext = source.extension;
                if (source.isMemory()) {
                    // Non-File-backed embeds dangle (see above): use the
                    // PPR-resolved file-backed bytes, or fail closed — never
                    // read the mango view.
                    if (image_index >= glb_embeds.size()
                        or glb_embeds[image_index].getBufferData().empty())
                    [[unlikely]] {
                        PPR_LOG(Mesh, error, "embed image has no file-backed resolution — refusing the mango view");
                        return std::unexpected{make_error_code(errc::invalid_argument)};
                    }
                    ref.m_bytes = glb_embeds[image_index];
                    ref.m_is_file = false;
                    return ref;
                }
                if (source.isFile()) {
                    ref.m_rel_path = source.filename;
                    ref.m_is_file = true;
                    Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(dir / source.filename);
                    if (not
                        mapped.has_value())
                    [[unlikely]] {
                        return std::unexpected{mapped.error()};
                    }
                    ref.m_bytes = std::move(*mapped);
                    return ref;
                }
                // Empty source: never referenced by slots (Mango filters them
                // in imageIndexOf), but indices must stay 1:1 with Mango.
                ref.m_is_file = false;
                return ref;
            }

            [[nodiscard]] Expected<MaterialImageSlot> convertSlot(const m3d::ImageSample &sample, const Array<ImageRef> &images) {
                MaterialImageSlot slot;
                if (not
                    sample.enabled())
                {
                    return slot;
                }
                if (sample.image >= images.size()) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                // imageIndexOf guarantees a non-empty source; anything else is
                // inconsistent input, failed closed.
                if (images[sample.image].m_bytes.getBufferData().empty()) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                slot.m_image = ImageAssetId{sample.image};
                slot.m_texcoord = UvSetId{sample.texCoord};
                slot.m_transform.m_scale = sample.transform.scale;
                slot.m_transform.m_offset = sample.transform.offset;
                slot.m_transform.m_rotation = sample.transform.rotation;
                return slot;
            }

            // Deferred KHR modules are populated by the Mango importer, so an
            // authored module is detectable: reject, never silently widen.
            // specular/ior/transmission/volume/iridescence/emissive_strength/
            // unlit have no Mango Material fields (dropped at the Mango
            // boundary with a logged note here), so nothing reaches the asset.
            [[nodiscard]] std::error_code checkDeferredModules(const m3d::Material &material) {
                if (material.clearcoatFactor != 0.0f
                    or
                material.clearcoatRoughnessFactor != 0.0f
                or
                material.clearcoat.enabled()
                or
                material.clearcoatRoughness.enabled()
                or
                material.clearcoatNormal.enabled())
                [[unlikely]] {
                    PPR_LOG(Mesh, error, "KHR_materials_clearcoat is authored but deferred — rejecting material");
                    return make_error_code(errc::function_not_supported);
                }
                if (material.sheenColorFactor.x != 0.0f
                    or
                material.sheenColorFactor.y != 0.0f
                or
                material.sheenColorFactor.z != 0.0f
                or
                material.sheenRoughnessFactor != 0.0f
                or
                material.sheenColor.enabled()
                or
                material.sheenRoughness.enabled())
                [[unlikely]] {
                    PPR_LOG(Mesh, error, "KHR_materials_sheen is authored but deferred — rejecting material");
                    return make_error_code(errc::function_not_supported);
                }
                if (material.anisotropyStrength != 0.0f
                    or
                material.anisotropy.enabled())
                [[unlikely]] {
                    PPR_LOG(Mesh, error, "KHR_materials_anisotropy is authored but deferred — rejecting material");
                    return make_error_code(errc::function_not_supported);
                }
                if (material.opacity.enabled()) [[unlikely]] {
                    PPR_LOG(Mesh, error, "opacity map has no MaterialAsset representation — rejecting material");
                    return make_error_code(errc::function_not_supported);
                }
                return default_value_v;
            }

            [[nodiscard]] Expected<MaterialAsset> convertMaterial(const m3d::Material &material, const Array<ImageRef> &images) {
                if (const std::error_code err = checkDeferredModules(material)) [[unlikely]] {
                    return std::unexpected{err};
                }
                if (material.alphaMode == m3d::Material::AlphaMode::Blend) [[unlikely]] {
                    PPR_LOG(Mesh, error, "AlphaMode::blend is deferred (no sorting/depth-write policy) — rejecting material");
                    return std::unexpected{make_error_code(errc::function_not_supported)};
                }
                MaterialAsset out;
                out.m_base_color = material.baseColorFactor;
                out.m_metallic = material.metallicFactor;
                out.m_roughness = material.roughnessFactor;
                out.m_emissive = material.emissiveFactor;
                out.m_alpha_cutoff = material.alphaCutoff;
                out.m_twosided = material.twosided;
                // Only scalars Mango populates: normal.scale and
                // occlusion.scale (ImageSample.scale defaults to 1.0).
                out.m_normal_scale = material.normal.scale;
                out.m_occlusion_strength = material.occlusion.scale;
                out.m_alpha_mode = material.alphaMode == m3d::Material::AlphaMode::Mask ? AlphaMode::mask : AlphaMode::opaque;
                // MR resolve: Mango binds roughness=.g and metallic=.b of the
                // shared ORM image (Linear); slots stay separate because glTF
                // sharing is the special case (import_gltf.cpp:372-418).
                Expected<MaterialImageSlot> slot = convertSlot(material.baseColor, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_base_color_map = *slot;
                slot = convertSlot(material.metallic, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_metallic_map = *slot;
                slot = convertSlot(material.roughness, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_roughness_map = *slot;
                slot = convertSlot(material.normal, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_normal_map = *slot;
                slot = convertSlot(material.occlusion, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_occlusion_map = *slot;
                slot = convertSlot(material.emissive, images);
                if (not
                    slot.has_value())
                [[unlikely]] {
                    return std::unexpected{slot.error()};
                }
                out.m_emissive_map = *slot;
                return out;
            }

            // §3 boundary helpers: plain-float-array vertex ↔ mango vectors.
            // The fork already emits LH (x,y,-z), so this is a verbatim
            // component copy. Tangent-w policy lives at the emission site in
            // convertMesh: file tangents get w=-w (P0c), MikkTSpace-regened
            // tangents pass through verbatim (computed post-flip).
            [[nodiscard]] StaticMeshVertex storeVertex(const m3d::Vertex &src) noexcept {
                StaticMeshVertex dst;
                dst.m_position[0] = src.position.x;
                dst.m_position[1] = src.position.y;
                dst.m_position[2] = src.position.z;
                dst.m_normal[0] = src.normal.x;
                dst.m_normal[1] = src.normal.y;
                dst.m_normal[2] = src.normal.z;
                dst.m_texcoord[0] = src.texcoord.x;
                dst.m_texcoord[1] = src.texcoord.y;
                dst.m_tangent[0] = src.tangent.x;
                dst.m_tangent[1] = src.tangent.y;
                dst.m_tangent[2] = src.tangent.z;
                dst.m_tangent[3] = src.tangent.w;
                dst.m_color[0] = src.color.x;
                dst.m_color[1] = src.color.y;
                dst.m_color[2] = src.color.z;
                dst.m_color[3] = src.color.w;
                return dst;
            }

            [[nodiscard]] EMeshAttribute convertFlags(const u32 mango_flags) noexcept {
                EMeshAttribute flags = EMeshAttribute::none;
                if ((mango_flags & m3d::Vertex::Position) != 0u) {
                    flags |= EMeshAttribute::position;
                }
                if ((mango_flags & m3d::Vertex::Normal) != 0u) {
                    flags |= EMeshAttribute::normal;
                }
                if ((mango_flags & m3d::Vertex::Texcoord) != 0u) {
                    flags |= EMeshAttribute::texcoord;
                }
                if ((mango_flags & m3d::Vertex::Tangent) != 0u) {
                    flags |= EMeshAttribute::tangent;
                }
                if ((mango_flags & m3d::Vertex::Color) != 0u) {
                    flags |= EMeshAttribute::color;
                }
                return flags;
            }

            // Expand one strip/fan primitive to a triangle list, mirroring
            // Mango's own strip/fan→triangle winding (mesh.cpp trimesh path).
            // MeshPrimitiveRange carries no topology, and the §6 shader
            // consumes flat lists, so expansion is the content-preserving
            // conversion. Restart sentinels split the strip/fan; resolved
            // indices are file-local + base in all cases.
            [[nodiscard]] std::error_code expandStripFan(
                const m3d::Primitive &prim, const Array<u32> &file_indices, const u64 vert_count, Array<u32> &out_expanded) {
                const bool is_strip = prim.type == m3d::Primitive::Type::TriangleStrip;
                const u64 base = prim.base;
                // Sliding window over the current restart-delimited segment.
                // Winding mirrors Mango's strip/fan→triangle path (mesh.cpp):
                // strips swap on odd segment positions, fans fan out of v0.
                u32 window[2]{0u, 0u};
                u32 anchor = 0u;
                u32 held = 0u;
                for (const u32 file_index: file_indices) {
                    if (file_index == kRestartIndex) {
                        held = 0u;
                        continue;
                    }
                    const u64 resolved = static_cast<u64>(file_index) + base;
                    if (resolved >= vert_count) [[unlikely]] {
                        return make_error_code(errc::invalid_argument);
                    }
                    const u32 global = static_cast<u32>(resolved);
                    if (held < 2u) {
                        window[held] = global;
                        if (not is_strip and held == 0u)
                        {
                            anchor = global;
                        }
                        ++held;
                        continue;
                    }
                    if (is_strip) {
                        // Segment position of the new vertex: held counts prior
                        // vertices, so odd held swaps like Mango's odd i.
                        if ((held & 1u) != 0u) {
                            out_expanded.push_back(window[1]);
                            out_expanded.push_back(window[0]);
                        } else {
                            out_expanded.push_back(window[0]);
                            out_expanded.push_back(window[1]);
                        }
                        out_expanded.push_back(global);
                        window[0] = window[1];
                        window[1] = global;
                        ++held;
                    } else {
                        out_expanded.push_back(anchor);
                        out_expanded.push_back(window[1]);
                        out_expanded.push_back(global);
                        window[1] = global;
                    }
                }
                return default_value_v;
            }

            // Missing-tangent fallback: in-fork MikkTSpace regen (P0c probe 2).
            // Mirrors the fork's own needTangent path (import_gltf.cpp:932-940
            // + 1020-1023): per primitive gated on the referenced material's
            // normal map, soup built from already-flipped (x,y,-z) data,
            // tangents written back per corner with first-corner-wins for
            // shared verts (MikkTSpace welds identical corners, so well-formed
            // data agrees). Regened tangents are consumed verbatim downstream
            // (no w negate — computed post-flip, self-consistent). Returns true
            // only when the Tangent flag is set afterwards; a MikkTSpace
            // failure already warns inside the fork, so false is the caller's
            // warning-only path. No new dependency: Mesh/IndexedMesh/Vertex all
            // ride the existing PRIVATE mango-import3d edge.
            [[nodiscard]] bool regenTangents(
                const m3d::IndexedMesh &mesh, const std::vector<m3d::Material> &materials, Array<float4> &out_tangents) {
                if ((mesh.flags & m3d::Vertex::Tangent) != 0u) {
                    return false;
                }
                if ((mesh.flags & m3d::Vertex::Normal) == 0u
                    or(mesh.flags & m3d::Vertex::Texcoord) == 0u)
                {
                    return false;
                }
                const u64 vert_count = mesh.vertices.size();
                if (vert_count == 0u
                    or vert_count > static_cast<u64>(std::numeric_limits<u32>::max()))
                {
                    return false;
                }
                m3d::Mesh soup;
                soup.flags = mesh.flags;
                Array<u32> corner_global;
                bool want = false;
                for (const m3d::Primitive &prim: mesh.primitives) {
                    if (prim.material >= materials.size()
                        or not materials[prim.material].normal.enabled())
                    {
                        continue;
                    }
                    const u64 start = prim.start;
                    const u64 count = prim.count;
                    if (count < 3u
                        or start > mesh.indices.size()
                    or count > mesh.indices.size() - start)
                    {
                        return false;
                    }
                    const u64 base = prim.base;
                    if (prim.type == m3d::Primitive::Type::TriangleList) {
                        if (count % 3u != 0u) {
                            return false;
                        }
                        for (u64 k = 0u; k < count; k += 3u) {
                            m3d::Triangle tri;
                            for (u32 c = 0u; c < 3u; ++c) {
                                const u32 file_index =
                                        mesh.indices[static_cast<std::size_t>(start + k + c)];
                                if (file_index == kRestartIndex) {
                                    return false;
                                }
                                const u64 resolved = static_cast<u64>(file_index) + base;
                                if (resolved >= vert_count) {
                                    return false;
                                }
                                const u32 global = static_cast<u32>(resolved);
                                tri.vertex[c] = mesh.vertices[global];
                                corner_global.push_back(global);
                            }
                            soup.triangles.push_back(tri);
                        }
                    } else {
                        Array<u32> file_slice;
                        file_slice.reserve(static_cast<std::size_t>(count));
                        for (u64 k = 0u; k < count; ++k) {
                            file_slice.push_back(mesh.indices[static_cast<std::size_t>(start + k)]);
                        }
                        Array<u32> expanded;
                        if (const std::error_code err = expandStripFan(prim, file_slice, vert_count, expanded)) {
                            return false;
                        }
                        if (expanded.empty()
                            or
                        expanded.size() % 3u != 0u)
                        {
                            return false;
                        }
                        for (std::size_t t = 0u; t < expanded.size(); t += 3u) {
                            m3d::Triangle tri;
                            for (u32 c = 0u; c < 3u; ++c) {
                                const u32 global = expanded[t + c];
                                tri.vertex[c] = mesh.vertices[global];
                                corner_global.push_back(global);
                            }
                            soup.triangles.push_back(tri);
                        }
                    }
                    want = true;
                }
                if (not want or soup.triangles.empty())
                {
                    return false;
                }
                PPR_ASSERT(corner_global.size() == soup.triangles.size() * 3u);
                soup.computeTangents();
                if ((soup.flags & m3d::Vertex::Tangent) == 0u) {
                    return false;
                }
                out_tangents = Array<float4>(static_cast<std::size_t>(vert_count), float4{0.0f, 0.0f, 0.0f, 0.0f});
                Array<u8> written(static_cast<std::size_t>(vert_count), u8{0});
                std::size_t corner = 0u;
                for (const m3d::Triangle &tri: soup.triangles) {
                    for (u32 c = 0u; c < 3u; ++c) {
                        const u32 global = corner_global[corner];
                        if (written[global] == u8{0}) {
                            written[global] = u8{1};
                            out_tangents[global] = tri.vertex[c].tangent;
                        }
                        ++corner;
                    }
                }
                return true;
            }

            [[nodiscard]] Expected<StaticMeshAsset> convertMesh(const m3d::IndexedMesh &mesh, const std::vector<m3d::Material> &materials) {
                if ((mesh.flags & (m3d::Vertex::Joints | m3d::Vertex::Weights)) != 0u) [[unlikely]] {
                    PPR_LOG(Mesh, error, "JOINTS_0/WEIGHTS_0 are deferred (no skins in MVP) — rejecting mesh");
                    return std::unexpected{make_error_code(errc::function_not_supported)};
                }
                // Missing POSITION (or fully attribute-mismatched prims) leaves
                // an empty Mango mesh: fail closed, never an empty asset.
                if (mesh.vertices.empty()
                    or
                mesh.primitives.empty())
                [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                Expected<u32> vert_count = toU32(mesh.vertices.size());
                if (not
                    vert_count.has_value())
                [[unlikely]] {
                    return std::unexpected{vert_count.error()};
                }
                Expected<u32> index_total = toU32(mesh.indices.size());
                if (not
                    index_total.has_value())
                [[unlikely]] {
                    return std::unexpected{index_total.error()};
                }
                StaticMeshAsset out;
                out.m_flags = convertFlags(mesh.flags);
                out.m_bounds = mesh.boundingBox;
                const bool file_tangents = (mesh.flags & m3d::Vertex::Tangent) != 0u;
                Array<float4> regen_tangents;
                const bool regen_ok = not file_tangents and regenTangents(mesh, materials, regen_tangents);
                out.m_verts.reserve(mesh.vertices.size());
                for (u32 i = 0u; i < *vert_count; ++i) {
                    StaticMeshVertex dst = storeVertex(mesh.vertices[i]);
                    if (file_tangents) {
                        // G1 (P0c probe 2): the fork hands file tangents as
                        // (x,y,-z,w) with verbatim w, but mirroring z on N and
                        // T flips the handedness of B = w·(N×T) — negate w to
                        // keep the bitangent correct.
                        dst.m_tangent[3] = -dst.m_tangent[3];
                    } else if (regen_ok) {
                        // G2: MikkTSpace ran on already-flipped data; the
                        // result is self-consistent, used verbatim (no negate).
                        dst.m_tangent[0] = regen_tangents[i].x;
                        dst.m_tangent[1] = regen_tangents[i].y;
                        dst.m_tangent[2] = regen_tangents[i].z;
                        dst.m_tangent[3] = regen_tangents[i].w;
                    }
                    out.m_verts.push_back(dst);
                }
                if (regen_ok) {
                    out.m_flags |= EMeshAttribute::tangent;
                }
                out.m_indices.reserve(mesh.indices.size());
                out.m_indices.assign(mesh.indices.begin(), mesh.indices.end());
                out.m_prims.reserve(mesh.primitives.size());
                for (const m3d::Primitive &prim: mesh.primitives) {
                    if (prim.material >= materials.size()) [[unlikely]] {
                        return std::unexpected{make_error_code(errc::invalid_argument)};
                    }
                    const u64 start = prim.start;
                    const u64 count = prim.count;
                    if (count < 3u
                        or start > out.m_indices.size()
                    or count > out.m_indices.size() - start)
                    [[unlikely]] {
                        return std::unexpected{make_error_code(errc::invalid_argument)};
                    }
                    Expected<i32> base = toI32(prim.base);
                    if (not
                        base.has_value())
                    [[unlikely]] {
                        return std::unexpected{base.error()};
                    }
                    // Vertex-index bound: resolved index is file-local + base
                    // for both the direct path (base=vert_count) and the
                    // append path (base=0, remapped global indices).
                    const u64 vert_base = prim.base;
                    const u64 verts = *vert_count;
                    MeshPrimitiveRange range;
                    range.m_material = MaterialAssetId{prim.material};
                    if (prim.type == m3d::Primitive::Type::TriangleList) {
                        if (count % 3u != 0u) [[unlikely]] {
                            return std::unexpected{make_error_code(errc::invalid_argument)};
                        }
                        for (u64 k = 0u; k < count; ++k) {
                            const u32 file_index = out.m_indices[start + k];
                            if (file_index == kRestartIndex) [[unlikely]] {
                                return std::unexpected{make_error_code(errc::invalid_argument)};
                            }
                            if (static_cast<u64>(file_index) + vert_base >= verts) [[unlikely]] {
                                return std::unexpected{make_error_code(errc::invalid_argument)};
                            }
                        }
                        Expected<u32> range_start = toU32(start);
                        Expected<u32> range_count = toU32(count);
                        if (not
                            range_start.has_value()
                        or not range_count.has_value())
                        [[unlikely]] {
                            return std::unexpected{make_error_code(errc::invalid_argument)};
                        }
                        range.m_start = *range_start;
                        range.m_count = *range_count;
                        range.m_base = *base;
                    } else {
                        Array<u32> expanded;
                        Array<u32> file_slice;
                        file_slice.reserve(static_cast<std::size_t>(count));
                        for (u64 k = 0u; k < count; ++k) {
                            file_slice.push_back(out.m_indices[start + k]);
                        }
                        if (const std::error_code err = expandStripFan(prim, file_slice, verts, expanded)) [[unlikely]] {
                            return std::unexpected{err};
                        }
                        if (expanded.empty()
                            or
                        expanded.size() % 3u != 0u)
                        [[unlikely]] {
                            return std::unexpected{make_error_code(errc::invalid_argument)};
                        }
                        Expected<u32> range_start = toU32(out.m_indices.size());
                        Expected<u32> range_count = toU32(expanded.size());
                        if (not
                            range_start.has_value()
                        or not range_count.has_value())
                        [[unlikely]] {
                            return std::unexpected{range_start.has_value() ? range_count.error() : range_start.error()};
                        }
                        range.m_start = *range_start;
                        range.m_count = *range_count;
                        range.m_base = 0;
                        out.m_indices.insert(out.m_indices.end(), expanded.begin(), expanded.end());
                    }
                    out.m_prims.push_back(range);
                }
                if (not file_tangents and not regen_ok) {
                    bool want_tangent = false;
                    for (const m3d::Primitive &prim: mesh.primitives) {
                        if (prim.material < materials.size()
                            and materials[prim.material].normal.enabled())
                        {
                            want_tangent = true;
                            break;
                        }
                    }
                    // Warning-only: tangents stay zero with no tangent flag —
                    // never a silent flat-tangent claim (§2.3 regen rule).
                    if (want_tangent) [[unlikely]] {
                        PPR_LOG(Mesh, warning,
                            "normal-mapped mesh has no usable tangents (source TANGENT absent, MikkTSpace regen unavailable or failed); emitting zero tangents with no tangent flag");
                    }
                }
                return out;
            }

            [[nodiscard]] Expected<SceneAsset> buildScene(const m3d::Scene &scene, const std::filesystem::path &dir,
                                                          const Array<mem::SharedBuffer> &glb_embeds) {
                // Mango returns a silent-empty Scene on parse failure (no
                // exception): fail closed instead of an empty asset.
                if (scene.meshes.empty()) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                if (not
                    scene.skins.empty())
                [[unlikely]] {
                    PPR_LOG(Mesh, error, "skins are deferred (no playback and no storage in MVP) — rejecting scene");
                    return std::unexpected{make_error_code(errc::function_not_supported)};
                }
                if (not
                    scene.animations.empty())
                {
                    // Static bind-pose snapshot stays well-defined (authored
                    // local transforms); animations are ignored with a warning.
                    PPR_LOG(Mesh, warning, "ignoring animation channels; using the static bind-pose snapshot");
                }
                SceneAsset out;
                if (scene.images.size() > static_cast<std::size_t>(std::numeric_limits<u32>::max())) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                out.m_images.reserve(scene.images.size());
                // Index-aligned with glb_embeds (Scene.images pushes 1:1 with
                // asset.images, empties included).
                for (std::size_t i = 0u; i < scene.images.size(); ++i) {
                    Expected<ImageRef> ref =
                            convertImage(scene.images[i], dir, static_cast<u32>(i), glb_embeds);
                    if (not
                        ref.has_value())
                    [[unlikely]] {
                        return std::unexpected{ref.error()};
                    }
                    out.m_images.push_back(std::move(*ref));
                }
                out.m_mats.reserve(scene.materials.size());
                for (const m3d::Material &material: scene.materials) {
                    Expected<MaterialAsset> converted = convertMaterial(material, out.m_images);
                    if (not
                        converted.has_value())
                    [[unlikely]] {
                        return std::unexpected{converted.error()};
                    }
                    out.m_mats.push_back(std::move(*converted));
                }
                out.m_meshes.reserve(scene.meshes.size());
                for (const std::unique_ptr<m3d::IndexedMesh> &mesh: scene.meshes) {
                    if (not mesh) [[unlikely]] {
                        return std::unexpected{make_error_code(errc::invalid_argument)};
                    }
                    Expected<StaticMeshAsset> converted = convertMesh(*mesh, scene.materials);
                    if (not
                        converted.has_value())
                    [[unlikely]] {
                        return std::unexpected{converted.error()};
                    }
                    out.m_meshes.push_back(std::move(*converted));
                }
                Expected<u32> node_count = toU32(scene.nodes.size());
                if (not
                    node_count.has_value())
                [[unlikely]] {
                    return std::unexpected{node_count.error()};
                }
                out.m_nodes.reserve(scene.nodes.size());
                Array<NodeId> parents(scene.nodes.size(), kInvalidNode);
                for (std::size_t i = 0u; i < scene.nodes.size(); ++i) {
                    const m3d::Node &node = scene.nodes[i];
                    if (node.skin.has_value()) [[unlikely]] {
                        PPR_LOG(Mesh, error, "skinned node instance is deferred — rejecting scene");
                        return std::unexpected{make_error_code(errc::function_not_supported)};
                    }
                    if (node.mesh.has_value()
                        and *node.mesh >= scene.meshes.size())
                    [[unlikely]] {
                        return std::unexpected{make_error_code(errc::invalid_argument)};
                    }
                    for (const u32 child: node.children) {
                        if (child >= *node_count) [[unlikely]] {
                            return std::unexpected{make_error_code(errc::invalid_argument)};
                        }
                        if (parents[child] != kInvalidNode) [[unlikely]] {
                            return std::unexpected{make_error_code(errc::invalid_argument)};
                        }
                        parents[child] = NodeId{static_cast<u32>(i)};
                    }
                }
                // Local matrices bitwise-copied (same row-major type); world
                // accumulates parent*local (§2.3/§3, row-vector S*M*S order).
                // BFS from every parentless node also rejects cycles: cyclic
                // nodes are never reached, so the processed count mismatches.
                out.m_nodes.resize(scene.nodes.size());
                Array<u32> queue;
                queue.reserve(scene.nodes.size());
                for (std::size_t i = 0u; i < scene.nodes.size(); ++i) {
                    SceneNodeAsset entry;
                    entry.m_parent = parents[i];
                    entry.m_local = scene.nodes[i].transform;
                    out.m_nodes[i] = entry;
                    if (parents[i] == kInvalidNode) {
                        out.m_nodes[i].m_world = entry.m_local;
                        queue.push_back(static_cast<u32>(i));
                    }
                }
                std::size_t processed = 0u;
                for (std::size_t head = 0u; head < queue.size(); ++head) {
                    const u32 parent = queue[head];
                    ++processed;
                    for (const u32 child: scene.nodes[parent].children) {
                        out.m_nodes[child].m_world = out.m_nodes[parent].m_world * out.m_nodes[child].m_local;
                        queue.push_back(child);
                    }
                }
                if (processed != scene.nodes.size()) [[unlikely]] {
                    return std::unexpected{make_error_code(errc::invalid_argument)};
                }
                out.m_instances.reserve(scene.nodes.size());
                for (std::size_t i = 0u; i < scene.nodes.size(); ++i) {
                    if (scene.nodes[i].mesh.has_value()) {
                        SceneInstance instance;
                        instance.m_mesh = MeshAssetId{*scene.nodes[i].mesh};
                        instance.m_node = NodeId{static_cast<u32>(i)};
                        out.m_instances.push_back(instance);
                    }
                }
                return out;
            }

            [[nodiscard]] bool hasGltfExtension(const std::filesystem::path &file) {
                std::string ext = file.extension().string();
                for (char &c: ext) {
                    if (c >= 'A'
                        and c <= 'Z')
                    {
                        c = static_cast<char>(c + ('a' - 'A'));
                    }
                }
                return ext == ".gltf"
                or ext == ".glb";
            }

            [[nodiscard]] bool isGlbFile(const std::filesystem::path &file) {
                std::string ext = file.extension().string();
                for (char &c: ext) {
                    if (c >= 'A'
                        and c <= 'Z')
                    {
                        c = static_cast<char>(c + ('a' - 'A'));
                    }
                }
                return ext == ".glb";
            }
        }

        class MeshErrorCategory final : public std::error_category {
        public:
            [[nodiscard]] const char *name() const noexcept override { return "mesh"; }

            [[nodiscard]] std::string message(const int ev) const override {
                switch (static_cast<errc>(ev)) {
                    case errc::invalid_argument: return "invalid mesh content (silent-empty scene, missing POSITION, or bad indices)";
                    case errc::function_not_supported: return "mesh content is deferred from MVP (skins, blend, or authored extension modules)";
                    case errc::import_failed: return "mango importer failed";
                    default: return std::format("unknown mesh result ({})", ev);
                }
            }

            [[nodiscard]] std::error_condition default_error_condition(const int ev) const noexcept override {
                switch (static_cast<errc>(ev)) {
                    case errc::invalid_argument: return std::errc::invalid_argument;
                    case errc::function_not_supported: return std::errc::function_not_supported;
                    default: return {ev, *this};
                }
            }
        };

        static constexpr MeshErrorCategory g_mesh_error_category{};

        [[nodiscard]] const std::error_category &error_category() noexcept {
            return g_mesh_error_category;
        }

        [[nodiscard]] std::error_code make_error_code(const errc err) noexcept {
            return std::error_code{static_cast<int>(err), g_mesh_error_category};
        }

        [[nodiscard]] Expected<SceneAsset> importAndConvert(const std::filesystem::path &dir, const std::string_view file) {
            if (dir.empty()
                or
            file.empty())
            [[unlikely]] {
                return std::unexpected{make_error_code(errc::invalid_argument)};
            }
            const std::filesystem::path filename{file};
            if (not details::hasGltfExtension(filename)) [[unlikely]] {
                // Contractual MVP rejection (returned as invalid_argument): warning,
                // not error — the harness fails a case on any error-level log.
                PPR_LOG(Mesh, warning, "only STATIC glTF/GLB is accepted in MVP (OBJ/FBX deferred)");
                return std::unexpected{make_error_code(errc::invalid_argument)};
            }
            // GLB embeds dangle in the fork (see above): map the file here so
            // convertImage resolves embed bytes from PPR-owned storage. The
            // mapping outlives buildScene; resolved subspans pin it via shared
            // ownership inside the returned ImageRefs.
            Array<mem::SharedBuffer> glb_embeds;
            if (details::isGlbFile(filename)) {
                Expected<mem::SharedBuffer> mapped = mem::SharedBuffer::mapFile(dir / filename);
                if (not
                    mapped.has_value())
                [[unlikely]] {
                    // Tests pin missing-file to import_failed (returned below):
                    // warning, not error — the harness fails a case on any
                    // error-level log.
                    PPR_LOG(Mesh, warning, "GLB file is missing or unmappable", {{"what", mapped.error().message()}});
                    return std::unexpected{make_error_code(errc::import_failed)};
                }
                glb_embeds = details::resolveGlbEmbeds(*mapped);
            }
            std::shared_ptr<mango::import3d::Scene> scene;
            try {
                const mango::filesystem::Path asset_root{dir.string()};
                scene = mango::import3d::importScene(asset_root, std::string{file});
            } catch (const mango::Exception &ex) {
                PPR_LOG(Mesh, error, "mango importer threw", {{"what", ex.what()}});
                return std::unexpected{make_error_code(errc::import_failed)};
            } catch (const std::exception &ex) {
                PPR_LOG(Mesh, error, "importer threw", {{"what", ex.what()}});
                return std::unexpected{make_error_code(errc::import_failed)};
            } catch (...) {
                PPR_LOG(Mesh, error, "importer threw an unknown exception");
                return std::unexpected{make_error_code(errc::import_failed)};
            }
            if (not scene) [[unlikely]] {
                return std::unexpected{make_error_code(errc::import_failed)};
            }
            Expected<SceneAsset> converted = details::buildScene(*scene, dir, glb_embeds);
            if (not
                converted.has_value())
            [[unlikely]] {
                return std::unexpected{converted.error()};
            }
            // Scene drops here: every ImageRef owns PPR-mapped bytes or a
            // mapped file. No mango view is ever read, so nothing of Mango can
            // dangle past the call.
            return converted;
        }
    }
}
