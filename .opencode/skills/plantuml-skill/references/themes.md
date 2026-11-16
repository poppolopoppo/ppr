# Themes

Load when choosing or changing diagram styling. Re-render and validate through
[the shared workflow](../SKILL.md#workflow) after styling edits.

```plantuml
!theme plain       ← clean, minimal (recommended)
!theme cerulean    ← blue-tinted
!theme blueprint   ← dark blue background
!theme aws-orange  ← AWS style
!theme vibrant     ← vivid colors
```

Or use `skinparam` for custom styling:
```plantuml
skinparam backgroundColor #FAFAFA
skinparam componentBorderColor #555555
skinparam ArrowColor #333333
skinparam FontName Arial
```
