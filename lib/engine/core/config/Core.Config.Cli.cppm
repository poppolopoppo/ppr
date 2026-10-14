module;

#include "pP/Macros.h"

export module engine.core:config.cli;

import :config;
import :containers.stl;
import :types;
import :utility;

import std;

export namespace pP::config {
    // ------------------------------------------------------------------
    // config.cli
    // ------------------------------------------------------------------

    // Exit code the executable returns for invalid invocation or
    // configuration. Enforced by the caller, never by parsing.
    inline constexpr int kExitInvalidUsage = 2;

    // One CLI binding. m_canonical references a schema property; the CLI
    // owns no second set of validators or defaults.
    //
    // - m_is_switch: boolean presence flag; bare presence means true, and
    //   an explicit --flag=<boolean-token> form is also accepted.
    // - m_value_optional: the value may be omitted; omission records
    //   m_absent_value (e.g. "--shuffle" means "--shuffle=<absent>").
    // - Otherwise the value is required: "--name value" or "--name=value".
    // - An explicitly empty value ("--name=") records an explicit reset,
    //   valid only for optional properties (rejected at resolve time),
    //   unless m_preserve_empty_value keeps the empty text (string options
    //   whose domain allows empty, for legacy compatibility).
    // - Scalar repetition is last-wins; m_repeatable documents intent for
    //   future append-semantics properties.
    // - m_negations holds "--no-..." spellings resolving to boolean false
    //   on the same canonical property.
    struct OptionDecl {
        std::string_view m_canonical{};
        std::string_view m_description{};
        Array<std::string_view> m_aliases{};
        bool m_is_switch{false};
        bool m_value_optional{false};
        std::string_view m_absent_value{"true"};
        bool m_repeatable{false};
        Array<std::string_view> m_negations{};
        // Keep explicitly empty text ("--name=", "--name \"\"") instead of
        // recording a reset. For string properties whose domain allows
        // empty; the absent-value path (bare "--name" on value-optional
        // options) still records the absent/reset behavior.
        bool m_preserve_empty_value{false};
    };

    struct CommandDecl {
        std::string_view m_name{};
        std::string_view m_description{};
    };

    struct CliSchema {
        std::string_view m_program{};
        std::string_view m_description{};
        Array<OptionDecl> m_options{};
        Array<CommandDecl> m_commands{};
    };

    struct CliResult {
        Array<Assignment> m_assignments{};
        std::string m_command{};
        bool m_help_requested{false};
    };

    // Tokenizes argv into owned assignments. Supports "--name value" and
    // "--name=value", single-dash aliases, "--" termination, command
    // selection (first-position usage preserved; selectors elsewhere are a
    // compatible extension; conflicting selectors are errors), and
    // "--help"/"-h" (reserved; sets m_help_requested and still validates).
    [[nodiscard]] Expected<CliResult> parseCliArgs(
        std::span<const std::string_view> args, const CliSchema &schema, Diagnostics &diagnostics);

    [[nodiscard]] Expected<CliResult> parseCliArgs(
        int argc, char *const *argv, const CliSchema &schema, Diagnostics &diagnostics);

    // Wraps a parse result as a CLI precedence layer.
    [[nodiscard]] Layer toLayer(CliResult result, std::string origin = "cli");

    // Generated help text from schema metadata.
    [[nodiscard]] std::string generateHelp(const CliSchema &schema);

    template<typename Settings>
    [[nodiscard]] std::string generateHelp(const CliSchema &schema, const Settings &defaults) {
        std::string result = generateHelp(schema);

        Array<std::pair<std::string_view, std::string>> default_texts{};
        visit(defaults, [&](const auto &member, const auto &field) {
            // Defaults are shown only for CLI-bound properties; anything
            // else (e.g. file-only settings) is not actionable here.
            bool bound = false;
            for (const auto &option: schema.m_options) {
                if (option.m_canonical == member.m_metadata.m_name) {
                    bound = true;
                    break;
                }
            }
            if (not bound) {
                return;
            }
            if (member.m_metadata.m_sensitive) {
                default_texts.emplace_back(member.m_metadata.m_name, "<redacted>");
            } else {
                default_texts.emplace_back(member.m_metadata.m_name, details::defaultText(member, field));
            }
        });

        if (default_texts.empty()) {
            return result;
        }
        result += "\nDefaults:\n";
        for (const auto &entry: default_texts) {
            result += configFormat("  {} = {}\n", entry.first, entry.second);
        }
        return result;
    }

