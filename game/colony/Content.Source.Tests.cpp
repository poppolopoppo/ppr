module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module game.tests;

import engine.asset;
import engine.core;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::detail::content_sources {
    PPR_DEFINE_LOG_CATEGORY(ContentSourceTests, debug, none);

    // ------------------------------------------------------------------
    // complete two-pack text fixtures
    // ------------------------------------------------------------------

    constexpr std::string_view kElement = "schema_version = 1\nphase = \"solid\"\ndensity = 2000.0\n";
    constexpr std::string_view kEditedElement = "schema_version = 1\nphase = \"solid\"\ndensity = 3000.0\n";

    struct Fixture {
        std::filesystem::path m_root, m_base, m_shared;
        colony::ContentSourceSet m_source;

        Fixture() {
            static std::atomic<u32> ordinal{};
            const auto parent = hal::process::currentExecutablePath().parent_path();

            for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
                const auto candidate = parent / ("content-source-test-" + std::to_string(ordinal.fetch_add(1u)));
                if (std::filesystem::create_directory(candidate)) {
                    m_root = std::filesystem::canonical(candidate);
                    break;
                }
            }

            PPR_TEST_ASSERT(not m_root.empty());

            m_base = m_root / "base";
            m_shared = m_root / "shared";
            PPR_TEST_ASSERT(std::filesystem::create_directory(m_base));
            PPR_TEST_ASSERT(std::filesystem::create_directory(m_shared));

            m_source.m_packs = {
                {m_base,
                    {m_base / "pack.toml", "schema_version = 1\npack_id = \"ppr.base\"\n"
                                           "[requires.shared]\npack_id = \"ppr.shared\"\n"
                                           "[assets.stone]\nkind = \"sim.element\"\ndescriptor = \"Original.TOML\"\n"}},
                {m_shared,
                    {m_shared / "pack.toml", "schema_version = 1\npack_id = \"ppr.shared\"\n"
                                             "[assets.solid]\nkind = \"sim.element\"\ndescriptor = \"shared.toml\"\n"}},
            };
            m_source.m_descriptors = {
                {m_base / "Original.TOML", std::string{kElement}},
                {m_shared / "shared.toml", std::string{kElement}},
            };

            for (const auto &pack: m_source.m_packs) {
                write(pack.m_manifest.m_path, pack.m_manifest.m_bytes);
            }

            for (const auto &descriptor: m_source.m_descriptors) {
                write(descriptor.m_path, descriptor.m_bytes);
            }
        }

        ~Fixture() {
            // Best-effort fixture teardown, after all synchronous readers have returned.
            std::error_code error;
            std::filesystem::remove_all(m_root, error);
            PPR_LOG_WARNING_ON_FAIL(ContentSourceTests, error);
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        static void write(const std::filesystem::path &path, const std::string_view bytes) {
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.close();
            PPR_TEST_ASSERT(not stream.fail());
        }
    };

    void padComment(std::string &bytes, const std::size_t size) {
        PPR_TEST_ASSERT(bytes.size() < size);
        bytes += '#';
        bytes.resize(size, 'x');
    }

    void appendElement(
        colony::ContentSourceSet &source, const std::size_t pack_index, const std::string_view key,
        const std::string_view relative) {
        auto &pack = source.m_packs[pack_index];
        pack.m_manifest.m_bytes += "\n[assets." +
                                   std::string{key} +
                                   "]\nkind = \"sim.element\"\ndescriptor = \"" +
                                   std::string{relative} +
                                   "\"\n";
        source.m_descriptors.push_back({pack.m_root / relative, std::string{kElement}});
    }

    // ------------------------------------------------------------------
    // complete source admission and retained text limits
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (empty_roots_and_missing_disk_sources_return_errors) {
        config::Diagnostics diagnostics;
        const auto empty = colony::ContentSourceSnapshot::index(colony::ContentSourceSet{}, diagnostics);
        PPR_TEST_ASSERT(not empty and empty.error() == std::errc::invalid_argument);

        const auto no_roots = colony::loadContentSourceSet(std::span<const std::filesystem::path>{}, diagnostics);
        PPR_TEST_ASSERT(not no_roots and no_roots.error() == std::errc::invalid_argument);

        Fixture fixture;
        const std::filesystem::path roots[] = {fixture.m_base, fixture.m_shared};
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_source.m_descriptors.front().m_path));
        const auto missing_descriptor = colony::loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not missing_descriptor and missing_descriptor.error() == std::errc::no_such_file_or_directory);

        Fixture::write(fixture.m_source.m_descriptors.front().m_path, kElement);
        PPR_TEST_ASSERT(std::filesystem::remove(fixture.m_shared / "pack.toml"));
        const auto missing_manifest = colony::loadContentSourceSet(roots, diagnostics);
        PPR_TEST_ASSERT(not missing_manifest and missing_manifest.error() == std::errc::no_such_file_or_directory);

        Fixture::write(fixture.m_shared / "pack.toml", "");
        PPR_TEST_ASSERT(not colony::loadContentSourceSet(roots, diagnostics));
    };

    PPR_UNIT_TEST (source_membership_is_exact_and_mount_order_is_irrelevant) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto indexed = colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics);
        PPR_TEST_ASSERT(indexed.has_value());
        PPR_TEST_ASSERT(indexed->packOwner()->assets().size() == 2u);

        auto reversed = fixture.m_source;
        std::ranges::reverse(reversed.m_packs);
        std::ranges::reverse(reversed.m_descriptors);
        auto reordered = colony::ContentSourceSnapshot::index(reversed, diagnostics);
        PPR_TEST_ASSERT(reordered.has_value());
        PPR_TEST_ASSERT(reversed.sameMounts(fixture.m_source));
        PPR_TEST_ASSERT(reversed.sameMounts(*indexed->packOwner()));

        const auto initial_assets = indexed->packOwner()->assets();
        const auto reordered_assets = reordered->packOwner()->assets();
        PPR_TEST_ASSERT(initial_assets.size() == reordered_assets.size());

        for (std::size_t index = 0u; index < initial_assets.size(); ++index) {
            PPR_TEST_ASSERT(initial_assets[index].m_key == reordered_assets[index].m_key);
            PPR_TEST_ASSERT(initial_assets[index].m_descriptor == reordered_assets[index].m_descriptor);
        }

        auto missing = fixture.m_source;
        missing.m_descriptors.pop_back();
        const auto incomplete = colony::ContentSourceSnapshot::index(missing, diagnostics);
        PPR_TEST_ASSERT(not incomplete and incomplete.error() == config::EConfigErrc::missing_value);

        auto orphan = fixture.m_source;
        orphan.m_descriptors.push_back({fixture.m_base / "unreferenced.toml", std::string{kElement}});
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(std::move(orphan), diagnostics));

        auto duplicate = fixture.m_source;
        duplicate.m_descriptors.push_back(duplicate.m_descriptors.front());
        const auto repeated = colony::ContentSourceSnapshot::index(std::move(duplicate), diagnostics);
        PPR_TEST_ASSERT(not repeated and repeated.error() == config::EConfigErrc::duplicate_key);

        reversed.m_packs.front().m_root = fixture.m_root;
        PPR_TEST_ASSERT(not reversed.sameMounts(fixture.m_source));
        PPR_TEST_ASSERT(not reversed.sameMounts(*indexed->packOwner()));
    };

    PPR_UNIT_TEST (file_and_cross_pack_aggregate_text_limits_are_enforced) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto oversized_manifest = fixture.m_source;
        padComment(oversized_manifest.m_packs.front().m_manifest.m_bytes, asset::kMaxManifestBytes + 1u);
        const auto manifest_rejected = colony::ContentSourceSnapshot::index(std::move(oversized_manifest), diagnostics);
        PPR_TEST_ASSERT(not manifest_rejected and manifest_rejected.error() == config::EConfigErrc::out_of_range);

        auto oversized_descriptor = fixture.m_source;
        padComment(oversized_descriptor.m_descriptors.front().m_bytes, asset::kMaxDescriptorBytes + 1u);
        const auto descriptor_rejected = colony::ContentSourceSnapshot::index(
            std::move(oversized_descriptor), diagnostics);
        PPR_TEST_ASSERT(not descriptor_rejected and descriptor_rejected.error() == config::EConfigErrc::out_of_range);

        auto boundary = fixture.m_source;
        for (std::size_t index = 0u; index < 5u; ++index) {
            const auto key = "budget_" + std::to_string(index);
            appendElement(boundary, index % 2u, key, "elements/asset-" + key + ".toml");
        }

        std::size_t total{};
        for (auto &pack: boundary.m_packs) {
            padComment(pack.m_manifest.m_bytes, asset::kMaxManifestBytes);
            total += pack.m_manifest.m_bytes.size();
        }

        for (std::size_t index = 0u; index + 1u < boundary.m_descriptors.size(); ++index) {
            padComment(boundary.m_descriptors[index].m_bytes, asset::kMaxDescriptorBytes);
            total += boundary.m_descriptors[index].m_bytes.size();
        }

        const auto last_size = colony::ContentSourceLimits::maximum_text_bytes - total;
        PPR_TEST_ASSERT(last_size <= asset::kMaxDescriptorBytes);
        padComment(boundary.m_descriptors.back().m_bytes, last_size);
        const auto accepted = colony::ContentSourceSnapshot::index(boundary, diagnostics);
        PPR_TEST_ASSERT(accepted.has_value());

        boundary.m_descriptors.back().m_bytes += 'x';
        PPR_TEST_ASSERT(boundary.m_descriptors.back().m_bytes.size() <= asset::kMaxDescriptorBytes);
        const auto aggregate_rejected = colony::ContentSourceSnapshot::index(std::move(boundary), diagnostics);
        PPR_TEST_ASSERT(not aggregate_rejected and aggregate_rejected.error() == config::EConfigErrc::out_of_range);
    };

    // ------------------------------------------------------------------
    // immutable owned text and synchronous reader lifetime
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (edited_snapshot_owns_text_and_provenance_after_sources_disappear) {
        config::Diagnostics diagnostics;
        std::optional<colony::ContentSourceSnapshot> retained;
        std::filesystem::path edited_path;

        {
            Fixture fixture;
            const std::filesystem::path roots[] = {fixture.m_base, fixture.m_shared};
            auto disk = colony::loadContentSourceSet(roots, diagnostics);
            PPR_TEST_ASSERT(disk.has_value());

            auto draft = disk->source();
            edited_path = draft.m_descriptors.front().m_path;
            draft.m_descriptors.front().m_bytes = kEditedElement;
            auto edited = colony::ContentSourceSnapshot::index(draft, diagnostics);
            PPR_TEST_ASSERT(edited.has_value());
            retained.emplace(std::move(*edited));

            draft.m_descriptors.clear();
            draft.m_packs.clear();
            Fixture::write(edited_path, "invalid external bytes");
            const auto unchanged = colony::loadContentCatalog(*disk, diagnostics);
            PPR_TEST_ASSERT(unchanged.has_value());
            PPR_TEST_ASSERT(unchanged->elements().definitions().front().m_descriptor.m_value.m_density == 2000.0);
        }

        PPR_TEST_ASSERT(not std::filesystem::exists(edited_path));
        {
            const auto scratch = mem::ScratchPad::open();
            Array<std::string, mem::ScratchPad> reuse(128u, std::string(128u, 'x'));
        }

        const auto imported = colony::loadContentCatalog(*retained, diagnostics);
        PPR_TEST_ASSERT(imported.has_value());
        PPR_TEST_ASSERT(imported->elements().definitions().size() == 2u);
        const auto &element = imported->elements().definitions().front().m_descriptor;
        PPR_TEST_ASSERT(element.m_value.m_density == 3000.0);
        PPR_TEST_ASSERT(element.m_source.m_path == edited_path);
        PPR_TEST_ASSERT(element.m_source.m_bytes == kEditedElement);

        const auto origin = std::ranges::find(element.m_origins, "density", &config::PropertyOrigin::m_name);
        PPR_TEST_ASSERT(origin != element.m_origins.end());
        PPR_TEST_ASSERT(origin->m_location == edited_path.string() + ":3");
        PPR_TEST_ASSERT(retained->packOwner()->packs().front().m_manifest.m_value.m_pack_id == "ppr.base");
    };

    PPR_UNIT_TEST (required_reader_acquires_once_and_missing_or_empty_records_do_not_use_disk) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        std::map<std::filesystem::path, std::size_t> reads;
        std::optional<colony::ContentSourceSnapshot> retained;

        {
            const auto reader = [&](const asset::IndexedAsset &entry, config::Diagnostics &local) {
                ++reads[entry.m_descriptor];
                return fixture.m_source.readDescriptor(entry, local);
            };

            auto snapshot = colony::ContentSourceSnapshot::index(fixture.m_source.m_packs, reader, {}, diagnostics);
            PPR_TEST_ASSERT(snapshot.has_value());
            retained.emplace(std::move(*snapshot));
        }

        PPR_TEST_ASSERT(reads.size() == 2u);
        for (const auto &[path, count]: reads) {
            PPR_TEST_ASSERT(count == 1u);
        }

        PPR_TEST_ASSERT(colony::loadContentCatalog(*retained, diagnostics).has_value());

        for (const auto &[path, count]: reads) {
            PPR_TEST_ASSERT(count == 1u);
        }

        auto missing = retained->source();
        missing.m_descriptors.clear();
        const auto missing_reader = [&](const asset::IndexedAsset &entry, config::Diagnostics &local) {
            return missing.readDescriptor(entry, local);
        };

        const auto absent = colony::ContentSourceSnapshot::index(missing.m_packs, missing_reader, {}, diagnostics);
        PPR_TEST_ASSERT(not absent and absent.error() == config::EConfigErrc::missing_value);
        const auto missing_catalog = colony::loadContentCatalog(missing, diagnostics);
        PPR_TEST_ASSERT(not missing_catalog and missing_catalog.error() == config::EConfigErrc::missing_value);

        auto empty = retained->source();
        empty.m_descriptors.front().m_bytes.clear();
        auto empty_snapshot = colony::ContentSourceSnapshot::index(std::move(empty), diagnostics);
        PPR_TEST_ASSERT(empty_snapshot.has_value());
        PPR_TEST_ASSERT(not colony::loadContentCatalog(*empty_snapshot, diagnostics));
        PPR_TEST_ASSERT(std::filesystem::exists(fixture.m_source.m_descriptors.front().m_path));
    };

    // ------------------------------------------------------------------
    // new targets, restoration authority and physical ownership
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (new_targets_are_injective_and_device_safe_for_valid_keys) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto snapshot = colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics);
        PPR_TEST_ASSERT(snapshot.has_value());
        auto source = snapshot->source();

        for (const auto key: {"con", "nul"}) {
            std::set<std::filesystem::path> targets;

            for (const auto kind: colony::kContentAssetKinds) {
                const auto path = colony::newContentDescriptorPath(
                    *snapshot->packOwner(), {"ppr.base", key}, kind, diagnostics);
                PPR_TEST_ASSERT(path.has_value());
                PPR_TEST_ASSERT(path->filename() == "asset-" + std::string{key} + ".toml");
                PPR_TEST_ASSERT(path->parent_path().parent_path() == fixture.m_base);
                PPR_TEST_ASSERT(targets.insert(*path).second);
            }

            const auto path = colony::newContentDescriptorPath(
                *snapshot->packOwner(), {"ppr.base", key}, sim::asset::kElementKind, diagnostics);
            PPR_TEST_ASSERT(path.has_value());
            PPR_TEST_ASSERT(not std::filesystem::exists(*path));
            appendElement(source, 0u, key, path->lexically_relative(fixture.m_base).generic_string());
        }

        const auto generated = colony::ContentSourceSnapshot::index(std::move(source), diagnostics);
        PPR_TEST_ASSERT(generated.has_value());
        const auto original = generated->packOwner()->resolve(
            "ppr.base", "stone", sim::asset::kElementKind, diagnostics);
        PPR_TEST_ASSERT(original.has_value());
        PPR_TEST_ASSERT((*original)->m_descriptor.filename() == "Original.TOML");
        PPR_TEST_ASSERT(not colony::newContentDescriptorPath(
            *snapshot->packOwner(), {"ppr.base", "../escape"}, sim::asset::kElementKind, diagnostics));
        PPR_TEST_ASSERT(not colony::newContentDescriptorPath(
            *snapshot->packOwner(), {"ppr.base", "valid"}, "unsupported.kind", diagnostics));
    };

    PPR_UNIT_TEST (arbitrary_missing_targets_require_exact_minted_restoration_proof) {
        Fixture fixture;
        config::Diagnostics diagnostics;
        auto snapshot = colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics);
        PPR_TEST_ASSERT(snapshot.has_value());
        const auto entry = snapshot->packOwner()->resolve("ppr.base", "stone", sim::asset::kElementKind, diagnostics);
        PPR_TEST_ASSERT(entry.has_value());
        auto proof = snapshot->ownedTarget((*entry)->m_key, diagnostics);
        PPR_TEST_ASSERT(proof.has_value());
        PPR_TEST_ASSERT(proof->name().string() == "ppr.base:stone");
        PPR_TEST_ASSERT(proof->kind() == sim::asset::kElementKind);
        PPR_TEST_ASSERT(proof->path() == fixture.m_base / "Original.TOML");
        PPR_TEST_ASSERT(proof->root() == fixture.m_base);

        PPR_TEST_ASSERT(std::filesystem::remove(proof->path()));
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics));
        auto moved = std::move(*proof);
        PPR_TEST_ASSERT(not proof->matches(*snapshot->packOwner(), **entry));
        PPR_TEST_ASSERT(moved.matches(*snapshot->packOwner(), **entry));
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(
            fixture.m_source, std::span{std::addressof(*proof), 1u}, diagnostics));

        auto assigned = moved;
        assigned = std::move(moved);
        PPR_TEST_ASSERT(not moved.matches(*snapshot->packOwner(), **entry));
        PPR_TEST_ASSERT(
            colony::ContentSourceSnapshot::index(fixture.m_source, std::span{std::addressof(assigned), 1u}, diagnostics)
                .has_value());

        auto renamed = fixture.m_source;
        auto &renamed_manifest = renamed.m_packs.front().m_manifest.m_bytes;
        renamed_manifest.replace(
            renamed_manifest.find("assets.stone"), std::string_view{"assets.stone"}.size(), "assets.other");
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(
            std::move(renamed), std::span{std::addressof(assigned), 1u}, diagnostics));

        auto changed_kind = fixture.m_source;
        auto &kind_manifest = changed_kind.m_packs.front().m_manifest.m_bytes;
        kind_manifest.replace(kind_manifest.find("sim.element"), sim::asset::kElementKind.size(), "sim.field");
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(
            std::move(changed_kind), std::span{std::addressof(assigned), 1u}, diagnostics));

        auto changed_path = fixture.m_source;
        auto &path_manifest = changed_path.m_packs.front().m_manifest.m_bytes;
        path_manifest.replace(
            path_manifest.find("Original.TOML"), std::string_view{"Original.TOML"}.size(), "elsewhere.toml");
        changed_path.m_descriptors.front().m_path = fixture.m_base / "elsewhere.toml";
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(
            std::move(changed_path), std::span{std::addressof(assigned), 1u}, diagnostics));

        Fixture relocated;
        PPR_TEST_ASSERT(std::filesystem::remove(relocated.m_source.m_descriptors.front().m_path));
        PPR_TEST_ASSERT(not colony::ContentSourceSnapshot::index(
            relocated.m_source, std::span{std::addressof(assigned), 1u}, diagnostics));
    };

    PPR_UNIT_TEST (physical_aliases_cannot_give_two_sources_ownership_of_one_file) {
        config::Diagnostics diagnostics;

        {
            Fixture fixture;
            const auto &first = fixture.m_source.m_descriptors.front().m_path;
            const auto &second = fixture.m_source.m_descriptors.back().m_path;
            PPR_TEST_ASSERT(std::filesystem::remove(first));
            std::filesystem::create_hard_link(second, first);

            fixture.m_source.m_descriptors.front().m_bytes = kEditedElement;
            PPR_TEST_ASSERT(
                fixture.m_source.m_descriptors.front().m_bytes != fixture.m_source.m_descriptors.back().m_bytes);

            const auto aliased = colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics);
            PPR_TEST_ASSERT(not aliased and aliased.error() == config::EConfigErrc::duplicate_key);
        }

        {
            Fixture fixture;
            const auto &descriptor = fixture.m_source.m_descriptors.front().m_path;
            PPR_TEST_ASSERT(std::filesystem::remove(descriptor));
            std::filesystem::create_hard_link(fixture.m_base / "pack.toml", descriptor);

            const auto manifest_alias = colony::ContentSourceSnapshot::index(fixture.m_source, diagnostics);
            PPR_TEST_ASSERT(not manifest_alias and manifest_alias.error() == config::EConfigErrc::duplicate_key);
        }
    };
}

namespace pP::game::tests {
    // ------------------------------------------------------------------
    // complete owned source suite
    // ------------------------------------------------------------------

    const UnitTest content_sources = UnitTest::Named("content_sources") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::content_sources::empty_roots_and_missing_disk_sources_return_errors,
            detail::content_sources::source_membership_is_exact_and_mount_order_is_irrelevant,
            detail::content_sources::file_and_cross_pack_aggregate_text_limits_are_enforced,
            detail::content_sources::edited_snapshot_owns_text_and_provenance_after_sources_disappear,
            detail::content_sources::required_reader_acquires_once_and_missing_or_empty_records_do_not_use_disk,
            detail::content_sources::new_targets_are_injective_and_device_safe_for_valid_keys,
            detail::content_sources::arbitrary_missing_targets_require_exact_minted_restoration_proof,
            detail::content_sources::physical_aliases_cannot_give_two_sources_ownership_of_one_file,
        });
    };

    const UnitTest &gameContentSourceTests() noexcept {
        return content_sources;
    }
}
