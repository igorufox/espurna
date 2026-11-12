/*

SENSOR DRIVER: DHT11 / DHT22 / AM2301 temperature and humidity
A module of its own (sensors/driver.h): needs the sensors module

*/

#if MODULE_BUILD

#include "driver.h"
#include "DHTSensor.h"

using namespace espurna::sensor::module;

namespace {

constexpr int dht_type_number(DHTChipType type) {
    return (type == DHTChipType::DHT11) ? 11 :
        (type == DHTChipType::DHT12) ? 12 :
        (type == DHTChipType::DHT21) ? 21 :
        (type == DHTChipType::AM2301) ? 2301 :
        (type == DHTChipType::SI7021) ? 7021 : 22;
}

DHTChipType dht_type(int number) {
    switch (number) {
    case 11:
        return DHTChipType::DHT11;
    case 12:
        return DHTChipType::DHT12;
    case 21:
        return DHTChipType::DHT21;
    case 2301:
        return DHTChipType::AM2301;
    case 7021:
        return DHTChipType::SI7021;
    }

    return DHTChipType::DHT22;
}

} // namespace

ESPURNA_SENSOR_DRIVER(dht, "dhtPin|gpio|GPIO|;dhtType|select|Type|22|11=DHT11,12=DHT12,21=DHT21,22=DHT22,2301=AM2301,7021=SI7021") {
    const auto pin = getSetting(F("dhtPin"), uint8_t{ DHT_PIN });
    if (pin_set(pin)) {
        auto* sensor = new DHTSensor();
        sensor->setGPIO(pin);
        sensor->setType(dht_type(getSetting(F("dhtType"), dht_type_number(DHT_TYPE))));
        sensorAdd(sensor);
    }
}

#endif // MODULE_BUILD
