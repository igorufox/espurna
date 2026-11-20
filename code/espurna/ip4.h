/*

An IPv4 address of ESPurna's own code (owner 2026-11-18: without the Arduino layer), instead of the layer's IPAddress:
the u32 of lwIP (ip4_addr_t.addr, network order: the first octet is the lowest byte)

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {

class Ip4 {
public:
    constexpr Ip4() = default;

    // lwIP's u32 (network order)
    constexpr explicit Ip4(uint32_t value) :
        _value(value)
    {}

    // a.b.c.d
    constexpr Ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) :
        _value(static_cast<uint32_t>(a)
            | (static_cast<uint32_t>(b) << 8)
            | (static_cast<uint32_t>(c) << 16)
            | (static_cast<uint32_t>(d) << 24))
    {}

    constexpr uint32_t value() const {
        return _value;
    }

    constexpr explicit operator uint32_t() const {
        return _value;
    }

    // the octet `index` (0: the first, a. of a.b.c.d)
    constexpr uint8_t operator[](size_t index) const {
        return static_cast<uint8_t>(_value >> (8 * index));
    }

    // neither 0.0.0.0 nor 255.255.255.255
    constexpr bool isSet() const {
        return (_value != 0) && (_value != 0xffffffffu);
    }

    constexpr bool operator==(const Ip4& other) const {
        return _value == other._value;
    }

    constexpr bool operator!=(const Ip4& other) const {
        return _value != other._value;
    }

    // "a.b.c.d"; false - not one (the address stays as it was)
    bool fromString(const char* text);

    // "a.b.c.d" into `out` (16 bytes and more), terminated; the length
    size_t toString(char* out, size_t size) const;

private:
    uint32_t _value { 0 };
};

} // namespace espurna
