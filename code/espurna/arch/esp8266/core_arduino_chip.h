// What the esp8266 Arduino core gave the libraries besides the API (lib/espurna-core Arduino.h includes it): the names
// the libraries' ESP8266 branches use (custom_module_lib_flags of platformio_esp8266.ini gives them ESP8266)
#pragma once

// va_list / va_start: the esp8266 core's Arduino.h has them (OLEDDisplay::printf)
#include <stdarg.h>

#include <esp_attr.h>
#include <esp8266/eagle_soc.h>
#include <esp8266/gpio_register.h>

// code an interrupt runs, in IRAM (a module's runs from flash: no IRAM pool, as on ESP32)
#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR IRAM_ATTR
#endif

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

// code in flash: the default of the RTOS SDK
#ifndef ICACHE_FLASH_ATTR
#define ICACHE_FLASH_ATTR
#endif

// os_delay_us(): the libraries declare it themselves (my92xx: unsigned int, the SDK's rom/ets_sys.h: word), the
// ROM function either way; rom/ets_sys.h comes with user_interface.h of the libraries that include it

// the interrupt lock of the NONOS SDK (IRremoteESP8266): the layer's noInterrupts() / interrupts()
#ifdef __cplusplus
extern "C" {
#endif
void noInterrupts(void);
void interrupts(void);
#ifdef __cplusplus
}
#endif

#ifndef ets_intr_lock
#define ets_intr_lock() noInterrupts()
#define ets_intr_unlock() interrupts()
#endif

#ifndef os_intr_lock
#define os_intr_lock() noInterrupts()
#define os_intr_unlock() interrupts()
#endif

// the GPIO registers by the esp8266 core's names (its esp8266_peri.h), the ones the libraries' direct pin code uses
// (OneWire): GPIO 0..15 at 0x60000300, GPIO 16 in the RTC block
#define ESP8266_REG(addr) *((volatile uint32_t *)(0x60000000 + (addr)))

#define GPO ESP8266_REG(0x300)
#define GPOS ESP8266_REG(0x304)
#define GPOC ESP8266_REG(0x308)
#define GPE ESP8266_REG(0x30C)
#define GPES ESP8266_REG(0x310)
#define GPEC ESP8266_REG(0x314)
#define GPI ESP8266_REG(0x318)

#define GP16O ESP8266_REG(0x768)
#define GP16E ESP8266_REG(0x774)
#define GP16I ESP8266_REG(0x78C)
#define GPC16 ESP8266_REG(0x790)
#define GPF16 ESP8266_REG(0x7A0)

// the function of a pin's mux: 0 (GPIO for 0, 2, 4, 5), 1 (GPIO16), 3 (GPIO for the others)
#define GPFFS_GPIO(p) ((((p) == 0) || ((p) == 2) || ((p) == 4) || ((p) == 5)) ? 0 : ((p) == 16) ? 1 : 3)
#define GP16FFS(f) (((f) & 0x03) | (((f) & 0x04) << 4))

// a bit of a register, the esp8266 core's Arduino.h has it (Adafruit NeoPixel's esp8266.c)
#ifndef _BV
#define _BV(b) (1UL << (b))
#endif

#ifdef __cplusplus
extern "C" {
#endif
// BearSSL's ESP8266 branch yields from its long computations to the esp8266 core's second stack (stack thunk); the
// RTOS SDK runs TLS on the loop task's own stack, nothing to yield to (core_esp8266.cpp)
void stack_thunk_yield(void);
#ifdef __cplusplus
}
#endif

