/*

HOST: the chip part of compat.h for the unit tests (code/test/unit), in place of arch/rtos/compat_chip.h. Only what
the tested sources of ESPurna take from a chip: no SDK, no Arduino

*/

#pragma once

#include <cstdint>
#include <cstring>
#include <strings.h>

// the owning text of ESPurna (text_types.h), declared here: compat.h comes before types.h
namespace espurna {
class string;
} // namespace espurna

#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef memcmp_P
#define memcmp_P memcmp
#endif
#ifndef strnlen_P
#define strnlen_P strnlen
#endif
#ifndef strncasecmp_P
#define strncasecmp_P strncasecmp
#endif
#ifndef strcasecmp_P
#define strcasecmp_P strcasecmp
#endif
#ifndef strcmp_P
#define strcmp_P strcmp
#endif

#define PROGMEM_STRING(NAME, X) static constexpr char NAME[] = (X)
#define STRING_VIEW(X) ::espurna::StringView(X)
#define STRING_VIEW_INLINE(NAME, X)\
        static constexpr char __pstr__ ## NAME ##  __ [] = (X);\
        static constexpr auto NAME = ::espurna::StringView(__pstr__ ## NAME ## __)

#define TZ_Etc_UTC "UTC0"

// the chip interface (arch/rtos/chip_sdk.h), host/host.cpp: the clock of the host, the cycle counter at 1 MHz
#include <chip_sdk.h>

#define F_CPU 1000000UL

inline uint32_t esp_get_cycle_count() {
    return static_cast<uint32_t>(::espurna::core::chip::micros());
}

void ets_delay_us(uint32_t);
