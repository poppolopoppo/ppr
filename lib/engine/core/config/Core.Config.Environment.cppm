module;

#include "pP/Macros.h"

export module engine.core:config.environment;

import :config;
import :containers.stl;
import :types;
import :utility;

import std;

export namespace pP::config {
    // Derives the environment name for a canonical key:
    // PPR_ prefix, dots become double underscores, dashes become single
    // underscores, remaining segments become uppercase.
    // display.cap_hz -> PPR_DISPLAY__CAP_HZ
    [[nodiscard]] std::string environmentName(std::string_view canonical);

    // Default injected lookup backed by the process environment. The
    // library never reads the environment on its own; callers pass a
    // lookup explicitly so tests stay hermetic.
    [[nodiscard]] std::optional<std::string> getenvLookup(std::string_view name);

    namespace details {
        // Optional schema hook for compatibility names. When present it
        // returns an explicit alternate environment name for a canonical
        // key (empty view means none).
        template<typename S>
        concept THasEnvAlias = requires (const std::string_view name) {
            { S::environmentAlias(name) } -> std::convertible_to<std::string_view>;
        };
    } // namespace details

    // Collects one layer of assignments for every known schema name
    // present in the lookup. Unknown process variables are never read.
    // Disallowed-source properties still produce assignments here; the
    // permission error is reported at resolve time.
    template<typename Settings, typename LookupT>
    [[nodiscard]] Array<Assignment> collectEnvironment(LookupT &&lookup, Diagnostics &diagnostics) {
        (void) diagnostics;
        Array<Assignment> assignments{};
        std::size_t order = 0;
        Settings probe{};

        visit(probe, [&](const auto &member, auto &) {
            const std::string_view canonical = member.m_metadata.m_name;
            const std::string primary = environmentName(canonical);
            if (const std::optional<std::string> value = lookup(std::string_view{primary})) {
                assignments.push_back(
                    Assignment{
                        std::string{canonical},
                        primary,
                        SourceValue::text(*value),
                        configFormat("env:{}", primary),
                        order++,
                    });
            }
            if constexpr (details::THasEnvAlias<Settings>) {
                const std::string_view alias = Settings::environmentAlias(canonical);
                if (not alias.empty() and alias != std::string_view{primary}) {
                    if (const std::optional<std::string> value = lookup(alias)) {
                        assignments.push_back(
                            Assignment{
                                std::string{canonical},
                                std::string{alias},
                                SourceValue::text(*value),
                                configFormat("env:{}", alias),
                                order++,
                            });
                    }
                }
            }
        });

        return assignments;
    }

    [[nodiscard]] Layer environmentLayer(Array<Assignment> assignments, std::string origin = "environment");

    // Executable check: derived environment names (plus aliases) must not
    // collide across the schema.
    template<typename Settings>
    [[nodiscard]] Expected<void> validateEnvironmentSchema(Diagnostics &diagnostics) {
        const std::size_t before = diagnostics.size();
        Settings probe{};

        Array<std::string> names{};
        visit(probe, [&](const auto &member, auto &) {
            names.push_back(environmentName(member.m_metadata.m_name));
            if constexpr (details::THasEnvAlias<Settings>) {
                const std::string_view alias = Settings::environmentAlias(member.m_metadata.m_name);
                if (not alias.empty()) {
                    names.push_back(std::string{alias});
                }
            }
        });

        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                if (names[i] == names[j]) {
                    diagnostics.add(
                        "environment-schema", names[i],
                        configFormat("environment name '{}' collides with another property", names[i]),
                        make_error_code(EConfigErrc::schema_invalid));
                }
            }
        }

        if (diagnostics.size() != before) {
            return std::unexpected{make_error_code(EConfigErrc::schema_invalid)};
        }
        return Expected<void>{};
    }
} // namespace pP::config
