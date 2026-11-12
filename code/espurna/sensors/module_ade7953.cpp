/*

SENSOR DRIVER: ADE7953 power meter, two channels (I2C)
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "ADE7953Sensor.h"

using namespace espurna::sensor::module;

ESPURNA_SENSOR_DRIVER(ade7953, "ade7953Addr|i2c|Address (0 - off)|0;ade7953CurrThreshold|number|Current threshold|2000") {
    using namespace espurna::sensor::driver::ade7953;
    if (i2c_bus() && settings::address()) {
        auto values = std::make_shared<CommonValues>();
        values->line_cycles = settings::lineCycles();
        values->current_threshold = settings::currentThreshold();

        auto port = std::make_shared<I2CPort>(settings::address());

        sensorAdd(new Common(values, port));
        sensorAdd(new Channel('A', RegistersA, values, port));
        sensorAdd(new Channel('B', RegistersB, values, port));
    }
}

#endif // MODULE_BUILD
