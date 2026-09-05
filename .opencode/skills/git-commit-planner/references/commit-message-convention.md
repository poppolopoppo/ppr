# PPR Commit Message Convention

This is the canonical message convention for both commit planning and push-time
message review.

## Subject

```text
<component>: <imperative lowercase summary>
```

- Keep the complete subject at or below 72 characters.
- Use the codebase component name (for example `HAL`, `cmake`, `Core.Memory`,
  `UnitTest`, or `Macros.h`). Use title case for files/classes and lowercase
  for directories.
- Write an imperative, lowercase summary with no trailing period.
- Do not use Conventional Commit type prefixes such as `feat:`, `fix:`, or
  `chore:`.

## Body

The body is optional. When present, separate it from the subject with one blank
line, wrap lines at 72 characters, and use one to three sentences explaining
why and what changed. Preserve required trailers such as `Signed-off-by:`.
