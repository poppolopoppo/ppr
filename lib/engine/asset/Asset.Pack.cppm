module;
#include "pP/Macros.h"

export module engine.asset:pack;

import engine.core;
import std;

export namespace pP::asset {
    // ------------------------------------------------------------------
    // stable names and identity
    // ------------------------------------------------------------------

    PPR_DECLARE_LOG_CATEGORY(Asset);

    struct AssetKeyTag;
    struct RequestGenerationTag;
    using AssetKey = Numeric<u64, AssetKeyTag>;
    using RequestGeneration = Numeric<u64, RequestGenerationTag>;

    struct QualifiedName {
        static constexpr std::size_t maximum_namespace_bytes = 128u;
        static constexpr config::StringDomain namespace_domain{maximum_namespace_bytes, false};
        static constexpr config::StringDomain reference_domain{
            maximum_namespace_bytes + 1u + config::kMaxKeyBytes, false};

        std::string m_pack{}, m_local{};

        [[nodiscard]] std::string string() const;
    };

    [[nodiscard]] bool validLocalName(std::string_view name) noexcept;
    [[nodiscard]] bool validNamespace(std::string_view name) noexcept;
    [[nodiscard]] Expected<QualifiedName> qualifyName(std::string_view reference, std::string_view declaring_pack);

    /// FNV-1a-64: "ppr.asset.key", NUL, 0x01, then namespace/key/kind,
    /// each prefixed by its unsigned 32-bit little-endian byte length.
    /// Inputs must already be canonical; kind is the explicit textual identifier.
    [[nodiscard]] AssetKey identityKey(const QualifiedName &name, std::string_view kind) noexcept;

    // ------------------------------------------------------------------
    // owned descriptor import
    // ------------------------------------------------------------------

    inline constexpr std::size_t kMaxDescriptorBytes = 1024u * 1024u;
    inline constexpr std::size_t kMaxManifestBytes = 64u * 1024u;

    [[nodiscard]] constexpr config::Metadata metadata(const std::string_view name) {
        return {name, "Asset descriptor", "Assets", config::EMutability::startup, config::ESource::file};
    }

    struct DescriptorSource {
        std::filesystem::path m_path{};
        std::string m_bytes{};
    };

    template<typename Descriptor>
    struct Imported {
        Descriptor m_value{};
        DescriptorSource m_source{};
        Array<config::PropertyOrigin> m_origins{};
    };

    [[nodiscard]] std::error_code reject(
        config::Diagnostics &diagnostics, std::string_view location, std::string_view property,
        std::string_view message, config::EConfigErrc error = config::EConfigErrc::cross_field_invalid);

    template<typename Descriptor>
    [[nodiscard]] Expected<Imported<Descriptor>> importDescriptor(
        DescriptorSource source, config::Diagnostics &diagnostics) {
        if (source.m_bytes.size() > kMaxDescriptorBytes) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                Asset, reject(
                           diagnostics, source.m_path.string(), "", "Descriptor exceeds 1 MiB",
                           config::EConfigErrc::out_of_range));
        }

        auto document = config::decode(source.m_bytes, source.m_path.string(), diagnostics, config::EDecodeMode::tree);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, document);

        const config::TreeLayer layers[] = {config::toTreeLayer(std::move(*document), source.m_path.string())};
        auto resolved = config::resolveRecursive<Descriptor>(layers, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, resolved);

        return Imported<Descriptor>{std::move(resolved->m_value), std::move(source), std::move(resolved->m_origins)};
    }

    // ------------------------------------------------------------------
    // manifest schema
    // ------------------------------------------------------------------

    struct AssetDeclaration {
        std::string m_kind{}, m_descriptor{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &AssetDeclaration::m_kind, metadata("kind"), config::StringDomain{128u, false},
                    config::EPresence::required),
                config::field(
                    &AssetDeclaration::m_descriptor, metadata("descriptor"), config::StringDomain{1024u, false},
                    config::EPresence::required)};
        }
    };

    struct PackDependency {
        std::string m_pack_id{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{config::field(
                &PackDependency::m_pack_id, metadata("pack_id"), QualifiedName::namespace_domain,
                config::EPresence::required)};
        }
    };

    struct Manifest {
        std::string m_pack_id{};
        config::KeyedCollection<AssetDeclaration> m_assets{};
        config::KeyedCollection<PackDependency> m_requires{};
        u32 m_schema_version{};

        [[nodiscard]] static constexpr auto describe() {
            return std::tuple{
                config::field(
                    &Manifest::m_schema_version, metadata("schema_version"), config::Range{1u, 1u},
                    config::EPresence::required),
                config::field(
                    &Manifest::m_pack_id, metadata("pack_id"), QualifiedName::namespace_domain,
                    config::EPresence::required),
                config::collection(&Manifest::m_requires, metadata("requires"), {0u, 128u}),
                config::collection(&Manifest::m_assets, metadata("assets"), {0u, 4096u})};
        }
    };

    struct PackSource {
        std::filesystem::path m_root{};
        DescriptorSource m_manifest{};
    };

    struct IndexedPack {
        std::filesystem::path m_root{};
        Imported<Manifest> m_manifest{};
    };

    struct IndexedAsset {
        QualifiedName m_name{};
        std::string m_kind{};
        std::filesystem::path m_descriptor{};
        AssetKey m_key{};
        std::size_t m_pack_index{};
    };

    // ------------------------------------------------------------------
    // immutable request index and checked lookup
    // ------------------------------------------------------------------

    class PackSet {
        // state:
        Array<IndexedPack> m_packs{};
        Array<IndexedAsset> m_assets{};
        Array<std::size_t> m_key_order{};

        // construction:
        PackSet() = default;

    public:
        // indexing:
        /// Supported kinds belong to the composition boundary, never a payload registry.
        [[nodiscard]] static Expected<PackSet> index(
            Array<PackSource> sources, std::span<const std::string_view> supported_kinds,
            config::Diagnostics &diagnostics);

        // queries:
        [[nodiscard]] std::span<const IndexedPack> packs() const noexcept;
        [[nodiscard]] std::span<const IndexedAsset> assets() const noexcept;

        /// Returned pointers observe this immutable value and cannot outlive it.
        [[nodiscard]] Expected<const IndexedAsset *> resolve(
            std::string_view declaring_pack, std::string_view reference, std::string_view expected_kind,
            config::Diagnostics &diagnostics) const;

        [[nodiscard]] Expected<const IndexedAsset *> resolve(
            AssetKey key, std::string_view expected_kind, config::Diagnostics &diagnostics) const;

        // sources:
        [[nodiscard]] Expected<DescriptorSource> readDescriptor(
            AssetKey key, std::string_view expected_kind, config::Diagnostics &diagnostics) const;

        [[nodiscard]] Expected<std::filesystem::path> containedPath(
            std::string_view pack, std::string_view relative, config::Diagnostics &diagnostics) const;
    };

    [[nodiscard]] Expected<PackSource> readPackSource(
        const std::filesystem::path &root, config::Diagnostics &diagnostics);

    template<typename Descriptor>
    [[nodiscard]] Expected<Imported<Descriptor>> importDescriptor(
        const PackSet &packs, const AssetKey key, const std::string_view expected_kind,
        config::Diagnostics &diagnostics) {
        auto source = packs.readDescriptor(key, expected_kind, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Asset, source);

        return importDescriptor<Descriptor>(std::move(*source), diagnostics);
    }
}
