/*

SENSOR DRIVER: BMP280 / BME280 pressure, temperature, humidity (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "BMX280Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(bmx280, "bmx280Number|select|Sensors|0|0=None,1=One,2=Two;bmx280Address|i2c|Address of the first (0 - find it)|0") {
    if (i2c_bus()) {
        // up to two sensors with full auto-discovery, 0 sensors means off
        const auto number = std::clamp(getSetting("bmx280Number", BMX280_NUMBER), 0, 2);

        // for the second sensor, if the first address is 0x00 then auto-discover, otherwise the other address
        static constexpr uint8_t Address { BMX280_ADDRESS };
        const uint8_t first = getSetting("bmx280Address", Address);
        const uint8_t second = (first == 0x00) ? 0x00 : (0x76 + 0x77 - first);

        const uint8_t address_map[2] { first, second };
        for (int n = 0; n < number; ++n) {
            auto* sensor = new BMX280Sensor();
            sensor->setAddress(address_map[n]);
            sensorAdd(sensor);
        }
    }
}

#endif // MODULE_BUILD
