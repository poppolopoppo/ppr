# Detects current platform for Hardware Abstraction Layer
if (WIN32)
    set(PPR_HAL_PLATFORM windows)
elseif(APPLE)
    set(PPR_HAL_PLATFORM darwin)
elseif(UNIX AND NOT APPLE)
    set(PPR_HAL_PLATFORM linux)
else()
    set(PPR_HAL_PLATFORM generic)
endif()

message(STATUS "HAL platform: ${PPR_HAL_PLATFORM}")
