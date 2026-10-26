# Inspecting native execution with MCP

Use CLion MCP for source navigation and live debugging, official WinDbg MCP for
native target/dump investigation, and PIX MCP for CPU timing or GPU captures.
Their availability is separate from having an identified target, verified
symbols, working execution controls, and useful captured evidence.

Tool selection and the preflight checklist below are repository recommendations,
not vendor guarantees. Official documentation establishes the interfaces; the
dated evidence section establishes what was actually exercised here.

The [execution-tools skill](../../.agents/skills/execution-tools/SKILL.md) routes
agents here. [Build-system](../../.opencode/skills/build-system/SKILL.md) owns
builds; [validation](../../.opencode/skills/validation/SKILL.md) owns serialized
execution. Use one execution owner. Inspect existing artifacts before reproducing.
Do not interrupt another session, replace its target, or run competing captures.

## Establish the route before an expensive reproduction

1. Record the exact question and required evidence: a paused stack/local value,
   a complete CPU interval, or GPU execution data. A CPU submission scope does
   not answer a GPU execution-time question.
2. Preserve executable/PDB hashes, compiled-source identity, arguments, working
   directory, environment/configuration, and the actual target PID plus creation
   time. A later source edit does not change which source produced that binary.
3. Discover the current schemas and inspect target/session state. Treat an empty
   session list as absence at that moment, not proof the debugger is broken.
4. Before relying on an unproven debugger route, verify breakpoint placement,
   launch/attach, suspension, stack/local inspection, resume, and clean exit on
   an appropriate existing executable. Do not add a demo or repeat a broad suite
   to test the transport. A working status query alone is not this verification.
5. Budget tool deadlines around the requested action and finalization. Enforce
   any external-process watchdog independently of the model's next message or
   a blocking shell call. A stated timeout is not an implemented watchdog.

## CLion: preferred source-level live route

Discover `execute_tool` and its current commands; pass `projectPath="E:/Code/ppr"`.
Do not assume another IDE's tool names or a fixed endpoint port. The verified
2026.2 router exposes the following workflow; recheck its live schema before use:

1. `xdebug_get_debugger_status` identifies existing sessions. Respect their owner.
2. `get_run_configurations` supplies exact configuration names. Choose an actual
   executable such as `engine.tests.app`, not a library or aggregate target.
   Inspect the selected configuration's executable, build tree/toolchain and
   Before launch actions. Keep builds on the build-system path; do not trigger
   an implicit IDE build or use a different binary accidentally.
3. Set and verify a source breakpoint with `xdebug_set_breakpoint` and
   `xdebug_list_breakpoints` against the compiled source. Start through
   `xdebug_start_debugger_session`. Supply per-launch arguments/working directory
   only when that configuration advertises `supportsDynamicLaunchOverrides=true`.
4. Use `xdebug_control_session` with `WAIT_FOR_PAUSE` or `PAUSE`, then inspect
   `xdebug_get_threads`, `xdebug_get_stack`, and `xdebug_get_frame_values`.
   Retain the current session/thread/frame identities; refresh them after resume.
5. Resume or stop only the owned session and preserve the actual target outcome.

The CLion MCP client timeout must exceed the requested start/wait timeout plus
response overhead. A 30s outer timeout cannot reliably accommodate a 60s start
or 120s wait. Prefer explicit bounded waits; changing the client timeout is a
configuration decision, not permission to lengthen an engine/test deadline.

