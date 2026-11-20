/*

SENSOR DRIVER: Current clamps on ADS1015 / ADS1115 (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "EmonADS1X15Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(emon_ads1x15, "ads1x15Addr|i2c|Address (0 - find it)|0;ads1x15Type|select|Chip|1|0=ADS1015,1=ADS1115;ads1x15Mask|number|Channels (bits: 1 - A0, 2 - A1, 4 - A2, 8 - A3)|15|1-15") {
    if (i2c_bus()) {
        auto port = std::make_shared<EmonADS1X15Sensor::I2CPort>(
            getSetting("ads1x15Addr", uint8_t{ EMON_ADS1X15_I2C_ADDRESS }),
            getSetting("ads1x15Type", uint8_t{ EMON_ADS1X15_TYPE }),
            EMON_ADS1X15_GAIN, EMON_ADS1X15_DATARATE);

        unsigned char mask = getSetting("ads1x15Mask", uint8_t{ EMON_ADS1X15_MASK });
        unsigned char channel { 0 };

        while (mask) {
            if (mask & 1) {
                auto* sensor = new EmonADS1X15Sensor(port);
                sensor->setVoltage(EMON_MAINS_VOLTAGE);
                sensor->setChannel(channel);
                sensorAdd(sensor);
            }
            ++channel;
            mask >>= 1;
        }
    }
}

#endif // MODULE_BUILD
