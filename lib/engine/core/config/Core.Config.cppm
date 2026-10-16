module;

#include "pP/Macros.h"

export module engine.core:config;

import :containers.stl;
import :logger;
import :memory.arena;
import :types;
import :utility;

import std;

namespace pP::config {
    PPR_DECLARE_LOG_CATEGORY(Config)
}

export namespace pP::config {
    // MSVC workaround: std::make_format_args is constexpr and performs
    // compile-time format-string checking even via std::vformat (C2672),
    // so avoid <format> entirely. This snprintf-free ostringstream helper
    // handles the simple "{}" placeholders used in this codebase (string,
    // string_view, integer, and floating-point arguments).
    template<typename T>
    inline void configPrintArg(
        std::ostringstream &oss, const T &value, const std::size_t target_idx, std::size_t &current_idx) {
        if (current_idx == target_idx) {
            oss << value;
        }
        ++current_idx;
    }

    [[nodiscard]] inline std::string configFormat(std::string_view format, auto &&...args) {
        std::ostringstream oss;
        std::size_t arg_idx = 0;

        for (std::size_t i = 0; i < format.size(); ++i) {
            if (i + 1 < format.size() and format[i] == '{' and format[i + 1] == '}') {
                std::size_t current_idx = 0;
                if constexpr (sizeof...(args) > 0) {
                    (configPrintArg(oss, args, arg_idx, current_idx), ...);
                }
                ++arg_idx;
                ++i; // skip '}'
            } else {
                oss << format[i];
            }
        }
        return oss.str();
    }

    // ------------------------------------------------------------------
    // error domain: every config failure returns std::error_code and
    // appends contextual detail to the caller-owned Diagnostics.
    // Parsing never prints, terminates, or initializes services.
    // Callers report invalid invocation with kExitInvalidUsage (see
    // :config.cli); the value lives there, not in parsing.
    // ------------------------------------------------------------------

    enum class EConfigErrc {
        unknown_property = 1,
        unknown_option,
        source_disallowed,
        type_mismatch,
        invalid_integer,
        invalid_float,
        invalid_boolean,
        unknown_enum_value,
        out_of_range,
        non_finite_not_allowed,
        duplicate_key,
        missing_value,
        unexpected_value,
        conflicting_command,
        cross_field_invalid,
        schema_invalid,
        codec_error,
    };

    [[nodiscard]] const std::error_category &configCategory() noexcept;

    [[nodiscard]] std::error_code make_error_code(EConfigErrc err) noexcept;
} // namespace pP::config

export template<>
struct std::is_error_code_enum<pP::config::EConfigErrc> : std::true_type { // NOLINT(*-dcl58-cpp)
};

export namespace pP::config {
    // ------------------------------------------------------------------
    // sources: bitmask of origins allowed to supply a property.
    // ESource::none on a Layer bypasses permission checks and marks a
    // trusted programmatic layer (named command/profile defaults).
    // ------------------------------------------------------------------

    enum class ESource : u8 {
        none = 0,
        file = 1 << 0,
        environment = 1 << 1,
        cli = 1 << 2,
        runtime = 1 << 3,
        all = file | environment | cli | runtime,
    };

    [[nodiscard]] constexpr ESource operator|(const ESource lhs, const ESource rhs) noexcept {
        return static_cast<ESource>(
            static_cast<std::underlying_type_t<ESource>>(lhs) | static_cast<std::underlying_type_t<ESource>>(rhs));
    }

    [[nodiscard]] constexpr ESource operator&(const ESource lhs, const ESource rhs) noexcept {
        return static_cast<ESource>(
            static_cast<std::underlying_type_t<ESource>>(lhs) & static_cast<std::underlying_type_t<ESource>>(rhs));
    }

    constexpr ESource &operator|=(ESource &lhs, const ESource rhs) noexcept {
        return lhs = lhs | rhs;
    }

    constexpr ESource &operator&=(ESource &lhs, const ESource rhs) noexcept {
        return lhs = lhs & rhs;
    }

    [[nodiscard]] constexpr bool any(const ESource sources) noexcept {
        return static_cast<std::underlying_type_t<ESource>>(sources) != 0;
    }

    [[nodiscard]] std::string_view sourceName(ESource source) noexcept;

    // ------------------------------------------------------------------
    // mutability: startup properties are fixed after startup resolution;
    // runtime properties accept accepted runtime overrides.
    // ------------------------------------------------------------------

    enum class EMutability : u8 {
        startup,
        runtime,
    };

    // ------------------------------------------------------------------
    // metadata: one descriptor per property. All views are static
    // (string literals); m_sensitive redacts values in diagnostics.
    // ------------------------------------------------------------------

    struct Metadata {
        std::string_view m_name{};
        std::string_view m_description{};
        std::string_view m_category{};
        EMutability m_mutability{EMutability::startup};
        ESource m_sources{ESource::none};
        bool m_persistable{false};
        bool m_sensitive{false};
        std::string_view m_reference_category{}, m_units{};
        // Expected editing interval; constraints remain the legal domain.
        std::optional<double> m_slider_min{}, m_slider_max{};
        bool m_read_only{false};
    };

    // Canonical segments use a restricted identifier alphabet so that
    // environment-name derivation and CLI spelling are unambiguous.
    [[nodiscard]] bool isValidCanonicalName(std::string_view name) noexcept;

    // Slice 1a: canonical definition — recursive configuration vocabulary.
    enum class EPresence : u8 {
        defaulted,
        required,
        optional
    };
    enum class ENodeKind : u8 {
        scalar,
        group,
        collection,
        variant
    };
    enum class EReferenceScope : u8 {
        root,
        // Collection within an explicitly supplied owner, never a lexical parent.
        // The consumer supplies that owner and resolves existence/suitability.
        owner
    };

    struct ReferenceTarget {
        std::string_view m_collection{};
        std::string_view m_suitability{};
        EReferenceScope m_scope{EReferenceScope::root};
    };

    template<typename T>
    struct KeyedEntry {
        std::string m_key{};
        T m_value{};
    };

    template<typename T>
    struct KeyedCollection {
        Array<KeyedEntry<T>> m_entries{};
    };

    struct CollectionDomain {
        std::size_t m_min{};
        std::size_t m_max{(std::numeric_limits<std::size_t>::max)()};
    };

    template<typename T>
    struct Alternative {
        using Type = T;
        std::string_view m_tag;
    };

    template<typename... T>
    struct VariantDomain {
        std::tuple<Alternative<T>...> m_alternatives;
    };

    template<typename T>
    [[nodiscard]] constexpr Alternative<T> alternative(const std::string_view tag) {
        return {tag};
    }

    inline constexpr std::size_t kMaxKeyBytes = 128u;

    [[nodiscard]] bool isValidKey(std::string_view key) noexcept;

    template<typename Ref>
    struct ReferenceDomain {
        ReferenceTarget m_target{};

        [[nodiscard]] std::error_code validate(const Ref &value) const noexcept {
            if (not isValidKey(value.m_key)) {
                PPR_RETURN_ERROR_ON_FAIL(Config, EConfigErrc::out_of_range);
            }
            return {};
        }

        [[nodiscard]] std::string describe() const {
            return configFormat("reference key ([a-z0-9_-], 1..{} characters)", kMaxKeyBytes);
        }
    };

    // ------------------------------------------------------------------
    // constraints: small vocabulary, no general validation framework.
    // Each constraint validates its own destination type and describes
    // its expected domain for diagnostics.
    // ------------------------------------------------------------------

    struct AnyDomain {
        template<typename T>
        [[nodiscard]] std::error_code validate(const T &) const noexcept {
            return {};
        }

        [[nodiscard]] std::string describe() const;
    };

    inline constexpr AnyDomain any_domain{};

    template<typename T>
        requires std::is_arithmetic_v<T>
    struct Range {
        T m_min{};
        T m_max{};

        [[nodiscard]] std::error_code validate(const T &value) const noexcept {
            if constexpr (std::is_floating_point_v<T>) {
                if (not std::isfinite(value)) {
                    return make_error_code(EConfigErrc::non_finite_not_allowed);
                }
            }
            if (value < m_min or value > m_max) {
                return make_error_code(EConfigErrc::out_of_range);
            }
            return {};
        }

        [[nodiscard]] std::string describe() const {
            if constexpr (std::is_floating_point_v<T>) {
                return configFormat("[{}, {}]", m_min, m_max);
            } else {
                return configFormat("[{}, {}]", static_cast<std::int64_t>(m_min), static_cast<std::int64_t>(m_max));
            }
        }
    };

    struct BooleanDomain {
        [[nodiscard]] std::error_code validate(const bool &) const noexcept {
            return {};
        }

        [[nodiscard]] std::string describe() const;
    };

    inline constexpr BooleanDomain boolean_domain{};

    template<typename E>
    struct EnumEntry {
        E m_value{};
        std::string_view m_name{};
    };

    template<typename E, std::size_t N>
        requires std::is_enum_v<E>
    struct EnumDomain {
        std::array<EnumEntry<E>, N> m_entries{};

        [[nodiscard]] std::error_code validate(const E &value) const noexcept {
            for (const auto &entry: m_entries) {
                if (entry.m_value == value) {
                    return {};
                }
            }
            return make_error_code(EConfigErrc::unknown_enum_value);
        }

        [[nodiscard]] std::error_code parse(const std::string_view text, E &out) const noexcept {
            for (const auto &entry: m_entries) {
                if (entry.m_name == text) {
                    out = entry.m_value;
                    return {};
                }
            }
            return make_error_code(EConfigErrc::unknown_enum_value);
        }

        [[nodiscard]] std::string_view nameOf(const E value) const noexcept {
            for (const auto &entry: m_entries) {
                if (entry.m_value == value) {
                    return entry.m_name;
                }
            }
            return {};
        }

        [[nodiscard]] std::string describe() const {
            std::string result{"one of "};
            bool first = true;
            for (const auto &entry: m_entries) {
                if (not first) {
                    result += ", ";
                }
                first = false;
                result += entry.m_name;
            }
            return result;
        }
    };

