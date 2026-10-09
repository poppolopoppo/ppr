# PPR Configuration and Display Control Design

> **Document scope:** Design/proposal record, not a current implementation inventory or repository-wide instruction. Apply its task-specific requirements only when the active task explicitly adopts this design. Verify implementation claims against source/CMake and the current [documentation guide](../README.md); historical paths, counts and phase status may be stale.

> Oracle output (ses_ee4238e69ffela3nSJqhCSK4vI), rescoped (ses_ee3c7cbaeffe6yKGY07Cj7kFJt): simulation/render decoupling deferred to a separate future effort (§8).

## Recommendation

Build a **typed, visitor-described configuration library in `engine.core`**, with independent CLI, environment, and TOML adapters. Resolve configuration at executable boundaries and inject typed values into consumers.

Use that foundation for an **app-owned display controller** that translates settings into CPU pacing, window changes, and acknowledged render-lane surface reconfiguration. Keep the panel game-owned and intent-driven.

Deliver configuration, CLI migration, pacing policy, runtime display controls, and only the small observational cleanup induced by that work. Simulation/render decoupling is deferred to a separate future effort (§8).

This design preserves the important existing foundations:

- `FramePacer` remains a generic minimum-period pacer.
- `Renderer` remains content-free and lane-affine.
- Simulation remains renderer-independent.
- Frame credits continue accounting for render work, not simulation ticks.
- Runtime operations retain explicit error and lifetime contracts.

All new names below are proposed. Existing behavior and locations are based on the supplied evidence, with additional inspection of the repository maps and `tests::parseCli`.

---

## 1. Configuration system

### 1.1 Boundaries and ownership

Separate four concerns:

1. **Schema:** typed fields, defaults, metadata, source permissions, validators.
2. **Sources:** CLI tokens, environment entries, configuration-file bytes.
3. **Resolution:** precedence, conversion, validation, provenance.
4. **Application:** consumers applying a resolved configuration to live resources.

`engine.core` owns the first three. It must know nothing about displays, rendering, profiling fixtures, or test execution.

Application and executable modules own their schemas and application policy:

| Owner | Configuration |
|---|---|
| `engine.app` | Display and application startup settings |
| `game` | Commands, profiling, colony-specific settings |
| `engine.tests` | Test selection, shuffle, loops, child execution |

A configuration property does not acquire a service or change an object while parsing.

### 1.2 Proposed files

| Files | Responsibility |
|---|---|
| `core/config/Core.Config.cppm/.cpp` | Metadata, visitor, typed resolution, diagnostics, provenance |
| `core/config/Core.Config.Cli.cppm/.cpp` | Tokenization, bindings, aliases, command selection, generated help |
| `core/config/Core.Config.Environment.cppm/.cpp` | Environment-name mapping and injected lookup |
| `core/config/Core.Config.File.cppm/.cpp` | Format-neutral document and codec contract; TOML implementation hidden in `.cpp` |
| `core/Core.cppm` | Re-export new partitions |
| `core/CMakeLists.txt` | Module registration and private TOML dependency |

Use partitions `:config`, `:config.cli`, `:config.environment`, and `:config.file`.

Keep templates in interfaces and non-template implementations in matching `.cpp` files. Use `pP::Array` and existing string/container conventions for owned runtime data; `std::tuple`, `std::array`, and spans suit immutable schema descriptions.

Do not introduce a general reflection framework, plugin registry, or mandatory configuration service.

---

### 1.3 Authoring: ordinary members, explicit metadata, one visitor

C++23 cannot enumerate arbitrary struct members or inspect custom attributes. The honest implementation is an ordinary struct plus a small explicit descriptor list.

Descriptors **decorate members through typed member pointers**. There is no custom preprocessor, source generator, field-name parsing, or compiler extension.

Illustrative core vocabulary:

```cpp
export namespace pP::config {
    enum class EMutability {
        startup,
        runtime
    };

    template<typename T>
    struct Range {
        T m_min;
        T m_max;
    };

    struct Metadata {
        std::string_view m_name;
        std::string_view m_description;
        std::string_view m_category;
        EMutability m_mutability;
        ESourceMask m_sources;
        bool m_persistable;
    };

    template<typename Owner, typename T, typename Constraint>
    struct Member {
        T Owner::*m_member;
        Metadata m_metadata;
        Constraint m_constraint;
    };

    template<typename Owner, typename T, typename Constraint>
    [[nodiscard]] constexpr auto field(
        T Owner::*member,
        Metadata metadata,
        Constraint constraint) {
        return Member<Owner, T, Constraint>{
            member, metadata, constraint
        };
    }

    template<typename Settings, typename Visitor>
    constexpr void visit(Settings &settings, Visitor &&visitor) {
        using Schema = std::remove_cvref_t<Settings>;
        const auto members = Schema::describe();

        std::apply(
            [&](const auto &...member) {
                (visitor(member, settings.*(member.m_member)), ...);
            },
            members);
    }
}
```

Application-side sketch:

```cpp
export namespace pP {
    enum class EFramePacing {
        uncapped,
        follow_vsync,
        paced
    };

    struct DisplaySettings {
        EFramePacing m_pacing{EFramePacing::follow_vsync};
        u32 m_cap_hz{60};
        bool m_paced_vsync{true};
        bool m_throttle_unfocused{true};

        [[nodiscard]] static constexpr auto describe() {
            using namespace config;

            return std::tuple{
                field(
                    &DisplaySettings::m_pacing,
                    Metadata{
                        "display.pacing",
                        "Foreground frame pacing mode",
                        "Display",
                        EMutability::runtime,
                        all_sources,
                        true
                    },
                    pacing_domain),
                field(
                    &DisplaySettings::m_cap_hz,
                    Metadata{
                        "display.cap_hz",
                        "CPU frame cap used in paced mode",
                        "Display",
                        EMutability::runtime,
                        all_sources,
                        true
                    },
                    Range<u32>{1, 1000}),
                field(
                    &DisplaySettings::m_paced_vsync,
                    Metadata{
                        "display.paced_vsync",
                        "Enable presentation synchronization in paced mode",
                        "Display",
                        EMutability::runtime,
                        all_sources,
                        true
                    },
                    boolean_domain),
                field(
                    &DisplaySettings::m_throttle_unfocused,
                    Metadata{
                        "display.throttle_unfocused",
                        "Limit frame production while unfocused",
                        "Display",
                        EMutability::runtime,
                        all_sources,
                        true
                    },
                    boolean_domain)
            };
        }
    };
}
```

