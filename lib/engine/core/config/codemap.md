# lib/engine/core/config/

## Responsibility

Typed, visitor-described configuration foundation (`pP::config`): schema
metadata/descriptors, transactional resolution with provenance, CLI and
environment adapters, and a format-neutral file codec. Knows nothing about
displays, rendering, profiling, or test execution; app/game own their
schemas and application policy on top of this library. See the app display/startup
settings and game invocation/display modules for current consumers.

## Design

- **Core.Config.cppm/.cpp** (`:config`): `EMutability`, `ESource` bitmask
  (`none` = trusted programmatic layer bypassing permission checks),
  `Metadata` (name/description/category/mutability/sources/persistable plus
  `m_sensitive` for diagnostic redaction), constraint vocabulary
  (`AnyDomain`, `Range<T>`, `BooleanDomain`, `EnumDomain<E,N>` +
  `enumDomain()` factory, `StringDomain`, `PathDomain`, `FloatDomain`),
  `Member`/`field()`/`visit()` over `Schema::describe()` tuples, strictly
  typed `SourceValue`/`Assignment`/`Layer`, caller-owned `Diagnostics`,
  `Resolved<T>` (value + per-property origins + owner-assigned revision),
  `resolve()` (validates every supplied value including shadowed ones;
  later layers win; CLI repetition last-wins), `applyRuntime()`
  (scratch-then-commit; rejects startup-only properties), `validateSchema()`,
  and opt-in cross-field validation via
  `static std::error_code validate(const S&, Diagnostics&)`.
  `FloatDomain` supports an exclusive minimum for strict-positivity fields.
  `EConfigErrc` + `pP.config` error category (message + error_condition
  mapping) follow the RawChannel precedent. Text conversion trims ASCII
  whitespace for numerics/booleans/enums; strings retain contents.
  CLI/env boolean token set: 1/true/yes/y/on/enable/enabled (and
  0/false/no/n/off/disable/disabled), case-insensitive.
- **Core.Config.Cli.cppm/.cpp** (`:config.cli`): `OptionDecl` (canonical +
  aliases + negations, switch/required/optional values, repeatable marker,
  `m_preserve_empty_value` for legacy empty-string options),
  `--name value`/`--name=value`, single-dash aliases, `--` termination,
  command selection (conflicting selectors error), reserved `--help`/`-h`,
  generated help (plus `generateHelp(schema, defaults)` with typed
  defaults), `validateCliSchema<Settings>()`, `kExitInvalidUsage = 2`
  (enforced by callers, never by parsing). Empty `--name=` records an
  explicit reset (optional properties only).
- **Core.Config.Environment.cppm/.cpp** (`:config.environment`):
  `environmentName()` (`PPR_` prefix, dots to `__`, dashes to `_`,
  uppercase), injected lookup only (`getenvLookup()` convenience),
  `collectEnvironment<Settings>()` reads known names exclusively,
  optional `Settings::environmentAlias()` compatibility hook,
  `validateEnvironmentSchema<Settings>()` collision check.
- **Core.Config.File.cppm/.cpp** (`:config.file`): `Document` of canonical
  keys + typed values; `decode`/`encode`/`toLayer` seam;
  `documentFrom<Settings>()` returns `Expected<Document>` (values outside
  TOML's signed-64-bit integer range fail instead of truncating). `decode`
  is backed by the pinned private `toml++` dependency (vcpkg with a CPM
  fallback in `cmake/external/tomlplusplus.cmake`): the parsed tree is
  flattened into the document and arrays/dates are rejected afterward.
  Opt-in `EDecodeMode::records` rejects empty tables and literal dotted/empty
  key segments before flattening can discard their structural meaning. The
  default scalar mode preserves existing settings behavior.
  Inline tables flatten into dotted keys; the writer emits canonical TOML.

## Key Files

- `Core.Config.cppm/.cpp` — metadata, visitor, conversion, resolution, diagnostics
- `Core.Config.Cli.cppm/.cpp` — CLI tokenization, bindings, help
- `Core.Config.Environment.cppm/.cpp` — env-name mapping, injected lookup
- `Core.Config.File.cppm/.cpp` — document, codec seam, TOML subset
