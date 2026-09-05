// Local opencode plugin: injects cost-aware Oracle/orchestrator evidence rules.
// Auto-discovered from .opencode/plugin/ — no opencode.json plugin entry needed.
//
// This stays repository-local and avoids broad preset rewrites by appending only
// the narrow guidance requested for Oracle and the orchestrator.

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

export default async () => ({
  "experimental.chat.system.transform": async (input, output) => {
    if (!output || !Array.isArray(output.system)) {
      return;
    }

    const systemText = output.system.join("\n");

    if (
      isOrchestratorPrompt(systemText) &&
      !systemText.includes(ORCHESTRATOR_GUARD)
    ) {
      output.system.push(ORCHESTRATOR_EVIDENCE_RULE);
    }

    if (
      isOraclePrompt(systemText) &&
      !systemText.includes(ORACLE_GUARD)
    ) {
      output.system.push(ORACLE_EVIDENCE_RULE);
    }
  },
});
