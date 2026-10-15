module;

#include "pP/Macros.h"

module engine.core;

import :config;
import :config.cli;
import :hal;

import std;

namespace pP::config {
    // ------------------------------------------------------------------
    // config.cli
    // ------------------------------------------------------------------

    namespace {
        struct OptionMatch {
            const OptionDecl *m_option{nullptr};
            bool m_negated{false};
            std::string m_spelling{};
        };

        [[nodiscard]] std::string_view shortName(const std::string_view canonical) noexcept {
            if (const std::size_t pos = canonical.rfind('.'); pos != std::string_view::npos) {
                return canonical.substr(pos + 1);
            }
            return canonical;
        }

        [[nodiscard]] OptionMatch findOption(const CliSchema &schema, const std::string_view name) {
            const hal::ProfileScope scope{"Config.Cli.FindOption"};
            // First pass: exact canonical, alias, and negation spellings.
            // Explicit spellings win over derived short names.
            for (const auto &option: schema.m_options) {
                if (option.m_canonical == name) {
                    return {std::addressof(option), false, std::string{name}};
                }
                for (const std::string_view alias: option.m_aliases) {
                    if (alias == name) {
                        return {std::addressof(option), false, std::string{name}};
                    }
                }
                for (const std::string_view negation: option.m_negations) {
                    if (negation == name) {
                        return {std::addressof(option), true, std::string{name}};
                    }
                }
            }
            // Second pass: trailing canonical segment ("app.loops" accepts
            // "--loops"). Full canonicals above take precedence.
            for (const auto &option: schema.m_options) {
                const std::string_view suffix = shortName(option.m_canonical);
                if (suffix != option.m_canonical and suffix == name) {
                    return {std::addressof(option), false, std::string{name}};
                }
            }
            return {};
        }

        [[nodiscard]] bool looksLikeOption(const std::string_view token) noexcept {
            if (token.size() < 2 or token[0] != '-') {
                return false;
            }
            const char second = token[1];
            // A dash followed by a digit (or dot) is a negative number, not
            // an option; everything else dash-led is option-like.
            if ((second >= '0' and second <= '9') or second == '.') {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool isHelpSpelling(const std::string_view name) noexcept {
            return name == "help" or name == "h";
        }
    } // namespace

    [[nodiscard]] Expected<CliResult> parseCliArgs(
        const std::span<const std::string_view> args, const CliSchema &schema, Diagnostics &diagnostics) {
        const hal::ProfileScope scope{"Config.Cli.Parse"};
        CliResult result{};
        std::error_code first_error{};
        std::size_t order = 0;
        bool after_terminator = false;

        const auto fail = [&](const std::size_t index, std::string property, std::string message,
                              const std::error_code code, std::string alias = {}) {
            diagnostics.add(
                configFormat("argv[{}]", index), std::move(property), std::move(message), code, std::move(alias));
            if (not first_error) {
                first_error = code;
            }
        };

        const auto pushAssignment = [&](std::string canonical, SourceValue value, const std::size_t index,
                                        std::string alias) {
            std::string name = std::move(canonical);
            if (alias == name) {
                alias.clear();
            }
            result.m_assignments.push_back(
                Assignment{
                    std::move(name),
                    std::move(alias),
                    std::move(value),
                    configFormat("argv[{}]", index),
                    order++,
                });
        };

        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view token = args[i];

            if (not after_terminator and token == "--") {
                after_terminator = true;
                continue;
            }

            bool is_long = false;
            bool is_short = false;
            std::string_view body{};
            if (not after_terminator and token.size() >= 3 and token[0] == '-' and token[1] == '-') {
                is_long = true;
                body = token.substr(2);
            } else if (not after_terminator and token.size() >= 2 and token[0] == '-' and token[1] != '-') {
                is_short = true;
                body = token.substr(1);
            }

            if (not is_long and not is_short) {
                // Positional: command selection or error.
                const std::string word{token};
                bool is_command = false;
                for (const auto &command: schema.m_commands) {
                    if (command.m_name == word) {
                        is_command = true;
                        break;
                    }
                }
                if (is_command) {
                    if (result.m_command.empty()) {
                        result.m_command = word;
                    } else if (result.m_command != word) {
                        fail(
                            i, word,
                            configFormat("conflicting command '{}' (already selected '{}')", word, result.m_command),
                            make_error_code(EConfigErrc::conflicting_command));
                    } else {
                        fail(
                            i, word, configFormat("duplicate command selector '{}'", word),
                            make_error_code(EConfigErrc::conflicting_command));
                    }
                } else {
                    fail(
                        i, word, configFormat("unexpected positional argument '{}'", word),
                        make_error_code(EConfigErrc::unexpected_value));
                }
                continue;
            }

            std::string_view name = body;
            std::string_view inline_value{};
            bool has_inline_value = false;
            if (const std::size_t eq = body.find('='); eq != std::string_view::npos) {
                name = body.substr(0, eq);
                inline_value = body.substr(eq + 1);
                has_inline_value = true;
            }

            if (name.empty()) {
                fail(i, "", "empty option name", make_error_code(EConfigErrc::unknown_option));
                continue;
            }

            // --help/-h are reserved and never collide with schema options.
            if (isHelpSpelling(name)) {
                if (has_inline_value) {
                    fail(
                        i, std::string{name}, "help takes no value", make_error_code(EConfigErrc::unexpected_value),
                        std::string{name});
                } else {
                    result.m_help_requested = true;
                }
                continue;
            }

            const OptionMatch match = findOption(schema, name);
            if (match.m_option == nullptr) {
                const std::string display = configFormat("{}{}", is_long ? "--" : "-", name);
                fail(
                    i, std::string{name}, configFormat("unknown option '{}'", display),
                    make_error_code(EConfigErrc::unknown_option), std::string{name});
                continue;
            }

            const OptionDecl &option = *match.m_option;
            const std::string canonical{option.m_canonical};

            if (match.m_negated) {
                if (has_inline_value) {
                    fail(
                        i, canonical, configFormat("negated option '{}' takes no value", name),
                        make_error_code(EConfigErrc::unexpected_value), std::string{name});
                    continue;
                }

                pushAssignment(canonical, SourceValue::boolean(false), i, std::string{name});
                continue;
            }

            if (option.m_is_switch) {
                if (has_inline_value) {
                    if (inline_value.empty()) {
                        pushAssignment(canonical, SourceValue::reset(), i, std::string{name});
                    } else {
                        pushAssignment(canonical, SourceValue::text(std::string{inline_value}), i, std::string{name});
                    }
                } else {
                    pushAssignment(canonical, SourceValue::boolean(true), i, std::string{name});
                }
                continue;
            }

            if (has_inline_value) {
                if (inline_value.empty() and not option.m_preserve_empty_value) {
                    pushAssignment(canonical, SourceValue::reset(), i, std::string{name});
                } else {
                    pushAssignment(canonical, SourceValue::text(std::string{inline_value}), i, std::string{name});
                }
                continue;
            }

            if (option.m_value_optional) {
                if (i + 1 < args.size() and not looksLikeOption(args[i + 1]) and args[i + 1] != "--") {
                    ++i;
                    if (args[i].empty() and not option.m_preserve_empty_value) {
                        pushAssignment(canonical, SourceValue::reset(), i, std::string{name});
                    } else {
                        pushAssignment(canonical, SourceValue::text(std::string{args[i]}), i, std::string{name});
                    }
                } else if (option.m_absent_value.empty()) {
                    pushAssignment(canonical, SourceValue::reset(), i, std::string{name});
                } else {
                    pushAssignment(
                        canonical, SourceValue::text(std::string{option.m_absent_value}), i, std::string{name});
                }
                continue;
            }

            // Required value: consumed from the next token. Option-like
            // tokens and the "--" terminator are never consumed as data so
            // that "--label --help" reports a missing value instead of
            // stealing the next option; negative numbers ("--depth -2")
            // keep working via looksLikeOption's exemption. Dash-prefixed
            // strings need the inline form ("--label=--help").
            if (i + 1 >= args.size() or args[i + 1] == "--" or looksLikeOption(args[i + 1])) {
                fail(
                    i, canonical, configFormat("option '{}' requires a value", name),
                    make_error_code(EConfigErrc::missing_value), std::string{name});
                continue;
            }

            ++i;
            if (args[i].empty() and not option.m_preserve_empty_value) {
                pushAssignment(canonical, SourceValue::reset(), i, std::string{name});
            } else {
                pushAssignment(canonical, SourceValue::text(std::string{args[i]}), i, std::string{name});
            }
        }

        if (first_error) {
            return std::unexpected{first_error};
        }
        return result;
    }