`pacing_domain` is an immutable array of typed enum/name pairs, not inferred enum spelling.

This modest repetition is deliberate: adding an ordinary property requires its member/default and one descriptor entry. Existing consumers still receive plain values, not property wrappers.

The same visitor powers:

- Parsing and validation.
- Generated help.
- Configuration-file serialization.
- Provenance inspection.
- Runtime-mutability checks.
- UI labels, ranges, and enum choices.

Complex display widgets remain handwritten; metadata does not become a generic UI framework.

Schema validation detects duplicate canonical names, alias collisions, environment-name collisions, and invalid defaults. Use compile-time checks where practical and executable schema tests otherwise.

---

### 1.4 Typed conversion and validation

Every source produces **owned assignments with origins**, not mutations to live settings.

An assignment contains:

- Canonical property name.
- Source scalar: string, boolean, signed/unsigned integer, or floating value.
- Source location.
- Ordering within its layer.

Arrays are introduced only for actual list-valued settings; repeated CLI options can have explicit append semantics without making every property a list.

Conversion is directed by the destination member type:

| Destination | Rules |
|---|---|
| Integer | Full consumption, overflow checks, no silent narrowing |
| Floating point | Full consumption; reject non-finite values unless explicitly permitted |
| Boolean | File booleans remain typed; CLI/env accept a documented token set |
| Enum | Named domain entries; no unexplained ordinal integers |
| String/path | Owned storage; property-specific length/path validation |
| Optional | Absence differs from a supplied value; reset is an explicit operation |

A file integer must not become an enum ordinal accidentally. A file string containing `"60"` must not silently satisfy an integer property.

For CLI and environment text, trimming rules are documented per type; string values retain their contents. Paths are resolved against an explicit base: a file-relative path against that file's directory, CLI paths against the supplied working directory.

Resolution is transactional:

```cpp
template<typename Settings>
[[nodiscard]] std::expected<Resolved<Settings>, std::error_code> resolve(
    std::span<const Layer> layers,
    Diagnostics &diagnostics);
```

`Resolved<T>` owns:

- The typed value.
- Winning origin per property.
- A revision assigned by its owner when published.

Use the project's `Expected<T>` spelling where appropriate; sketches spell out the standard type to make the error contract explicit.

Validation has two levels:

1. **Field validation:** type, domain, range.
2. **Schema validation:** relationships among fields.

Platform capabilities are a third, later check performed by the app controller—not by core configuration.

Invalid supplied values produce diagnostics even if a higher-precedence source would shadow them. Hidden configuration mistakes should not become latent failures.

### Diagnostics

Return `std::error_code`; write contextual details into a caller-owned diagnostic collection:

```text
settings.toml:12: display.cap_hz:
expected integer in [1, 1000], received 0

argv[4] (--profile-skip):
missing required value
```

Include the canonical name, alias used, source location, expected domain, and error code. Redact values for properties marked sensitive.

The executable prints diagnostics and returns exit code `2` for invalid invocation/configuration. Parsing must not print, terminate the process, or initialize application services.

### 1.5 Precedence and source combinations

Core accepts an **ordered span of layers**. It does not impose a process-wide source order or automatically read anything.

Recommended game order, lowest to highest:

1. Struct defaults.
2. Named command/profile defaults.
3. Explicit base files, in argument order.
4. Optional generated user-settings overlay.
5. Environment.
6. CLI.
7. Accepted runtime overrides.

Any subset is valid. Tests normally use defaults plus CLI only.

Within a layer:

- CLI scalar repetition: last occurrence wins.
- File duplicate keys: error.
- Append behavior: only for properties explicitly declared repeatable.
- Aliases resolve to the same canonical property.
- Cross-field validation runs after merging.

Runtime overrides intentionally outrank CLI for runtime-editable settings. The panel displays their provenance; command-line settings are not an invisible permanent lock.

"Reset override" removes the runtime assignment and exposes the lower-precedence value. It is different from "set the factory default."

### Environment names

Derive names from canonical keys:

```text
display.cap_hz → PPR_DISPLAY__CAP_HZ
```

Rules:

- `PPR_` process prefix.
- Dots become double underscores.
- Remaining segments become uppercase.
- Canonical segments use a restricted identifier alphabet.
- Explicit alternate names are allowed for compatibility.

Read only known schema names through an injected environment lookup. Do not scan and reject unrelated process variables. Disallowed-source assignments produce an error when explicitly supplied.

Command selectors, `--child-run`, and intentional crash diagnostics are CLI-only. They should not be activated by a stray environment variable or settings file.

---

## 2. Configuration-file format decision

### Choose TOML

| Format | Hand editing | Read/dependency cost | Machine writing |
|---|---|---|---|
| Strict JSON | Familiar, but no comments | Existing `simdjson`; excellent reader | Requires a writer; no comments to preserve |
| YAML | Comments and expressive syntax | New dependency; substantially broader semantics than needed | Comment/style preservation is not automatic |
| TOML | Comments, sections, explicit scalar types | New dependency; startup-scale cost is acceptable | Straightforward canonical output |
| Minimal `key=value` | Initially simple | No dependency, but requires a parser contract | Escaping, lists, duplicates, sections, and comments become a homegrown format |

