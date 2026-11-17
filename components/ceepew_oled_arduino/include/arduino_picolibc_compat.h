#pragma once

#ifdef __cplusplus
#include <cmath>

// GCC 15 / C++26 + picolibc compatibility for arduino-esp32:
// In Arduino.h, `using std::isnan; using std::isinf;` is skipped under __PICOLIBC__.
// On GCC 15 / C++26, <cmath> undefines the C macros. On toolchains where picolibc's
// math.h omits C-linkage isnan/isinf declarations in C++ mode (e.g. GCC 15.3 in CI),
// unqualified calls in Print.cpp fail to resolve.
// Providing function templates in the global namespace resolves calls to std::isnan
// when no global non-template overload exists, while gracefully deferring to any
// existing non-template declaration without signature conflicts.
#ifndef isnan
template<typename T>
inline bool isnan(T x) {
    return std::isnan(x);
}
#endif

#ifndef isinf
template<typename T>
inline bool isinf(T x) {
    return std::isinf(x);
}
#endif

#endif
