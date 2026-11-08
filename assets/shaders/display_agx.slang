// Copyright (C) 2020 The Android Open Source Project
// Copyright (C) 2021 The Android Open Source Project
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// AgxToneMapper scalar Punchy approximation adapted to Slang from Filament
// fd9ec7ff9510df148011c5326bcb50d4bfb811ca, filament/src/ToneMapper.cpp.
// ColorSpaceUtils.h at that same revision supplies the primary conversions.
// See third_party/FilamentAgX/README.md and LICENSE. This is not an exact
// v22 or OCIO transform. All matrices below use PPR's row-vector convention:
// their rows are the columns in Filament's column-major mat3f constructors.

float3 displaySrgbToRec2020(float3 color) {
    float3 xyz = mul(color, float3x3(
        0.4124560, 0.2126730, 0.0193339,
        0.3575760, 0.7151520, 0.1191920,
        0.1804380, 0.0721750, 0.9503040));
    return mul(xyz, float3x3(
        1.7166634, -0.6666738, 0.0176425,
        -0.3556733, 1.6164557, -0.0427770,
        -0.2533681, 0.0157683, 0.9422433));
}

float3 displayRec2020ToSrgb(float3 color) {
    float3 xyz = mul(color, float3x3(
        0.6369530, 0.2626983, 0.0,
        0.1446169, 0.6780088, 0.0280731,
        0.1688558, 0.0592929, 1.0608272));
    return mul(xyz, float3x3(
        3.2404542, -0.9692660, 0.0556434,
        -1.5371385, 1.8760108, -0.2040259,
        -0.4985314, 0.0415560, 1.0572252));
}

float3 displayAgxPunchy(float3 scene_linear, float exposure) {
    // Finite RGBA16F input is a producer contract. Negative intermediates
    // survive scene/history storage and are clipped only at this boundary.
    float3 value = displaySrgbToRec2020(max(scene_linear, 0.0) * exposure);
    value = mul(max(value, 0.0), float3x3(
        0.856627153315983, 0.137318972929847, 0.11189821299995,
        0.0951212405381588, 0.761241990602591, 0.0767994186031903,
        0.0482516061458583, 0.101439036467562, 0.811302368396859));
    value = clamp((log2(max(value, 1.0e-10)) + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);

    float3 x2 = value * value;
    float3 x4 = x2 * x2;
    float3 x6 = x4 * x2;
    value = -17.86 * x6 * value + 78.01 * x6 - 126.7 * x4 * value +
        92.06 * x4 - 28.72 * x2 * value + 4.361 * x2 - 0.1718 * value + 0.002857;

    // Preserve the upstream Punchy convention, including luma before power.
    float luma = dot(value, float3(0.2126, 0.7152, 0.0722));
    value = luma + 1.4 * (pow(value, float3(1.35)) - luma);
    // Inverse of upstream AgXOutsetMatrixInv, evaluated as a constant.
    value = mul(value, float3x3(
        1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
        -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
        -0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
    value = pow(max(value, 0.0), float3(2.2));

    // Upstream returns linear Rec.2020. Return linear sRGB display values;
    // final SDR gamut clipping is distinct from the attachment's encoding.
    return saturate(displayRec2020ToSrgb(value));
}
