# Official Slang references

Use with the [entry skill](../SKILL.md) when language, compiler, layout,
cursor, resource, or target behavior needs official documentation.

Prefer these official sources over search snippets or HLSL folklore:

- Matrix layout: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a1-01-matrix-layout.html>
- Conventional language and resource features: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/02-conventional-features.html>
- Compilation API: <https://shader-slang.org/docs/compilation-api/>
- Compilation and targets: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/08-compiling.html>
- Shader cursor model: <https://docs.shader-slang.org/en/latest/shader-cursors.html>
- Parameter blocks: <https://docs.shader-slang.org/en/latest/parameter-blocks.html>
- SPIR-V target behavior: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html>
- Metal target behavior: <https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a2-02-metal-target-specific.html>
- Bindless convenience features: <https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/03-convenience-features.html>
- Slang target compatibility: <https://github.com/shader-slang/slang/blob/master/docs/target-compatibility.md>
- Slang-RHI status and source: <https://github.com/shader-slang/slang-rhi>

When an official statement conflicts with observed behavior in PPR's current
Slang-RHI revision, record the minimal reproducer and treat the local behavior
as a version-specific integration constraint, not as a general Slang fact.
