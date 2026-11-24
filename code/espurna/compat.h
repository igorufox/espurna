/*

COMPATIBILITY: C++ backports for the toolchains, then the chip part
(arch/esp8266/compat_chip.h, arch/rtos/compat_chip.h)

*/

#pragma once

#include "progmem.h"

// ------------------------------------------------------------------------------
// Arduino.h hijacks several useful names
// ------------------------------------------------------------------------------

#undef min
#undef max
#undef _min
#undef _max
#undef bit
#undef word
#undef constrain

// -----------------------------------------------------------------------------
// various backports for C++11, since we still use it with gcc v4.8
// -----------------------------------------------------------------------------

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

#if __cplusplus >= 201806L
#include <bit>
#endif

namespace std {

#if !defined(__cpp_lib_remove_cvref) && (__cplusplus < 202002L)
template <typename T>
using remove_cvref = typename std::remove_cv<std::remove_reference<T>>::type;
#endif

#if __cplusplus < 201304L
template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
#endif

#if __cplusplus < 201411L
template <typename T, size_t Size>
constexpr size_t size(const T (&)[Size]) {
    return Size;
}

template <typename T>
constexpr size_t size(const T& value) {
    return value.size();
}

template <typename T>
constexpr auto cbegin(const T& value) -> decltype(std::begin(value)) {
    return std::begin(value);
}

template <typename T>
constexpr auto cend(const T& value) -> decltype(std::end(value)) {
    return std::end(value);
}
#endif

#if __cplusplus < 201603L
template <typename T>
constexpr const T& clamp(const T& value, const T& low, const T& high) {
    return (value < low) ? low : (high < value) ? high : value;
}
#endif

#if __cplusplus < 201806L
template <typename To, typename From>
constexpr To bit_cast(const From& src) noexcept {
    static_assert(sizeof(From) == sizeof(To), "");
    static_assert(std::is_trivially_copyable<From>::value, "");
    static_assert(std::is_trivially_copyable<To>::value, "");

    To dst;
    __builtin_memcpy(&dst, &src, sizeof(dst));

    return dst;
}
#endif

#if __cplusplus < 202102L
template <typename Enum, typename Type = typename std::underlying_type<Enum>::type>
constexpr Type to_underlying(Enum value) {
    return static_cast<Type>(value);
}
#endif

} // namespace std

// An allocation that fails: std::bad_alloc where the build has exceptions (ESP32, Beken), the abort of the core where
// it has none (ESP8266, -fno-exceptions). What the compiler has, not the chip: the code checks the heap before it
// allocates much (nspanel heapAvailable()), the catch is the last resort where there is one
#if __cpp_exceptions
#define ESPURNA_TRY try
#define ESPURNA_CATCH_BAD_ALLOC catch (const std::bad_alloc&)
#else
#define ESPURNA_TRY if (true)
#define ESPURNA_CATCH_BAD_ALLOC else
#endif

// constexpr inline not always available
#ifdef __cpp_inline_variables
#define CONSTEXPR_INLINE constexpr inline
#else
#define CONSTEXPR_INLINE static constexpr
#endif

// -----------------------------------------------------------------------------
// What the esp8266 Core lacks, and the esp8266 Core API shims of the other chips
// -----------------------------------------------------------------------------

// the chip's: arch/esp8266/compat_chip.h, arch/rtos/compat_chip.h (ESP32, Beken), by the -I of the chip's ini
#include "compat_chip.h"

