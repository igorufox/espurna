/*

CRC-32 (IEEE 802.3, the one of zlib.crc32 and of the scripts), bitwise without a table: one version for every place
that needs it (the module packages and directory, modules.cpp; the Beken settings slots and the bootloader detection,
arch/beken). Plain C++, no framework.

    crc = crc32Update(0, data, size);       // a whole buffer
    crc = crc32Update(crc, more, size);     // continued

*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace espurna {
namespace crc32_ieee {

inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }

    return ~crc;
}

} // namespace crc32_ieee
} // namespace espurna
