module;
#include "pP/Macros.h"
#include "pP/UnitTest.h"

module engine.tests.asset;

import engine.asset;
import engine.core;
import std;

#include "../../asset/Asset.Identity.Validation.hpp"

namespace pP::tests::detail::Packs {
    // ------------------------------------------------------------------
    // owned manifest fixtures
    // ------------------------------------------------------------------

    constexpr std::string_view kKinds[] = {"sim.material", "sim.world", "image.texture"};

    PPR_DEFINE_LOG_CATEGORY(AssetPackReaderTests, verbose, none)

    asset::PackSource source(const std::string_view name, std::string bytes) {
        const auto root = std::filesystem::current_path();
        return {root, {root / std::string{name}, std::move(bytes)}};
    }

    Array<asset::PackSource> twoPacks() {
        return {
            source("base.pack.toml", R"(schema_version = 1
pack_id = "ppr.base"
[requires.shared]
pack_id = "ppr.shared"
[assets.world]
kind = "sim.world"
descriptor = "worlds/frontier.toml"
)"),
            source("shared.pack.toml", R"(schema_version = 1
pack_id = "ppr.shared"
[assets.rock]
kind = "sim.material"
descriptor = "materials/rock.toml"
[assets.rock_albedo]
kind = "image.texture"
descriptor = "textures/rock.toml"
)")};
    }

    // ------------------------------------------------------------------
    // stable identity and naming
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (canonical_names_and_frozen_identity) {
        const auto local = asset::qualifyName("rock", "ppr.base");
        const auto qualified = asset::qualifyName("ppr.base:rock", "ppr.shared");
        PPR_TEST_ASSERT(local.has_value() and qualified.has_value());
        PPR_TEST_ASSERT(local->string() == "ppr.base:rock");
        PPR_TEST_ASSERT(local->string() == qualified->string());
        PPR_TEST_ASSERT(*asset::identityKey(*local, "sim.material") == 0xfeb0b05e2204a87bull);
        PPR_TEST_ASSERT(*asset::identityKey({"ppr.shared", "rock_albedo"}, "image.texture") == 0x5c7286efae017167ull);
        PPR_TEST_ASSERT(*asset::identityKey({"a", "b"}, "c") == 0x63b514d865ddaf22ull);

        for (const auto invalid:
            {"Ppr.base:rock", "ppr..base:rock", "ppr.base.:rock", ":rock", "ppr.base:", "ppr.base:rock:stone",
                "rock.stone", "röck", "../rock"}) {
            PPR_TEST_ASSERT(not asset::qualifyName(invalid, "ppr.base"));
        }

        PPR_TEST_ASSERT(asset::validLocalName("rock_1-sand"));
        PPR_TEST_ASSERT(not asset::validLocalName(std::string(config::kMaxKeyBytes + 1u, 'a')));
        PPR_TEST_ASSERT(
            not asset::validNamespace(std::string(asset::QualifiedName::maximum_namespace_bytes + 1u, 'a')));
        PPR_TEST_ASSERT(*asset::identityKey(*local, "sim.material") != *asset::identityKey(*local, "image.texture"));

        const std::string longest_local(config::kMaxKeyBytes, 'a');
        const std::string longest_namespace(asset::QualifiedName::maximum_namespace_bytes, 'p');
        const auto longest_reference = longest_namespace + ":" + longest_local;
        PPR_TEST_ASSERT(config::isValidKey(longest_local) and asset::validLocalName(longest_local));
        PPR_TEST_ASSERT(not asset::QualifiedName::reference_domain.validate(longest_reference));
        PPR_TEST_ASSERT(asset::qualifyName(longest_reference, "ppr.base").has_value());

        const auto too_long_reference = longest_reference + "a";
        PPR_TEST_ASSERT(
            asset::QualifiedName::reference_domain.validate(too_long_reference) == config::EConfigErrc::out_of_range);
        PPR_TEST_ASSERT(not asset::qualifyName(too_long_reference, "ppr.base"));
    };

    PPR_UNIT_TEST (two_pack_checked_resolution_is_mount_order_independent) {
        config::Diagnostics diagnostics{16u};
        auto input = twoPacks();
        auto first = asset::PackSet::index(input, kKinds, diagnostics);
        PPR_TEST_ASSERT(first.has_value());

        std::ranges::reverse(input);
        auto second = asset::PackSet::index(std::move(input), kKinds, diagnostics);
        PPR_TEST_ASSERT(second.has_value());
        PPR_TEST_ASSERT(first->assets().size() == second->assets().size());

        for (std::size_t index = 0u; index < first->assets().size(); ++index) {
            PPR_TEST_ASSERT(*first->assets()[index].m_key == *second->assets()[index].m_key);
            PPR_TEST_ASSERT(first->assets()[index].m_name.string() == second->assets()[index].m_name.string());
        }

        const auto rock = first->resolve("ppr.base", "ppr.shared:rock", "sim.material", diagnostics);
        PPR_TEST_ASSERT(rock.has_value());
        PPR_TEST_ASSERT((*rock)->m_name.string() == "ppr.shared:rock");
        PPR_TEST_ASSERT(first->resolve((*rock)->m_key, "sim.material", diagnostics).has_value());
        PPR_TEST_ASSERT(not diagnostics.hasErrors());

        PPR_TEST_ASSERT(not first->resolve("ppr.base", "rock", "sim.material", diagnostics));
        const auto wrong_kind = first->resolve((*rock)->m_key, "image.texture", diagnostics);
        PPR_TEST_ASSERT(not wrong_kind and wrong_kind.error() == config::EConfigErrc::type_mismatch);
        PPR_TEST_ASSERT(not first->resolve(asset::AssetKey{42u}, "sim.material", diagnostics));
        PPR_TEST_ASSERT(not first->resolve("ppr.shared", "ppr.base:world", "sim.world", diagnostics));
    };

    // ------------------------------------------------------------------
    // rejection and owned import contracts
    // ------------------------------------------------------------------

    PPR_UNIT_TEST (duplicate_namespace_dependencies_and_missing_pack_reject) {
        config::Diagnostics diagnostics{4u};
        auto duplicates = twoPacks();
        duplicates.push_back(duplicates.front());
        const auto duplicate = asset::PackSet::index(std::move(duplicates), kKinds, diagnostics);
        PPR_TEST_ASSERT(not duplicate and duplicate.error() == config::EConfigErrc::duplicate_key);

        auto missing = twoPacks();
        missing.pop_back();
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(missing), kKinds, diagnostics));

