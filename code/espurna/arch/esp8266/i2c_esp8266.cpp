/*

I2C MODULE, ESP8266 on the RTOS SDK: chip::i2c_* of arch/rtos/chip_sdk.h on the software I2C master of the SDK.
In the i2c module only (custom_modules of platformio_esp8266.ini), the head has no I2C

*/

#include "espurna.h"

#if I2C_SUPPORT

#include <chip_sdk.h>

#include <freertos/FreeRTOS.h>

#include <driver/i2c.h>

#include <string.h>

namespace espurna {
namespace core {
namespace esp8266 {
namespace {

// -----------------------------------------------------------------------------
// I2C master: the software master of the SDK, the command link (any write / read / restart sequence, like Wire needs)
// -----------------------------------------------------------------------------

constexpr i2c_port_t I2cPort { I2C_NUM_0 };
constexpr TickType_t I2cTimeout { pdMS_TO_TICKS(50) };

bool i2c_started { false };
uint8_t i2c_sda { 0 };
uint8_t i2c_scl { 0 };

// write() without the stop: the bytes go out with the read that follows (repeated start)
uint8_t i2c_pending_address { 0 };
uint8_t i2c_pending[128];
size_t i2c_pending_size { 0 };

uint8_t i2c_error(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_FAIL:
        // a NACK; the driver does not say of what, the address is the usual one (a probe)
        return 2;
    case ESP_ERR_TIMEOUT:
        return 5;
    }

    return 4;
}

// clock stretching ticks of the software master for the frequency (the SDK examples: 300 at 80 MHz CPU)
uint32_t i2c_stretch(uint32_t) {
    return 300;
}

} // namespace
} // namespace esp8266

namespace chip {

using namespace esp8266;

bool i2c_begin(uint8_t sda, uint8_t scl, uint32_t frequency) {
    i2c_end();

    if (i2c_driver_install(I2cPort, I2C_MODE_MASTER) != ESP_OK) {
        return false;
    }

    i2c_config_t config{};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = static_cast<gpio_num_t>(sda);
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_io_num = static_cast<gpio_num_t>(scl);
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.clk_stretch_tick = i2c_stretch(frequency);
    if (i2c_param_config(I2cPort, &config) != ESP_OK) {
        i2c_driver_delete(I2cPort);
        return false;
    }

    i2c_sda = sda;
    i2c_scl = scl;
    i2c_started = true;
    return true;
}

void i2c_end() {
    if (i2c_started) {
        i2c_driver_delete(I2cPort);
        i2c_started = false;
    }

    i2c_pending_size = 0;
}

// the software master runs at what the CPU gives it
void i2c_clock(uint32_t) {
}

uint8_t i2c_write(uint8_t address, const uint8_t* data, size_t size, bool stop) {
    if (!i2c_started) {
        return 4;
    }

    if (!stop) {
        i2c_pending_address = address;
        i2c_pending_size = (size < sizeof(i2c_pending)) ? size : sizeof(i2c_pending);
        memcpy(i2c_pending, data, i2c_pending_size);
        return 0;
    }

    auto cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(address << 1) | I2C_MASTER_WRITE, true);
    if (size) {
        i2c_master_write(cmd, const_cast<uint8_t*>(data), size, true);
    }
    i2c_master_stop(cmd);

    const auto err = i2c_master_cmd_begin(I2cPort, cmd, I2cTimeout);
    i2c_cmd_link_delete(cmd);

    return i2c_error(err);
}

size_t i2c_read(uint8_t address, uint8_t* out, size_t size, bool) {
    if (!i2c_started || !size) {
        return 0;
    }

    auto cmd = i2c_cmd_link_create();
    if (i2c_pending_size && (i2c_pending_address == address)) {
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, static_cast<uint8_t>(address << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write(cmd, i2c_pending, i2c_pending_size, true);
    }
    i2c_pending_size = 0;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>(address << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, out, size, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);

    const auto err = i2c_master_cmd_begin(I2cPort, cmd, I2cTimeout);
    i2c_cmd_link_delete(cmd);

    return (err == ESP_OK) ? size : 0;
}

} // namespace chip
} // namespace core
} // namespace espurna

#endif
