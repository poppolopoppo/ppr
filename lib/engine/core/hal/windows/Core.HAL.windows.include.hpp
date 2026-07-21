// Common Windows HAL preamble — included in the global module fragment
// of each Core.HAL.windows.*.cpp file.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif

#define NOGDICAPMASKS
#define NOATOM
#define NODRAWTEXT
#define NOKERNEL
#define NOMEMMGR
#define NOMETAFILE
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOCOMM
#define NOKANJI
#define NOHELP
#ifdef NDEBUG
#   define NOPROFILER
#endif
#define NODEFERWINDOWPOS
#define NOMCX
#define NOCRYPT
#define NOTAPE
#define NOIMAGE
#define NOPROXYSTUB
#define NORPC

#include <Windows.h>

#undef CreateProcess
#undef CreateSemaphore
#undef CreateWindow
#undef MemoryBarrier
#undef MoveFile
#undef RegisterClass
#undef RemoveDirectory
#undef Yield
#undef small
#undef min
#undef max

#include "pP/Macros.h"