**Decision:** use TOML with a pinned `toml++` dependency, privately behind the file adapter. Confirm the package/target wiring through the existing vcpkg and CPM dependency paths during implementation. No other new dependency is authorized.

`simdjson` already being available is a meaningful advantage for JSON, but it does not provide comments or solve machine writing. Configuration files are small and read infrequently; parser throughput is not the deciding constraint.

Do not implement "JSON with comments" by stripping text: quoted strings and diagnostics make that a language implementation.

Example:

```toml
[display]
# Foreground pacing; unfocused throttling is independent.
pacing = "follow-vsync"
cap_hz = 144
paced_vsync = true
throttle_unfocused = true
```

### One format seam

Expose one codec boundary:

```text
decode(file bytes, origin) → owned configuration document
encode(configuration document) → owned bytes
```

Both return expected results with contextual diagnostics. The document contains canonical keys and typed values; no TOML types escape the implementation.

File I/O is separate and injected. This allows a future codec replacement without changing schemas, resolution, CLI, or consumers. Do not ship multiple codecs initially.

### Persistence policy

Do not rewrite hand-edited base files.

The panel's explicit **Save** action writes a separate, generated user-settings overlay:

- Only persistable display properties selected for saving.
- Only acknowledged, successfully applied settings.
- Never profiling, validation, child-run, or crash switches.
- Preserve unrelated overlay entries.
- Use a same-directory temporary file and atomic replacement where the platform supports it.
- Report write failure; do not claim settings were saved.
- Document that environment and CLI still override this file on the next launch.

A generated overlay avoids promising comment-preserving editing that the serializer does not provide.

The executable chooses the user-settings path through an explicit platform boundary. No implicit current-directory search. Test executables do not auto-load this overlay.

---

## 3. Resolved state and runtime application

Startup resolution occurs before constructing the application, so settings such as GPU validation reach initialization in time.

The executable owns its complete resolved startup snapshot. Consumers receive the smallest useful typed subconfiguration by value or const reference.

For display changes, `ApplicationEditor` owns a `DisplayController` containing:

- Committed resolved display settings.
- One pending candidate transaction.
- Applied window/surface observations.
- Provenance and revision.
- Optional persistence dependency.

There is no universal mutable configuration singleton.

An existing service boundary may expose a read-only display view if necessary, but resolve it at the top level and inject it into consumers. Do not make lower-level code query `ServicesStore` for arbitrary property names.

### Notifications

No generic callback bus initially.

- Main-thread consumers read the controller's committed revision at a defined update boundary.
- Render-lane consumers receive owned typed messages.
- Completion messages contain transaction ID, generation, result, and applied state.
- The panel receives a copied view.

That is sufficient for the concrete runtime consumers and avoids subscription/lifetime machinery without a demonstrated need.

---

## 4. Frame pacing and presentation

### 4.1 Schema and exact mapping

Recommended initial fields:

| Property | Meaning |
|---|---|
| `display.pacing` | `uncapped`, `follow-vsync`, `paced` |
| `display.cap_hz` | Integer `[1,1000]`, used only when paced |
| `display.paced_vsync` | Whether paced mode also synchronizes presentation |
| `display.throttle_unfocused` | Existing focus-throttle policy |
| `display.window_mode` | Initially supported windowed/fullscreen modes |
| `display.monitor` | Explicit monitor preference or automatic selection |
| `display.width`, `display.height` | Window/exclusive-mode dimensions |
| `display.refresh_hz` | Requested exclusive-mode refresh, where supported |

Do not initially expose surface image count or an arbitrary present-mode enumeration: the accepted backend evidence establishes a vsync boolean, not those capabilities.

Foreground mapping:

| Mode | `Application` target | Renderer vsync |
|---|---|---|
| Uncapped | `nullopt` | `false` |
| Follow vsync | `nullopt` | `true` |
| Paced | `1 / cap_hz` | `paced_vsync` |

Use `setTargetFrameDuration(std::nullopt)` or the supported equivalent; do not encode "uncapped" as a zero rate in the new schema.

"Uncapped" means **no intentional CPU cap and no requested vsync**. GPU load, compositor behavior, and the two-credit admission bound can still limit throughput.

"Follow vsync" means **no CPU cap**. It does not promise an exact measured refresh rate or frame/display phase lock.

In paced mode, enabling vsync can lower achieved throughput below the requested cap. Show that interaction rather than silently rewriting the requested rate.

### Defaults

The approved interactive default is `follow-vsync`.

This intentionally replaces the current initialization policy in `App.Application.Editor.cpp:263–274`, which installs a CPU cap from monitor refresh while renderer vsync is also enabled. Verify this approved policy change in the Phase 3 acceptance gate; no further default-policy approval is needed.

Keep the existing pacing setters usable by tests and non-editor applications.

---

### 4.2 Focus/background throttling

Background throttling is an overlay, not another pacing mode.

When `display.throttle_unfocused` is enabled and the existing background-priority/focus policy activates throttling:

```text
effective period = max(foreground period or zero, 200 ms)
```

Otherwise use the foreground target.

This preserves a slower explicit cap instead of speeding a 1 FPS configuration up to 5 FPS. It is a small, intentional change from the existing unconditional background target and needs a regression test.

Vsync state does not toggle on focus changes. Foreground mode is remembered and restored when focus returns.

`--no-focus-throttle` maps to:

```text
display.throttle_unfocused = false
```

The existing wakeable target-resampling behavior in `advanceClock_` remains valuable. Keep unclamped elapsed time; do not fix scheduling policy by silently clamping `dt`.

### 4.3 What stays in core

`FramePacer` retains:

- Injected time/sleep seams.
- Minimum-period policy.
- No catch-up bursts.
- Wakeable retargeting.
- No display knowledge.
- No phase-locked scheduling.