    template<typename E, std::size_t N>
    [[nodiscard]] constexpr EnumDomain<E, N> enumDomain(const EnumEntry<E> (&entries)[N]) {
        EnumDomain<E, N> domain{};
        for (std::size_t i = 0; i < N; ++i) {
            domain.m_entries[i] = entries[i];
        }
        return domain;
    }

    struct StringDomain {
        std::size_t m_max_length{1024};
        bool m_allow_empty{true};

        [[nodiscard]] std::error_code validate(const std::string &value) const noexcept;
        [[nodiscard]] std::string describe() const;
    };

    struct PathDomain {
        std::size_t m_max_length{1024};
        bool m_allow_empty{false};

        [[nodiscard]] std::error_code validate(const std::string &value) const noexcept;
        [[nodiscard]] std::string describe() const;
    };

    struct FloatDomain {
        double m_min{0.0};
        double m_max{0.0};
        bool m_allow_non_finite{false};
        // When true the minimum itself is rejected (open lower bound), so
        // {0.0, max, false, true} accepts every positive duration while
        // rejecting zero at field granularity (including shadowed values).
        bool m_exclusive_min{false};

        [[nodiscard]] std::error_code validate(const double &value) const noexcept;
        [[nodiscard]] std::string describe() const;
    };

    // ------------------------------------------------------------------
    // member descriptors: typed member pointers decorated with metadata.
    // Schemas are ordinary structs plus Schema::describe() returning a
    // std::tuple of Members. Optional cross-field validation is an
    // opt-in static: std::error_code validate(const S&, Diagnostics&).
    // ------------------------------------------------------------------

    template<typename Owner, typename T, typename Constraint>
    struct Member {
        T Owner::*m_member;
        Metadata m_metadata;
        Constraint m_constraint;
        EPresence m_presence{EPresence::defaulted};
    };

    template<typename Owner, typename T, typename Constraint>
    [[nodiscard]] constexpr Member<Owner, T, Constraint> field(
        T Owner::*member, const Metadata metadata, const Constraint constraint) {
        return Member<Owner, T, Constraint>{member, metadata, constraint};
    }

    // Slice 1a: canonical definition — typed recursive member declarations.
    template<typename Owner, typename T, typename Constraint>
    [[nodiscard]] constexpr Member<Owner, T, Constraint> field(
        T Owner::*member, const Metadata metadata, const Constraint constraint, const EPresence presence) {
        return {member, metadata, constraint, presence};
    }

    namespace details {
        struct GroupDomain {};
    }

    template<typename Owner, typename T>
    [[nodiscard]] constexpr Member<Owner, T, details::GroupDomain> group(
        T Owner::*member, const Metadata metadata, const EPresence presence) {
        return {member, metadata, {}, presence};
    }

    template<typename Owner, typename T>
    [[nodiscard]] constexpr Member<Owner, KeyedCollection<T>, CollectionDomain> collection(
        KeyedCollection<T> Owner::*member, const Metadata metadata, const CollectionDomain domain) {
        return {member, metadata, domain, EPresence::defaulted};
    }

    template<typename Owner, typename T, typename... A>
    [[nodiscard]] constexpr Member<Owner, T, VariantDomain<A...>> taggedVariant(
        T Owner::*member, const Metadata metadata, const EPresence presence, const Alternative<A>... alternatives) {
        return {member, metadata, {std::tuple{alternatives...}}, presence};
    }

    /// Display-neutral runtime projection of a schema member. Enum names refer
    /// to the schema's static strings; the choice list itself is owned.
    struct PropertyDescriptor {
        std::optional<double> m_min{}, m_max{};
        bool m_exclusive_min{}, m_exclusive_max{}, m_optional{}, m_read_only{};
        double m_slider_min{}, m_slider_max{};
        Array<std::string_view> m_choices{};
        std::string_view m_reference{}, m_description{}, m_units{};
        ENodeKind m_kind{ENodeKind::scalar};
        EPresence m_presence{EPresence::defaulted};
        std::optional<ReferenceTarget> m_reference_target{};
        std::optional<CollectionDomain> m_collection_domain{};
    };
    namespace details {
        template<typename T>
        struct IsOptional : std::false_type {};
        template<typename T>
        struct IsOptional<std::optional<T>> : std::true_type {};
        template<typename T>
        inline constexpr bool is_optional_v = IsOptional<std::remove_cvref_t<T>>::value;

        template<typename T>
        struct UnwrapOptional {
            using Type = T;
        };
        template<typename T>
        struct UnwrapOptional<std::optional<T>> {
            using Type = T;
        };
        template<typename T>
        using Unwrapped = typename UnwrapOptional<std::remove_cvref_t<T>>::Type;

        template<typename T>
        struct CollectionTraits {
            static constexpr bool value = false;
        };
        template<typename T>
        struct CollectionTraits<KeyedCollection<T>> {
            static constexpr bool value = true;
            using Element = T;
        };

        template<typename C>
        inline constexpr bool is_variant_domain_v = false;
        template<typename... A>
        inline constexpr bool is_variant_domain_v<VariantDomain<A...>> = true;

        template<typename T>
        concept TDescribed = requires { T::describe(); };

        template<typename T, typename C>
        struct VariantCompatible : std::false_type {};
        template<typename... T, typename... A>
        struct VariantCompatible<std::variant<T...>, VariantDomain<A...>> {
            template<typename U>
            static constexpr std::size_t payload_count = (std::size_t{std::is_same_v<U, T>} + ... + 0u);
            template<typename U>
            static constexpr std::size_t descriptor_count = (std::size_t{std::is_same_v<U, A>} + ... + 0u);
            static constexpr bool value = sizeof...(T) == sizeof...(A) and
                                          sizeof...(A) > 0u and
                                          ((payload_count<A> == 1u) and ...) and
                                          ((descriptor_count<T> == 1u) and ...) and
                                          (TDescribed<A> and ...);
        };
        template<typename T, typename C>
        inline constexpr bool variant_compatible_v = VariantCompatible<Unwrapped<T>, C>::value;

        template<typename C>
        inline constexpr ENodeKind node_kind_v = std::is_same_v<C, GroupDomain>        ? ENodeKind::group
                                                 : std::is_same_v<C, CollectionDomain> ? ENodeKind::collection
                                                 : is_variant_domain_v<C>              ? ENodeKind::variant
                                                                                       : ENodeKind::scalar;

        [[nodiscard]] std::string recursivePath(std::string_view prefix, std::string_view name);
    }

    /// The discriminator is schema metadata, never a mutable payload field.
    template<typename Owner, typename T, typename... A, typename V>
    [[nodiscard]] std::string_view variantTag(const Member<Owner, T, VariantDomain<A...>> &member, const V &value) {
        if constexpr (details::is_optional_v<V>) {
            return value ? variantTag(member, *value) : std::string_view{};
        } else {
            std::string_view tag;
            if constexpr (details::variant_compatible_v<V, VariantDomain<A...>>) {
                std::apply(
                    [&](const auto &...alt) {
                        ((std::holds_alternative<typename std::remove_cvref_t<decltype(alt)>::Type>(value)
                                 ? void(tag = alt.m_tag)
                                 : void()),
                            ...);
                    },
                    member.m_constraint.m_alternatives);
            }
            return tag;
        }
    }
    template<typename Owner, typename T, typename Constraint>
    [[nodiscard]] PropertyDescriptor describeProperty(const Member<Owner, T, Constraint> &member) {
        PropertyDescriptor result;
        const auto &domain = member.m_constraint;
        const auto &metadata = member.m_metadata;
        result.m_reference = metadata.m_reference_category;
        result.m_description = metadata.m_description;
        result.m_units = metadata.m_units;
        result.m_read_only = metadata.m_read_only;
        result.m_kind = details::node_kind_v<Constraint>;
        result.m_presence = member.m_presence;
        if constexpr (std::is_same_v<Constraint, CollectionDomain>) {
            result.m_collection_domain = domain;
        }
        if constexpr (requires { domain.m_target; }) {
            result.m_reference_target = domain.m_target;
        }

        if constexpr (not std::is_same_v<Constraint, CollectionDomain> and requires {
                          domain.m_min;
                          domain.m_max;
                      }) {
            result.m_min = static_cast<double>(domain.m_min);
            result.m_max = static_cast<double>(domain.m_max);
            if constexpr (requires { domain.m_exclusive_min; }) {
                result.m_exclusive_min = domain.m_exclusive_min;
            }
            result.m_slider_min = metadata.m_slider_min.value_or(*result.m_min >= -1.0e6 ? *result.m_min : -1000.0);
            result.m_slider_max = metadata.m_slider_max.value_or(*result.m_max <= 1.0e6 ? *result.m_max : 1000.0);
            result.m_slider_min = std::clamp(result.m_slider_min, *result.m_min, *result.m_max);
            result.m_slider_max = std::clamp(result.m_slider_max, result.m_slider_min, *result.m_max);
            if (result.m_exclusive_min and result.m_slider_min == *result.m_min) {
                result.m_slider_min = std::nextafter(result.m_slider_min, result.m_slider_max);
            }
        }

        if constexpr (requires { domain.m_entries; }) {
            result.m_choices.reserve(domain.m_entries.size());
            for (const auto &entry: domain.m_entries) {
                result.m_choices.push_back(entry.m_name);
            }
        }

        if constexpr (details::is_variant_domain_v<Constraint>) {
            std::apply(
                [&](const auto &...alt) { (result.m_choices.push_back(alt.m_tag), ...); }, domain.m_alternatives);
        }

        result.m_optional = details::is_optional_v<T>;
        return result;
    }

    template<typename Settings, typename Visitor>
    void describeRecursive(Visitor &&visitor, std::string_view prefix = {});

    template<typename Settings, typename Visitor>
    void visitRecursive(Settings &settings, Visitor &&visitor, std::string_view prefix = {});
}

export namespace pP::config {
    template<typename Settings, typename Visitor>
    constexpr void visit(Settings &settings, Visitor &&visitor) {
        const auto members = settings.describe();
        std::apply(
            [&](const auto... member) -> void { (void) (visitor(member, settings.*member.m_member), ...); }, members);
    }

