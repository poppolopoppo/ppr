// Area-preserving reconstruction: integral stays pi*r*r and the isolated
// isocontour crosses the physical radius r as support grows. Shared by splat
// geometry, kernel accumulation, optical height and conservative visibility.
static const float kLiquidSupport = 3.0;
static const float kLiquidAmplitude = 4.0 / (kLiquidSupport * kLiquidSupport);
static const float kLiquidRadiusLevel = 1.0 - 1.0 / (kLiquidSupport * kLiquidSupport);
static const float kLiquidIsoLevel = kLiquidAmplitude * kLiquidRadiusLevel * kLiquidRadiusLevel * kLiquidRadiusLevel;
