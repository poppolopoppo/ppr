// Shared visible-band thermal emission. The one 2,048-row table is generated
// offline from the pinned CIE data by scripts/generate_thermal_lut.py and
// uploaded once; this helper never normalizes per temperature. Rows are
// log-spaced: Ti = 256*(65504/256)^(i/2047), 16-byte stride (linear RGB plus
// one zero padding word).

static const uint kThermalLutRows = 2048u;
static const float kThermalLutMinKelvin = 256.0;
static const float kThermalLutMaxKelvin = 65504.0;

StructuredBuffer<float4> g_thermal_lut;

// Decoded published half Kelvin in, engine-linear RGB out. T<=256 K is exactly
// zero: the omitted maximum RGB there is about 3.63e-35 engine units, far below
// the RGB9E5 minimum positive value. Invalid input is contained as zero; the
// publication boundary rejects it before GPU upload.
float3 thermalRGB(float kelvin) {
    if (!(kelvin > kThermalLutMinKelvin)) return float3(0.0);

    float q = float(kThermalLutRows - 1u) * log(kelvin / kThermalLutMinKelvin) /
              log(kThermalLutMaxKelvin / kThermalLutMinKelvin);
    q = clamp(q, 0.0, float(kThermalLutRows - 1u));

    uint lower = uint(floor(q));
    uint upper = min(lower + 1u, kThermalLutRows - 1u);
    float fraction = q - float(lower);

    float3 first = g_thermal_lut[lower].rgb;
    float3 second = g_thermal_lut[upper].rgb;
    return lerp(first, second, fraction);
}
