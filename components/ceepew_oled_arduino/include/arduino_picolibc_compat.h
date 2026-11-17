#pragma once

#if defined(__cplusplus) && defined(__PICOLIBC__)
#include <cmath>

// GCC 15 + picolibc compatibility for arduino-esp32:
// Arduino.h skips `using std::isnan; using std::isinf;` when __PICOLIBC__ is
// defined, relying on picolibc math.h macros (e.g. __builtin_isnan/__builtin_isinf).
// However, in GCC 15 / C++26 libstdc++'s <cmath> undefines these C macros.
// When Print.cpp calls unqualified isnan(number) and isinf(number), compilation fails.
// We restore the __builtin macros for the arduino-esp32 component under picolibc.
#ifndef isnan
#define isnan(x) __builtin_isnan(x)
#endif

#ifndef isinf
#define isinf(x) __builtin_isinf(x)
#endif

#endif