    template<typename Settings, typename Visitor>
    constexpr void visit(const Settings &settings, Visitor &&visitor) {
        const auto members = settings.describe();
        std::apply(
            [&](const auto... member) -> void { (void) (visitor(member, settings.*member.m_member), ...); }, members);
    }

    // Slice 1a: canonical definition — preorder schema/value traversal.
    // Callbacks receive (logical path, typed Member, type_identity<T>/T&).
    // Member metadata remains local; the owned path carries the full prefix.
    template<typename Settings, typename Visitor>
    void describeRecursive(Visitor &&visitor, const std::string_view prefix) {
        const auto members = Settings::describe();
        const auto describe = [&](const auto &member) {
            using T = std::remove_cvref_t<decltype(std::declval<Settings>().*member.m_member)>;
            using C = std::remove_cvref_t<decltype(member.m_constraint)>;
            const auto path = details::recursivePath(prefix, member.m_metadata.m_name);
            visitor(path, member, std::type_identity<T>{});

            if constexpr (details::node_kind_v<C> == ENodeKind::group and details::TDescribed<details::Unwrapped<T>>) {
                describeRecursive<details::Unwrapped<T>>(visitor, path);
            } else if constexpr (details::node_kind_v<C> == ENodeKind::variant) {
                std::apply(
                    [&](const auto &...alt) {
                        const auto describe_alternative = [&](const auto &alternative) {
                            using A = typename std::remove_cvref_t<decltype(alternative)>::Type;
                            if constexpr (details::TDescribed<A>) {
                                auto metadata = member.m_metadata;
                                metadata.m_name = alternative.m_tag;
                                const auto payload = group(&KeyedEntry<A>::m_value, metadata, EPresence::required);
                                const auto payload_path = details::recursivePath(path, alternative.m_tag);
                                visitor(payload_path, payload, std::type_identity<A>{});
                                describeRecursive<A>(visitor, payload_path);
                            }
                        };
                        (describe_alternative(alt), ...);
                    },
                    member.m_constraint.m_alternatives);
            } else if constexpr (details::node_kind_v<C> == ENodeKind::collection and
                                 details::CollectionTraits<T>::value) {
                using E = typename details::CollectionTraits<T>::Element;
                if constexpr (details::TDescribed<E>) {
                    const auto entry_path = details::recursivePath(path, "{key}");
                    auto metadata = member.m_metadata;
                    metadata.m_name = "{key}";
                    const auto entry = group(&KeyedEntry<E>::m_value, metadata, EPresence::required);
                    visitor(entry_path, entry, std::type_identity<E>{});

                    describeRecursive<E>(visitor, entry_path);
                }
            }
        };
        std::apply([&](const auto &...member) -> void { (describe(member), ...); }, members);
    }

    template<typename Settings, typename Visitor>
    void visitRecursive(Settings &settings, Visitor &&visitor, const std::string_view prefix) {
        visit(settings, [&]<typename T0>(const auto &member, T0 &value) {
            using C = std::remove_cvref_t<decltype(member.m_constraint)>;
            using T = std::remove_cvref_t<T0>;
            const auto path = details::recursivePath(prefix, member.m_metadata.m_name);
            visitor(path, member, value);

            if constexpr (details::node_kind_v<C> == ENodeKind::group and details::TDescribed<details::Unwrapped<T>>) {
                if constexpr (details::is_optional_v<T>) {
                    if (value) {
                        visitRecursive(*value, visitor, path);
                    }
                } else {
                    visitRecursive(value, visitor, path);
                }
            } else if constexpr (details::node_kind_v<C> == ENodeKind::variant and
                                 details::variant_compatible_v<T, C>) {
                const auto visit_payload = [&](auto &variant) {
                    std::apply(
                        [&](const auto &...alt) {
                            const auto visit_alternative = [&](const auto &alternative) {
                                using A = typename std::remove_cvref_t<decltype(alternative)>::Type;
                                if (auto *payload = std::get_if<A>(std::addressof(variant))) {
                                    auto metadata = member.m_metadata;
                                    metadata.m_name = alternative.m_tag;
                                    const auto descriptor = group(
                                        &KeyedEntry<A>::m_value, metadata, EPresence::required);
                                    const auto payload_path = details::recursivePath(path, alternative.m_tag);
                                    visitor(payload_path, descriptor, *payload);
                                    visitRecursive(*payload, visitor, payload_path);
                                }
                            };
                            (visit_alternative(alt), ...);
                        },
                        member.m_constraint.m_alternatives);
                };
                if constexpr (details::is_optional_v<T>) {
                    if (value) {
                        visit_payload(*value);
                    }
                } else {
                    visit_payload(value);
                }
            } else if constexpr (details::node_kind_v<C> == ENodeKind::collection and
                                 details::CollectionTraits<T>::value) {
                using E = typename details::CollectionTraits<T>::Element;
                if constexpr (details::TDescribed<E>) {
                    for (auto &entry: value.m_entries) {
                        const auto entry_path = details::recursivePath(path, entry.m_key);
                        auto metadata = member.m_metadata;
                        metadata.m_name = entry.m_key;
                        const auto descriptor = group(&KeyedEntry<E>::m_value, metadata, EPresence::required);
                        visitor(entry_path, descriptor, entry.m_value);
                        visitRecursive(entry.m_value, visitor, entry_path);
                    }
                }
            }
        });
    }

    // ------------------------------------------------------------------
    // source values: owned, strictly typed. File bytes keep their TOML
    // scalar kind; CLI/environment/runtime text arrives as text and is
    // parsed per destination type. A file string "60" never satisfies an
    // integer property; a file integer never becomes an enum ordinal.
    // ------------------------------------------------------------------

    enum class ESourceKind : u8 {
        boolean,
        integer,
        floating,
        text,
        reset,
    };

    // Strict file decoding versus lenient text parsing (CLI/env/runtime).
    enum class ETextMode : u8 {
        strict,
        text,
    };

    [[nodiscard]] constexpr ETextMode modeForSource(const ESource source) noexcept {
        return source == ESource::file ? ETextMode::strict : ETextMode::text;
    }

    struct SourceValue {
        ESourceKind m_kind{ESourceKind::text};
        bool m_boolean{false};
        i64 m_integer{0};
        double m_floating{0.0};
        std::string m_text{};

        [[nodiscard]] static SourceValue boolean(const bool value) noexcept {
            SourceValue result{};
            result.m_kind = ESourceKind::boolean;
            result.m_boolean = value;
            return result;
        }

        [[nodiscard]] static SourceValue integer(const i64 value) noexcept {
            SourceValue result{};
            result.m_kind = ESourceKind::integer;
            result.m_integer = value;
            return result;
        }

        [[nodiscard]] static SourceValue floating(const double value) noexcept {
            SourceValue result{};
            result.m_kind = ESourceKind::floating;
            result.m_floating = value;
            return result;
        }

        [[nodiscard]] static SourceValue text(std::string value) noexcept {
            SourceValue result{};
            result.m_kind = ESourceKind::text;
            result.m_text = std::move(value);
            return result;
        }

        [[nodiscard]] static SourceValue reset() noexcept {
            SourceValue result{};
            result.m_kind = ESourceKind::reset;
            return result;
        }

        // Human-readable rendering for diagnostics; redacted when the
        // property is marked sensitive.
        [[nodiscard]] std::string describe(bool redact) const;
    };

    struct Assignment {
        std::string m_name{}; // canonical property name
        std::string m_alias_used{}; // spelling used at the source (may be empty)
        SourceValue m_value{};
        std::string m_location{}; // e.g. "argv[3]", "settings.toml:12", "env:PPR_X"
        std::size_t m_order{0};
    };

    // One precedence layer. Later layers in the resolve span win.
    // Recommended order (low to high): struct defaults (implicit),
    // named command/profile defaults (ESource::none trusted layer),
    // explicit base files, generated user overlay, environment, CLI,
    // accepted runtime overrides.
    struct Layer {
        ESource m_source{ESource::none};
        std::string m_origin{};
        Array<Assignment> m_assignments{};
    };

    // Slice 1a: canonical definition — presence is independent of leaf values.
    struct GroupPresence {
        std::string m_path{};
        std::string m_location{};
    };

    struct TreeLayer {
        Layer m_scalars{};
        Array<GroupPresence> m_groups{};
    };

    // ------------------------------------------------------------------
    // diagnostics: caller-owned collection. Every entry carries the
    // canonical name, alias used, source location, expected domain,
    // and error code. Sensitive values are redacted at the source.
    // ------------------------------------------------------------------

    struct Diagnostic {
        std::string m_location{};
        std::string m_property{};
        std::string m_alias{};
        std::string m_message{};
        std::error_code m_code{};
        Array<std::string> m_related_properties{};
    };

    class Diagnostics {
        Array<Diagnostic> m_entries{};
        std::size_t m_max_entries{(std::numeric_limits<std::size_t>::max)()};
        std::size_t m_total_count{};

    public:
        Diagnostics() = default;
        /// Retain the first max_entries records; counting and failure remain visible after the cap.
        explicit Diagnostics(std::size_t max_entries) noexcept;

        void add(
            std::string location, std::string property, std::string message, std::error_code code,
            std::string alias = {}, Array<std::string> related_properties = {});
        /// Preserve failure multiplicity when forwarding an already capped collector.
        void addOmitted(std::size_t count) noexcept;

        [[nodiscard]] bool hasErrors() const noexcept {
            return m_total_count != 0u;
        }

        [[nodiscard]] const Array<Diagnostic> &entries() const noexcept {
            return m_entries;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return m_total_count;
        }

        [[nodiscard]] std::size_t omittedCount() const noexcept {
            return m_total_count - m_entries.size();
        }

        /// A saturated counter cannot prove absence of additional errors; fail closed until clear().
        [[nodiscard]] bool hasErrorsSince(const std::size_t before) const noexcept {
            return m_total_count != before or m_total_count == (std::numeric_limits<std::size_t>::max)();
        }

