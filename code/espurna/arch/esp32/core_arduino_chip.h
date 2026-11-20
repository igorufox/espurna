/*

What the libraries take from Arduino-ESP32 3.x besides the Arduino API, ESP32 only (lib/espurna-core Arduino.h
includes it when the chip has one): FreeRTOS and the ESP-IDF version in every Arduino.h, the log_x() macros, the
RMT (rmt*, Adafruit NeoPixel's output). The same
signatures as Arduino-ESP32 3.3, on ESP-IDF in core_esp32.cpp

*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

#include <esp_attr.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <sdkconfig.h>

// ets_delay_us() and the other ROM functions of the core's Arduino.h
#include <esp32/rom/ets_sys.h>

// esp32-hal-gpio.h: whether the number is a GPIO of the chip (OneWire)
#include <driver/gpio.h>
#define digitalPinIsValid(pin) GPIO_IS_VALID_GPIO(pin)

// the ESP8266 names of the code placement attributes, Arduino-ESP32's esp8266-compat.h
#define ICACHE_FLASH_ATTR
#define ICACHE_RAM_ATTR IRAM_ATTR

// the CPU clock of the board, Arduino-ESP32's F_CPU (sdkconfig.defaults: 240 MHz)
#ifndef F_CPU
#define F_CPU (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL)
#endif

// the API generation of Arduino-ESP32 the libraries pick their code by (Adafruit NeoPixel: the 3.x RMT)
#define ESP_ARDUINO_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))
#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 12
#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(3, 3, 12)

#define log_e(format, ...) ESP_LOGE("arduino", format, ##__VA_ARGS__)
#define log_w(format, ...) ESP_LOGW("arduino", format, ##__VA_ARGS__)
#define log_i(format, ...) ESP_LOGI("arduino", format, ##__VA_ARGS__)
#define log_d(format, ...) ESP_LOGD("arduino", format, ##__VA_ARGS__)
#define log_v(format, ...) ESP_LOGV("arduino", format, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

// RMT transmit (RMT TX channel with the copy encoder of ESP-IDF), one channel per pin
typedef union {
    struct {
        uint32_t duration0 : 15;
        uint32_t level0 : 1;
        uint32_t duration1 : 15;
        uint32_t level1 : 1;
    };
    uint32_t val;
} rmt_data_t;

typedef enum {
    RMT_RX_MODE = 0,
    RMT_TX_MODE = 1,
} rmt_ch_dir_t;

typedef enum {
    RMT_MEM_NUM_BLOCKS_1 = 1,
    RMT_MEM_NUM_BLOCKS_2 = 2,
    RMT_MEM_NUM_BLOCKS_3 = 3,
    RMT_MEM_NUM_BLOCKS_4 = 4,
} rmt_reserve_memsize_t;

#define RMT_WAIT_FOR_EVER ((uint32_t)portMAX_DELAY)

bool rmtInit(int pin, rmt_ch_dir_t direction, rmt_reserve_memsize_t memsize, uint32_t frequency);
bool rmtWrite(int pin, rmt_data_t* data, size_t symbols, uint32_t timeout_ms);
bool rmtDeinit(int pin);

#ifdef __cplusplus
} // extern "C"
#endif