App policy determines the effective target. Monitor enumeration and backend presentation behavior belong above core.

Monitor refresh is used for display information, exclusive-mode validation, and a panel action such as **Use current refresh as cap**. Follow-vsync does not compute a CPU period from it.

Unknown refresh is displayed as unknown, not guessed. Monitor preferences must be revalidated after enumeration; never persist a raw monitor handle.

---

## 5. Runtime surface reconfiguration

Changing `Renderer::m_enable_vsync` alone cannot implement runtime switching: it currently takes effect only in the resize path.

### API direction

Introduce a lane-affine renderer operation along these lines:

```text
reconfigureWindowSurface(window identity, generation, settings)
    → expected<AppliedSurfaceSettings, error_code>
```

Make mutable presentation configuration private. Supply initial settings explicitly and use the same configuration-building helper for creation, resize, and runtime reconfiguration.

Extend `RenderControl::Kind` with a reconfigure request and an acknowledgement path. Messages own their payloads; they must not borrow panel state or scratch memory.

### Serialized transaction protocol

One outstanding display transaction is sufficient initially:

1. Validate the candidate configuration and current capabilities.
2. Stop new render admission for the affected surface.
3. Retire or explicitly cancel old work through existing settlement paths.
4. For monitor/window changes, perform the window-service operation on its required thread and collect the resulting geometry.
5. Apply the combined surface configuration on the render lane, at a safe resource boundary.
6. Return an acknowledgement containing transaction ID, surface generation, applied settings, and error.
7. Commit CPU pacing and the configuration revision at the application boundary; resume admission with the new generation.

Important contracts:

- Control requests do **not** consume frame credits.
- A barrier must allow the lane to drain existing work; it cannot wait while preventing that work from running.
- Display-control completion, failure, and exit handling must remain serviceable through existing main-side pump/completion seams while affected-surface admission is suspended; none may require another admitted frame or client update.
- Resize notifications and explicit reconfiguration must converge on one surface-update path.
- Stale requests/acks cannot configure a recreated window.
- Old packets rejected by generation still settle their credit and staging outcomes exactly once.
- GPU submission acceptance and presentation success remain distinct.
- A successful submit is never "undone" because presentation or reconfiguration subsequently fails.

Channel-full admission returns an error or leaves a visibly unsubmitted candidate for bounded retry. Do not block the UI thread indefinitely or silently drop the request.

Existing release/acquire publication through channels should be preserved rather than supplemented with unsynchronized shared fields.

### Failure and shutdown

If reconfiguration fails:

- Preserve the previous configuration only if the backend can prove it remains usable.
- Otherwise attempt explicit restoration.
- If restoration also fails, report actual degraded/unavailable state and follow existing recovery/terminal policy.
- Never label a candidate "applied" merely because it was queued.

Window changes and GPU surface changes are not a truly atomic OS transaction. The controller coordinates them and reports rollback failure honestly.

Shutdown:

1. Disable panel submissions and new transactions.
2. Cancel unsubmitted candidates.
3. Wake/drain accepted control work and settle acknowledgements.
4. Join the lane before releasing request/state storage.
5. Preserve the first error while attempting independent cleanup.

A CPU-blocking `present()` may delay a control acknowledgement. Do not claim the control channel can interrupt it; measure this on supported backends.

---

### 5.1 Telemetry

Report configuration/controller state:

- Requested and applied pacing mode/vsync.
- Foreground and effective CPU targets.
- Unfocused-throttle setting and active reason.
- Known monitor refresh.
- Pending display transaction and last failure.

Retain or add cheap cadence observations only at existing hooks: produced/admitted render packets, successful GPU submissions, successful present calls, and pacer/credit/present-call wait durations. Use existing timing/publication paths and explicit clock seams. Expose simulation tick counts only if already available; do not add simulation instrumentation or dependencies solely for this panel.

Define each counter's event and each duration's boundaries. A successful present call is not proof of physical display delivery. Mark unavailable measurements as unavailable; do not infer them from one "FPS" counter.

These observations must not introduce new scheduling hooks, channels, synchronization protocols, packet retention, or ownership/lifetime changes. Defer any metric that cannot meet that constraint.

---

## 6. Display settings panel

### Location

Extend the existing game-owned panel/intent flow:

- `game/colony/ColonyPanel.cppm/.cpp`
- `ColonyUiState`
- `EColonyIntent`
- `TurboLarbin::drawPanel_`
- Intent dispatch and `postNotice_` in `game/main.cpp`

Use the help-window precedent for a dedicated Display window. The controller belongs in `engine.app`; ImGui state remains in game/client code.

### Data flow

```text
DisplayController view
    → Colony panel draft
    → typed Apply/Save/Reset intent
    → TurboLarbin dispatch
    → DisplayController transaction
    → application/window/render-lane operations
    → acknowledgement
    → updated view / notice
```

The panel must not mutate `Renderer`, call lane-affine functions, or resolve arbitrary services.

### Initial controls

- Pacing mode.
- Cap input, enabled only in paced mode.
- Paced-mode vsync checkbox.
- Unfocused throttling.
- Monitor and supported window/fullscreen choices.
- Resolution/refresh choices appropriate to the selected mode.
- Apply, reset runtime overrides, and explicit Save.
- Requested/applied values and pending/error status.

Use metadata for labels/ranges/domains and a capability view for available monitors/modes.

Do not advertise borderless fullscreen until its exact window-service implementation is verified.

For potentially disruptive monitor/mode changes, provide **Keep / Revert** with an injected-time countdown. Start the countdown after successful application; revert through the same transaction path. A failed revert is an operational error, not a dismissed notification.

---

## 7. CLI and test-harness migration

### 7.1 One parser, executable-owned schemas

The CLI adapter supports:

