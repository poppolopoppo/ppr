// Local opencode plugin: enforces "delegate first" for the orchestrator only.
// Auto-discovered from .opencode/plugin/ — no opencode.json plugin entry needed.
//
// It hooks experimental.chat.system.transform and appends a delegation mandate
// to the orchestrator's system prompt. Detection is by a marker string that
// exists only in the orchestrator prompt, so specialist agents (fixer,
// explorer, librarian, oracle, designer) are never affected. This is
// model-agnostic: the rule is injected regardless of which model the
// orchestrator preset selects.

const DELEGATE_FIRST_RULE = `
# Delegation-first mandate (injected)

You are the orchestrator. Your job is to plan, schedule, delegate, and
reconcile — NOT to execute toolchains yourself.

## Default: delegate
- The FIRST response to any non-trivial task is to delegate to the appropriate
  specialist (@explorer, @librarian, @oracle, @designer, @fixer). Do not run
  the work yourself first "to scope it."
- Running builds, tests, compilers, linters, or any multi-step implementation
  is NEVER a direct-handling case. Always delegate execution to @fixer (or the
  relevant specialist).
- A few file reads for discovery are fine ONLY when it is a single, trivial,
  read-only lookup that no specialist would improve. When in doubt, delegate.

## Narrow direct-handling exception
Handle directly ONLY when no suitable agent exists AND the action is a single,
read-only, low-risk operation (e.g. one targeted read/grep to answer a factual
question). Even then, prefer delegating if the result feeds further work.
`;

// Unique to the orchestrator prompt; absent from every specialist prompt.
const ORCHESTRATOR_MARKER = "workflow manager for coding work";
const INJECTED_GUARD = "Delegation-first mandate (injected)";

export default async () => ({
  "experimental.chat.system.transform": async (_input, output) => {
    if (!output || !Array.isArray(output.system)) return;
    const systemText = output.system.join("\n");
    if (
      systemText.includes(ORCHESTRATOR_MARKER) &&
      !systemText.includes(INJECTED_GUARD)
    ) {
      output.system.push(DELEGATE_FIRST_RULE);
    }
  },
});
