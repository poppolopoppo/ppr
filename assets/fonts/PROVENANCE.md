# assets/fonts/ — Provenance

Pinned UI assets for [the ImGui refinement plan](../../docs/plans/imgui-ricing-plan.md).
The current default composition is recorded under **Concept typography** below.
The original three font files and notices were present and their byte counts/
full SHA-256 values reverified on 2026-10-15. They remain fixtures; that historical
identity check alone does not establish deployment, loadability or visual acceptance.

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

`game/CMakeLists.txt` requires the fonts directory through the existing asset
facility; `StageAssets.cmake` admits its destination leaf. The focused font tests
use the staged executable-relative files and validate their exact bytes before
baking. Native deployment/cross-CWD acceptance is recorded separately in progress;
configure success alone does not prove those checks.

Local execution details: [font manifest](../../.slim/deepwork/imgui-ricing/font-manifest.md).
That Git-ignored record is supplementary; the artifact pins above are self-contained.

## Concept typography — selected 2026-10-18

Pixelify Sans Regular (400) and SemiBold (600) are the default body/emphasis faces at 16 logical px. Big Shoulders Display Bold (700) supplies 22 px headings. The Inspector read-out binds the body file a second time at 12 px for its compact key/value table, which adds a baked face but no artifact or licence. Font Awesome remains merged into body only. All are static upstream TTF artifacts; no conversion/subsetting. Earlier Inter/JBMono files remain as legacy test fixtures. Runtime checks exact byte counts and SHA-256 before atlas creation.

| File | Pinned upstream commit/path | Bytes | SHA-256 |
|---|---|---:|---|
| PixelifySans-Regular.ttf | [39df74aba80df8157546034b878e8be1eb565ced / fonts/ttf/PixelifySans-Regular.ttf](https://raw.githubusercontent.com/eifetx/Pixelify-Sans/39df74aba80df8157546034b878e8be1eb565ced/fonts/ttf/PixelifySans-Regular.ttf) | 93780 | 5436d485d1713afa376e1744911cbbcc0849eae1eb55ec9bc34c0753993ffef9 |
| PixelifySans-SemiBold.ttf | [39df74aba80df8157546034b878e8be1eb565ced / fonts/ttf/PixelifySans-SemiBold.ttf](https://raw.githubusercontent.com/eifetx/Pixelify-Sans/39df74aba80df8157546034b878e8be1eb565ced/fonts/ttf/PixelifySans-SemiBold.ttf) | 97492 | a4b54982991cd47450df317a451a7f066bf6b91aa033e105643aba2cf7bd35c3 |
| PixelifySans-OFL.txt | [39df74aba80df8157546034b878e8be1eb565ced / OFL.txt](https://raw.githubusercontent.com/eifetx/Pixelify-Sans/39df74aba80df8157546034b878e8be1eb565ced/OFL.txt) | 4488 | b66ba46f511a851ab09998b5a5a9fdbb102545a3864cb993095e1745996873a7 |
| BigShouldersDisplay-Bold.ttf | [8ba99c9e347396d828b263b8b818269cb99eb27c / Big-Shoulders/fonts/ttf/BigShouldersDisplay-Bold.ttf](https://raw.githubusercontent.com/xotypeco/big_shoulders/8ba99c9e347396d828b263b8b818269cb99eb27c/Big-Shoulders/fonts/ttf/BigShouldersDisplay-Bold.ttf) | 122896 | cbfe03f89e2bb3aba487644cdaf9abc9b3eb59946efaf894dda5cd05e1690c83 |
| BigShouldersDisplay-OFL.txt | [8ba99c9e347396d828b263b8b818269cb99eb27c / OFL.txt](https://raw.githubusercontent.com/xotypeco/big_shoulders/8ba99c9e347396d828b263b8b818269cb99eb27c/OFL.txt) | 4396 | 338f9c050f19daeda1d597243faf79f3a3d437c338af58cb7047617d0ce08771 |
