// The esp8266 core's <sys/pgmspace.h>, which the libraries' ESP8266 branch includes (BearSSL's bearssl_pgmspace.h).
// On the RTOS SDK the constants are read through plain pointers: lib/espurna-core's pgmspace.h (the Arduino API's),
// the same names as every other chip of ESPurna
#pragma once

#include <stdint.h>

#ifndef ICACHE_RODATA_ATTR
#define ICACHE_RODATA_ATTR
#endif

#if __has_include(<pgmspace.h>)
#include <pgmspace.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char *
#endif
#ifndef PGM_VOID_P
#define PGM_VOID_P const void *
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_float(addr) (*(const float *)(addr))
#define pgm_read_ptr(addr) (*(const void * const *)(addr))
#define memcpy_P memcpy
#define strlen_P strlen
#endif

// BearSSL's X.509 decoders compare constants with memcmp_P (the esp8266 core's)
#ifndef memcmp_P
#define memcmp_P memcmp
#endif