        void clear() {
            m_entries.clear();
            m_total_count = 0u;
        }

        [[nodiscard]] std::string format() const;
    };

    // ------------------------------------------------------------------
    // resolved state: typed value plus the winning origin per property
    // plus a revision assigned by the owner when published.
    // ------------------------------------------------------------------

    struct PropertyOrigin {
        std::string m_name{};
        ESource m_source{ESource::none};
        std::string m_location{"default"};
    };

    template<typename Settings>
    struct Resolved {
        Settings m_value{};
        Array<PropertyOrigin> m_origins{};
        u64 m_revision{0};

        [[nodiscard]] const PropertyOrigin *find(const std::string_view name) const noexcept {
            for (const auto &origin: m_origins) {
                if (origin.m_name == name) {
                    return std::addressof(origin);
                }
            }
            return nullptr;
        }
    };

} // namespace pP::config

namespace pP::config::details {
    using ScratchText = std::basic_string<char, std::char_traits<char>, mem::STL<char, mem::ScratchPad>>;

    [[nodiscard]] ScratchText scratchPath(std::string_view prefix, std::string_view name);

    // Borrows source paths for one resolution; source arrays outlive the index.
    class SourcePathIndex {
    public:
        struct Entry {
            std::string_view m_path;
            std::size_t m_position;
        };

        explicit SourcePathIndex(std::span<const Assignment> assignments);
        explicit SourcePathIndex(std::span<const GroupPresence> groups);
        explicit SourcePathIndex(std::span<const ScratchText> paths);
        [[nodiscard]] std::span<const Entry> exact(std::string_view path) const noexcept;
        [[nodiscard]] bool supplied(std::string_view path, bool descendants) const;
        [[nodiscard]] Array<std::size_t, mem::ScratchPad> subtree(std::string_view path) const;

    private:
        Array<Entry, mem::ScratchPad> m_paths;
        void sort_();
    };
}

export namespace pP::config {
    namespace details {
        // Machinery used by resolve/applyRuntime; not application API.
        // ASCII-only helpers (config names are restricted alphabet).
        [[nodiscard]] std::string_view trim(std::string_view text) noexcept;
        [[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept;

        struct ResolveState {
            using OriginPositions = Array<std::size_t, mem::ScratchPad>;
            using OriginIndex = std::map<
                ScratchText, OriginPositions, std::less<>,
                mem::STL<std::pair<const ScratchText, OriginPositions>, mem::ScratchPad>>;

            std::span<const Layer *const> m_layers{};
            std::span<const SourcePathIndex> m_assignment_indices;
            Array<SourcePathIndex, mem::ScratchPad> m_assignment_storage;
            OriginIndex m_origin_indices;
            // Slots and liveness scale with all origins visited during this resolution.
            Array<u8, mem::ScratchPad> m_origin_live;
            Diagnostics *m_diagnostics{nullptr};
            Array<PropertyOrigin> *m_origins{nullptr};
            std::error_code m_first_error{};
            bool m_enforce_runtime_mutability{false};
            bool m_origin_index_valid{false};

            void retain(const std::error_code err) noexcept {
                if (err and not m_first_error) {
                    m_first_error = err;
                }
            }

            void report(
                std::string_view location, std::string_view property, std::string_view message, std::error_code code,
                std::string alias = {});
            void upsertOrigin(std::string_view name, ESource source, std::string_view location);
            void replaceOrigins(std::string_view path, Array<PropertyOrigin> &replacement);

            void rejectUnknown(std::span<const std::string_view> names);
            void indexSources();
            void indexOrigins();
            void finalizeOrigins();
        };

        template<typename S>
        concept THasCrossValidate = requires (const S &settings, Diagnostics &diagnostics) {
            { S::validate(settings, diagnostics) } -> std::convertible_to<std::error_code>;
        };

        template<typename T>
        [[nodiscard]] std::error_code parseIntegerText(std::string_view text, T &out) noexcept
            requires std::is_integral_v<T>
        {
            const std::string_view trimmed = trim(text);
            if (trimmed.empty()) {
                return make_error_code(EConfigErrc::invalid_integer);
            }

            if constexpr (std::is_signed_v<T>) {
                i64 wide = 0;
                const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), wide);
                if (ec == std::errc::result_out_of_range) {
                    return make_error_code(EConfigErrc::out_of_range);
                }
                if (ec != std::errc{} or ptr != trimmed.data() + trimmed.size()) {
                    return make_error_code(EConfigErrc::invalid_integer);
                }
                if (wide < static_cast<i64>(std::numeric_limits<T>::min()) or
                    wide > static_cast<i64>(std::numeric_limits<T>::max())) {
                    return make_error_code(EConfigErrc::out_of_range);
                }

                out = static_cast<T>(wide);
            } else {
                u64 wide = 0;
                const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), wide);
                if (ec == std::errc::result_out_of_range) {
                    return make_error_code(EConfigErrc::out_of_range);
                }
                if (ec != std::errc{} or ptr != trimmed.data() + trimmed.size()) {
                    return make_error_code(EConfigErrc::invalid_integer);
                }
                if (wide > static_cast<u64>(std::numeric_limits<T>::max())) {
                    return make_error_code(EConfigErrc::out_of_range);
                }

