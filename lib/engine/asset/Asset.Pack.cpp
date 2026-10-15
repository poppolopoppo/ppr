module;
#include "pP/Macros.h"

module engine.asset;

import :pack;
import engine.core;
import std;

#include "Asset.Identity.Validation.hpp"

namespace pP::asset {
    PPR_DEFINE_LOG_CATEGORY(Asset, debug, none);

    // ------------------------------------------------------------------
    // stable names and identity
    // ------------------------------------------------------------------

    std::string QualifiedName::string() const {
        return m_pack + ":" + m_local;
    }

    bool validLocalName(const std::string_view name) noexcept {
        return config::isValidKey(name);
    }

    bool validNamespace(const std::string_view name) noexcept {
        if (name.empty() or name.size() > 128u) {
            return false;
        }

        std::size_t start{};
        while (start < name.size()) {
            const auto end = name.find('.', start);
            const auto count = end == std::string_view::npos ? name.size() - start : end - start;
            if (not validLocalName(name.substr(start, count))) {
                return false;
            }

            if (end == std::string_view::npos) {
                return true;
            }

            start = end + 1u;
        }

        return false;
    }

    Expected<QualifiedName> qualifyName(const std::string_view reference, const std::string_view declaring_pack) {
        const auto colon = reference.find(':');
        const auto pack = colon == std::string_view::npos ? declaring_pack : reference.substr(0u, colon);
        const auto local = colon == std::string_view::npos ? reference : reference.substr(colon + 1u);
        if (not validNamespace(pack) or not validLocalName(local)) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, config::EConfigErrc::out_of_range);
        }

        return QualifiedName{std::string{pack}, std::string{local}};
    }

    AssetKey identityKey(const QualifiedName &name, const std::string_view kind) noexcept {
        u64 hash = 14695981039346656037ull;
        const auto append = [&](const u8 byte) {
            hash ^= byte;
            hash *= 1099511628211ull;
        };

        for (const unsigned char byte: std::string_view{"ppr.asset.key"}) {
            append(byte);
        }
        append(0u);
        append(1u);

        for (const auto field: {std::string_view{name.m_pack}, std::string_view{name.m_local}, kind}) {
            const auto length = static_cast<u32>(field.size());
            for (u32 shift = 0u; shift < 32u; shift += 8u) {
                append(static_cast<u8>(length >> shift));
            }

            for (const unsigned char byte: field) {
                append(byte);
            }
        }

        return AssetKey{hash};
    }

    // ------------------------------------------------------------------
    // diagnostics and contained owned sources
    // ------------------------------------------------------------------

    std::error_code reject(
        config::Diagnostics &diagnostics, const std::string_view location, const std::string_view property,
        const std::string_view message, const config::EConfigErrc error) {
        const auto code = config::make_error_code(error);
        diagnostics.add(std::string{location}, std::string{property}, std::string{message}, code);
        PPR_RETURN_ERROR_ON_FAIL(Asset, code);
        return {};
    }

    namespace {
        [[nodiscard]] Expected<std::filesystem::path> contained(
            const std::filesystem::path &root, const std::string_view relative, config::Diagnostics &diagnostics) {
            const std::filesystem::path path{relative};
            if (relative.empty() or
                relative.find('\\') != std::string_view::npos or
                relative.find(':') != std::string_view::npos or
                path.is_absolute() or
                path.has_root_path()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(diagnostics, root.string(), relative, "Expected a pack-relative path"));
            }

            for (const auto &component: path) {
                if (component.empty() or component == "." or component == "..") {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Asset, reject(diagnostics, root.string(), relative, "Path dot segments are not permitted"));
                }
            }

            std::error_code error;
            auto resolved = std::filesystem::weakly_canonical(root / path, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, error);

            const auto mismatch = std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end());
            if (mismatch.first != root.end() or mismatch.second == resolved.end()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(diagnostics, root.string(), relative, "Path escapes its declaring pack root"));
            }

            return resolved;
        }

        [[nodiscard]] Expected<DescriptorSource> readSource(
            const std::filesystem::path &path, const std::size_t maximum, config::Diagnostics &diagnostics) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error) {
                diagnostics.add(path.string(), "", "Cannot inspect descriptor source", error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, error);
            }

            if (size > maximum) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(
                               diagnostics, path.string(), "", "Descriptor source exceeds its byte limit",
                               config::EConfigErrc::out_of_range));
            }

            DescriptorSource source{path, std::string(static_cast<std::size_t>(size), '\0')};
            std::ifstream stream{path, std::ios::binary};
            if (not stream or not stream.read(source.m_bytes.data(), static_cast<std::streamsize>(size))) {
                const auto read_error = std::make_error_code(std::errc::io_error);
                diagnostics.add(path.string(), "", "Cannot read complete descriptor source", read_error);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, read_error);
            }

            if (stream.peek() != std::char_traits<char>::eof()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(diagnostics, path.string(), "", "Descriptor source changed while being read"));
            }

            return source;
        }
    }

    Expected<PackSource> readPackSource(const std::filesystem::path &root, config::Diagnostics &diagnostics) {
        std::error_code error;
        auto canonical = std::filesystem::canonical(root, error);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, error);

        auto path = contained(canonical, "pack.toml", diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, path);

        auto source = readSource(*path, kMaxManifestBytes, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, source);

        return PackSource{std::move(canonical), std::move(*source)};
    }

    // ------------------------------------------------------------------
    // immutable pack indexing
    // ------------------------------------------------------------------

    Expected<PackSet> PackSet::index(
        Array<PackSource> sources, const std::span<const std::string_view> supported_kinds,
        config::Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Asset.Pack.Index"};
        std::ranges::sort(sources, {}, [](const auto &source) {
            return std::tuple{
                source.m_root.generic_string(), source.m_manifest.m_path.generic_string(),
                std::string_view{source.m_manifest.m_bytes}};
        });

        PackSet result;
        result.m_packs.reserve(sources.size());

        for (auto &source: sources) {
            if (source.m_manifest.m_bytes.size() > kMaxManifestBytes) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(
                               diagnostics, source.m_manifest.m_path.string(), "", "Manifest exceeds 64 KiB",
                               config::EConfigErrc::out_of_range));
            }

            auto imported = importDescriptor<Manifest>(std::move(source.m_manifest), diagnostics);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, imported);

            if (not validNamespace(imported->m_value.m_pack_id)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(
                               diagnostics, imported->m_source.m_path.string(), "pack_id",
                               "Expected a lowercase ASCII pack namespace"));
            }

            std::error_code error;
            auto root = std::filesystem::canonical(source.m_root, error);
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, error);

            result.m_packs.push_back({std::move(root), std::move(*imported)});
        }

        std::ranges::sort(
            result.m_packs, {}, [](const auto &pack) { return std::string_view{pack.m_manifest.m_value.m_pack_id}; });

        for (std::size_t index = 0u; index < result.m_packs.size(); ++index) {
            const auto &pack = result.m_packs[index];
            const auto &manifest = pack.m_manifest.m_value;
            const auto origin = pack.m_manifest.m_source.m_path.string();
            if (index != 0u and manifest.m_pack_id == result.m_packs[index - 1u].m_manifest.m_value.m_pack_id) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset, reject(
                               diagnostics, origin, "pack_id", "Pack namespace is mounted more than once",
                               config::EConfigErrc::duplicate_key));
            }

            Array<std::string_view> dependencies;
            for (const auto &dependency: manifest.m_requires.m_entries) {
                const auto &id = dependency.m_value.m_pack_id;
                const auto property = "requires." + dependency.m_key + ".pack_id";
                if (not validNamespace(id) or std::ranges::find(dependencies, id) != dependencies.end()) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Asset, reject(diagnostics, origin, property, "Invalid or duplicate dependency namespace"));
                }

                const auto target = std::ranges::find_if(result.m_packs, [&](const auto &candidate) {
                    return candidate.m_manifest.m_value.m_pack_id == id;
                });
                if (target == result.m_packs.end()) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Asset, reject(diagnostics, origin, property, "Required pack is not mounted"));
                }

                dependencies.push_back(id);
            }

            for (const auto &declaration: manifest.m_assets.m_entries) {
                const auto &definition = declaration.m_value;
                const auto property = "assets." + declaration.m_key;
                if (std::ranges::find(supported_kinds, definition.m_kind) == supported_kinds.end()) {
                    PPR_RETURN_UNEXPECTED_ON_FAIL(
                        Asset, reject(
                                   diagnostics, origin, property + ".kind", "Unsupported asset kind",
                                   config::EConfigErrc::unknown_enum_value));
                }

                auto path = contained(pack.m_root, definition.m_descriptor, diagnostics);
                PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, path);

                QualifiedName name{manifest.m_pack_id, declaration.m_key};
                const auto key = identityKey(name, definition.m_kind);
                result.m_assets.push_back({std::move(name), definition.m_kind, std::move(*path), key, index});
            }
        }

        std::ranges::sort(
            result.m_assets, {}, [](const auto &entry) { return std::tie(entry.m_name.m_pack, entry.m_name.m_local); });

        result.m_key_order.reserve(result.m_assets.size());

        for (std::size_t index = 0u; index < result.m_assets.size(); ++index) {
            result.m_key_order.push_back(index);
        }

        std::ranges::sort(
            result.m_key_order, {}, [&](const std::size_t index) { return *result.m_assets[index].m_key; });

        PPR_RETURN_UNEXPECTED_ON_FAIL(
            Asset, validateIdentityCollisions<IndexedAsset>(result.m_assets, result.m_key_order, diagnostics));

        return result;
    }

    // ------------------------------------------------------------------
    // checked references and descriptor access
    // ------------------------------------------------------------------

    std::span<const IndexedPack> PackSet::packs() const noexcept {
        return m_packs;
    }

    std::span<const IndexedAsset> PackSet::assets() const noexcept {
        return m_assets;
    }

    Expected<const IndexedAsset *> PackSet::resolve(
        const std::string_view declaring_pack, const std::string_view reference, const std::string_view expected_kind,
        config::Diagnostics &diagnostics) const {
        auto name = qualifyName(reference, declaring_pack);
        if (not name) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(diagnostics, declaring_pack, reference, "Invalid asset reference name"));
        }

        const auto owner = std::ranges::find_if(
            m_packs, [&](const auto &pack) { return pack.m_manifest.m_value.m_pack_id == declaring_pack; });
        if (owner == m_packs.end()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(diagnostics, declaring_pack, reference, "Declaring pack is not mounted"));
        }

        if (name->m_pack != declaring_pack) {
            const auto &dependency_entries = owner->m_manifest.m_value.m_requires.m_entries;
            const auto dependency = std::ranges::find_if(
                dependency_entries, [&](const auto &entry) { return entry.m_value.m_pack_id == name->m_pack; });
            if (dependency == dependency_entries.end()) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    Asset,
                    reject(
                        diagnostics, declaring_pack, reference, "Cross-pack reference requires a declared dependency"));
            }
        }

        const auto target = std::ranges::lower_bound(
            m_assets, std::tie(name->m_pack, name->m_local), {},
            [](const auto &entry) { return std::tie(entry.m_name.m_pack, entry.m_name.m_local); });
        if (target == m_assets.end() or
            target->m_name.m_pack != name->m_pack or
            target->m_name.m_local != name->m_local) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(diagnostics, declaring_pack, reference, "Referenced asset is not declared"));
        }

        if (target->m_kind != expected_kind) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(
                           diagnostics, declaring_pack, reference, "Referenced asset has the wrong kind",
                           config::EConfigErrc::type_mismatch));
        }

        return std::addressof(*target);
    }

    Expected<const IndexedAsset *> PackSet::resolve(
        const AssetKey key, const std::string_view expected_kind, config::Diagnostics &diagnostics) const {
        const auto found = std::ranges::lower_bound(
            m_key_order, *key, {}, [&](const std::size_t index) { return *m_assets[index].m_key; });
        if (found == m_key_order.end() or *m_assets[*found].m_key != *key) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(diagnostics, "", "", "Asset key is not a member of this pack set"));
        }

        const auto &entry = m_assets[*found];
        if (entry.m_kind != expected_kind) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(
                           diagnostics, entry.m_descriptor.string(), entry.m_name.string(),
                           "Asset key has the wrong kind", config::EConfigErrc::type_mismatch));
        }

        return std::addressof(entry);
    }

    Expected<std::filesystem::path> PackSet::containedPath(
        const std::string_view pack, const std::string_view relative, config::Diagnostics &diagnostics) const {
        const auto owner = std::ranges::find_if(
            m_packs, [&](const auto &entry) { return entry.m_manifest.m_value.m_pack_id == pack; });
        if (owner == m_packs.end()) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, reject(diagnostics, pack, relative, "Declaring pack is not mounted"));
        }

        return contained(owner->m_root, relative, diagnostics);
    }

    Expected<DescriptorSource> PackSet::readDescriptor(
        const AssetKey key, const std::string_view expected_kind, config::Diagnostics &diagnostics) const {
        auto entry = resolve(key, expected_kind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, entry);

        const auto &pack = m_packs[(*entry)->m_pack_index];
        const auto relative = (*entry)->m_descriptor.lexically_relative(pack.m_root).generic_string();
        auto path = contained(pack.m_root, relative, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, path);

        return readSource(*path, kMaxDescriptorBytes, diagnostics);
    }
}
