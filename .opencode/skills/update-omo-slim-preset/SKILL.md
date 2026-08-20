---
name: update-omo-slim-preset
description: Automates updating OMO-slim preset configurations (ppr-free, ppr-paid, ppr-pro) with the best available models based on cost-efficiency scoring. Retrieves the live model list via `opencode models`, researches capabilities online, scores models, tests availability, and falls back to next-best options when models become unavailable. Use when the user asks to refresh, update, or optimize their OMO-slim preset model assignments.
---

# Update OMO-slim Preset

Automates the selection and assignment of models to the three PPR presets (`ppr-free`, `ppr-paid`, `ppr-pro`) in both user-level (`~/.config/opencode/oh-my-opencode-slim.json`) and project-level (`.opencode/oh-my-opencode-slim.json`) OMO-slim configurations.

## When to Use

- User asks to refresh/update/optimize preset model assignments
- Sub-agents are failing because a model in a preset is no longer available
- A new model has been released and should be evaluated for inclusion
- Preset costs need rebalancing after pricing changes

## Workflow

### 1. Retrieve Live Model List

Run `opencode models` to get the authoritative list of currently available models. Filter to:
- All `opencode/*` models (OpenCode Zen)
- `openrouter/openai/gpt-5-nano` (permanently free on OpenRouter)

All other providers are excluded — only OpenCode Zen models are considered available, with the single OpenRouter exception above.

### 2. Research Model Capabilities

For each candidate model, research online:
- **Capabilities**: coding, reasoning, vision, tool calling, context window
- **Performance**: benchmark scores (SWE-bench, HumanEval, LiveCodeBench, GPQA, etc.)
- **Intended usage**: what the model is optimized for (orchestration, code review, fast scanning, design, etc.)
- **Pricing**: input/output cost per 1M tokens (from https://opencode.ai/docs/zen/)

Use `@librarian` for focused research on unfamiliar models or rapidly changing capabilities.

### 3. Score Models by Cost Efficiency

Assign each model a **cost-efficiency score** = (quality benchmark) / (cost per 1M tokens).

Quality tiers (from research):
- **S-tier**: Claude Opus 4.5/4.6/4.7/4.8/5, GPT 5.6 Sol, Claude Sonnet 5
- **A-tier**: Claude Sonnet 4.5/4.6, GPT 5.6 Terra, GPT 5.5, minimax-m3
- **B-tier**: Claude Haiku 4.5, GPT 5.4 Pro, GPT 5.1 Codex Max, minimax-m2.7
- **C-tier**: GPT 5.4 Mini, GPT 5.1 Codex Mini, minimax-m2.5
- **D-tier**: GPT 5 Nano, GPT 5.4 Nano, Gemini 3.5 Flash Lite

Free models get a separate track (all score equally on cost = $0, ranked by quality only).

### 4. Select Models Per Preset

**ppr-free** (only $0 models):
- orchestrator: best free reasoning model (e.g., `hy3-free`, `big-pickle`)
- oracle: best free reasoning with max variant (e.g., `big-pickle`)
- explorer/librarian: fast free model with large context (e.g., `nemotron-3-ultra-free`)
- fixer: fast free model (e.g., `nemotron-3.5-lightning-free`)
- designer/observer: multimodal free model (e.g., `mimo-v2.5-free`)
- council: best free reasoning (e.g., `big-pickle`)

**ppr-paid** (cost-efficient mix):
- orchestrator: best value paid model (e.g., `minimax-m3` at $0.30/$1.20)
- oracle: strong reasoning at low cost (e.g., `qwen3.6-plus` at $0.50/$3.00)
- explorer/librarian/fixer: fast cheap model (e.g., `deepseek-v4-flash` at $0.22-$0.44)
- designer: coding specialist (e.g., `kimi-k2.7-code` at $0.95/$4.00)
- observer: free multimodal (e.g., `mimo-v2.5-free`)
- council: best value (e.g., `minimax-m3`)
- general: `openrouter/openai/gpt-5-nano` (free)

**ppr-pro** (best effort, premium):
- orchestrator: best value orchestrator (e.g., `minimax-m3`)
- oracle: top-tier reasoning (e.g., `claude-opus-4-5`, `gpt-5.6-sol`)
- explorer/librarian/fixer: fast reliable model (e.g., `deepseek-v4-flash`)
- designer: coding specialist (e.g., `kimi-k2.7-code`)
- observer: free multimodal (e.g., `mimo-v2.5-free`)
- council: best value (e.g., `minimax-m3`)

### 5. Test Model Availability

Before assigning any model, test it:

```bash
# Test via opencode CLI or direct API call
opencode models --refresh
```

Or send a minimal request to verify the model responds. Models on OpenCode Zen can become unavailable frequently (free models especially). If a model fails:
1. Mark it as unavailable
2. Fall back to the next-best model in the same tier
3. Repeat until a working model is found

### 6. Update Both Config Files

Update both:
- `~/.config/opencode/oh-my-opencode-slim.json` (user-level)
- `.opencode/oh-my-opencode-slim.json` (project-level, if it exists)

Preserve PPR-specific skills and `clion` MCP in the project-level config.

### 7. Validate Schema Compliance

After editing:
- Parse JSON to confirm validity
- Verify no `_comment` or other unknown fields (schema has `additionalProperties: false` on agent blocks)
- Confirm all model names are in the live model list from step 1

### 8. Report Changes

Summarize:
- Models changed per preset per agent
- Cost impact (estimated $/month based on typical usage)
- Any models that fell back due to unavailability
- Any models that were removed from the candidate list

## Fallback Chain

When a selected model is unavailable, use this fallback order:

**Orchestrator/Oracle/Council (reasoning-heavy):**
1. `claude-opus-4-5` → `claude-opus-4-6` → `claude-sonnet-4-5` → `gpt-5.6-sol` → `gpt-5.6-terra` → `minimax-m3` → `qwen3.6-plus` → `big-pickle` (free)

**Explorer/Librarian/Fixer (fast scanning):**
1. `deepseek-v4-flash` → `gemini-3.5-flash` → `gpt-5.4-mini` → `nemotron-3-ultra-free` (free) → `nemotron-3.5-lightning-free` (free)

**Designer (coding/UI):**
1. `kimi-k2.7-code` → `kimi-k2.6` → `claude-sonnet-4-5` → `mimo-v2.5-free` (free)

**Observer (multimodal):**
1. `mimo-v2.5-free` → `gemini-3.5-flash` → `gpt-5.4-nano`

## Constraints

- Never use a model not in the live `opencode models` list (except `openrouter/openai/gpt-5-nano`)
- Never add `_comment` or other fields not in the OMO-slim schema
- Never remove PPR-specific skills or `clion` MCP from the project-level config
- Always test model availability before assignment
- Always update both user-level and project-level configs together

## Related Files

- `~/.config/opencode/oh-my-opencode-slim.json` — user-level config
- `.opencode/oh-my-opencode-slim.json` — project-level config (PPR-specific)
- `~/.config/opencode/opencode.json` — default model + providers
- https://opencode.ai/docs/zen/ — authoritative model list and pricing
- https://unpkg.com/oh-my-opencode-slim@latest/oh-my-opencode-slim.schema.json — config schema