- Canonical names and legacy aliases.
- `--name value` and `--name=value` where supported.
- Presence switches and explicit boolean forms.
- Negated aliases.
- Required and explicitly optional values.
- Repeated options with declared semantics.
- `--` termination.
- Command selection.
- Generated help.

CLI metadata references canonical properties rather than owning a second set of validators/defaults.

Not every CLI token is persistent configuration: help, command selection, and child execution belong to an invocation schema using the same visitor machinery.

### Main migration map

| Existing syntax | New destination |
|---|---|
| `--smoke` | Command selector |
| `--load100` | Command selector |
| `--time-trials` | Command selector |
| `--profile100` | Profiling preset selector |
| `--profile-csv N` / `=N` | Typed profiling field |
| `--profile-label X` / `=X` | Profiling label |
| `--profile-skip N` / `=N` | Profiling skip field |
| `--profile-edits`, `--profile-pan` | Profiling fields |
| `--no-focus-throttle` | False assignment to `display.throttle_unfocused` |
| `--auto-exit`, `--exit-after` | Aliases for one duration property |
| `--core-validation`, `--gpu-validation` | Startup validation settings |
| `--diag-crash=...` | CLI-only diagnostic action |

Add metadata-declared `--config PATH` and `--help`/`-h`.

Replace the first-argument special dispatch and later if/else parser with one invocation-resolution function before constructing the app. Preserve existing first-position command usage. Allowing selectors elsewhere is a compatible extension; conflicting selectors are errors.

Explicitly audit options previously ignored by special runners. Rejecting ignored garbage is a diagnostic improvement, but must not accidentally reject options used by existing automation.

### Profiling policy

Replace the side-effect `profiling` boolean with a named profiling-default layer:

- The same existing profiling triggers select it.
- It supplies `throttle_unfocused = false`.
- An explicit setting can override it.
- Help documents the policy.

Preserve legacy profiling field meanings, defaults, and accepted value forms; freeze them with compatibility tests before deleting the parser.

### Colony smoke

Change the implementation entry point to receive typed startup/smoke options. Remove its second `--gpu-validation` parse.

If an argv-taking entry point must remain temporarily, make it a thin adapter to the shared game invocation parser. Do not retain two sources of CLI semantics.

### Tests

Refactor `lib/engine/tests/shared/Tests.cpp`:

```text
parse invocation → resolve TestSettings → construct Context → runSuite
```

Keep:

- `--run-test`
- `--child-run`
- `--shuffle [seed]`
- `--no-shuffle`
- `--loop N`
- `--help`, `-h`

Important details from the current implementation:

- Shuffle defaults to enabled with an automatically generated seed.
- Later loop iterations currently generate fresh seeds.
- Help is currently represented by zero loops.
- Unknown/malformed inputs are not consistently rejected.

Recommended migration:

- Represent help explicitly, rather than overloading `loops == 0`.
- Initially preserve `--loop 0` as the existing successful no-op.
- Preserve existing later-loop reseeding behavior in the compatibility phase; document it.
- Inject seed generation at the runner boundary. Parsing itself must not use `random_device`.
- Reject malformed numeric tokens completely, including trailing garbage.
- Preserve explicit-seed first-iteration behavior and option-order precedence.
- Help precedence compatibility decision: today `--help` prints help and, when combined with `--loop N`, the suite still runs (`--help --loop 1` runs, `--loop 1 --help` does not). The explicit-help migration must pick one behavior: preserve the quirks, or make help uniformly suppress execution and list that as an approved compatibility exception in the Phase 2 gate (not as malformed-input tightening).
- Return expected parse results and clean diagnostics.

The four tier mains become thin calls to a shared entry helper that handles parse/help/error/run. This is an appropriate abstraction because it removes four identical lifecycle adapters.

Keep test configuration hermetic by default. File and environment support remains available explicitly, not automatically enabled. Child invocations must carry the relevant resolved execution options without accidentally reading unrelated user configuration.

---

## 8. Deferred to a future effort

Simulation/render decoupling is a separate future effort, not a phase or prerequisite of this plan. This includes independent update/wake scheduling, input cadence changes, packet/coherence and grid-acknowledgement redesign, interpolation and snapshot lifetime changes, simulation threading, runtime simulation-Hz changes, prediction/extrapolation, rollback networking, and broader simulation rewrites.

This plan preserves existing update-loop phase order, render-admission coupling, input consumption, simulation timestep/overrun policy, packet coherence, and frame/staging acknowledgement semantics. Display pacing can still affect simulation/input service cadence; these controls do not establish independent simulation execution.

Only observational cleanup is retained: documenting existing identities/contracts, cheap telemetry at existing observation points, and characterization tests using existing seams. No scheduling, ownership, or lifetime refactor is justified by that cleanup. Future work requires its own design and approval.

---

## 9. Phased implementation and gates

### Phase 0 — Freeze contracts and backend facts

Before replacement:

- Capture CLI compatibility cases and defaults.
- Confirm window-service mode capabilities.
- Confirm pinned Slang-RHI surface reconfiguration/resource-idle requirements and failure behavior.
- Document current simulation tick, render-packet/frame, surface-generation, and staging acknowledgement identities, including existing equality/coherence checks and credit/staging settlement invariants. Describe current contracts; do not redesign them.
- Inventory existing cadence observations and define the cheap subset retained under §5.1.
- Add focused characterization cases through existing test seams for current render-admission gating, update ordering, and packet/ack settlement where directly touched by this refactor. Do not introduce a scheduler or test-harness refactor to obtain coverage.

**Gate:** no unresolved backend assumption about safely changing vsync; touched contracts and baseline behavior are recorded. Characterization asserts current behavior, not simulation/input progress without render credits. Backend evidence gates do not delay schema work; unavailable optional telemetry does not block delivery.

### Phase 1 — Core configuration foundation

