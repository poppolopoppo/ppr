module;
#include "pP/Macros.h"

module game.tests;

import engine.core;
import engine.asset;
import engine.sim;
import game.colony.content;
import std;

namespace pP::game::tests::fixtures {
    // ------------------------------------------------------------------
    // test-owned programmatic definition fragments
    // ------------------------------------------------------------------

    PPR_DEFINE_LOG_CATEGORY(ContentFixture, debug, none)

    /// These fragments exercise the flattened typed model and projection. They
    /// are not pack manifests/descriptors and never enter the production loader.
    Expected<config::Document> definitionDocument(
        const std::span<const asset::DescriptorSource> fragments, config::Diagnostics &diagnostics) {
        config::Document result;
        std::set<std::string> destinations;
        std::vector<config::GroupPresence> deferred_group_presence;

        const auto rulePath = [](const std::string_view path) {
            if (not path.starts_with("rules.")) {
                return std::string{path};
            }

            const auto tail = path.substr(6u);
            const auto dot = tail.find('.');
            return dot == std::string_view::npos
                       ? std::string{}
                       : "worlds." + std::string{tail.substr(0u, dot)} + ".rules." + std::string{tail.substr(dot + 1u)};
        };

        for (const auto &fragment: fragments) {
            PPR_RETURN_UNEXPECTED_ON_FAIL(
                ContentFixture, fragment.m_bytes.size() > asset::kMaxDescriptorBytes
                                    ? asset::reject(
                                          diagnostics, fragment.m_path.string(), "",
                                          "Test fragment exceeds source bound", config::EConfigErrc::out_of_range)
                                    : std::error_code{});

            auto decoded = config::decode(
                fragment.m_bytes, fragment.m_path.string(), diagnostics, config::EDecodeMode::tree);
            PPR_RETURN_UNEXPECTED_ON_FAIL(ContentFixture, decoded);

            for (auto &entry: decoded->m_entries) {
                entry.m_key = rulePath(entry.m_key);
                PPR_RETURN_UNEXPECTED_ON_FAIL(
                    ContentFixture, destinations.insert(entry.m_key).second
                                        ? std::error_code{}
                                        : asset::reject(
                                              diagnostics, entry.m_location, entry.m_key, "Duplicate fixture property",
                                              config::EConfigErrc::duplicate_key));
                result.m_entries.push_back(std::move(entry));
            }

            for (auto &group: decoded->m_groups) {
                if (group.m_path == "rules") {
                    continue;
                }

                const auto original_path = group.m_path;
                group.m_path = rulePath(group.m_path);
                if (not group.m_path.empty()) {
                    result.m_groups.push_back(std::move(group));
                    continue;
                }

                // An intermediate rules.<name> table names a world that its rules
                // fragment also supplies. Defer its presence so a phantom world's
                // orphan errors stay attributed to that fragment rather than
                // falling back to a broader ancestor.
                if (original_path.starts_with("rules.") and original_path.size() > 6u) {
                    deferred_group_presence.push_back({"worlds." + original_path.substr(6u), group.m_location});
                }
            }
        }

        // Append deferred presence only when the owning worlds document did not
        // already declare it; a duplicate group path would be rejected as a
        // duplicate declaration in the layer.
        for (auto &presence: deferred_group_presence) {
            const auto existing = std::ranges::find(result.m_groups, presence.m_path, &config::GroupPresence::m_path);
            if (existing == result.m_groups.end()) {
                result.m_groups.push_back(std::move(presence));
            }
        }

        return result;
    }

    Expected<colony::PackDefinition> resolveDefinition(
        const config::Document &document, config::Diagnostics &diagnostics) {
        config::Diagnostics local{128u};
        const config::TreeLayer layer = config::toTreeLayer(document, "programmatic fixture");
        auto resolved = config::resolveRecursive<colony::PackDefinition>(std::span{&layer, 1u}, local);
        const auto error = resolved ? sim::definition::validateDefinition(resolved->m_value, local) : resolved.error();

        for (const auto &entry: local.entries()) {
            std::string_view location = entry.m_location;
            std::string_view property = entry.m_property;
            while (not property.empty()) {
                const auto scalar = std::ranges::find(document.m_entries, property, &config::Entry::m_key);
                if (scalar != document.m_entries.end()) {
                    location = scalar->m_location;
                    break;
                }

                const auto group = std::ranges::find(document.m_groups, property, &config::GroupPresence::m_path);
                if (group != document.m_groups.end()) {
                    location = group->m_location;
                    break;
                }

                const auto dot = property.rfind('.');
                property = dot == std::string_view::npos ? std::string_view{} : property.substr(0u, dot);
            }

            diagnostics.add(
                std::string{location}, entry.m_property, entry.m_message, entry.m_code, entry.m_alias,
                entry.m_related_properties);
        }

        diagnostics.addOmitted(local.omittedCount());
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentFixture, error);
        return std::move(resolved->m_value);
    }

    Expected<colony::PackDefinition> resolveDefinition(
        const std::span<const asset::DescriptorSource> fragments, config::Diagnostics &diagnostics) {
        auto document = definitionDocument(fragments, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentFixture, document);

        auto definition = resolveDefinition(*document, diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(ContentFixture, definition);
        return definition;
    }
}