    [[nodiscard]] Expected<CliResult> parseCliArgs(
        const int argc, char *const *argv, const CliSchema &schema, Diagnostics &diagnostics) {
        Array<std::string_view> args{};
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }

        return parseCliArgs(std::span<const std::string_view>{args.data(), args.size()}, schema, diagnostics);
    }

    [[nodiscard]] Layer toLayer(CliResult result, std::string origin) {
        Layer layer{};
        layer.m_source = ESource::cli;
        layer.m_origin = std::move(origin);
        layer.m_assignments = std::move(result.m_assignments);
        return layer;
    }

    [[nodiscard]] std::string generateHelp(const CliSchema &schema) {
        const hal::ProfileScope scope{"Config.Cli.GenerateHelp"};
        std::string result{};
        if (not schema.m_program.empty()) {
            result += configFormat("Usage: {}", schema.m_program);
            if (not schema.m_options.empty()) {
                result += " [options]";
            }
            if (not schema.m_commands.empty()) {
                result += " [command]";
            }
            result += "\n";
        }

        if (not schema.m_description.empty()) {
            result += configFormat("\n{}\n", schema.m_description);
        }

        if (not schema.m_commands.empty()) {
            result += "\nCommands:\n";
            for (const auto &command: schema.m_commands) {
                result += configFormat("  {}", command.m_name);
                if (not command.m_description.empty()) {
                    result += configFormat(" - {}", command.m_description);
                }
                result += "\n";
            }
        }

        if (not schema.m_options.empty()) {
            result += "\nOptions:\n";
            for (const auto &option: schema.m_options) {
                result += configFormat("  --{}", option.m_canonical);
                const std::string_view suffix = shortName(option.m_canonical);
                if (suffix != option.m_canonical) {
                    result += configFormat(", --{}", suffix);
                }
                for (const std::string_view alias: option.m_aliases) {
                    result += configFormat(", --{}", alias);
                }

                if (option.m_is_switch) {
                    result += " (switch)";
                } else if (option.m_value_optional) {
                    result += configFormat(" [value, default '{}']", option.m_absent_value);
                } else {
                    result += " <value>";
                }
                if (not option.m_description.empty()) {
                    result += configFormat(" - {}", option.m_description);
                }
                result += "\n";

                for (const std::string_view negation: option.m_negations) {
                    result += configFormat("    --{} sets {} to false\n", negation, option.m_canonical);
                }
            }
            result += "  --help, -h - show this help\n";
        }
        return result;
    }
} // namespace pP::config
