# CIE 1931 2-degree colour-matching functions

Upstream dataset:

- Title: *Colour-matching functions of CIE 1931 standard colorimetric observer*
- Publisher: International Commission on Illumination (CIE), Vienna, AT
- DOI: [10.25039/CIE.DS.xvudnb9b](https://doi.org/10.25039/CIE.DS.xvudnb9b)
- Dataset page:
  <https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer>
- File: `CIE_xyz_1931_2deg.csv` (471 rows, 360-830 nm inclusive at 1 nm;
  columns `lambda`, `x_bar`, `y_bar`, `z_bar`)
- License: **Creative Commons Attribution-ShareAlike 4.0 International**
  (CC BY-SA 4.0), <https://creativecommons.org/licenses/by-sa/4.0/>

Attribution: International Commission on Illumination (CIE), Vienna, AT.
`CIE_xyz_1931_2deg.csv` is redistributed unmodified under CC BY-SA 4.0.

## Pin and verification

`CIE_xyz_1931_2deg.csv` SHA-256:

```text
fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1
```

The checksum matches the upstream dataset metadata (`checksums.sha256`) and is
re-verified by `scripts/generate_thermal_lut.py` before every generation or
`--check`. The CIE metadata also publishes the column sums
`[280245, 106.865469489595, 106.8569171011719, 106.892251278636]` and the
479 nm sample row `479,0.104297900000,0.1334528000000,0.856619300000`, which the
generator regression tests check against this file.

## Derivation in PPR

`scripts/generate_thermal_lut.py` integrates Planck spectral radiance over this
table in binary64 with trapezoidal endpoint half-weights and wavelengths in
metres, converts XYZ to linear sRGB with the explicit matrix from the lighting
design specification, projects negative gamut components to zero, scales by
`Y / dot(RGB, luminance_weights)`, and divides by `Y(6500 K)` so a 6500 K black
body has engine luminance one. It writes the committed
`assets/lighting/thermal_lut.bin` (2,048 rows, 16-byte stride, 32 KiB) and
`assets/lighting/thermal_lut.json`. Ordinary builds and startup never download
this dataset or rerun the integration.

`CIE_xyz_1931_2deg_reference.inc` is a frozen, generated C++ initializer copy of
the same CSV rows (shortest round-trip binary64 literals) used only by
`lib/engine/tests/app/App.WorldLighting.Codec.Tests.cpp` as the independent
binary64 reference for the generated table. It is not a runtime asset. Its
regeneration command is recorded in its header comment.