Implement descriptors, visitor, conversion, resolution, provenance, CLI/environment adapters, and TOML codec.

Add the pinned `toml++` dependency through the existing build mechanisms; keep library types private.

**Tests:**

- New `lib/engine/tests/core/Core.Config.Tests.cpp`.
- New `Core.Config.Cli.Tests.cpp`.
- New `Core.Config.File.Tests.cpp`.
- Registration in `Core.Tests.cpp` and core-test CMake.

Cover defaults, source subsets, precedence, aliases, duplicate keys, numeric overflow, enum domains, cross-field validation, source restrictions, provenance, TOML comments, writer round-trips, and failure atomicity.

**Gate:** GLFW-free tests; no schema or parser dependency on app/game/tests.

### Phase 2 — CLI migration

Add proposed:

- `game/Game.Config.cppm/.cpp`.
- App startup/display schema files.
- Test invocation schema in `engine.tests`.

Migrate:

1. Shared test parser and four tier mains.
2. `game/main.cpp`.
3. `ColonySmoke.cppm/.cpp`.

Add a test-shared CLI test group hosted by the core executable. Put game schema tests in the game build area; do not make engine test tiers depend upward on game modules.

**Gate:** legacy invocation corpus produces equivalent typed results and exit behavior, except explicitly approved malformed-input improvements. Help works without GPU/window initialization or configuration-file availability.

### Phase 3 — Pacing policy without runtime surface switching

Implement a pure app pacing-policy helper and initial renderer presentation settings.

Update:

- `App.Application.cppm/.cpp`.
- `App.Application.Editor.cppm/.cpp`.
- New `App.Display.Settings.cppm/.cpp`.
- New `App.Display.cppm/.cpp`.

Remove monitor-derived CPU cap assignment from editor initialization.

Wire `display.throttle_unfocused` to the existing background-throttle activation policy, applying the overlay in §4.2 without changing focus detection or input routing.

Expose foreground/effective targets and throttle state through the controller view. Retain cheap existing pacer/credit-wait observations under §5.1; do not change the pacer or update-loop structure to obtain telemetry.

Extend:

- `App.Frame.Tests.cpp`.
- New `App.Display.Tests.cpp`.
- Existing `Core.Timer.Pacer.Tests.cpp` only if the pacer itself changes; it should not need to.

**Gate:** exact mode mapping; approved follow-vsync default with no monitor-derived CPU cap; configurable unfocused overlay, including preservation of a slower explicit cap and restoration on refocus; unclamped elapsed time. Existing update phase order, render-admission coupling, input consumption, and simulation timestep/overrun policy remain unchanged. No phase-locked scheduling.

### Phase 4 — Runtime render-lane transactions

Update:

- `App.Renderer.cppm/.cpp`.
- `App.Frame.RenderLane.cppm/.cpp`.
- Associated control/completion definitions.
- Application/editor admission and acknowledgement integration.

Test:

- Vsync-only changes without resizing.
- Resize/reconfigure overlap.
- Stale generation and destroyed window.
- Full/closed channels.
- Two outstanding frame credits.
- Failed reconfigure and failed rollback.
- Shutdown while pending.
- Acknowledgement and admission resumption while affected-surface admission is suspended (main-side completion processing must not require another admitted frame).
- Exactly-once credit and staging settlement.

Use deterministic lane/backend seams for protocol tests; add real-backend checks for present behavior.

Retain or add only the cheap render observations permitted by §5.1 at existing production/admission/submission/present-call points. Extend settlement characterization only where the display transaction touches existing paths; do not split packet/tick identities, replace coherence checks, or version grid acknowledgements as cleanup.

**Gate:** no UI-thread renderer mutation, no leaked credits, and no claim of success before display-control acknowledgement. Existing frame/staging acknowledgement semantics and exactly-once settlement remain intact; new display-control acknowledgements are distinct. No simulation scheduling, input semantics, or snapshot lifetime changes.

### Phase 5 — Panel and persistence

Extend `ColonyPanel` and `TurboLarbin` intent dispatch. Add draft/applied state, capability choices, notices, keep/revert, and generated-overlay saving.

Show the available §5.1 state/observations from copied controller views, with unavailable measurements labeled explicitly. Do not add instrumentation infrastructure to fill missing panel fields.

**Gate:** panel changes are intents; invalid/disallowed fields are rejected; persistence failures are visible; saved values reflect successful application. Observations distinguish requested policy from measured outcomes and do not imply independent simulation cadence.

### Repository-wide completion gates

For every phase:

- New sources and CMake registration land together.
- Update relevant codemaps and umbrella exports.
- Preserve existing test paths/shard coverage.
- Orchestrator runs the selected build/test gates.
- Run canonical formatting and touched-file semantic-style checks during implementation.
- Use `PPR_UNIT_TEST` and `PPR_TEST_ASSERT`; no new macros.
- Role boundary: the orchestrator delegates, the oracle designs and validates, the fixer implements, and the explorer surveys. Fixer lanes never run the test/validation matrix — the orchestrator consolidates writer lanes first, then dispatches a dedicated validation lane for the matrix.
- Remain within C++23; no C++26 facilities and no new dependency beyond `toml++`.
- Cleanup remains observational and local. Display transactions may add their required control/acknowledgement lifecycle, but may not become a vehicle for simulation/render decoupling or unrelated scheduling/ownership changes.

No builds or validation runs were performed for this design.

---

## 10. Decisions needing user confirmation

TOML + `toml++`, the interactive follow-vsync default, orthogonal unfocused throttling, and deferral of simulation/render decoupling are recorded below, not pending decisions.

