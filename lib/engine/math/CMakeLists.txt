add_library(EngineMath)

target_sources(EngineMath
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Math.cppm
)

setup_ppr_project(EngineMath
  INTERNAL_PUBLIC_DEPS EngineCore
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango
)

# Mango's SIMD flags (e.g. -mavx2) are PUBLIC on the mango target,
# so consumers of EngineMath need them too for module PCM consistency.
target_link_libraries(EngineMath PUBLIC mango)
