// Explicit experimental Vulkan module. The ordinary module never includes FP64
// or RTE64 code, preserving its existing backend and feature requirements.
#define PPR_TERRAIN_FP64_REFINEMENT 1
#include "radiance_cascades.slang"
