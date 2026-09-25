// Local opencode plugin: injects cost-aware Oracle/orchestrator evidence rules.
// Auto-discovered from .opencode/plugins/ — no opencode.json plugin entry needed.
//
// This stays repository-local and avoids broad preset rewrites by appending only
// the narrow guidance requested for Oracle and the orchestrator.
//
// V1 -> V2: the V1 `experimental.chat.system.transform` hook became
// `ctx.session.hook("context", ...)`, and system content is now a list of typed
// blocks rather than an array of plain strings. `Plugin.define` is an identity
// function, so the definition is exported as a plain object.

const ORCHESTRATOR_EVIDENCE_RULE = `
# Oracle evidence brief discipline (injected)

Only when actually delegating to @oracle:

- pass a compact accepted-evidence summary with the decision request, strongest
  evidence, and exact files / hunks / references Oracle should use.
- do not add a separate artifact or extra model pass just to restate supplied
  evidence.
- reuse any existing planning artifact already carrying the relevant evidence
  instead of duplicating it.
`;

const ORACLE_EVIDENCE_RULE = `
# Oracle evidence discipline (injected)

Use supplied scoped evidence first.

- Do not restart with broad repository discovery, repeated CodeGraph
  exploration, or full-diff rereads unless a decisive claim remains unresolved.
- Prefer references and scoped hunks over rediscovery.
- If decisive evidence is still missing, return precise EVIDENCE_REQUESTS that
  name the exact file, symbol, hunk, output, or fact needed to decide.
`;

const ORCHESTRATOR_MARKER = "workflow manager for coding work";
const ORACLE_MARKER = "You are Oracle - a strategic technical advisor and code reviewer.";
const ORCHESTRATOR_GUARD = "Oracle evidence brief discipline (injected)";
const ORACLE_GUARD = "Oracle evidence discipline (injected)";

function isOraclePrompt(systemText) {
  return systemText.includes(ORACLE_MARKER);
}

function isOrchestratorPrompt(systemText) {
  return systemText.includes(ORCHESTRATOR_MARKER);
}

// The V2 system list holds typed content blocks; tolerate plain strings too.
function systemTextOf(system) {
  if (!Array.isArray(system)) return "";
  return system
    .map((block) =>
      typeof block === "string" ? block : (block?.text ?? ""),
    )
    .join("\n");
}

export default {
  id: "ppr.oracle-evidence-discipline",
  async setup(ctx) {
    await ctx.session.hook("context", (event) => {
      const systemText = systemTextOf(event.system);

      if (
        isOrchestratorPrompt(systemText) &&
        !systemText.includes(ORCHESTRATOR_GUARD)
      ) {
        event.system.push({ type: "text", text: ORCHESTRATOR_EVIDENCE_RULE });
      }

      if (isOraclePrompt(systemText) && !systemText.includes(ORACLE_GUARD)) {
        event.system.push({ type: "text", text: ORACLE_EVIDENCE_RULE });
      }
    });
  },
};