        auto repeated = twoPacks();
        repeated.front().m_manifest.m_bytes += "\n[requires.again]\npack_id = \"ppr.shared\"\n";
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(repeated), kKinds, diagnostics));
    };

    PPR_UNIT_TEST (unsupported_kind_duplicate_local_key_and_old_manifest_reject) {
        config::Diagnostics diagnostics{4u};
        auto unsupported = twoPacks();
        unsupported.back().m_manifest.m_bytes +=
            "\n[assets.sound]\nkind = \"audio.clip\"\ndescriptor = \"audio/x.toml\"\n";
        const auto unknown = asset::PackSet::index(std::move(unsupported), kKinds, diagnostics);
        PPR_TEST_ASSERT(not unknown and unknown.error() == config::EConfigErrc::unknown_enum_value);

        auto repeated = twoPacks();
        repeated.back().m_manifest.m_bytes += "\n[assets.rock]\nkind = \"image.texture\"\ndescriptor = \"rock.toml\"\n";
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(repeated), kKinds, diagnostics));

        Array<asset::PackSource> old_format{source(
            "old.pack.toml", "schema_version = 1\npack_id = \"old\"\n[files]\nmaterials = \"materials.toml\"\n")};
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(old_format), kKinds, diagnostics));
    };

    PPR_UNIT_TEST (containment_rejects_absolute_and_dot_segment_paths) {
        config::Diagnostics diagnostics{16u};
        auto packs = asset::PackSet::index(twoPacks(), kKinds, diagnostics);
        PPR_TEST_ASSERT(packs.has_value());

        for (const auto invalid:
            {"../outside.toml", "a/../outside.toml", "./file.toml", "/absolute.toml", "C:/absolute.toml",
                "a\\file.toml", ""}) {
            PPR_TEST_ASSERT(not packs->containedPath("ppr.base", invalid, diagnostics));
        }

        PPR_TEST_ASSERT(packs->containedPath("ppr.base", "images/rock.png", diagnostics).has_value());
        PPR_TEST_ASSERT(not packs->containedPath("unknown", "images/rock.png", diagnostics));

        auto escape = twoPacks();
        escape.back().m_manifest.m_bytes +=
            "\n[assets.escape]\nkind = \"image.texture\"\ndescriptor = \"../outside.toml\"\n";
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(escape), kKinds, diagnostics));
    };

    PPR_UNIT_TEST (imported_values_and_provenance_own_source_storage) {
        config::Diagnostics diagnostics{8u};
        auto input = twoPacks();
        auto imported = asset::importDescriptor<asset::Manifest>(std::move(input.front().m_manifest), diagnostics);
        PPR_TEST_ASSERT(imported.has_value());

        input.clear();
        {
            const auto scratch = mem::ScratchPad::open();
            Array<std::string, mem::ScratchPad> reuse;
            reuse.resize(128u, std::string(128u, 'x'));
        }

        PPR_TEST_ASSERT(imported->m_value.m_pack_id == "ppr.base");
        PPR_TEST_ASSERT(imported->m_source.m_bytes.find("ppr.base") != std::string::npos);
        PPR_TEST_ASSERT(not imported->m_origins.empty());
        PPR_TEST_ASSERT(imported->m_origins.front().m_location.find("base.pack.toml") != std::string::npos);
    };

    PPR_UNIT_TEST (complete_snapshot_reader_owns_values_and_never_falls_back_to_disk) {
        static std::atomic<u32> ordinal{};
        std::filesystem::path root;
        const auto parent = hal::process::currentExecutablePath().parent_path();

        for (u32 attempt = 0u; attempt < 4096u; ++attempt) {
            const auto candidate = parent / ("asset-reader-test-" + std::to_string(ordinal.fetch_add(1u)));
            if (std::filesystem::create_directory(candidate)) {
                root = std::filesystem::canonical(candidate);
                break;
            }
        }

        PPR_TEST_ASSERT(not root.empty());
        PPR_DEFER {
            // Best-effort fixture cleanup after owned imports are no longer used.
            std::error_code error;
            std::filesystem::remove_all(root, error);
            PPR_LOG_WARNING_ON_FAIL(AssetPackReaderTests, error);
        };

        const auto path = root / "descriptor.toml";
        {
            std::ofstream disk{path, std::ios::binary};
            disk << "This disk descriptor is intentionally corrupt.";
            PPR_TEST_ASSERT(static_cast<bool>(disk));
        }

        config::Diagnostics diagnostics;
        auto indexed = asset::PackSet::index(
            {{root, {root / "pack.toml", "schema_version = 1\npack_id = \"reader\"\n"
                                         "[assets.item]\nkind = \"sim.world\"\ndescriptor = \"descriptor.toml\"\n"}}},
            kKinds, diagnostics);
        PPR_TEST_ASSERT(indexed.has_value());
        const auto key = indexed->assets().front().m_key;
        std::size_t reads{};
        std::optional<asset::Imported<asset::Manifest>> retained;

        {
            std::string snapshot = "schema_version = 1\npack_id = \"edited\"\n";
            const auto reader = [&](const asset::IndexedAsset &entry,
                                    config::Diagnostics &) -> Expected<asset::DescriptorSource> {
                ++reads;
                return asset::DescriptorSource{entry.m_descriptor, snapshot};
            };

            auto imported = asset::importDescriptor<asset::Manifest>(*indexed, key, "sim.world", reader, diagnostics);
            PPR_TEST_ASSERT(imported.has_value() and reads == 1u);
            retained.emplace(std::move(*imported));

            const auto wrong_kind = asset::importDescriptor<asset::Manifest>(
                *indexed, key, "image.texture", reader, diagnostics);
            PPR_TEST_ASSERT(not wrong_kind and wrong_kind.error() == config::EConfigErrc::type_mismatch);
            PPR_TEST_ASSERT(reads == 1u);

            snapshot.clear();
            const auto empty = asset::importDescriptor<asset::Manifest>(
                *indexed, key, "sim.world", reader, diagnostics);
            PPR_TEST_ASSERT(not empty and reads == 2u);
        }

        const auto missing = [](const asset::IndexedAsset &,
                                 config::Diagnostics &) -> Expected<asset::DescriptorSource> {
            PPR_RETURN_UNEXPECTED_ON_FAIL(AssetPackReaderTests, std::errc::no_such_file_or_directory);
            std::unreachable();
        };
        const auto absent = asset::importDescriptor<asset::Manifest>(*indexed, key, "sim.world", missing, diagnostics);
        PPR_TEST_ASSERT(not absent and absent.error() == std::errc::no_such_file_or_directory);

        const auto wrong_provenance = [&](const asset::IndexedAsset &,
                                          config::Diagnostics &) -> Expected<asset::DescriptorSource> {
            ++reads;
            return asset::DescriptorSource{root / "different.toml", "schema_version = 1\npack_id = \"edited\"\n"};
        };
        const auto mismatched = asset::importDescriptor<asset::Manifest>(
            *indexed, key, "sim.world", wrong_provenance, diagnostics);
        PPR_TEST_ASSERT(not mismatched and reads == 3u);

        {
            const auto scratch = mem::ScratchPad::open();
            Array<std::string, mem::ScratchPad> reuse(128u, std::string(128u, 'x'));
        }

        PPR_TEST_ASSERT(retained->m_value.m_pack_id == "edited" and retained->m_source.m_path == path);
        PPR_TEST_ASSERT(retained->m_source.m_bytes.find("edited") != std::string::npos);
        const auto origin = std::ranges::find(retained->m_origins, "schema_version", &config::PropertyOrigin::m_name);
        PPR_TEST_ASSERT(origin != retained->m_origins.end() and origin->m_location == path.string() + ":1");
    };

    PPR_UNIT_TEST (diagnostics_are_bounded_and_deterministic) {
        const auto make_invalid = [] {
            auto input = twoPacks();
            input.back().m_manifest.m_bytes += "\n[assets.sound]\nkind = \"audio.clip\"\ndescriptor = \"sound.toml\"\n";
            return input;
        };

        config::Diagnostics first{1u}, second{1u};
        auto input = make_invalid();
        PPR_TEST_ASSERT(not asset::PackSet::index(input, kKinds, first));

        std::ranges::reverse(input);
        PPR_TEST_ASSERT(not asset::PackSet::index(std::move(input), kKinds, second));
        PPR_TEST_ASSERT(first.format() == second.format());
        PPR_TEST_ASSERT(first.entries().size() == 1u);

        PPR_TEST_ASSERT(not asset::PackSet::index(make_invalid(), kKinds, first));
        PPR_TEST_ASSERT(first.size() == 2u and first.omittedCount() == 1u);
    };

    PPR_UNIT_TEST (invalid_same_root_manifests_have_deterministic_first_error) {
        const auto verify = [](Array<asset::PackSource> input) {
            config::Diagnostics first{8u}, second{8u};
            const auto initial = asset::PackSet::index(input, kKinds, first);
            PPR_TEST_ASSERT(not initial);

            std::ranges::reverse(input);
            const auto reversed = asset::PackSet::index(std::move(input), kKinds, second);
            PPR_TEST_ASSERT(not reversed);
            PPR_TEST_ASSERT(initial.error() == reversed.error());
            PPR_TEST_ASSERT(first.format() == second.format());
            PPR_TEST_ASSERT(first.hasErrors());
        };

        verify(
            {source("a.pack.toml", "schema_version = 0\npack_id = \"ppr.base\"\n"),
                source("z.pack.toml", "schema_version = \"invalid\"\npack_id = \"ppr.base\"\n")});

        verify(
            {source("same.pack.toml", "schema_version = 0\npack_id = \"ppr.base\"\n"),
                source("same.pack.toml", "schema_version = \"invalid\"\npack_id = \"ppr.base\"\n")});
    };

    PPR_UNIT_TEST (prehashed_collision_fixture_uses_production_validator) {
        const asset::AssetKey shared_key{123u};
        Array<asset::IndexedAsset> entries{
            {{"ppr.base", "rock"}, "sim.material", "rock.toml", shared_key, 0u},
            {{"ppr.shared", "sand"}, "sim.material", "sand.toml", shared_key, 1u}};
        const std::size_t key_order[] = {0u, 1u};
        config::Diagnostics diagnostics{4u};
        const auto collision = asset::validateIdentityCollisions<asset::IndexedAsset>(entries, key_order, diagnostics);
        PPR_TEST_ASSERT(collision == config::EConfigErrc::duplicate_key);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_message.find("hash collision") != std::string::npos);

        diagnostics.clear();
        entries.back().m_name = entries.front().m_name;
        const auto duplicate = asset::validateIdentityCollisions<asset::IndexedAsset>(entries, key_order, diagnostics);
        PPR_TEST_ASSERT(duplicate == config::EConfigErrc::duplicate_key);
        PPR_TEST_ASSERT(diagnostics.entries().front().m_message.find("Duplicate asset") != std::string::npos);
    };
}

namespace pP::tests {
    // ------------------------------------------------------------------
    // foundation suite registration
    // ------------------------------------------------------------------

    const UnitTest packs = UnitTest::Named("packs") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::Packs::canonical_names_and_frozen_identity,
            detail::Packs::two_pack_checked_resolution_is_mount_order_independent,
            detail::Packs::duplicate_namespace_dependencies_and_missing_pack_reject,
            detail::Packs::unsupported_kind_duplicate_local_key_and_old_manifest_reject,
            detail::Packs::containment_rejects_absolute_and_dot_segment_paths,
            detail::Packs::imported_values_and_provenance_own_source_storage,
            detail::Packs::complete_snapshot_reader_owns_values_and_never_falls_back_to_disk,
            detail::Packs::diagnostics_are_bounded_and_deterministic,
            detail::Packs::invalid_same_root_manifests_have_deterministic_first_error,
            detail::Packs::prehashed_collision_fixture_uses_production_validator,
        });
    };

    const UnitTest &packTests() noexcept {
        return packs;
    }
}
