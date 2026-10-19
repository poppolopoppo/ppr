# simdjson

Upstream: https://github.com/simdjson/simdjson

Version: **4.2.3** resolved by the vcpkg manifest at the pinned baseline; no
override and no CPM fallback. The port enables `deprecated`, `exceptions`,
`threads`, and `utf8-validation`.

## License

Apache-2.0 (the preserved file carries the full Apache-2.0 text). Keep the
notice with redistributed software.

| File | Bytes | SHA-256 |
|---|---:|---|
| LICENSE | 12675 | 8bba4c86681e778878ad5bbff33baa524d450b2a42e256f3b931fc58a5ec2bfd |

The preserved file is the installed vcpkg notice (`share/simdjson/copyright`).

## Integration scope

- PPR code never includes simdjson. It is a Mango `import3d` dependency
  (glTF/GLB JSON parsing inside `mango-import3d`, which PPR enables with
  `BUILD_IMPORT3D ON`), linked through the vcpkg-resolved build.
- On the Windows `x64-windows` triplet simdjson ships as a runtime DLL staged
  beside PPR executables, so binary redistribution carries this notice.
- `engine.mesh` consumes only Mango's import API and re-resolves GLB embeds
  against its own file mappings; no JSON vocabulary leaks past that boundary.