                out = static_cast<T>(wide);
            }
            return {};
        }

        template<typename T>
        [[nodiscard]] std::error_code parseFloatText(
            std::string_view text, T &out, const bool allow_non_finite) noexcept
            requires std::is_floating_point_v<T>
        {
            const std::string_view trimmed = trim(text);
            if (trimmed.empty()) {
                return make_error_code(EConfigErrc::invalid_float);
            }

            double wide = 0.0;
            const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), wide);
            if (ec == std::errc::result_out_of_range) {
                return make_error_code(EConfigErrc::out_of_range);
            }
            if (ec != std::errc{} or ptr != trimmed.data() + trimmed.size()) {
                return make_error_code(EConfigErrc::invalid_float);
            }
            if (not allow_non_finite and not std::isfinite(wide)) {
                return make_error_code(EConfigErrc::non_finite_not_allowed);
            }
            if (wide < static_cast<double>(std::numeric_limits<T>::lowest()) or
                wide > static_cast<double>(std::numeric_limits<T>::max())) {
                if (std::isfinite(wide)) {
                    return make_error_code(EConfigErrc::out_of_range);
                }
            }

            out = static_cast<T>(wide);
            return {};
        }

        [[nodiscard]] std::error_code parseBooleanText(std::string_view text, bool &out) noexcept;

        template<typename C>
        [[nodiscard]] constexpr bool allowsNonFinite(const C &) noexcept {
            return false;
        }

        [[nodiscard]] constexpr bool allowsNonFinite(const FloatDomain &domain) noexcept {
            return domain.m_allow_non_finite;
        }

        template<typename C, typename T>
        [[nodiscard]] std::error_code checkConstraint(const C &constraint, const T &value) {
            return constraint.validate(value);
        }

        template<typename C, typename T>
        [[nodiscard]] std::error_code checkConstraint(const C &constraint, const std::optional<T> &value) {
            if (not value.has_value()) {
                return {};
            }
            return constraint.validate(*value);
        }

        template<typename C, typename E>
        [[nodiscard]] std::error_code parseEnumValue(const C &, const std::string_view, E &) {
            return make_error_code(EConfigErrc::unknown_enum_value);
        }

        template<typename E, std::size_t N>
        [[nodiscard]] std::error_code parseEnumValue(
            const EnumDomain<E, N> &domain, const std::string_view text, E &out) {
            return domain.parse(trim(text), out);
        }

        // Typed conversion directed by the destination type.
        template<typename T, typename C>
            requires (std::is_integral_v<T> and not std::is_same_v<T, bool>)
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, T &out, const ETextMode mode, const C &, std::string &expected) {
            expected = "integer";
            if (src.m_kind == ESourceKind::integer) {
                if (not std::in_range<T>(src.m_integer)) {
                    return make_error_code(EConfigErrc::out_of_range);
                }

                out = static_cast<T>(src.m_integer);
                return {};
            }
            if (src.m_kind == ESourceKind::text and mode == ETextMode::text) {
                return parseIntegerText(src.m_text, out);
            }
            return make_error_code(EConfigErrc::type_mismatch);
        }

        template<typename T, typename C>
            requires std::is_floating_point_v<T>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, T &out, const ETextMode mode, const C &constraint, std::string &expected) {
            expected = "number";
            const bool allow_non_finite = allowsNonFinite(constraint);
            if (src.m_kind == ESourceKind::floating) {
                if (not allow_non_finite and not std::isfinite(src.m_floating)) {
                    return make_error_code(EConfigErrc::non_finite_not_allowed);
                }
                if (std::isfinite(src.m_floating) and
                    (src.m_floating < static_cast<double>(std::numeric_limits<T>::lowest()) or
                        src.m_floating > static_cast<double>(std::numeric_limits<T>::max()))) {
                    return make_error_code(EConfigErrc::out_of_range);
                }

                out = static_cast<T>(src.m_floating);
                return {};
            }
            if (src.m_kind == ESourceKind::integer) {
                out = static_cast<T>(src.m_integer);
                if (not allow_non_finite and not std::isfinite(out)) {
                    return make_error_code(EConfigErrc::non_finite_not_allowed);
                }
                return {};
            }
            if (src.m_kind == ESourceKind::text and mode == ETextMode::text) {
                return parseFloatText(src.m_text, out, allow_non_finite);
            }
            return make_error_code(EConfigErrc::type_mismatch);
        }

        template<typename C>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, bool &out, const ETextMode mode, const C &, std::string &expected) {
            expected = "boolean (one of true/false/yes/no/on/off/1/0)";
            if (src.m_kind == ESourceKind::boolean) {
                out = src.m_boolean;
                return {};
            }
            if (src.m_kind == ESourceKind::text and mode == ETextMode::text) {
                return parseBooleanText(src.m_text, out);
            }
            return make_error_code(EConfigErrc::type_mismatch);
        }

        template<typename C>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, std::string &out, const ETextMode, const C &, std::string &expected) {
            expected = "string";
            if (src.m_kind == ESourceKind::text) {
                out = src.m_text;
                return {};
            }
            return make_error_code(EConfigErrc::type_mismatch);
        }

        template<typename E, typename C>
            requires std::is_enum_v<E>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, E &out, const ETextMode, const C &constraint, std::string &expected) {
            expected = constraint.describe();
            // Named domain entries only: ordinals never cross the boundary.
            if (src.m_kind != ESourceKind::text) {
                return make_error_code(EConfigErrc::type_mismatch);
            }

            return parseEnumValue(constraint, src.m_text, out);
        }

        template<typename Ref>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, Ref &out, const ETextMode, const ReferenceDomain<Ref> &, std::string &expected) {
            expected = "reference key";
            if (src.m_kind != ESourceKind::text) {
                PPR_RETURN_ERROR_ON_FAIL(Config, EConfigErrc::type_mismatch);
            }
            out.m_key = src.m_text;
            return {};
        }

        template<typename T, typename C>
        [[nodiscard]] std::error_code convertTo(
            const SourceValue &src, std::optional<T> &out, const ETextMode mode, const C &constraint,
            std::string &expected) {
            if (src.m_kind == ESourceKind::reset) {
                out.reset();
                return {};
            }

            T inner{};
            if (const std::error_code err = convertTo(src, inner, mode, constraint, expected)) {
                return err;
            }

            out = std::move(inner);
            return {};
        }

        template<typename T, typename Owner, typename Constraint>
        void applyMember(T &field, const Member<Owner, T, Constraint> &member, ResolveState &state) {
            const std::string_view name = member.m_metadata.m_name;

            const Assignment *winner = nullptr;
            ESource winner_source = ESource::none;

            for (std::size_t layer_index = 0u; layer_index < state.m_layers.size(); ++layer_index) {
                const auto &layer = *state.m_layers[layer_index];
                const auto assignments = state.m_assignment_indices[layer_index].exact(name);
                if (layer.m_source != ESource::none and
                    (member.m_metadata.m_sources & layer.m_source) == ESource::none) {
                    // Disallowed source: still validate values below so hidden
                    // mistakes surface even though nothing is applied.
                    for (const auto &match: assignments) {
                        const auto &assignment = layer.m_assignments[match.m_position];
                        T scratch = field;
                        std::string expected;
                        std::error_code err = convertTo(
                            assignment.m_value, scratch, modeForSource(layer.m_source), member.m_constraint, expected);
                        if (not err) {
                            err = checkConstraint(member.m_constraint, scratch);
                        }

                        state.report(
                            assignment.m_location, std::string{name},
                            configFormat(
                                "property does not accept {} values (allowed: {}){}", sourceName(layer.m_source),
                                expected,
                                err ? configFormat("; value also invalid: {}", err.message()) : std::string{}),
                            make_error_code(EConfigErrc::source_disallowed), assignment.m_alias_used);
                        state.retain(make_error_code(EConfigErrc::source_disallowed));
                    }
                    continue;
                }

                if (state.m_enforce_runtime_mutability and member.m_metadata.m_mutability != EMutability::runtime) {
                    for (const auto &match: assignments) {
                        const auto &assignment = layer.m_assignments[match.m_position];
                        state.report(
                            assignment.m_location, std::string{name},
                            "startup-only property cannot be changed at runtime",
                            make_error_code(EConfigErrc::source_disallowed), assignment.m_alias_used);
                        state.retain(make_error_code(EConfigErrc::source_disallowed));
                    }
                    continue;
                }

                for (const auto &match: assignments) {
                    const auto &assignment = layer.m_assignments[match.m_position];
                    // Validate every supplied value, even shadowed ones.
                    T scratch = field;
                    std::string expected;
                    const std::error_code err = [&]() -> std::error_code {
                        if (assignment.m_value.m_kind == ESourceKind::reset and not is_optional_v<T>) {
                            expected = "a value (property is not optional; reset is explicit)";
                            return make_error_code(EConfigErrc::type_mismatch);
                        }
                        if (const std::error_code conv = convertTo(
                                assignment.m_value, scratch, modeForSource(layer.m_source), member.m_constraint,
                                expected)) {
                            return conv;
                        }
                        return checkConstraint(member.m_constraint, scratch);
                    }();
                    if (err) {
                        const bool redact = member.m_metadata.m_sensitive;
                        const std::string domain = member.m_constraint.describe();
                        std::string expectation = expected;
                        if (not domain.empty() and domain != expectation and domain != "any") {
                            if (domain.starts_with(expectation) or expectation.starts_with(domain)) {
                                expectation = domain.size() >= expectation.size() ? domain : expectation;
                            } else {
                                expectation += configFormat(" {}", domain);
                            }
                        }
                        state.report(
                            assignment.m_location, std::string{name},
                            configFormat(
                                "expected {}, received {}{}", expectation, assignment.m_value.describe(redact),
                                err.message().empty() ? std::string{} : configFormat(" ({})", err.message())),
                            err, assignment.m_alias_used);
                        state.retain(err);
                        continue;
                    }

                    winner = std::addressof(assignment);
                    winner_source = layer.m_source;
                    field = std::move(scratch);
                }
            }

            if (winner != nullptr) {
                state.upsertOrigin(name, winner_source, winner->m_location);
            }
        }

        // Renders a typed default for generated help text.
        template<typename Owner, typename T, typename Constraint, typename V>
        [[nodiscard]] std::string defaultText(const Member<Owner, T, Constraint> &member, const V &value) {
            if constexpr (is_optional_v<V>) {
                if (not value.has_value()) {
                    return "none";
                }
                return defaultText(member, *value);
            } else if constexpr (std::is_same_v<V, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_enum_v<V>) {
                if constexpr (requires { member.m_constraint.nameOf(value); }) {
                    const std::string_view entry = member.m_constraint.nameOf(value);
                    if (not entry.empty()) {
                        return std::string{entry};
                    }
                }
                return configFormat("#{}", static_cast<std::int64_t>(value));
            } else if constexpr (std::is_arithmetic_v<V>) {
                if constexpr (std::is_floating_point_v<V>) {
                    return configFormat("{}", value);
                } else {
                    return configFormat("{}", static_cast<std::int64_t>(value));
                }
            } else if constexpr (std::is_convertible_v<V, std::string_view>) {
                return configFormat("\"{}\"", std::string_view{value});
            } else {
                return "<complex>";
            }
        }

        template<typename C, typename T>
            struct DomainCompatible : std::bool_constant < requires(const C &c, const T &v) {
            {c.validate(v)}->std::convertible_to<std::error_code>;
            c.describe();
        }>{};
        template<typename T>
        struct DomainCompatible<AnyDomain, T> : std::bool_constant<not std::is_enum_v<T>> {};
        template<typename D, typename T>
        struct DomainCompatible<Range<D>, T> : std::bool_constant<std::is_same_v<D, T>> {};
        template<typename T>
        struct DomainCompatible<BooleanDomain, T> : std::bool_constant<std::is_same_v<T, bool>> {};
        template<typename T>
        struct DomainCompatible<FloatDomain, T> : std::bool_constant<std::is_floating_point_v<T>> {};
        template<typename T>
        struct DomainCompatible<StringDomain, T> : std::bool_constant<std::is_same_v<T, std::string>> {};
        template<typename T>
        struct DomainCompatible<PathDomain, T> : std::bool_constant<std::is_same_v<T, std::string>> {};
        template<typename E, std::size_t N, typename T>
        struct DomainCompatible<EnumDomain<E, N>, T> : std::bool_constant<std::is_same_v<E, T>> {};
        template<typename Ref, typename T>
        struct DomainCompatible<ReferenceDomain<Ref>, T> : std::bool_constant<std::is_same_v<Ref, T>> {};

        template<typename T, typename C>
        inline constexpr bool scalar_compatible_v = DomainCompatible<C, Unwrapped<T>>::value and
                                                    (std::is_arithmetic_v<Unwrapped<T>> or
                                                        std::is_enum_v<Unwrapped<T>> or
                                                        std::is_same_v<Unwrapped<T>, std::string> or
                                                        (std::is_same_v<C, ReferenceDomain<Unwrapped<T>>> and
                                                            requires (Unwrapped<T> v) {
                                                                { v.m_key } -> std::same_as<std::string &>;
                                                            }));

        template<typename Settings>
        [[nodiscard]] constexpr bool hasRecursiveMembers() {
            using Members = decltype(Settings::describe());
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (
                    (node_kind_v<std::remove_cvref_t<decltype(std::declval<std::tuple_element_t<I, Members>>()
                                .m_constraint)>> != ENodeKind::scalar) or
                    ...);
            }(std::make_index_sequence<std::tuple_size_v<Members>>{});
        }

        inline void schemaError(Diagnostics &diagnostics, const std::string &path, std::string message) {
            diagnostics.add("schema", path, std::move(message), make_error_code(EConfigErrc::schema_invalid));
        }

        template<typename C>
        void validateDomain(const C &domain, const std::string &path, Diagnostics &diagnostics) {
            if constexpr (requires {
                              domain.m_min;
                              domain.m_max;
                          }) {
                bool invalid = domain.m_min > domain.m_max;
                if constexpr (std::is_floating_point_v<decltype(domain.m_min)>) {
                    invalid = invalid or not std::isfinite(domain.m_min) or not std::isfinite(domain.m_max);
                }
                if constexpr (requires { domain.m_exclusive_min; }) {
                    invalid = invalid or (domain.m_exclusive_min and domain.m_min == domain.m_max);
                }
                if (invalid) {
                    schemaError(diagnostics, path, "inconsistent domain bounds");
                }
            }
            if constexpr (requires { domain.m_entries; }) {
                if (domain.m_entries.empty()) {
                    schemaError(diagnostics, path, "enum domain has no entries");
                }
                for (std::size_t i = 0; i < domain.m_entries.size(); ++i) {
                    if (domain.m_entries[i].m_name.empty()) {
                        schemaError(diagnostics, path, "enum name is empty");
                    }
                    for (std::size_t j = 0; j < i; ++j) {
                        if (domain.m_entries[i].m_name == domain.m_entries[j].m_name or
                            domain.m_entries[i].m_value == domain.m_entries[j].m_value) {
                            schemaError(diagnostics, path, "duplicate enum name or value");
                        }
                    }
                }
            }
            if constexpr (requires { domain.m_target; }) {
                if (not isValidCanonicalName(domain.m_target.m_collection) or
                    (domain.m_target.m_scope != EReferenceScope::root and
                        domain.m_target.m_scope != EReferenceScope::owner)) {
                    schemaError(diagnostics, path, "invalid reference target");
                }
            }
        }

        // Stored variant initializers are values, not authored layers: required
        // payload leaves are checked for validity rather than authoring presence.
        template<typename Settings>
        void validateStoredTreeValue(const Settings &value, const std::string_view prefix, Diagnostics &diagnostics) {
            const auto initial_errors = diagnostics.size();
            const auto cross_validate = [&](const auto &object, const std::string_view path) {
                using S = std::remove_cvref_t<decltype(object)>;
                if constexpr (THasCrossValidate<S>) {
                    if (diagnostics.hasErrorsSince(initial_errors)) {
                        return;
                    }
                    const auto before = diagnostics.size();
                    const auto error = S::validate(object, diagnostics);
                    if (error and not diagnostics.hasErrorsSince(before)) {
                        diagnostics.add(
                            "default", std::string{path}, "initializer cross-field validation failed", error);
                    }
                }
            };
            visitRecursive(
                value,
                [&](const auto &path, const auto &member, const auto &field_value) {
                    using T = std::remove_cvref_t<decltype(field_value)>;
                    using C = std::remove_cvref_t<decltype(member.m_constraint)>;
                    if constexpr (node_kind_v<C> == ENodeKind::scalar and scalar_compatible_v<T, C>) {
                        if (const auto error = checkConstraint(member.m_constraint, field_value)) {
                            diagnostics.add("default", path, "initializer violates its domain", error);
                        }
                    } else if constexpr (node_kind_v<C> == ENodeKind::variant) {
                        if constexpr (is_optional_v<T>) {
                            if (field_value and variantTag(member, field_value).empty()) {
                                diagnostics.add(
                                    "default", path, "invalid variant initializer",
                                    make_error_code(EConfigErrc::unknown_enum_value));
                            }
                        } else if (variantTag(member, field_value).empty()) {
                            diagnostics.add(
                                "default", path, "invalid variant initializer",
                                make_error_code(EConfigErrc::unknown_enum_value));
                        }
                    } else if constexpr (node_kind_v<C> == ENodeKind::collection and CollectionTraits<T>::value) {
                        if (field_value.m_entries.size() < member.m_constraint.m_min or
                            field_value.m_entries.size() > member.m_constraint.m_max) {
                            diagnostics.add(
                                "default", path, "initializer violates cardinality bounds",
                                make_error_code(EConfigErrc::out_of_range));
                        }
                        Array<std::string_view> keys;
                        for (const auto &entry: field_value.m_entries) {
                            if (not isValidKey(entry.m_key) or std::ranges::find(keys, entry.m_key) != keys.end()) {
                                diagnostics.add(
                                    "default", recursivePath(path, entry.m_key), "invalid or duplicate key",
                                    make_error_code(EConfigErrc::duplicate_key));
                            }
                            keys.push_back(entry.m_key);
                        }
                    }
                },
                prefix);

            // Whole-object validators run only after every active descendant's
            // domain has been checked; preorder callbacks alone are too early.
            if (not diagnostics.hasErrorsSince(initial_errors)) {
                visitRecursive(
                    value,
                    [&](const auto &path, const auto &member, const auto &field_value) {
                        using T = std::remove_cvref_t<decltype(field_value)>;
                        using C = std::remove_cvref_t<decltype(member.m_constraint)>;
                        if constexpr (node_kind_v<C> == ENodeKind::group and TDescribed<Unwrapped<T>>) {
                            if constexpr (is_optional_v<T>) {
                                if (field_value) {
                                    cross_validate(*field_value, path);
                                }
                            } else {
                                cross_validate(field_value, path);
                            }
                        }
                    },
                    prefix);
            }
            cross_validate(value, prefix);
        }

        template<typename Settings>
        void validateRecursiveSchema(
            const Settings &probe, const std::string_view prefix, Diagnostics &diagnostics,
            const bool validate_initializers, const bool default_group = false) {
            Array<std::string_view> names;
            visit(probe, [&](const auto &member, const auto &value) {
                using T = std::remove_cvref_t<decltype(value)>;
                using C = std::remove_cvref_t<decltype(member.m_constraint)>;
                const auto name = member.m_metadata.m_name;
                const auto path = recursivePath(prefix, name);
                if (not isValidCanonicalName(name) or name.find('.') != std::string_view::npos) {
                    schemaError(diagnostics, path, "recursive names must be single non-empty canonical segments");
                }
                if (std::ranges::find(names, name) != names.end()) {
                    schemaError(diagnostics, path, "duplicate sibling name");
                }
                names.push_back(name);
                const bool check_default = validate_initializers and member.m_presence != EPresence::required;
                if (validate_initializers and default_group and member.m_presence == EPresence::required) {
                    schemaError(diagnostics, path, "a group initializer cannot supply a required descendant");
                }

                if constexpr (node_kind_v<C> == ENodeKind::group) {
                    if constexpr (TDescribed<Unwrapped<T>>) {
                        if constexpr (is_optional_v<T>) {
                            if (member.m_presence != EPresence::optional) {
                                schemaError(diagnostics, path, "optional groups must declare optional presence");
                            }
                            if (value) {
                                validateRecursiveSchema(*value, path, diagnostics, check_default, check_default);
                            } else {
                                validateRecursiveSchema(Unwrapped<T>{}, path, diagnostics, false);
                            }
                        } else {
                            if (member.m_presence == EPresence::optional) {
                                schemaError(diagnostics, path, "optional presence requires optional<T>");
                            }
                            validateRecursiveSchema(value, path, diagnostics, check_default, check_default);
                        }
                    } else {
                        schemaError(diagnostics, path, "group requires a described object");
                    }
                } else if constexpr (node_kind_v<C> == ENodeKind::variant) {
                    if (member.m_presence == EPresence::optional and not is_optional_v<T>) {
                        schemaError(diagnostics, path, "optional variant presence requires optional<variant>");
                    }
                    Array<std::string_view> tags;
                    std::apply(
                        [&](const auto &...alt) {
                            const auto check_alternative = [&](const auto &alternative) {
                                using A = typename std::remove_cvref_t<decltype(alternative)>::Type;
                                const auto payload_path = recursivePath(path, alternative.m_tag);
                                if (not isValidCanonicalName(alternative.m_tag) or
                                    alternative.m_tag.find('.') != std::string_view::npos or
                                    alternative.m_tag == "tag") {
                                    schemaError(diagnostics, payload_path, "invalid or reserved variant tag");
                                }
                                if (std::ranges::find(tags, alternative.m_tag) != tags.end()) {
                                    schemaError(diagnostics, payload_path, "duplicate variant tag");
                                }
                                tags.push_back(alternative.m_tag);
                                if constexpr (TDescribed<A>) {
                                    validateRecursiveSchema(A{}, payload_path, diagnostics, false);
                                } else {
                                    schemaError(
                                        diagnostics, payload_path, "variant alternatives must be described objects");
                                }
                            };
                            (check_alternative(alt), ...);
                        },
                        member.m_constraint.m_alternatives);
                    if constexpr (not variant_compatible_v<T, C>) {
                        schemaError(diagnostics, path, "variant domain must cover each payload type exactly once");
                    } else if (check_default and member.m_presence == EPresence::defaulted) {
                        const auto check_value = [&](const auto &variant) {
                            const auto before = diagnostics.size();
                            if (variant.valueless_by_exception()) {
                                schemaError(diagnostics, path, "valueless variant initializer");
                                return;
                            }
                            std::visit(
                                [&](const auto &payload) {
                                    validateStoredTreeValue(
                                        payload, recursivePath(path, variantTag(member, variant)), diagnostics);
                                },
                                variant);
                            if (diagnostics.hasErrorsSince(before)) {
                                schemaError(diagnostics, path, "invalid variant initializer");
                            }
                        };
                        if constexpr (is_optional_v<T>) {
                            if (value) {
                                check_value(*value);
                            }
                        } else {
                            check_value(value);
                        }
                    }
                } else if constexpr (node_kind_v<C> == ENodeKind::collection) {
                    validateDomain(member.m_constraint, path, diagnostics);
                    if (member.m_presence == EPresence::optional) {
                        schemaError(diagnostics, path, "collections do not support optional presence");
                    }
                    if constexpr (CollectionTraits<T>::value) {
                        using E = typename CollectionTraits<T>::Element;
                        if constexpr (TDescribed<E>) {
                            // Empty collections still expose and validate the complete element schema.
                            validateRecursiveSchema(E{}, recursivePath(path, "{key}"), diagnostics, false);
                            if (check_default) {
                                if (value.m_entries.size() < member.m_constraint.m_min or
                                    value.m_entries.size() > member.m_constraint.m_max) {
                                    schemaError(diagnostics, path, "default collection violates cardinality bounds");
                                }
                            }

                            Array<std::string_view> keys;
                            for (const auto &entry: value.m_entries) {
                                if (check_default) {
                                    if (not isValidKey(entry.m_key) or
                                        std::ranges::find(keys, entry.m_key) != keys.end()) {
                                        schemaError(
                                            diagnostics, recursivePath(path, entry.m_key), "invalid or duplicate key");
                                    }
                                    keys.push_back(entry.m_key);
                                }
                                validateRecursiveSchema(
                                    entry.m_value, recursivePath(path, entry.m_key), diagnostics, check_default,
                                    check_default);
                            }
                        } else {
                            schemaError(diagnostics, path, "collection elements must be described objects");
                        }
                    } else {
                        schemaError(diagnostics, path, "collection descriptor requires KeyedCollection<T>");
                    }
                } else {
                    validateDomain(member.m_constraint, path, diagnostics);
                    if constexpr (scalar_compatible_v<T, C>) {
                        const auto &metadata = member.m_metadata;
                        const bool numeric = std::is_arithmetic_v<Unwrapped<T>> and
                                             not std::is_same_v<Unwrapped<T>, bool>;
                        if ((metadata.m_slider_min and (not numeric or not std::isfinite(*metadata.m_slider_min))) or
                            (metadata.m_slider_max and (not numeric or not std::isfinite(*metadata.m_slider_max))) or
                            (metadata.m_slider_min and
                                metadata.m_slider_max and
                                *metadata.m_slider_min > *metadata.m_slider_max)) {
                            schemaError(diagnostics, path, "inconsistent numeric editing interval");
                        }
                        if (member.m_presence == EPresence::optional and not is_optional_v<T>) {
                            schemaError(diagnostics, path, "optional scalar presence requires optional<T>");
                        }
                        // Leaf defaults remain part of the schema even when the enclosing initializer is absent.
                        // Only required leaf placeholders are exempt from domain validation.
                        if (member.m_presence != EPresence::required) {
                            if (const auto error = checkConstraint(member.m_constraint, value)) {
                                schemaError(
                                    diagnostics, path, configFormat("invalid default value ({})", error.message()));
                            }
                        }
                    } else {
                        schemaError(diagnostics, path, "incompatible scalar descriptor and member type");
                    }
                }
            });
        }

        struct TreeResolveState {
            std::span<const TreeLayer> m_layers;
            ResolveState m_scalars;
            std::span<const SourcePathIndex> m_group_indices;
            Array<SourcePathIndex, mem::ScratchPad> m_group_storage;
            Array<ScratchText, mem::ScratchPad> m_scalar_paths;
            Array<ScratchText, mem::ScratchPad> m_group_paths;
            // Each boundary delegates consumed-path checks to an isolated
            // one-layer subtree. Paths from different tags/layers never mix.
            Array<ScratchText, mem::ScratchPad> m_atomic_paths;
            std::string_view m_scope;
            bool m_atomic_replacement{};

            [[nodiscard]] bool supplied(std::string_view path, bool descendants) const;
            void report(
                std::string_view location, std::string_view path, std::string_view message, std::error_code error);
            void presence(std::string_view path, const Metadata &metadata, bool required);
            void rejectUnknown();
            [[nodiscard]] Array<std::string_view, mem::ScratchPad> entryKeys(std::string_view path);
            void indexSources();
        };

        template<typename Settings>
        void resolveTreeObject(Settings &object, std::string_view prefix, TreeResolveState &state);

        template<typename Owner, typename T, typename C>
        void resolveTreeVariant(
            T &value, const Member<Owner, T, C> &member, const std::string_view path, TreeResolveState &state) {
            state.presence(path, member.m_metadata, member.m_presence == EPresence::required);
            state.m_atomic_paths.emplace_back(path);
            const auto tag_path = scratchPath(path, "tag");
            const bool supplied = state.supplied(path, true);
            if constexpr (is_optional_v<T>) {
                if (member.m_presence == EPresence::optional) {
                    value.reset();
                }
            }
            if (not supplied) {
                const auto record_default = [&](const auto &variant) {
                    state.m_scalars.upsertOrigin(tag_path, ESource::none, "default");
                    if (variant.valueless_by_exception()) {
                        return;
                    }
                    const auto payload_path = recursivePath(path, variantTag(member, variant));
                    state.m_scalars.upsertOrigin(payload_path, ESource::none, "default");
                    std::visit(
                        [&](const auto &payload) {
                            visitRecursive(
                                payload,
                                [&](const auto &child_path, const auto &child_member, const auto &child_value) {
                                    state.m_scalars.upsertOrigin(child_path, ESource::none, "default");
                                    using ChildC = std::remove_cvref_t<decltype(child_member.m_constraint)>;
                                    if constexpr (node_kind_v<ChildC> == ENodeKind::variant) {
                                        if (not variantTag(child_member, child_value).empty()) {
                                            state.m_scalars.upsertOrigin(
                                                recursivePath(child_path, "tag"), ESource::none, "default");
                                        }
                                    }
                                },
                                payload_path);
                        },
                        variant);
                };
                if (member.m_presence == EPresence::defaulted) {
                    if constexpr (is_optional_v<T>) {
                        if (value) {
                            record_default(*value);
                        }
                    } else {
                        record_default(value);
                    }
                }
                return;
            }

            for (std::size_t layer_index = 0u; layer_index < state.m_layers.size(); ++layer_index) {
                const auto &layer = state.m_layers[layer_index];
                const auto &assignments = state.m_scalars.m_assignment_indices[layer_index];
                const auto &groups = state.m_group_indices[layer_index];
                if (not assignments.supplied(path, true) and not groups.supplied(path, true)) {
                    continue;
                }

                // This replacement observes only its prefix in the original immutable layer.
                // Reuse the original indexes instead of copying all subtree values and paths.
                const Layer *const scalar_layer = std::addressof(layer.m_scalars);
                Array<PropertyOrigin> origins;
                TreeResolveState replacement{};
                replacement.m_layers = std::span{std::addressof(layer), 1u};
                replacement.m_scalars.m_layers = std::span{std::addressof(scalar_layer), 1u};
                replacement.m_scalars.m_assignment_indices = std::span{std::addressof(assignments), 1u};
                replacement.m_group_indices = std::span{std::addressof(groups), 1u};
                replacement.m_scalars.m_diagnostics = state.m_scalars.m_diagnostics;
                replacement.m_scalars.m_origins = std::addressof(origins);
                replacement.m_scope = path;
                replacement.m_atomic_replacement = true;
                replacement.m_group_paths.emplace_back(path);
                replacement.m_scalar_paths.push_back(tag_path);

                const Assignment *tag = nullptr;
                for (const auto &match: assignments.exact(tag_path)) {
                    tag = std::addressof(layer.m_scalars.m_assignments[match.m_position]);
                }

                const auto &location = tag != nullptr ? tag->m_location : layer.m_scalars.m_origin;
                replacement.m_scalars.upsertOrigin(path, layer.m_scalars.m_source, location);
                bool selected = false;
                if (tag == nullptr) {
                    replacement.report(
                        location, tag_path, "variant tag was not supplied",
                        make_error_code(EConfigErrc::missing_value));
                } else if (tag->m_value.m_kind != ESourceKind::text) {
                    replacement.report(
                        location, tag_path, "variant tag must be text", make_error_code(EConfigErrc::type_mismatch));
                } else {
                    replacement.m_scalars.upsertOrigin(tag_path, layer.m_scalars.m_source, location);
                    std::apply(
                        [&](const auto &...alt) {
                            const auto resolve_alternative = [&](const auto &alternative) {
                                using A = typename std::remove_cvref_t<decltype(alternative)>::Type;
                                if (tag->m_value.m_text != alternative.m_tag) {
                                    return;
                                }
                                selected = true;
                                const auto payload_path = scratchPath(path, alternative.m_tag);
                                replacement.presence(payload_path, member.m_metadata, true);
                                A payload{};
                                resolveTreeObject(payload, payload_path, replacement);
                                if constexpr (is_optional_v<T>) {
                                    value.emplace(std::in_place_type<A>, std::move(payload));
                                } else {
                                    value.template emplace<A>(std::move(payload));
                                }
                            };
                            (resolve_alternative(alt), ...);
                        },
                        member.m_constraint.m_alternatives);
                    if (not selected) {
                        replacement.report(
                            location, tag_path, "unknown variant tag",
                            make_error_code(EConfigErrc::unknown_enum_value));
                    }
                }

                // Validate even a shadowed replacement, then retain only the
                // latest complete subtree's origins (including same-tag edits).
                replacement.rejectUnknown();
                state.m_scalars.retain(replacement.m_scalars.m_first_error);

                replacement.m_scalars.finalizeOrigins();
                state.m_scalars.replaceOrigins(path, origins);
            }
        }

        template<typename Settings>
        void resolveTreeObject(Settings &object, const std::string_view prefix, TreeResolveState &state) {
            visit(object, [&](const auto &member, auto &value) {
                using T = std::remove_cvref_t<decltype(value)>;
                using C = std::remove_cvref_t<decltype(member.m_constraint)>;
                const auto path = scratchPath(prefix, member.m_metadata.m_name);
                if constexpr (node_kind_v<C> == ENodeKind::scalar) {
                    state.m_scalar_paths.push_back(path);
                    state.m_scalars.upsertOrigin(path, ESource::none, "default");
                    const bool supplied = state.supplied(path, false);
                    if (member.m_presence == EPresence::required and not supplied) {
                        state.report(
                            "resolve", path, "required property was not supplied",
                            make_error_code(EConfigErrc::missing_value));
                    }
                    if constexpr (is_optional_v<T>) {
                        if (member.m_presence == EPresence::optional and not supplied) {
                            value.reset();
                        }
                    }
                    if constexpr (scalar_compatible_v<T, C>) {
                        auto qualified = member;
                        qualified.m_metadata.m_name = path;
                        applyMember(value, qualified, state.m_scalars);
                        if (not supplied and member.m_presence != EPresence::required) {
                            if (const auto error = checkConstraint(member.m_constraint, value)) {
                                state.report("default", path, "retained initializer violates its domain", error);
                            }
                        }
                    }
                } else if constexpr (node_kind_v<C> == ENodeKind::group and TDescribed<Unwrapped<T>>) {
                    state.presence(path, member.m_metadata, member.m_presence == EPresence::required);
                    if constexpr (is_optional_v<T>) {
                        if (state.supplied(path, true)) {
                            // Authoring an optional group starts a new T{}, not a hidden engaged initializer.
                            value.emplace();
                            resolveTreeObject(*value, path, state);
                        } else {
                            value.reset();
                        }
                    } else {
                        resolveTreeObject(value, path, state);
                    }
                } else if constexpr (node_kind_v<C> == ENodeKind::variant and variant_compatible_v<T, C>) {
                    resolveTreeVariant(value, member, path, state);
                } else if constexpr (node_kind_v<C> == ENodeKind::collection and CollectionTraits<T>::value) {
                    using E = typename CollectionTraits<T>::Element;
                    if constexpr (TDescribed<E>) {
                        state.presence(path, member.m_metadata, member.m_presence == EPresence::required);
                        Array<std::string_view, mem::ScratchPad> initial_keys;
                        for (const auto &entry: value.m_entries) {
                            if (not isValidKey(entry.m_key)) {
                                state.report(
                                    "default", recursivePath(path, entry.m_key), "invalid collection key",
                                    make_error_code(EConfigErrc::out_of_range));
                            }
                            if (std::ranges::find(initial_keys, entry.m_key) != initial_keys.end()) {
                                state.report(
                                    "default", recursivePath(path, entry.m_key), "duplicate collection key",
                                    make_error_code(EConfigErrc::duplicate_key));
                            }
                            initial_keys.push_back(entry.m_key);
                        }
                        const auto keys = state.entryKeys(path);
                        for (const auto &key: keys) {
                            if (std::ranges::find(value.m_entries, key, &KeyedEntry<E>::m_key) ==
                                value.m_entries.end()) {
                                value.m_entries.push_back({std::string{key}, E{}});
                            }
                        }
                        for (auto &entry: value.m_entries) {
                            const auto entry_path = scratchPath(path, entry.m_key);
                            state.presence(entry_path, member.m_metadata, false);
                            resolveTreeObject(entry.m_value, entry_path, state);
                        }
                        if (value.m_entries.size() < member.m_constraint.m_min or
                            value.m_entries.size() > member.m_constraint.m_max) {
                            state.report(
                                "resolve", path, "collection violates cardinality bounds",
                                make_error_code(EConfigErrc::out_of_range));
                        }
                    }
                }
            });

            // Incomplete records are never passed to whole-record validators.
            if constexpr (THasCrossValidate<Settings>) {
                if (not state.m_scalars.m_first_error) {
                    auto &diagnostics = *state.m_scalars.m_diagnostics;
                    const auto before = diagnostics.size();
                    const auto error = Settings::validate(object, diagnostics);
                    if (error and not diagnostics.hasErrorsSince(before)) {
                        state.report("resolve", std::string{prefix}, "cross-field validation failed", error);
                    }
                    state.m_scalars.retain(error);
                    if (diagnostics.hasErrorsSince(before)) {
                        state.m_scalars.retain(make_error_code(EConfigErrc::cross_field_invalid));
                    }
                }
            }
        }
    } // namespace details

    // ------------------------------------------------------------------
    // resolution: transactional. Every supplied value is converted and
    // validated (even shadowed ones); on any failure no Resolved is
    // produced and diagnostics explain every problem.
    // ------------------------------------------------------------------

    template<typename Settings>
    [[nodiscard]] Expected<Resolved<Settings>> resolve(std::span<const Layer> layers, Diagnostics &diagnostics) {
        const auto scratch_scope = mem::ScratchPad::open();
        Array<const Layer *, mem::ScratchPad> borrowed_layers;
        borrowed_layers.reserve(layers.size());
        for (const auto &layer: layers) {
            borrowed_layers.push_back(std::addressof(layer));
        }

        details::ResolveState state{};
        state.m_layers = borrowed_layers;
        state.indexSources();
        state.m_diagnostics = std::addressof(diagnostics);

        Resolved<Settings> result{};
        state.m_origins = std::addressof(result.m_origins);

        Array<std::string_view, mem::ScratchPad> names{};
        visit(result.m_value, [&](const auto &member, auto &) { names.push_back(member.m_metadata.m_name); });

        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                if (names[i] == names[j]) {
                    diagnostics.add(
                        "schema", std::string{names[i]}, "duplicate canonical property name in schema",
                        make_error_code(EConfigErrc::schema_invalid));
                    return std::unexpected{make_error_code(EConfigErrc::schema_invalid)};
                }
            }
        }

        state.rejectUnknown(names);

        visit(result.m_value, [&](const auto &member, auto &field) {
            state.upsertOrigin(member.m_metadata.m_name, ESource::none, "default");
            details::applyMember(field, member, state);
        });

        if constexpr (details::THasCrossValidate<Settings>) {
            const std::size_t before = diagnostics.size();
            if (const std::error_code err = Settings::validate(result.m_value, diagnostics)) {
                if (diagnostics.size() == before) {
                    diagnostics.add("resolve", "", "cross-field validation failed", err);
                }
                state.retain(err);
            }
        }

        if (state.m_first_error) {
            return std::unexpected{state.m_first_error};
        }

        state.finalizeOrigins();
        return result;
    }

    // Applies one accepted runtime-override layer onto committed state.
    // Startup-only properties are rejected; failure leaves resolved
    // untouched (scratch-then-commit).
    template<typename Settings>
    [[nodiscard]] std::error_code applyRuntime(
        Resolved<Settings> &resolved, const Layer &layer, Diagnostics &diagnostics) {
        static_assert(
            std::is_copy_constructible_v<Settings>, "Settings must be copyable for transactional runtime apply");
        static_assert(std::is_copy_assignable_v<Settings>, "Settings must be copyable for transactional runtime apply");

        const auto scratch_scope = mem::ScratchPad::open();
        Settings scratch = resolved.m_value;
        Array<PropertyOrigin> origins = resolved.m_origins;

        details::ResolveState state{};
        const Layer *const single = std::addressof(layer);
        state.m_layers = std::span{std::addressof(single), 1u};
        state.indexSources();
        state.m_diagnostics = std::addressof(diagnostics);
        state.m_origins = std::addressof(origins);
        state.m_enforce_runtime_mutability = true;

        Array<std::string_view, mem::ScratchPad> names{};
        visit(scratch, [&](const auto &member, auto &) { names.push_back(member.m_metadata.m_name); });
        state.rejectUnknown(names);

        visit(scratch, [&](const auto &member, auto &field) { details::applyMember(field, member, state); });

        if constexpr (details::THasCrossValidate<Settings>) {
            const std::size_t before = diagnostics.size();
            if (const std::error_code err = Settings::validate(scratch, diagnostics)) {
                if (diagnostics.size() == before) {
                    diagnostics.add("runtime", "", "cross-field validation failed", err);
                }
                state.retain(err);
            }
        }

        if (state.m_first_error) {
            return state.m_first_error;
        }

        state.finalizeOrigins();
        resolved.m_value = std::move(scratch);
        resolved.m_origins = std::move(origins);
        return {};
    }

    // Executable schema check: duplicate names, restricted alphabet,
    // and default values satisfying their own constraints.
    template<typename Settings>
    [[nodiscard]] Expected<void> validateSchema(Diagnostics &diagnostics) {
        using pP::make_error_code;
        const std::size_t before = diagnostics.size();
        Settings probe{};

        if constexpr (details::hasRecursiveMembers<Settings>()) {
            details::validateRecursiveSchema(probe, {}, diagnostics, true);
            if (diagnostics.hasErrorsSince(before)) {
                PPR_RETURN_UNEXPECTED_ON_FAIL(Config, EConfigErrc::schema_invalid);
            }
            return Expected<void>{};
        } else {

            Array<std::string_view> names{};
            visit(probe, [&](const auto &member, auto &field) {
                const std::string_view name = member.m_metadata.m_name;
                names.push_back(name);
                if (not isValidCanonicalName(name)) {
                    diagnostics.add(
                        "schema", std::string{name}, "canonical name must use [a-z0-9_.-] with no empty segments",
                        make_error_code(EConfigErrc::schema_invalid));
                }
                details::validateDomain(member.m_constraint, std::string{name}, diagnostics);
                if (member.m_presence != EPresence::required) {
                    if (const std::error_code err = details::checkConstraint(member.m_constraint, field)) {
                        diagnostics.add(
                            "schema", std::string{name}, configFormat("invalid default value ({})", err.message()),
                            make_error_code(EConfigErrc::schema_invalid));
                    }
                }
            });

            for (std::size_t i = 0; i < names.size(); ++i) {
                for (std::size_t j = i + 1; j < names.size(); ++j) {
                    if (names[i] == names[j]) {
                        diagnostics.add(
                            "schema", std::string{names[i]}, "duplicate canonical property name in schema",
                            make_error_code(EConfigErrc::schema_invalid));
                    }
                }
            }

            if (diagnostics.hasErrorsSince(before)) {
                return std::unexpected{make_error_code(EConfigErrc::schema_invalid)};
            }
            return Expected<void>{};
        }
    }

    // Slice 1a: canonical definition — transactional keyed tree resolution.
    template<typename Settings>
    [[nodiscard]] Expected<Resolved<Settings>> resolveRecursive(
        const std::span<const TreeLayer> layers, Diagnostics &diagnostics) {
        using pP::make_error_code;
        const auto scratch_scope = mem::ScratchPad::open();
        const auto schema = validateSchema<Settings>(diagnostics);
        PPR_RETURN_UNEXPECTED_ON_FAIL(Config, schema);

        Array<const Layer *, mem::ScratchPad> scalars;
        scalars.reserve(layers.size());
        for (const auto &layer: layers) {
            scalars.push_back(std::addressof(layer.m_scalars));
        }

        Resolved<Settings> result;
        details::TreeResolveState state{};
        state.m_layers = layers;
        state.m_scalars.m_layers = scalars;
        state.indexSources();
        state.m_scalars.m_diagnostics = std::addressof(diagnostics);
        state.m_scalars.m_origins = std::addressof(result.m_origins);
        details::resolveTreeObject(result.m_value, {}, state);
        state.rejectUnknown();
        PPR_RETURN_UNEXPECTED_ON_FAIL(Config, state.m_scalars.m_first_error);

        state.m_scalars.finalizeOrigins();
        return result;
    }
} // namespace pP::config