See [JetBrains' router/debugger guide](https://www.jetbrains.com/help/clion/agentic-debugging.html)
and [current tool schemas](https://www.jetbrains.com/help/clion/mcp-server.html).
IDE source-index readiness and debugger-session readiness are separate checks.

## WinDbg: use the official MCP session

Follow [windbg-triage](../../.opencode/skills/windbg-triage/SKILL.md).
`list_sessions` enumerates WinDbg service instances; the listed PID belongs to
WinDbg, not necessarily its debug target. `connect_session` reserves the selected
instance for one MCP client. Verify `get_target_state` after connecting; do not
take a session already owned by another client.

Keep Secure Mode enabled. `NoTarget` needs a separately established authorized
target; do not try `.create` as a connection probe or silently replace a target.
Use advertised MCP execution controls rather than Ctrl+C through a shell. If
loading/launching/attaching is unavailable under the active schema/security
policy, report that precise prerequisite. Do not disable protections to proceed.
See [Microsoft setup](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/set-up-windbg-mcp).
Microsoft's [Secure Mode restrictions](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/features-of-secure-mode)
also cover symbol-server transports and caches. Do not apply a suggested remote
symbol path blindly; matching local PDBs may be needed under the active policy.
Microsoft documents other clients, including custom Codex wiring, as best effort;
its officially supported clients are VS Code with GitHub Copilot and Copilot CLI.
Protected output may require MCP sampling. Keep that compatibility issue separate
from Secure Mode restrictions and from an absent debugging target.

CDB terminal debugging is a distinct fallback, not a test of WinDbg MCP. The
October 2026 RTK/PTY route failed its interrupt check: Ctrl+C terminated the
session. Do not reuse it without a successful transport check in a changed
environment. Resolve module-qualified symbols and verify installed breakpoints;
do not infer an application crash from ASan first-chance exception notifications.
Pass source/symbol path commands separately: a path-setting command can consume
semicolon-separated text as paths. A zero CDB exit with an error/no stack is
failed diagnosis, not success.

## PIX: separate capture, response, and analysis

Follow [pix-profiling](../../.opencode/skills/pix-profiling/SKILL.md).
Microsoft PIX is the profiler; PozLabs supplies the third-party MCP wrapper.
Microsoft's [Timing Captures documentation](https://devblogs.microsoft.com/pix/timing-captures-new/)
describes optional CPU samples, context-switch stacks and GPU timings, their
overhead, and PDB requirements for function information. The
[pinned wrapper schema](https://github.com/PozLabs/pix-mcp/blob/v0.1.1/src/tools/capture.rs)
and live discovery determine which of those settings MCP can actually control.
`pix_status` must confirm readiness and elevation for timing capture. It does
not verify the requested capture settings or analysis result.

- CPU timing can attach to a normally launched target. Start capture early
  enough to include both edges of the phase, and keep the target alive through
  capture completion. Do not substitute a launcher/CTest PID for the executable.
- GPU capture by PID requires launch under PIX. Use the supported GPU launch
  route and retain its argument-transport limitations.
- The observed timing schema accepts only PID, duration and output path; it
  cannot specify or verify CPU sampling/callstack settings. Report those settings
  as unknown unless another permitted, documented route establishes them.
- Capture duration must fit within the MCP tool timeout with attachment,
  finalization and response allowance. For example, a 60-65s capture needs more
  than a 60s tool deadline; 120s is a possible client allowance, not an engine
  timeout change. Verify the effective configuration instead of assuming it.
  OpenAI documents [tool_timeout_sec](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)
  with a 60s default; it does not prescribe the 120s example. The pinned wrapper's
  [blocking subprocess call](https://github.com/PozLabs/pix-mcp/blob/v0.1.1/src/pix/pixtool.rs)
  does not establish a fixed 30s finalization allowance or cancellation cleanup.
- Parse complete structured process-discovery output, handling zero, one and
  multiple matches explicitly. Do not slice JSON from its first opening brace
  or silently catch parse errors. Retain raw status/errors on discovery failure.
- After a tool timeout, inspect artifact and owned-process state before any
  retry. Client cancellation does not prove that the capturer/target stopped.

Use the maintained CPU marker query helper for timing files; `pix_get_event_list`
is a D3D12 event query, not a CPU stack exporter. Check process instance,
interval coverage and missing beginnings/endings. `clipped=0` over returned pairs
does not prove the capture included every phase. File size proves neither
complete coverage nor useful analysis. Read sampled stacks through a supported
analysis route; do not invent undocumented database joins.

## Evidence checkpoint: 20 October 2026

These are dated observations, not permanent tool-state claims:

| Route | Verified evidence | Still unproven or failed |
|---|---|---|
| CLion 2026.2.3 | Native router/status/configuration queries; executable configurations advertise dynamic launch overrides | No complete breakpoint/pause/stack/resume check in this investigation |
| Official WinDbg MCP | Proxy/session discovery; later discovery found an available Secure Mode session | Session discovery alone does not verify target attachment or stack inspection |
| PIX 2603.25 | Elevated server; one partial timing capture yielded 352 valid CPU marker pairs | Slow first phase was not fully bracketed; later capture had no useful markers and the profiled test timed out |
| CDB via RTK/PTY | Initial break and resume on a harmless target | Interrupt killed terminal; separate noninvasive attach returned 0x80004002/no stack and exceeded its intended 30s bound |
| Temporary producer logging | Native test passed; bounded ordered same-thread timestamps localized first FinishCommands to 39561.1169ms, reused 25.7277ms | No internal finalization stack or GPU execution-time attribution |

Original evidence: [native/CDB record](../../.slim/deepwork/unified-matter-v2/resume-gpu-integration2b-validation.md),
[PIX attempts](../../.slim/deepwork/unified-matter-v2/resume-gpu-pix-followup-capture-20261020.md),
and [earlier debugger setup failures](../../.slim/deepwork/unified-matter-v2/resume-gpu-integration1-validation.md).
These local artifacts are gitignored and may be absent from a fresh checkout.

Producer logging is a bounded fallback for CPU phase localization: preserve
producer timestamps, phase/thread ordering, sink lifetime and flush/sentinel
checks; remove temporary instrumentation afterward. It does not replace stacks
or GPU profiling. In this case raw timestamps were not printed, only asserted
ordering and derived durations; preserve that limitation when reusing evidence.
