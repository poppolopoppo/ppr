#pragma once

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif

#include <GLFW/glfw3.h>

#if defined(_WIN32) || defined(__APPLE__)
#include <GLFW/glfw3native.h>
#endif