| Decision | Recommendation |
|---|---|
| Paced-mode vsync | Separate checkbox, default enabled; uncapped always requests it off |
| Runtime versus CLI precedence | Runtime edits win for runtime-editable properties |
| Persistence | Explicit Save to a generated overlay; never rewrite hand-edited files |
| Dangerous display changes | Keep/Revert confirmation with injected-time timeout |
| Test configuration discovery | None by default; explicit files/environment only |
| Legacy malformed CLI behavior | Preserve valid invocations, reject malformed/unknown options clearly |

The "Simulation work" and "Runtime simulation Hz" rows are removed, not converted into implementation commitments.

## Explicitly out of scope

Simulation/render scheduling, input/packet/acknowledgement redesign, interpolation, and related simulation work are deferred explicitly in §8. They are neither implementation phases nor completion gates for this plan.

Other exclusions:

- C++ reflection emulation beyond the small visitor schema.
- Generic runtime console/CVar registry.
- File watching and automatic hot reload.
- YAML/JSON/TOML format multiplicity.
- Comment-preserving edits to user-authored files.
- HDR, VRR control, mailbox selection, and adjustable surface image count.
- Phase-locked frame pacing.

---

## Decisions recorded (user, 2026-10-08)

1. **Config file format:** TOML + `toml++` (private pinned dependency; codec seam as designed).
2. **Interactive default pacing:** `follow-vsync`. The unfocused resource-reduction behavior (existing background throttle, currently hardcoded at 5 fps and not wired to configuration) must become the orthogonal `display.throttle_unfocused` option as designed — semantics: overlay on top of the foreground mode, not a fourth pacing mode.
3. **Scope:** simulation/render decoupling is deferred to a separate future effort (§8). Retain only cheap, local cleanup induced by configuration/CLI/pacing/display work; no update-loop phase reordering, input or existing frame/staging acknowledgement semantic changes, interpolation, or cleanup-driven scheduling/lifetime refactors.
4. **Implementation:** All phases authorized by the user on 2026-10-08, conditional on each phase's gate being validated before the next phase starts. The orchestrator consults the oracle (ora-2 session) in the user's stead when in doubt.
5. **Validation (ora-2, 2026-10-08):** final narrow validation verdict = APPROVE WITH FIXES; both fixes applied — (§5/Phase 4) main-side display-control completion must remain serviceable while affected-surface admission is suspended, plus a Phase 4 liveness test; (§7) explicit help-precedence compatibility decision for the test CLI. Phase 1 is technically standalone and authorizable independently; implementation remains on hold until the user lifts it.
6. **Phase 1 validation (2026-10-08):** PASSED. Build clean (toml++ compiled), format gate passed, semantic style gate passed (0 blocking), 689/689 core tests green (26 config + 15 CLI + 13 file + 635 existing). Code review findings (9 MAJOR + 4 MINOR) all addressed. CLion inspection unavailable in this environment. Phase 2 authorized to proceed.
7. **Phase 2 validation (2026-10-08):** PASSED. Build clean (app.game + all 5 test targets), format gate passed (32 files), semantic style gate passed (0 blocking), 701/701 core tests + 16/16 game.tests green. Legacy invocation corpus verified via --help output and 15 game-owned compatibility tests. Code review findings (5 MAJOR + 1 MINOR) all addressed. Phase 3 authorized to proceed.

---

## Implementation tracking (2026-10-09)

| Phase | Status | Evidence |
|---|---|---|
| 0 — Freeze contracts | DONE | Backend facts captured; characterization tests added |
| 1 — Core config foundation | ✅ PASSED | 689/689 core tests; review fixes applied; gate green |
| 2 — CLI migration | ✅ PASSED | 701/701 core + 16/16 game.tests; review fixes applied; gate green |
| 3 — Pacing policy | ✅ PASSED | ora-1 findings closed; one complete green core loop 704/704; validation qualification below |
| 4 — Runtime surface reconfigure | ✅ PASSED | ora-3 local completion PASS; 9/9 one-loop matrix green (377s, -j4); no blocking findings |
| 5 — Panel + persistence | ✅ PASSED (follow-ups tracked) | Ora-17 final PASS-WITH-FOLLOWUP; 10/10 matrix (393s); 3 re-review MAJORs + all follow-ups closed; detail below |

### Phase 3 detail
- **fix-12**: `engine.app:display` partition (DisplayController, pure helpers, overlay); monitor-cap removed from Editor::initialize; `setDisplaySettings` wired into Application/Editor.
- **ora-5 review**: MAJOR — game/main.cpp never called `setDisplaySettings` (legacy bridge only applied throttle_unfocused); MINOR — telemetry disagrees with legacy setters; MINOR → blur→enable→refocus state loss.
- **fix-13**: all three fixed; `displayView()` now reads live `m_target_frame_time`; `backgroundThrottleWanted_()` returns raw focus loss.
- **ora-1 finding closure / ora-2 handoff**: ora-1 findings closed; Phase 3 PASS; Phase 4 authorized to proceed.
- **Validation qualification**: results establish one complete green core loop (704/704), not a green 9/9 run. The core `TIMEOUT 120` kill was a test-budget defect, closed by `TIMEOUT 300` plus the one-loop development gate. Phase 4 requires the full nine-entry phase-completion matrix including asset, and three-loop affected suites for its high-risk merge gate.

### Phase 4 detail
- **fix-6 (lane A)**: lane-affine `reconfigureWindowSurface` + owned control/ack protocol in `App.Renderer`/`App.Frame.RenderLane`; generation guards; explicit restoration; 5 deterministic protocol tests; real-backend `editor_scene_flow` extension.
- **fix-7 (lane B)**: explicit startup `SurfaceSettings`; executor binding; `m_surface_result` dispatch before settlement incl. suspended-admission pumps; commit-on-ack pacing + generation mirror; degraded reporting; 7 `app/display` transaction tests; ack-while-suspended liveness test.
- **fix-8 (contract)**: error-reporting `setWindowMonitor` (interface + GLFW impl + fake + codemaps); new run_gate monitor test wired into the app_a1 shard (41/41).
- **Validation**: C4459 `app`-shadowing fixed (`test_app`); first 9/9 voided (stale binaries, unlinked build — runner now fails loudly on any build FAILED line); real 9/9 one-loop matrix green in 377s under approved `-j4`.
- **Open follow-ups (not Phase 4 blockers)**: `Asset.Render.Gates.Tests.cpp` 42GB single-TU compile triage; three-loop affected-suite gate at high-risk merge; platform/build matrix at pre-merge CI.

