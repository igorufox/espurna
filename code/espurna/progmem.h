/*

PROGMEM / PSTR and the _P string calls for ESPurna's own code (owner 2026-11-18: without the Arduino layer). The chips
map their flash for reading (the RTOS SDK's .rodata is in flash already, ESP32 and Beken read it as memory), so these
are the plain C calls. A library header of a module may define the same names first: those are kept (#ifndef)

*/

#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef PGM_P
#define PGM_P const char*
#endif

#ifndef PSTR
#define PSTR(s) (s)
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*reinterpret_cast<const uint8_t*>(addr))
#endif

#ifndef pgm_read_word
#define pgm_read_word(addr) (*reinterpret_cast<const uint16_t*>(addr))
#endif

#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*reinterpret_cast<const uint32_t*>(addr))
#endif

#ifndef pgm_read_float
#define pgm_read_float(addr) (*reinterpret_cast<const float*>(addr))
#endif

#ifndef pgm_read_ptr
#define pgm_read_ptr(addr) (*reinterpret_cast<const void* const*>(addr))
#endif

#ifndef strlen_P
#define strlen_P strlen
#endif

#ifndef strnlen_P
#define strnlen_P strnlen
#endif

#ifndef memcpy_P
#define memcpy_P memcpy
#endif

#ifndef memcmp_P
#define memcmp_P memcmp
#endif

#ifndef strcmp_P
#define strcmp_P strcmp
#endif

#ifndef strncmp_P
#define strncmp_P strncmp
#endif

#ifndef strcasecmp_P
#define strcasecmp_P strcasecmp
#endif

#ifndef strncasecmp_P
#define strncasecmp_P strncasecmp
#endif

#ifndef strcpy_P
#define strcpy_P strcpy
#endif

#ifndef strncpy_P
#define strncpy_P strncpy
#endif

#ifndef strstr_P
#define strstr_P strstr
#endif

#ifndef snprintf_P
#define snprintf_P snprintf
#endif

#ifndef sprintf_P
#define sprintf_P sprintf
#endif

#ifndef vsnprintf_P
#define vsnprintf_P vsnprintf
#endif

#ifndef printf_P
#define printf_P printf
#endif