    // Executable CLI-schema check: canonical names exist in Settings, and
    // canonical/alias/negation/command spellings do not collide (help/h
    // are reserved).
    template<typename Settings>
    [[nodiscard]] Expected<void> validateCliSchema(const CliSchema &schema, Diagnostics &diagnostics) {
        const std::size_t before = diagnostics.size();
        Settings probe{};

        Array<std::string_view> known{};
        visit(probe, [&](const auto &member, auto &) { known.push_back(member.m_metadata.m_name); });

        const auto is_known = [&](const std::string_view name) {
            for (const std::string_view candidate: known) {
                if (candidate == name) {
                    return true;
                }
            }
            return false;
        };

        Array<std::string_view> spellings{"help", "h"};
        for (const auto &command: schema.m_commands) {
            if (command.m_name.empty()) {
                diagnostics.add(
                    "cli-schema", "", "command with empty name", make_error_code(EConfigErrc::schema_invalid));
                continue;
            }
            bool duplicate = false;
            for (const std::string_view spelling: spellings) {
                if (spelling == command.m_name) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                diagnostics.add(
                    "cli-schema", std::string{command.m_name}, "command name collides with another spelling",
                    make_error_code(EConfigErrc::schema_invalid));
            } else {
                spellings.push_back(command.m_name);
            }
        }

        for (const auto &option: schema.m_options) {
            if (not is_known(option.m_canonical)) {
                diagnostics.add(
                    "cli-schema", std::string{option.m_canonical},
                    configFormat("CLI option references unknown property '{}'", option.m_canonical),
                    make_error_code(EConfigErrc::schema_invalid));
            }
            Array<std::string_view> option_spellings{};
            option_spellings.push_back(option.m_canonical);
            for (const std::string_view alias: option.m_aliases) {
                option_spellings.push_back(alias);
            }
            for (const std::string_view negation: option.m_negations) {
                option_spellings.push_back(negation);
            }
            // Trailing canonical segment is an accepted CLI spelling
            // ("app.loops" accepts "--loops"), so it participates in
            // collision detection like any explicit spelling.
            if (const std::size_t pos = option.m_canonical.rfind('.');
                pos != std::string_view::npos and pos + 1 < option.m_canonical.size()) {
                const std::string_view suffix = option.m_canonical.substr(pos + 1);
                bool already_listed = false;
                for (const std::string_view spelling: option_spellings) {
                    if (spelling == suffix) {
                        already_listed = true;
                        break;
                    }
                }
                if (not already_listed) {
                    option_spellings.push_back(suffix);
                }
            }
            for (const std::string_view spelling: option_spellings) {
                if (spelling.empty()) {
                    diagnostics.add(
                        "cli-schema", std::string{option.m_canonical}, "option with empty spelling",
                        make_error_code(EConfigErrc::schema_invalid));
                    continue;
                }
                bool duplicate = false;
                for (const std::string_view taken: spellings) {
                    if (taken == spelling) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    diagnostics.add(
                        "cli-schema", std::string{option.m_canonical},
                        configFormat("CLI spelling '{}' collides with another option or command", spelling),
                        make_error_code(EConfigErrc::schema_invalid));
                } else {
                    spellings.push_back(spelling);
                }
            }
        }

        if (diagnostics.hasErrorsSince(before)) {
            return std::unexpected{make_error_code(EConfigErrc::schema_invalid)};
        }
        return Expected<void>{};
    }
} // namespace pP::config
