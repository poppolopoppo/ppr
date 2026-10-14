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
  `m_sensitive` for diagnostic redaction, reference category, units and expected
  numeric editing interval), `PropertyDescriptor`/`describeProperty()` for an
  owned display-neutral projection of legal ranges, enum choices and optionality,
  constraint vocabulary
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
  **Slice 1a: canonical definition** adds `EPresence`, `ENodeKind`, `group()` /
  `collection()`, `KeyedCollection<T>`, root/owner-scoped `ReferenceDomain<Ref>` and
  preorder `describeRecursive()` / `visitRecursive()` callbacks (full path,
  typed member descriptor, type tag/value). Schema traversal expands optional
  groups and collection element schemas (`{key}`); value traversal visits only
  engaged groups and actual keyed entries. `TreeLayer` separates scalar values
  from `GroupPresence`; `resolveRecursive()` validates all supplied layers,
  merges entries by key, enforces supplied required fields and group source
  permissions, records full-path origins and rejects structural scalar/reset
  assignments. Reference validation covers key syntax only; target existence
  and suitability remain the content owner's responsibility. Diagnostics can
  carry related logical paths without changing their retention/count contract.
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
  **Slice 1a: canonical definition** adds opt-in `EDecodeMode::tree`, preserving
  table presence (including empty tables) in `Document::m_groups`, rejecting
  ambiguous literal dotted/empty segments and enforcing a 1 MiB tree source
  limit. `toTreeLayer()` preserves this presence for recursive resolution;
  `encode()` writes explicit groups while leaving scalar-only writer behavior
  intact. Existing scalar/record source patch contracts are unchanged.
  Inline tables flatten into dotted keys; the writer emits canonical TOML.
  Recursive export preserves groups and active textual variant tags, and writes
  ReferenceDomain values as scalar semantic keys rather than `.key` subgroups.
  `patchTree` inserts/removes/replaces a subtree as one reversible, checked patch,
  preserving empty-group presence and unrelated source bytes.
  `patchScalar` derives UTF-8 byte spans from parser source positions and returns
  an owning `ScalarSourcePatch`; `applySourcePatch` checks the expected bytes in
  either direction. Existing scalars keep their value kind; absent scalars may
  be inserted only into existing explicit tables. Untouched bytes, comments,
  BOMs and line endings remain intact. Patches enforce the existing 1 MiB
  content-document limit and reparse the candidate before returning it.
  `insertRecord` appends a new owned record with qualified scalar/nested-table
  keys while retaining existing source bytes and newline style. `removeRecord`
  derives the selected record's table/scalar line spans from parser locations,
  preserves unrelated interleaved tables and standalone comments, and checks
  that all unrelated decoded values survive. Inline record removal fails
  explicitly rather than deleting a shared assignment. Both return reversible
  source patches; authoring history and document ownership remain with callers.

## Key Files

- `Core.Config.cppm/.cpp` — metadata, visitor, conversion, resolution, diagnostics
- `Core.Config.Cli.cppm/.cpp` — CLI tokenization, bindings, help
- `Core.Config.Environment.cppm/.cpp` — env-name mapping, injected lookup
- `Core.Config.File.cppm/.cpp` — document, codec seam, TOML subset

`Diagnostics(max_entries)` bounds retained records while `size()` counts all
reported failures, including omissions. `entries()` is the retained prefix;
`clear()` resets counts and preserves the cap. World generation uses a 64-record
cap at the job/CLI boundary and in schema-resolution temporaries. Source byte
limits still apply independently; this is not an exact diagnostic-byte budget.
