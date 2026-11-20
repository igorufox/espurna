/*

BLE ADVERTISEMENTS OF THE SENSORS: what the ble_sensors driver reads out of the service data (AD type 0x16) of an
advertisement, the formats without a key (no encrypted payloads):

- ATC1441    UUID 0x181A, 13 bytes: MAC, temperature 0.1 °C (BE), humidity %, battery %, battery mV (BE), counter
             (the custom firmware of the Xiaomi LYWSD03MMC thermometers, atc1441 format)
- pvvx       UUID 0x181A, 15 bytes: MAC (LE), temperature 0.01 °C, humidity 0.01 %, battery mV, battery %, counter,
             flags (the same firmware, pvvx custom format)
- BTHome v2  UUID 0xFCD2: device info byte, then id + value objects (bthome.io); the pvvx firmware and many others
- MiBeacon   UUID 0xFE95: the Xiaomi stock firmware, the objects of the unencrypted frames (older thermometers,
             LYWSDCGQ, CGG1 ...)

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace ble {
namespace adv {

enum class Format : uint8_t {
    None,
    Atc1441,
    Pvvx,
    BTHome,
    MiBeacon,
};

struct Reading {
    Format format { Format::None };

    bool has_temperature { false };
    bool has_humidity { false };
    bool has_battery { false };
    bool has_voltage { false };

    float temperature { 0.0f };     // °C
    float humidity { 0.0f };        // %
    float battery { 0.0f };         // %
    float voltage { 0.0f };         // V

    void set_temperature(float value) {
        has_temperature = true;
        temperature = value;
    }

    void set_humidity(float value) {
        has_humidity = true;
        humidity = value;
    }

    void set_battery(float value) {
        has_battery = true;
        battery = value;
    }

    void set_voltage(float value) {
        has_voltage = true;
        voltage = value;
    }

    explicit operator bool() const {
        return has_temperature || has_humidity || has_battery || has_voltage;
    }
};

inline const char* format_name(Format format) {
    switch (format) {
    case Format::Atc1441:
        return "ATC1441";
    case Format::Pvvx:
        return "pvvx";
    case Format::BTHome:
        return "BTHome";
    case Format::MiBeacon:
        return "MiBeacon";
    case Format::None:
        break;
    }

    return "";
}

namespace detail {

inline uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t le24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline bool atc1441(const uint8_t* p, size_t len, Reading& out) {
    if (len != 13) {
        return false;
    }

    out.format = Format::Atc1441;
    out.set_temperature(static_cast<int16_t>(be16(&p[6])) / 10.0f);
    out.set_humidity(p[8]);
    out.set_battery(p[9]);
    out.set_voltage(be16(&p[10]) / 1000.0f);
    return true;
}

inline bool pvvx(const uint8_t* p, size_t len, Reading& out) {
    if (len != 15) {
        return false;
    }

    out.format = Format::Pvvx;
    out.set_temperature(static_cast<int16_t>(le16(&p[6])) / 100.0f);
    out.set_humidity(le16(&p[8]) / 100.0f);
    out.set_voltage(le16(&p[10]) / 1000.0f);
    out.set_battery(p[12]);
    return true;
}

// the size of the value of a BTHome v2 object, 0 when the id is not known (the rest of the frame can not be read)
inline size_t bthome_size(uint8_t id) {
    switch (id) {
    case 0x00: // packet id
    case 0x01: // battery %
    case 0x09: // count
    case 0x2e: // humidity %
    case 0x2f: // moisture %
    case 0x3a: // button
    case 0x46: // UV index
    case 0x57: // temperature °C, sint8
    case 0x58: // temperature 0.35 °C, sint8
    case 0x59: // count, sint8
    case 0x60: // channel
        return 1;
    case 0x02: // temperature 0.01 °C
    case 0x03: // humidity 0.01 %
    case 0x06: // mass kg
    case 0x07: // mass lb
    case 0x08: // dew point
    case 0x0c: // voltage 0.001 V
    case 0x0d: // pm2.5
    case 0x0e: // pm10
    case 0x12: // co2
    case 0x13: // tvoc
    case 0x14: // moisture 0.01 %
    case 0x3c: // dimmer
    case 0x3d: // count
    case 0x3f: // rotation
    case 0x40: // distance mm
    case 0x41: // distance m
    case 0x43: // current
    case 0x44: // speed
    case 0x45: // temperature 0.1 °C
    case 0x47: // volume
    case 0x48: // volume
    case 0x49: // volume flow rate
    case 0x4a: // voltage 0.1 V
    case 0x51: // acceleration
    case 0x52: // gyroscope
    case 0x5a: // count, sint16
    case 0x5d: // current, sint16
    case 0x5e: // direction
    case 0x5f: // precipitation
    case 0x61: // rotational speed
    case 0xf0: // device type id
        return 2;
    case 0x04: // pressure
    case 0x05: // illuminance
    case 0x0a: // energy
    case 0x0b: // power
    case 0x42: // duration
    case 0x4b: // gas
    case 0xf2: // firmware version, 3 bytes
        return 3;
    case 0x3e: // count
    case 0x4c: // gas
    case 0x4d: // energy
    case 0x4e: // volume
    case 0x4f: // water
    case 0x50: // timestamp
    case 0x5b: // count, sint32
    case 0x5c: // power, sint32
    case 0xf1: // firmware version, 4 bytes
        return 4;
    }

    // the binary sensors, one byte each
    if ((id >= 0x0f) && (id <= 0x11)) {
        return 1;
    }

    if ((id >= 0x15) && (id <= 0x2d)) {
        return 1;
    }

    return 0;
}

inline bool bthome(const uint8_t* p, size_t len, Reading& out) {
    if (len < 1) {
        return false;
    }

    const uint8_t info = p[0];
    if ((info & 0x01) || (((info >> 5) & 0x07) != 2)) {
        return false; // encrypted, or not v2
    }

    out.format = Format::BTHome;

    size_t index = 1;
    while (index < len) {
        const uint8_t id = p[index++];

        // text and raw: a length byte of their own
        if ((id == 0x53) || (id == 0x54)) {
            if (index >= len) {
                break;
            }
            index += 1 + p[index];
            continue;
        }

        const size_t size = bthome_size(id);
        if (!size || ((index + size) > len)) {
            break;
        }

        const uint8_t* value = &p[index];
        switch (id) {
        case 0x01:
            out.set_battery(value[0]);
            break;
        case 0x02:
            out.set_temperature(static_cast<int16_t>(le16(value)) / 100.0f);
            break;
        case 0x45:
            out.set_temperature(static_cast<int16_t>(le16(value)) / 10.0f);
            break;
        case 0x57:
            out.set_temperature(static_cast<int8_t>(value[0]));
            break;
        case 0x58:
            out.set_temperature(static_cast<int8_t>(value[0]) * 0.35f);
            break;
        case 0x03:
            out.set_humidity(le16(value) / 100.0f);
            break;
        case 0x2e:
            out.set_humidity(value[0]);
            break;
        case 0x0c:
            out.set_voltage(le16(value) / 1000.0f);
            break;
        case 0x4a:
            out.set_voltage(le16(value) / 10.0f);
            break;
        }

        index += size;
    }

    return static_cast<bool>(out);
}

inline bool mibeacon(const uint8_t* p, size_t len, Reading& out) {
    if (len < 5) {
        return false;
    }

    const uint16_t control = le16(&p[0]);
    if ((control & 0x0008) || !(control & 0x0040)) {
        return false; // encrypted, or no objects
    }

    size_t index = 5; // frame control, product id, frame counter
    if (control & 0x0010) {
        index += 6; // MAC
    }

    if (control & 0x0020) {
        if (index >= len) {
            return false;
        }

        const uint8_t capability = p[index++];
        if (capability & 0x20) {
            index += 2; // I/O capability
        }
    }

    out.format = Format::MiBeacon;

    while ((index + 3) <= len) {
        const uint16_t id = le16(&p[index]);
        const uint8_t size = p[index + 2];
        index += 3;
        if ((index + size) > len) {
            break;
        }

        const uint8_t* value = &p[index];
        switch (id) {
        case 0x1004:
            if (size == 2) {
                out.set_temperature(static_cast<int16_t>(le16(value)) / 10.0f);
            }
            break;
        case 0x1006:
            if (size == 2) {
                out.set_humidity(le16(value) / 10.0f);
            }
            break;
        case 0x100a:
            if (size >= 1) {
                out.set_battery(value[0]);
            }
            break;
        case 0x100d:
            if (size == 4) {
                out.set_temperature(static_cast<int16_t>(le16(value)) / 10.0f);
                out.set_humidity(le16(&value[2]) / 10.0f);
            }
            break;
        }

        index += size;
    }

    return static_cast<bool>(out);
}

} // namespace detail

// the readings of the service data of an advertisement (the AD structures of a legacy advertising PDU)
inline Reading parse(const uint8_t* data, size_t length) {
    Reading out;

    size_t index = 0;
    while ((index + 1) < length) {
        const size_t size = data[index];
        if (!size || ((index + 1 + size) > length)) {
            break;
        }

        const uint8_t type = data[index + 1];
        const uint8_t* value = &data[index + 2];
        const size_t value_size = size - 1;

        if ((type == 0x16) && (value_size >= 2)) {
            const uint16_t uuid = detail::le16(value);
            const uint8_t* payload = value + 2;
            const size_t payload_size = value_size - 2;

            bool done = false;
            switch (uuid) {
            case 0x181a:
                done = detail::atc1441(payload, payload_size, out)
                    || detail::pvvx(payload, payload_size, out);
                break;
            case 0xfcd2:
                done = detail::bthome(payload, payload_size, out);
                break;
            case 0xfe95:
                done = detail::mibeacon(payload, payload_size, out);
                break;
            }

            if (done) {
                break;
            }
        }

        index += 1 + size;
    }

    return out;
}

// the name of the device in an advertisement (complete or shortened local name), empty when it has none
inline size_t name(const uint8_t* data, size_t length, const char** out) {
    size_t index = 0;
    while ((index + 1) < length) {
        const size_t size = data[index];
        if (!size || ((index + 1 + size) > length)) {
            break;
        }

        const uint8_t type = data[index + 1];
        if ((type == 0x08) || (type == 0x09)) {
            *out = reinterpret_cast<const char*>(&data[index + 2]);
            return size - 1;
        }

        index += 1 + size;
    }

    return 0;
}

} // namespace adv
} // namespace ble
} // namespace espurna
