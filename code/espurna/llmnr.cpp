/*

LLMNR MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>

Link-Local Multicast Name Resolution (RFC 4795): Windows asks 224.0.0.252:5355 for a name, the board answers the A
record of its hostname. One responder on every chip, on lwIP (libs/udp.h, as the captive portal's
DNS); the ESP8266 core's ESP8266LLMNR is gone, ESP32 and Beken never had one

*/

#include "espurna.h"

#if LLMNR_SUPPORT

#include "llmnr.h"

#include "libs/udp.h"

#include <cstring>

namespace llmnr {
namespace {

constexpr uint16_t Port { 5355 };
constexpr uint16_t TypeA { 1 };
constexpr uint16_t TypeAny { 255 };
constexpr uint16_t ClassIn { 1 };
constexpr uint32_t Ttl { 30 };

espurna::udp::Socket udp;
espurna::string hostname;

uint16_t be16(const uint8_t* at) {
    return static_cast<uint16_t>((at[0] << 8) | at[1]);
}

// the one question of a query: its name the hostname (one label, any case), A or ANY, IN. Its end in `end`
bool asks(const uint8_t* data, size_t len, size_t& end) {
    if (len < 12) {
        return false;
    }

    // a query (QR 0, opcode 0), one question, no answers
    if ((data[2] & 0xf8) || (be16(&data[4]) != 1) || be16(&data[6]) || be16(&data[8])) {
        return false;
    }

    size_t at = 12;
    const size_t label = data[at];
    if (!label || (label != hostname.length()) || ((at + 1 + label + 1 + 4) > len)) {
        return false;
    }

    if (strncasecmp(reinterpret_cast<const char*>(&data[at + 1]), hostname.c_str(), label) != 0) {
        return false;
    }

    at += 1 + label;
    if (data[at] != 0) {
        return false;
    }
    at += 1;

    const auto type = be16(&data[at]);
    const auto klass = be16(&data[at + 2]) & 0x7fff;
    end = at + 4;
    return ((type == TypeA) || (type == TypeAny)) && (klass == ClassIn);
}

void answer(espurna::udp::Packet& packet, size_t question_end) {
    const auto ip = wifiStaIp();
    if (!ip[0]) {
        return;
    }

    espurna::udp::Message message(question_end + 16);
    if (!message) {
        return;
    }

    const uint8_t* data = packet.data();
    // the header: the same id, a response (QR), one question and one answer
    const uint8_t header[12] { data[0], data[1], 0x80, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    message.write(header, sizeof(header));
    message.write(&data[12], question_end - 12);

    // the answer: the name of the question (a pointer to it), A, IN, the TTL, the address
    const uint8_t record[16] {
        0xc0, 0x0c,
        0x00, TypeA,
        0x00, ClassIn,
        static_cast<uint8_t>(Ttl >> 24), static_cast<uint8_t>(Ttl >> 16), static_cast<uint8_t>(Ttl >> 8),
        static_cast<uint8_t>(Ttl),
        0x00, 0x04,
        ip[0], ip[1], ip[2], ip[3]};
    message.write(record, sizeof(record));

    udp.sendTo(message, packet.remoteIP(), packet.remotePort());
}

} // namespace
} // namespace llmnr

void llmnrSetup() {
    llmnr::hostname = systemHostname();

    // lwIP's context: answered right there
    llmnr::udp.onPacket([](espurna::udp::Packet& packet) {
        size_t end;
        if (llmnr::asks(packet.data(), packet.length(), end)) {
            llmnr::answer(packet, end);
        }
    });

    if (!llmnr::udp.listen(llmnr::Port) || !llmnr::udp.joinGroup(espurna::udp::ip4(224, 0, 0, 252))) {
        DEBUG_MSG_P(PSTR("[LLMNR] Cannot listen on %u\n"), llmnr::Port);
        return;
    }

    DEBUG_MSG_P(PSTR("[LLMNR] Configured for %s\n"), llmnr::hostname.c_str());
}

#endif // LLMNR_SUPPORT