### Phase 5 detail
- **des-1 (panel baseline)**: `game.display` intent model (draft, runtime layers, overlay merge/encode, Keep/Revert trial), Display window with Requested vs Device-report sections, 7 intents, overlay precedence base < overlay < env < CLI, 12 `game_display` tests. Copy reviewed by orchestrator (3 strings).
- **Engine follow-up (user-mandated, fix-10)**: 5 new schema fields (window_mode/monitor/width/height/refresh_hz, 9 total, visitor-derived CLI), capability-injected validation, native-geometry apply/rollback coordinated with the ack protocol, `configureDisplayWindow`/`displayWindowGeometry` on the service + GLFW, 12 app tests + 6 run_gate leaves (new app_a3 shard; 47/47 exact-once).
- **des-4 (switching controls)**: Window section (mode/monitor/resolution/refresh combos, capability-gated, unknown-as-unknown), observed-geometry telemetry, 9-field persistence as revalidated names, 16 new `game_display` tests. des-3 errored with no output and left nothing; replaced cleanly.
- **Fixes after first green run**: `window_mode_domain` bare-brace → explicit `EnumEntry` (C2078); `not`-vs-bare `error_code` assert (C2440); harness dtor `noexcept` (C2694); duplicate-monitor ambiguity asserts `invalid_argument`; fullscreen-without-support rejected with `operation_not_supported`; 9-field CLI provenance coverage.
- **Validation**: format PASSED (58 files); semantic 0 blocking; clean build (FAILED-scan gate); 10/10 one-loop matrix green in 345s under approved `-j4` (a1 45.8, a2 46.9, a3 11.5, b_sim 52.9, c_fence 24.5, d_io 63.5, asset 56.9, game 0.41, sim 12.3, core 43.4). All 28 game tests proven executing. Prior void 9/9 (stale binaries) and filter-limit shard failure both diagnosed and closed with runner/shard hardening.
- **Phase 5 final gate (ora-17)**: PASS-WITH-FOLLOWUP; the supplied clean build + 10/10 one-loop matrix satisfies local phase completion. This is not unconditional merge readiness.
- **Pre-merge validation (2026-10-09)**: full `msvc-dev` matrix passed 10/10 with three same-process loops (1128s); full `msvc-live` matrix passed 10/10 with one loop (130s); `msvc-rel` production build passed with `BUILD_TESTING=OFF` and `ctest -N` confirmed zero registered tests. The isolated `runtime_layer_preserves_long_monitor_name` test passed. The Release build exposed the unconditional `game.tests` target and `_CrtSetReportHook2` macro qualification; both were corrected, and the HAL translation unit compiled in dev, live, and release. The test-only coverage change intentionally does not validate allocation-failure recovery.
- **Remaining open**: fault-injection coverage for the oracle-adjudicated blind-catch changes is partial (production fixes landed; see test coverage limits in the delivery record); 42GB asset-TU triage remains deferred.

### Error-handling follow-up (user directive)
- **Policy**: `AGENTS.md` now forbids silent exception swallowing to satisfy `noexcept`, requires a comment for justified best-effort catches, and calls out explicit `error_code` assertion predicates. `unit-test-updater` records the required `};` terminator for `PPR_UNIT_TEST` and exact-path shard token budgeting/registration.
- **Engine blind-catch rulings**: production changes implemented; regression-test lane added 7 tests and extended 3, but explicitly left join-failure, allocation/startup-failure, acknowledgement-publication, stage-log allocation, accepted-submit-then-throw, admission-record allocation, stash-OOM, timeout-reclaim, and acquire/fallback-present fault injections blocked by missing production seams. No production test-only hooks were retained after the user rejected that approach. Native wake is `noexcept` and cannot be throw-injected.
- **Game blind-catch rulings**: production changes implemented with 4 focused tests. Remaining per-site failure-injection depth was not complete at this checkpoint. The runtime-layer global-allocation sweep was replaced with ordinary long-monitor-name preservation coverage after the injected MSVC Debug string-proxy allocation caused fail-fast; the exact exception boundary remains unproven. Allocation-failure recovery for this path is not validated. Do not claim every surveyed catch site has an injected runtime regression until compatible coverage exists.
- **Scheduler ASan follow-up**: `WorkerState_` now owns its `Signal` through `join()`, and initializes owner/signal before starting `jthread`; added `cancelled_close_join_repeated_with_gated_worker` (256 bounded cancellation/drain/join rounds). The one-loop `EngineCoreUnitTests` gate passed (84s), including the original nested-cancellation reproducer. External parent-context cancellation concurrent with scheduler destruction remains an explicitly unverified lifetime boundary; the fix does not add a BroadcastEvent detach/quiescence protocol.

### Key artifacts
- Plan: `docs/plans/config-display-cadence-oracle.md`
- Config core: `lib/engine/app/App.Display.cppm/.cpp`, `lib/engine/core/config/*`
- CLI migration: `game/Game.Config.cppm/.cpp`, `lib/engine/tests/shared/Tests.cppm/.cpp`
- Pacing policy: `lib/engine/app/App.Display.cppm/.cpp`, `App.Application.cppm/.cpp`, `App.Application.Editor.cppm/.cpp`
- Tests: `lib/engine/tests/app/App.Display.Tests.cpp`, `App.Frame.Tests.cpp` (extended)
