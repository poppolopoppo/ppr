# Filament AgX scalar display approximation

PPR adapts only the scalar `AgxToneMapper` Punchy implementation and primary
conversion constants from [Google Filament](https://github.com/google/filament),
pinned to commit `fd9ec7ff9510df148011c5326bcb50d4bfb811ca`:

- [`filament/src/ToneMapper.cpp`, AgX section](https://github.com/google/filament/blob/fd9ec7ff9510df148011c5326bcb50d4bfb811ca/filament/src/ToneMapper.cpp#L617)
- [`filament/src/ColorSpaceUtils.h`, conversion constants](https://github.com/google/filament/blob/fd9ec7ff9510df148011c5326bcb50d4bfb811ca/filament/src/ColorSpaceUtils.h#L37)
- [`filament/include/filament/ToneMapper.h`, linear Rec.2020 contract](https://github.com/google/filament/blob/fd9ec7ff9510df148011c5326bcb50d4bfb811ca/filament/include/filament/ToneMapper.h#L58)

The upstream files credit the Android Open Source Project (2020/2021) and are
Apache-2.0. The exact upstream license is preserved in [LICENSE](LICENSE).
Its SHA-256 is `02d70b593dd8e9355dd8e1ca8d13c303bee5bcaf3065b66ff6e4f534e1e6a2c1`.
No Filament engine, binary, build dependency or runtime LUT is included.
The upstream AgX implementation credits Troy Sobotka's AgX, Benjamin Wrensch's
minimal approximation and the Blender/AgX Rec.2020 matrices. PPR preserves the
selected source's Punchy power/saturation and pre-power luma convention.

## Integration scope

`assets/shaders/display_agx.slang` translates this scalar approximation to Slang.
PPR explicitly wraps linear-sRGB/Rec.2020 conversions, clips negative scene RGB
only at the display boundary, applies manual exposure, and clips the resulting
linear-sRGB display gamut to [0,1]. The upstream gamma-2.2 linearization belongs
to the transform; it is not the final sRGB attachment encoding. The destination
format independently selects hardware sRGB encoding, shader sRGB encoding for
UNorm, or linear FP16 diagnostic output. There is no special black correction.
This is an attributable AgX Punchy approximation, not verified v22 or exact OCIO
equivalence, an HDR-display/PQ output transform, or complete game integration.

## Independent frozen fixture values

`generate_goldens.py` is a separate double scalar transcription of the pinned
source. It does not import PPR shader helpers or read shader constants. It keeps
the upstream column-major matrices and independently inverts the source outset
matrix with Gauss-Jordan elimination, unlike the shader's row-vector literals.
Inputs first round through IEEE binary16, matching the RetroPass scene target.
`Golden.inc` freezes 32 patch/ramp inputs and linear display outputs at EV
0, +1, -16 and +16. The real RetroPass fixture compares FP16 readback against
these values and checks the final sRGB/UNorm byte contract separately.
The initial generated file SHA-256 is
`c77c5bfb4c166f0466bff391119d4c7fc36c76ad0b1b0b25a765f5d523b073c8`.

Regenerate only for an explicit accepted source/contract change:

```sh
python3 third_party/FilamentAgX/generate_goldens.py
```

Finite RGBA16F producer inputs (including negative intermediates) are required;
the fixture rejects NaN/infinity or overflow before its producer writes. This
does not claim that RetroPass scans arbitrary scene textures for invalid pixels.
Unexposed HDR history, native UI ordering, and all ordinary-world defaults stay
under the existing RetroPass/application owners.
