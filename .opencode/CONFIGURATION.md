# OpenCode Configuration Authority

`.opencode/oh-my-opencode-slim.json` is the authoritative, tracked configuration for the PPR OpenCode workflow.
`AGENTS.md` defines the project standards and role responsibilities; project skills define task-specific procedures.
Configuration grants must enable those existing mechanisms rather than duplicate their instructions in agent prompts.

## Maintaining grants

- Keep the same role-to-skill and role-to-MCP grants across every preset unless a preset has a documented, intentional exception.
- Grant `clion-tools` to `explorer`, `oracle`, and `fixer`: all three roles use CLion navigation, diagnostics, or bounded edits.
- Keep PPR-domain skills on the roles named by `AGENTS.md`; do not create substitute agents or duplicate role definitions.
- When adding a skill, add it only to the roles that own its workflow and update every preset deliberately in the same change.
- Preserve model and provider choices unless a configuration error requires changing them; grants and project instructions are the normal standards-enforcement mechanism.

## Prompt and schema limits

Do not add speculative built-in configuration keys such as `prompt` or `orchestratorPrompt` to agent definitions. They are not supported project configuration mechanisms. Put durable project guidance in `AGENTS.md` or an existing skill/command, and keep this configuration limited to supported role, skill, MCP, model, and preset settings.

Before committing configuration changes, parse the JSON and review the diff to confirm that all presets remain aligned and unrelated settings are unchanged.
