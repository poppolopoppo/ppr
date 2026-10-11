module;

#include "pP/Macros.h"

export module engine.core:config;

import :containers.stl;
import :types;
import :utility;

import std;

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

    [[nodiscard]] std::error_code make_error_code(const EConfigErrc err) noexcept;

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

    [[nodiscard]] std::string_view sourceName(const ESource source) noexcept;

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
    };

    // Canonical segments use a restricted identifier alphabet so that
    // environment-name derivation and CLI spelling are unambiguous.
    [[nodiscard]] bool isValidCanonicalName(std::string_view name) noexcept;

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
    };

    template<typename Owner, typename T, typename Constraint>
    [[nodiscard]] constexpr Member<Owner, T, Constraint> field(
        T Owner::*member, const Metadata metadata, const Constraint constraint) {
        return Member<Owner, T, Constraint>{member, metadata, constraint};
    }

    template<typename Settings, typename Visitor>
    constexpr void visit(Settings &settings, Visitor &&visitor) {
        auto members = std::remove_cvref_t<Settings>::describe();
        std::apply([&](const auto &...member) { (visitor(member, settings.*(member.m_member)), ...); }, members);
    }

    template<typename Settings, typename Visitor>
    constexpr void visit(const Settings &settings, Visitor &&visitor) {
        const auto members = std::remove_cvref_t<Settings>::describe();
        std::apply([&](const auto &...member) { (visitor(member, settings.*(member.m_member)), ...); }, members);
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
            std::string alias = {});
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

    namespace details {
        // Machinery used by resolve/applyRuntime; not application API.
        // ASCII-only helpers (config names are restricted alphabet).
        [[nodiscard]] std::string_view trim(std::string_view text) noexcept;
        [[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept;

        struct ResolveState {
            std::span<const Layer> m_layers{};
            Diagnostics *m_diagnostics{nullptr};
            Array<PropertyOrigin> *m_origins{nullptr};
            std::error_code m_first_error{};
            bool m_enforce_runtime_mutability{false};

            void retain(const std::error_code err) noexcept {
                if (err and not m_first_error) {
                    m_first_error = err;
                }
            }

            void report(
                std::string location, std::string property, std::string message, std::error_code code,
                std::string alias = {});
            void upsertOrigin(std::string name, ESource source, std::string location);
            void rejectUnknown(std::span<const std::string_view> names);
        };

        template<typename T>
        struct IsOptional : std::false_type {};

        template<typename T>
        struct IsOptional<std::optional<T>> : std::true_type {};

        template<typename T>
        inline constexpr bool is_optional_v = IsOptional<std::remove_cvref_t<T>>::value;

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

            for (const auto &layer: state.m_layers) {
                if (layer.m_source != ESource::none and
                    (member.m_metadata.m_sources & layer.m_source) == ESource::none) {
                    // Disallowed source: still validate values below so hidden
                    // mistakes surface even though nothing is applied.
                    for (const auto &assignment: layer.m_assignments) {
                        if (assignment.m_name != name) {
                            continue;
                        }
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
                    for (const auto &assignment: layer.m_assignments) {
                        if (assignment.m_name != name) {
                            continue;
                        }
                        state.report(
                            assignment.m_location, std::string{name},
                            "startup-only property cannot be changed at runtime",
                            make_error_code(EConfigErrc::source_disallowed), assignment.m_alias_used);
                        state.retain(make_error_code(EConfigErrc::source_disallowed));
                    }
                    continue;
                }
                for (const auto &assignment: layer.m_assignments) {
                    if (assignment.m_name != name) {
                        continue;
                    }
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
                state.upsertOrigin(std::string{name}, winner_source, winner->m_location);
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
    } // namespace details

    // ------------------------------------------------------------------
    // resolution: transactional. Every supplied value is converted and
    // validated (even shadowed ones); on any failure no Resolved is
    // produced and diagnostics explain every problem.
    // ------------------------------------------------------------------

    template<typename Settings>
    [[nodiscard]] Expected<Resolved<Settings>> resolve(std::span<const Layer> layers, Diagnostics &diagnostics) {
        details::ResolveState state{};
        state.m_layers = layers;
        state.m_diagnostics = std::addressof(diagnostics);

        Resolved<Settings> result{};
        state.m_origins = std::addressof(result.m_origins);

        Array<std::string_view> names{};
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
            result.m_origins.push_back(PropertyOrigin{std::string{member.m_metadata.m_name}, ESource::none, "default"});
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

        Settings scratch = resolved.m_value;
        Array<PropertyOrigin> origins = resolved.m_origins;

        details::ResolveState state{};
        const Layer *single = std::addressof(layer);
        state.m_layers = std::span{single, 1};
        state.m_diagnostics = std::addressof(diagnostics);
        state.m_origins = std::addressof(origins);
        state.m_enforce_runtime_mutability = true;

        Array<std::string_view> names{};
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
        resolved.m_value = std::move(scratch);
        resolved.m_origins = std::move(origins);
        return {};
    }

    // Executable schema check: duplicate names, restricted alphabet,
    // and default values satisfying their own constraints.
    template<typename Settings>
    [[nodiscard]] Expected<void> validateSchema(Diagnostics &diagnostics) {
        const std::size_t before = diagnostics.size();
        Settings probe{};

        Array<std::string_view> names{};
        visit(probe, [&](const auto &member, auto &field) {
            const std::string_view name = member.m_metadata.m_name;
            names.push_back(name);
            if (not isValidCanonicalName(name)) {
                diagnostics.add(
                    "schema", std::string{name}, "canonical name must use [a-z0-9_.-] with no empty segments",
                    make_error_code(EConfigErrc::schema_invalid));
            }
            if (const std::error_code err = details::checkConstraint(member.m_constraint, field)) {
                diagnostics.add(
                    "schema", std::string{name}, configFormat("invalid default value ({})", err.message()),
                    make_error_code(EConfigErrc::schema_invalid));
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
} // namespace pP::config

export template<>
struct std::is_error_code_enum<pP::config::EConfigErrc> : std::true_type { // NOLINT(*-dcl58-cpp)
};
