# assets/fonts/ — Provenance

Pinned UI assets for [the ImGui refinement plan](../../docs/plans/imgui-ricing-plan.md).
All three font files and three notice files below were present and their byte
counts/full SHA-256 values reverified on 2026-10-15. This proves local file
identity, not deployment, ImGui loadability or visual acceptance. Runtime font
loading remains planned.

## Pinned binaries

| Staged file | Version | Source archive / path | Upstream URL | Bytes | SHA-256 |
|---|---|---|---|---:|---|
| Inter-Regular.ttf | Inter 4.1 | Inter-4.1.zip / extras/ttf/ | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip | 411640 | 40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82 |
| JetBrainsMono-Regular.ttf | JB Mono 2.304 | JetBrainsMono-2.304.zip / fonts/ttf/ | https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip | 273900 | a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f |
| Font Awesome 7 Free-Solid-900.otf | FA Free 7.3.1 | fontawesome-free-7.3.1-desktop.zip / otfs/ | https://github.com/FortAwesome/Font-Awesome/releases/download/7.3.1/fontawesome-free-7.3.1-desktop.zip | 414704 | daa92774861a14e9ccbccc5e73a5fba514271ba4d1bd7e5adbf952182bc00835 |


FA7 is the current candidate. The earlier working record allows evaluating
FA Free 6.7.2 `fa-solid-900.ttf` if FA7 fails legibility review:
[web release archive](https://github.com/FortAwesome/Font-Awesome/releases/download/6.7.2/fontawesome-free-6.7.2-web.zip),
`webfonts/`, reported 426112 bytes and SHA-256
`d2f0593540b0e33ba6de255a54f272d466e31144806956bea8cfdbf7edffc9bd`.
That fallback is not present or reverified here. If selected, update its binary,
matching notice, mapping, sizing and deployment evidence together.

Abbreviated historical archive hashes are not verification records and included
an inconsistent FA7 prefix. Use the complete extracted-file hashes above.
For any new download, record a complete archive hash and verify extracted files;
no claim that these versions are latest is needed.

## License notices

| Notice file | Origin | Bytes | SHA-256 |
|---|---|---:|---|
| Inter-LICENSE.txt | Inter-4.1.zip LICENSE.txt | 4380 | 262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a |
| JetBrainsMono-OFL.txt | JetBrainsMono-2.304.zip OFL.txt | 4399 | 30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad |
| FontAwesome-LICENSE.txt | FA 7.3.1 desktop LICENSE.txt | 7427 | 20c6f40715a567c97b80f6944beb8bb325835cab47ea7dcab89ee3b8e077eced |


Ship these notices with the unmodified files. FA's upstream distribution separates
font, artwork and code terms; see the
[pinned license](https://github.com/FortAwesome/Font-Awesome/blob/7.3.1/LICENSE.txt).
Keeping binaries unmodified is this plan's choice, not a categorical claim that
OFL forbids subsetting. Modified fonts or external generated headers require
their own source/license/name review.

## Runtime integration constraints

The current-file cmap inspection found 558 FA7 codepoints in U+F000–U+F3FF,
with no overlap from Inter or JB Mono in that range. Select only icons actually
used. Inter/FA7 overlap elsewhere, so broad automatic icon merging is unsuitable.
Keep selected glyph ranges persistent for the static backend; verify rendered
coverage and baseline/advance metrics during M2.

The CFF/OTF container and cmap are not proof that the selected file has passed
PPR's exact bake/render path. Do not infer mandatory pixel sizes from cap/em ratios.
Text shaping and ligatures are not supplied automatically by switching rasterizer
([FreeType FAQ](https://freetype.org/freetype2/docs/ft2faq.html)).
The plan must define baked text coverage; no universal Unicode support is claimed.

## Staging

`game/CMakeLists.txt` conditionally registers the fonts directory with the existing
asset facility; `StageAssets.cmake` admits its destination leaf. Make registration
required when runtime depends on these files. Data-only synchronization,
deployed-file identity and cross-CWD chosen-font loading remain verification
tasks; configure success alone does not prove them.

Local execution details: [font manifest](../../.slim/deepwork/imgui-ricing/font-manifest.md).
That Git-ignored record is supplementary; the artifact pins above are self-contained.
