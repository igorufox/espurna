/*

The calls of the chip's SDK, the same names on every FreeRTOS chip: ESPurna calls them directly, lib/espurna-core (the
Arduino names, third-party libraries of the modules only) calls them as well. Implemented by the chip in
arch/<chip>/core_<chip>.cpp. No Arduino types: the pin modes and interrupt edges are plain numbers (the values of
ArduinoCore-API, so the layer passes its own ones as they are), a UART config is SERIAL_* of ArduinoCore-API as a number

*/

#pragma once

#include <cstddef>
#include <cstdint>

#include "wifi_sdk.h"

namespace espurna {
namespace core {
namespace chip {

// Time. micros() since boot, 64 bit
uint64_t micros();

// Sleep the calling task for `ms`, the other tasks and the chip timers keep running
void delay(uint32_t ms);

// Busy wait, for the bit-banged protocols (OneWire, DHT, HX711 ...)
void delay_us(uint32_t us);

// Let the other tasks of the same priority run
void yield();

// Before every loop() pass in the loop task (ESP32: the task watchdog of the loop, once enabled)
void loop_pass();

// All interrupts of the calling core off / back on (nested calls count)
void irq_disable();
void irq_enable();

// GPIO
constexpr uint8_t NoPin { 0xff };

namespace pin {

constexpr uint8_t Input { 0 };
constexpr uint8_t Output { 1 };
constexpr uint8_t InputPullup { 2 };
constexpr uint8_t InputPulldown { 3 };
constexpr uint8_t OutputOpenDrain { 4 };

} // namespace pin

namespace edge {

constexpr uint8_t Low { 0 };
constexpr uint8_t High { 1 };
constexpr uint8_t Change { 2 };
constexpr uint8_t Falling { 3 };
constexpr uint8_t Rising { 4 };

} // namespace edge

using PinHandler = void (*)(void*);

bool pin_valid(uint8_t pin);
void pin_mode(uint8_t pin, uint8_t mode);
void pin_write(uint8_t pin, bool level);
bool pin_read(uint8_t pin);

// Interrupt on an edge (Low / High level interrupts where the SDK has them)
bool pin_attach(uint8_t pin, PinHandler handler, void* arg, uint8_t edge);
void pin_detach(uint8_t pin);

// ADC, raw value of `bits` resolution
int adc_read(uint8_t pin);
uint8_t adc_bits();

// PWM: `duty` of `range` at `frequency` Hz, duty 0 stops the output; stop() gives the pin back as a plain GPIO.
// In the light_pwm module on every chip (arch/<chip>/pwm_<chip>.cpp), not in the head: the head and the other
// modules call it through chip::pwm_bus (pins::pwm(), the NSPanel's buzzer, analogWrite() of the Arduino emulation)
void pwm_write(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency);
void pwm_stop(uint8_t pin);

// the UART config: data bits | parity | stop bits, the SERIAL_* values of ArduinoCore-API
namespace serial {

constexpr uint16_t ParityEven { 0x1 };
constexpr uint16_t ParityOdd { 0x2 };
constexpr uint16_t ParityNone { 0x3 };
constexpr uint16_t ParityMask { 0xf };
constexpr uint16_t StopBit1 { 0x10 };
constexpr uint16_t StopBit15 { 0x20 };
constexpr uint16_t StopBit2 { 0x30 };
constexpr uint16_t StopBitMask { 0xf0 };
constexpr uint16_t Data5 { 0x100 };
constexpr uint16_t Data6 { 0x200 };
constexpr uint16_t Data7 { 0x300 };
constexpr uint16_t Data8 { 0x400 };
constexpr uint16_t DataMask { 0xf00 };

} // namespace serial

// UART. rx / tx NoPin = unused, both NoPin = the console UART on its default pins. open() picks the hardware port for
// the pins and returns its number, or -1
int uart_open(uint32_t baud, uint16_t config, uint8_t rx, uint8_t tx, bool invert, size_t rx_buffer);
void uart_close(int port);
void uart_baud(int port, uint32_t baud);
size_t uart_available(int port);
size_t uart_read(int port, uint8_t* out, size_t size);
int uart_peek(int port);
size_t uart_write_space(int port);
size_t uart_write(int port, const uint8_t* data, size_t size);
void uart_flush(int port);

// I2C master. write() returns 0 ok, 2 NACK on the address, 3 NACK on data, 4 other (endTransmission() of Wire)
bool i2c_begin(uint8_t sda, uint8_t scl, uint32_t frequency);
void i2c_end();
void i2c_clock(uint32_t frequency);
uint8_t i2c_write(uint8_t address, const uint8_t* data, size_t size, bool stop);
size_t i2c_read(uint8_t address, uint8_t* out, size_t size, bool stop);

// SPI master, mode 0..3, `in` may be null
bool spi_begin(uint8_t sck, uint8_t miso, uint8_t mosi);
void spi_end();
void spi_config(uint32_t frequency, uint8_t mode, bool msb_first);
void spi_transfer(const uint8_t* out, uint8_t* in, size_t size);

// The I2C and SPI masters above are in the i2c / spi modules on every chip, the PWM in light_pwm (arch/soft/i2c_soft.cpp, ESP32 arch/esp32/i2c_esp32.cpp,
// spi_<chip>.cpp), not in the head. The modules linked against them call them directly; Wire and SPI of the Arduino
// emulation (lib/espurna-core, the arduino_emu module, which needs neither bus) reach them through these tables, the
// module sets its table when it starts (null - the bus module is not there: nothing answers)
struct I2cBus {
    bool (*begin)(uint8_t sda, uint8_t scl, uint32_t frequency);
    void (*end)();
    void (*clock)(uint32_t frequency);
    uint8_t (*write)(uint8_t address, const uint8_t* data, size_t size, bool stop);
    size_t (*read)(uint8_t address, uint8_t* out, size_t size, bool stop);
};

struct SpiBus {
    bool (*begin)(uint8_t sck, uint8_t miso, uint8_t mosi);
    void (*end)();
    void (*config)(uint32_t frequency, uint8_t mode, bool msb_first);
    void (*transfer)(const uint8_t* out, uint8_t* in, size_t size);
};

struct PwmBus {
    void (*write)(uint8_t pin, uint32_t duty, uint32_t range, uint32_t frequency);
    void (*stop)(uint8_t pin);
};

// in the head (arch/rtos/bus_rtos.cpp)
extern const I2cBus* i2c_bus;
extern const SpiBus* spi_bus;
extern const PwmBus* pwm_bus;

// System
uint32_t heap_free();
uint32_t heap_max_alloc();
uint32_t flash_size();
uint32_t flash_mode();
uint32_t flash_speed();
uint32_t sketch_size();
uint32_t sketch_space();
uint32_t cycle_count();
uint32_t cpu_mhz();
const char* sdk_version();
const char* chip_model();
uint8_t chip_revision();
[[noreturn]] void restart();

#if defined(ESP8266)
// the ROM's UART download mode, as with GPIO0 low at a reset: esptool --before no-reset writes the flash (core_esp8266.cpp)
[[noreturn]] void rom_loader();
#endif

// the size of an image that is not known before its end
constexpr size_t OtaSizeUnknown { 0xFFFFFFFF };

// The image goes to the chip's update slot. begin() erases what it needs, write() takes the image in any parts, end()
// checks it and makes it the next boot. Each returns nullptr or the error text
const char* ota_begin(size_t size);
const char* ota_write(const uint8_t* data, size_t size);
const char* ota_end();
void ota_abort();

} // namespace chip

// The loop task body (lib/espurna-core main.cpp): setup(), then loop() for ever. Called by the chip entry
[[noreturn]] void run();

} // namespace core
} // namespace espurna
