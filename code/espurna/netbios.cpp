/*

NETBIOS MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

The NetBIOS name service (RFC 1002): a name query for the hostname on UDP 137 (broadcast) gets the address of the
board. One responder on every chip, on lwIP (EspurnaUdp of lib/DNSServer); the ESP8266 core's ESP8266NetBIOS is gone,
ESP32 and Beken never had one

*/

#include "espurna.h"

#if NETBIOS_SUPPORT

#include "netbios.h"

#include <EspurnaUdp.h>

#include <cctype>
#include <cstring>

namespace netbios {
namespace {

constexpr uint16_t Port { 137 };
constexpr uint8_t TypeNb { 0x20 };
constexpr uint32_t Ttl { 300000 };

EspurnaUdp udp;

// the first level encoding of the hostname (upper case, 15 characters padded with spaces, the suffix 0x00 of a
// workstation): 32 letters of the nibbles + 'A'
uint8_t encoded[32];

void encode(const String& hostname) {
    uint8_t name[16];
    std::memset(name, ' ', 15);
    name[15] = 0x00;
    for (size_t index = 0; (index < hostname.length()) && (index < 15); ++index) {
        name[index] = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(hostname[index])));
    }

    for (size_t index = 0; index < 16; ++index) {
        encoded[index * 2] = static_cast<uint8_t>('A' + (name[index] >> 4));
        encoded[(index * 2) + 1] = static_cast<uint8_t>('A' + (name[index] & 0x0f));
    }
}

// a name query (opcode 0, not a response) for our name, type NB, class IN
bool asks(const uint8_t* data, size_t len) {
    if (len < (12 + 34 + 4)) {
        return false;
    }

    if ((data[2] & 0xf8) || (data[4] != 0) || (data[5] != 1)) {
        return false;
    }

    if ((data[12] != 32) || (data[12 + 33] != 0)) {
        return false;
    }

    if (std::memcmp(&data[13], encoded, sizeof(encoded)) != 0) {
        return false;
    }

    return (data[12 + 34] == 0x00) && (data[12 + 35] == TypeNb) && (data[12 + 36] == 0x00) && (data[12 + 37] == 0x01);
}

void answer(EspurnaUdpPacket& packet) {
    const auto ip = WiFi.localIP();
    if (!ip[0]) {
        return;
    }

    EspurnaUdpMessage message(12 + 34 + 16);
    if (!message) {
        return;
    }

    const uint8_t* data = packet.data();
    // the same id, a response, authoritative, no recursion; one answer
    const uint8_t header[12] { data[0], data[1], 0x85, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    message.write(header, sizeof(header));
    message.write(&data[12], 34);

    const uint8_t record[16] {
        0x00, TypeNb, 0x00, 0x01,
        static_cast<uint8_t>(Ttl >> 24), static_cast<uint8_t>(Ttl >> 16), static_cast<uint8_t>(Ttl >> 8),
        static_cast<uint8_t>(Ttl),
        0x00, 0x06,
        0x00, 0x00,     // NB flags: a B node, unique name
        ip[0], ip[1], ip[2], ip[3]};
    message.write(record, sizeof(record));

    udp.sendTo(message, packet.remoteIP(), packet.remotePort());
}

} // namespace
} // namespace netbios

void netbiosSetup() {
    const auto hostname = systemHostname();
    netbios::encode(hostname);

    // lwIP's context: answered right there
    netbios::udp.onPacket([](EspurnaUdpPacket& packet) {
        if (netbios::asks(packet.data(), packet.length())) {
            netbios::answer(packet);
        }
    });

    if (!netbios::udp.listen(netbios::Port)) {
        DEBUG_MSG_P(PSTR("[NETBIOS] Cannot listen on %u\n"), netbios::Port);
        return;
    }

    DEBUG_MSG_P(PSTR("[NETBIOS] Configured for %s\n"), hostname.c_str());
}

#endif // NETBIOS_SUPPORT
