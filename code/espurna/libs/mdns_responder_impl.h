/*

MDNS RESPONDER - the implementation (libs/mdns_responder.h), included by mdns.cpp alone. Needs, defined before:
    MDNS_LOCK           the lock between lwIP's context and the loop / other tasks (lock(), unlock())
    MDNS_LOG(fmt, ...)  one log line (fmt a string literal)

Every lwIP call runs in lwIP's context (libs/lwip_call.h), never under MDNS_LOCK: the receive callback is in lwIP's
context already and takes the lock itself. The addresses of the interface are a copy, refreshed by the loop.

*/

#pragma once

#include "mdns_responder.h"
#include "lwip_call.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include <lwip/opt.h>
#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <lwip/igmp.h>
#include <lwip/ip.h>
#if LWIP_IPV6
#include <lwip/mld6.h>
#include <lwip/ip6_addr.h>
#endif

namespace espurna {
namespace mdns {
namespace responder {
namespace {

constexpr uint16_t Port { 5353 };
constexpr size_t PacketMax { 1440 };
constexpr uint32_t HostTtl { 120 };
constexpr uint32_t OtherTtl { 4500 };
constexpr uint32_t LegacyTtl { 10 };
constexpr uint8_t Announcements { 3 };
constexpr uint32_t AnnounceInterval { 1000 };
constexpr uint32_t RefreshInterval { 1000 };
constexpr size_t ServicesMax { 8 };

constexpr uint16_t TypeA { 1 };
constexpr uint16_t TypePtr { 12 };
constexpr uint16_t TypeTxt { 16 };
constexpr uint16_t TypeAaaa { 28 };
constexpr uint16_t TypeSrv { 33 };
constexpr uint16_t TypeAny { 255 };
constexpr uint16_t ClassIn { 1 };
constexpr uint16_t CacheFlush { 0x8000 };
constexpr uint16_t Unicast { 0x8000 };

struct Service {
    espurna::string owner;
    espurna::string instance;
    espurna::string type;      // "_matterc"
    espurna::string proto;     // "_udp" / "_tcp"
    uint16_t port { 0 };
    uint32_t ttl { 0 };  // 0 - the defaults
    std::vector<espurna::string> txt;
    std::vector<espurna::string> subtypes;
    bool stale { false };
    bool goodbye { false };  // removed: one goodbye, then dropped
};

#if LWIP_IPV6
constexpr size_t Addr6Max { LWIP_IPV6_NUM_ADDRESSES };
#else
constexpr size_t Addr6Max { 0 };
#endif

struct Addresses {
    netif* interface { nullptr };
    uint32_t v4 { 0 };             // network order, 0 - none
#if LWIP_IPV6
    ip6_addr_t v6[Addr6Max] {};
#endif
    size_t v6_count { 0 };

    bool same(const Addresses& other) const {
        if ((interface != other.interface) || (v4 != other.v4) || (v6_count != other.v6_count)) {
            return false;
        }
#if LWIP_IPV6
        for (size_t index = 0; index < v6_count; ++index) {
            if (!ip6_addr_cmp(&v6[index], &other.v6[index])) {
                return false;
            }
        }
#endif
        return true;
    }
};

// what the query of the loop collects (lwIP's context fills it under the lock)
struct Found {
    struct Srv {
        espurna::string instance;   // full name, lower case
        espurna::string target;
        uint16_t port;
    };
    struct A {
        espurna::string name;
        uint32_t address;
    };
    std::vector<espurna::string> instances;
    std::vector<Srv> srv;
    std::vector<A> a;
};

struct State {
    MDNS_LOCK lock;
    espurna::string hostname;
    std::vector<Service> services;
    Addresses addresses;
    udp_pcb* pcb { nullptr };
    bool running { false };
    uint8_t announce { 0 };
    uint32_t announce_last { 0 };
    uint32_t refresh_last { 0 };
    bool query_active { false };
    espurna::string query_type;    // "_mqtt._tcp.local", lower case
    Found found;
};

State& state() {
    static State out;
    return out;
}

espurna::string lower(espurna::string value) {
    value.toLowerCase();
    return value;
}

bool equal(const espurna::string& lhs, const char* rhs) {
    return lhs.equalsIgnoreCase(rhs);
}

// --- the packet ------------------------------------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(uint8_t* data) :
        _data(data)
    {}

    size_t size() const {
        return _size;
    }

    bool ok() const {
        return _ok;
    }

    void u8(uint8_t value) {
        if (_size + 1 > PacketMax) {
            _ok = false;
            return;
        }
        _data[_size++] = value;
    }

    void u16(uint16_t value) {
        u8(value >> 8);
        u8(value & 0xff);
    }

    void u32(uint32_t value) {
        u16(value >> 16);
        u16(value & 0xffff);
    }

    void bytes(const void* data, size_t size) {
        if (_size + size > PacketMax) {
            _ok = false;
            return;
        }
        std::memcpy(&_data[_size], data, size);
        _size += size;
    }

    void label(const espurna::string& value) {
        const auto size = std::min<size_t>(value.length(), 63);
        u8(size);
        bytes(value.c_str(), size);
    }

    // labels, then the root
    template <typename... Labels>
    void name(const Labels&... labels) {
        int unused[] { (label(labels), 0)... };
        (void)unused;
        u8(0);
    }

    // the length of the record data goes here, written by end_data()
    size_t begin_data() {
        const auto at = _size;
        u16(0);
        return at;
    }

    void end_data(size_t at) {
        if (!_ok) {
            return;
        }
        const auto size = _size - at - 2;
        _data[at] = size >> 8;
        _data[at + 1] = size & 0xff;
    }

    void count(size_t at, uint16_t value) {
        _data[at] = value >> 8;
        _data[at + 1] = value & 0xff;
    }

private:
    uint8_t* _data;
    size_t _size { 0 };
    bool _ok { true };
};

// labels of a name in the packet (compression followed), lower case; false when it is broken
bool readName(const uint8_t* data, size_t size, size_t& at, std::vector<espurna::string>& out) {
    out.clear();
    size_t cursor = at;
    bool jumped = false;
    for (size_t guard = 0; guard < 64; ++guard) {
        if (cursor >= size) {
            return false;
        }
        const uint8_t length = data[cursor];
        if (length == 0) {
            if (!jumped) {
                at = cursor + 1;
            }
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (cursor + 1 >= size) {
                return false;
            }
            if (!jumped) {
                at = cursor + 2;
            }
            jumped = true;
            cursor = ((length & 0x3F) << 8) | data[cursor + 1];
            continue;
        }
        if (cursor + 1 + length > size) {
            return false;
        }
        espurna::string part;
        part.reserve(length);
        for (size_t index = 0; index < length; ++index) {
            part += static_cast<char>(tolower(data[cursor + 1 + index]));
        }
        out.push_back(std::move(part));
        cursor += 1 + length;
    }

    return false;
}

espurna::string joined(const std::vector<espurna::string>& labels) {
    espurna::string out;
    for (const auto& label : labels) {
        if (out.length()) {
            out += '.';
        }
        out += label;
    }
    return out;
}

// an address for the log, without lwIP's ipaddr_ntoa(): the ESP8266 head has no ip4addr_ntoa (a macro of lwIP 2.1 there,
// the module could not link, 2026-11-16)
struct AddressText {
    char text[48];
};

AddressText address_text(const ip_addr_t* address) {
    AddressText out {};
#if LWIP_IPV6
    if (IP_IS_V6(address)) {
        const auto* v6 = ip_2_ip6(address);
        snprintf(out.text, sizeof(out.text), "%x:%x:%x:%x:%x:%x:%x:%x",
            IP6_ADDR_BLOCK1(v6), IP6_ADDR_BLOCK2(v6), IP6_ADDR_BLOCK3(v6), IP6_ADDR_BLOCK4(v6),
            IP6_ADDR_BLOCK5(v6), IP6_ADDR_BLOCK6(v6), IP6_ADDR_BLOCK7(v6), IP6_ADDR_BLOCK8(v6));
        return out;
    }
#endif
    const auto* v4 = ip_2_ip4(address);
    snprintf(out.text, sizeof(out.text), "%u.%u.%u.%u",
        ip4_addr1_16(v4), ip4_addr2_16(v4), ip4_addr3_16(v4), ip4_addr4_16(v4));
    return out;
}

// --- the records -----------------------------------------------------------------------------------------------------

enum class Kind : uint8_t {
    Enumeration,   // _services._dns-sd._udp.local PTR <type>.<proto>.local
    TypePtr,       // <type>.<proto>.local PTR <instance>...
    SubtypePtr,    // <sub>._sub.<type>.<proto>.local PTR <instance>...
    Srv,
    Txt,
    A,
    Aaaa,
};

struct Record {
    Kind kind;
    uint8_t service;
    uint8_t subtype;

    bool operator==(const Record& other) const {
        return (kind == other.kind) && (service == other.service) && (subtype == other.subtype);
    }
};

void push(std::vector<Record>& out, Record record) {
    if (std::find(out.begin(), out.end(), record) == out.end()) {
        out.push_back(record);
    }
}

uint32_t ttlOf(const State& self, const Record& record, bool legacy, bool goodbye) {
    if (goodbye) {
        return 0;
    }

    uint32_t out = OtherTtl;
    switch (record.kind) {
    case Kind::Srv:
    case Kind::A:
    case Kind::Aaaa:
        out = HostTtl;
        break;
    default:
        break;
    }

    if ((record.kind != Kind::A) && (record.kind != Kind::Aaaa) && (record.service < self.services.size())) {
        const auto ttl = self.services[record.service].ttl;
        if (ttl) {
            out = ttl;
        }
    }

    return legacy ? std::min(out, LegacyTtl) : out;
}

void writeInstance(Writer& out, const Service& service) {
    out.name(service.instance, service.type, service.proto, espurna::string("local"));
}

void writeHost(Writer& out, const State& self) {
    out.name(self.hostname, espurna::string("local"));
}

// one record; false when it has nothing (no address of that family)
bool writeRecord(Writer& out, const State& self, const Record& record, bool legacy, bool goodbye) {
    const uint16_t flush = legacy ? 0 : CacheFlush;
    const auto ttl = ttlOf(self, record, legacy, goodbye);
    const Service* service = (record.service < self.services.size()) ? &self.services[record.service] : nullptr;

    switch (record.kind) {
    case Kind::Enumeration: {
        out.name(espurna::string("_services"), espurna::string("_dns-sd"), espurna::string("_udp"), espurna::string("local"));
        out.u16(TypePtr);
        out.u16(ClassIn);
        out.u32(ttl);
        const auto at = out.begin_data();
        out.name(service->type, service->proto, espurna::string("local"));
        out.end_data(at);
        return true;
    }

    case Kind::TypePtr: {
        out.name(service->type, service->proto, espurna::string("local"));
        out.u16(TypePtr);
        out.u16(ClassIn);
        out.u32(ttl);
        const auto at = out.begin_data();
        writeInstance(out, *service);
        out.end_data(at);
        return true;
    }

    case Kind::SubtypePtr: {
        out.name(service->subtypes[record.subtype], espurna::string("_sub"), service->type, service->proto, espurna::string("local"));
        out.u16(TypePtr);
        out.u16(ClassIn);
        out.u32(ttl);
        const auto at = out.begin_data();
        writeInstance(out, *service);
        out.end_data(at);
        return true;
    }

    case Kind::Srv: {
        writeInstance(out, *service);
        out.u16(TypeSrv);
        out.u16(ClassIn | flush);
        out.u32(ttl);
        const auto at = out.begin_data();
        out.u16(0);    // priority
        out.u16(0);    // weight
        out.u16(service->port);
        writeHost(out, self);
        out.end_data(at);
        return true;
    }

    case Kind::Txt: {
        writeInstance(out, *service);
        out.u16(TypeTxt);
        out.u16(ClassIn | flush);
        out.u32(ttl);
        const auto at = out.begin_data();
        if (service->txt.empty()) {
            out.u8(0);
        }
        for (const auto& entry : service->txt) {
            out.label(entry);
        }
        out.end_data(at);
        return true;
    }

    case Kind::A: {
        if (!self.addresses.v4) {
            return false;
        }
        writeHost(out, self);
        out.u16(TypeA);
        out.u16(ClassIn | flush);
        out.u32(ttl);
        const auto at = out.begin_data();
        out.bytes(&self.addresses.v4, 4);
        out.end_data(at);
        return true;
    }

    case Kind::Aaaa:
#if LWIP_IPV6
        if (!self.addresses.v6_count) {
            return false;
        }
        for (size_t index = 0; index < self.addresses.v6_count; ++index) {
            writeHost(out, self);
            out.u16(TypeAaaa);
            out.u16(ClassIn | flush);
            out.u32(ttl);
            const auto at = out.begin_data();
            out.bytes(self.addresses.v6[index].addr, 16);
            out.end_data(at);
        }
        return true;
#else
        return false;
#endif
    }

    return false;
}

// AAAA writes one record per address: the count of what writeRecord() wrote
uint16_t recordCount(const State& self, const Record& record) {
    if (record.kind == Kind::Aaaa) {
        return self.addresses.v6_count;
    }
    if (record.kind == Kind::A) {
        return self.addresses.v4 ? 1 : 0;
    }
    return 1;
}

void hostRecords(std::vector<Record>& out) {
    push(out, Record{Kind::A, 0xff, 0});
    push(out, Record{Kind::Aaaa, 0xff, 0});
}

void serviceRecords(std::vector<Record>& answers, std::vector<Record>& additional, uint8_t index) {
    push(answers, Record{Kind::TypePtr, index, 0});
    push(additional, Record{Kind::Srv, index, 0});
    push(additional, Record{Kind::Txt, index, 0});
    hostRecords(additional);
}

// the answers to one question
void answer(const State& self, const std::vector<espurna::string>& name, uint16_t type,
    std::vector<Record>& answers, std::vector<Record>& additional)
{
    const auto size = name.size();
    if ((size < 2) || !equal(name[size - 1], "local")) {
        return;
    }

    // <hostname>.local
    if ((size == 2) && equal(name[0], self.hostname.c_str())) {
        if ((type == TypeA) || (type == TypeAny)) {
            push(answers, Record{Kind::A, 0xff, 0});
        }
        if ((type == TypeAaaa) || (type == TypeAny)) {
            push(answers, Record{Kind::Aaaa, 0xff, 0});
        }
        return;
    }

    for (size_t index = 0; index < self.services.size(); ++index) {
        const auto& service = self.services[index];
        if (service.goodbye) {
            continue;
        }

        const auto id = static_cast<uint8_t>(index);

        // _services._dns-sd._udp.local
        if ((size == 4) && equal(name[0], "_services") && equal(name[1], "_dns-sd") && equal(name[2], "_udp")) {
            if ((type == TypePtr) || (type == TypeAny)) {
                bool first = true;
                for (size_t other = 0; other < index; ++other) {
                    if (self.services[other].type.equalsIgnoreCase(service.type)
                        && self.services[other].proto.equalsIgnoreCase(service.proto))
                    {
                        first = false;
                    }
                }
                if (first) {
                    push(answers, Record{Kind::Enumeration, id, 0});
                }
            }
            continue;
        }

        // <type>.<proto>.local
        if ((size == 3) && name[0].equalsIgnoreCase(service.type) && name[1].equalsIgnoreCase(service.proto)) {
            if ((type == TypePtr) || (type == TypeAny)) {
                serviceRecords(answers, additional, id);
            }
            continue;
        }

        // <sub>._sub.<type>.<proto>.local
        if ((size == 5) && equal(name[1], "_sub")
            && name[2].equalsIgnoreCase(service.type) && name[3].equalsIgnoreCase(service.proto))
        {
            for (size_t sub = 0; sub < service.subtypes.size(); ++sub) {
                if (name[0].equalsIgnoreCase(service.subtypes[sub]) && ((type == TypePtr) || (type == TypeAny))) {
                    push(answers, Record{Kind::SubtypePtr, id, static_cast<uint8_t>(sub)});
                    push(additional, Record{Kind::Srv, id, 0});
                    push(additional, Record{Kind::Txt, id, 0});
                    hostRecords(additional);
                }
            }
            continue;
        }

        // <instance>.<type>.<proto>.local
        if ((size == 4) && name[0].equalsIgnoreCase(service.instance)
            && name[1].equalsIgnoreCase(service.type) && name[2].equalsIgnoreCase(service.proto))
        {
            if ((type == TypeSrv) || (type == TypeAny)) {
                push(answers, Record{Kind::Srv, id, 0});
                hostRecords(additional);
            }
            if ((type == TypeTxt) || (type == TypeAny)) {
                push(answers, Record{Kind::Txt, id, 0});
            }
        }
    }
}

// --- sending (lwIP's context) ----------------------------------------------------------------------------------------

// a failed reply is logged once until one goes out again: with the heap out every question of the LAN failed and
// logged from lwIP's task, into the debug queue of the loop (W72)
bool send_failing { false };

void sendTo(const uint8_t* data, size_t size, const ip_addr_t* address, uint16_t port, netif* interface) {
    if (!state().pcb || !interface || !size) {
        return;
    }

    auto* buffer = pbuf_alloc(PBUF_TRANSPORT, size, PBUF_RAM);
    if (!buffer) {
        if (!send_failing) {
            MDNS_LOG("[MDNS] No memory for a reply of %u bytes\n", static_cast<unsigned>(size));
        }
        send_failing = true;
        return;
    }

    std::memcpy(buffer->payload, data, size);
    const auto sent = udp_sendto_if(state().pcb, buffer, address, port, interface);
    if (sent != ERR_OK) {
        if (!send_failing) {
            MDNS_LOG("[MDNS] Send to %s failed: %d\n", address_text(address).text, static_cast<int>(sent));
        }
        send_failing = true;
    } else {
        send_failing = false;
    }
    pbuf_free(buffer);
}

void groupV4(ip_addr_t& out) {
    IP_ADDR4(&out, 224, 0, 0, 251);
}

#if LWIP_IPV6
void groupV6(ip_addr_t& out, netif* interface) {
    IP_ADDR6(&out, PP_HTONL(0xFF020000UL), 0, 0, PP_HTONL(0x000000FBUL));
#if LWIP_IPV6_SCOPES
    ip6_addr_assign_zone(ip_2_ip6(&out), IP6_MULTICAST, interface);
#else
    (void)interface;
#endif
}
#endif

// to both groups (each family the interface has an address of)
void sendGroups(const uint8_t* data, size_t size, const Addresses& addresses) {
    if (addresses.v4) {
        ip_addr_t group;
        groupV4(group);
        sendTo(data, size, &group, Port, addresses.interface);
    }
#if LWIP_IPV6
    if (addresses.v6_count) {
        ip_addr_t group;
        groupV6(group, addresses.interface);
        sendTo(data, size, &group, Port, addresses.interface);
    }
#endif
}

// a response of `answers` + `additional` into `out`; empty when nothing fits or nothing is there
size_t response(uint8_t* out, const State& self, uint16_t id, const std::vector<Record>& answers,
    const std::vector<Record>& additional, bool legacy, bool goodbye,
    const uint8_t* question = nullptr, size_t question_size = 0)
{
    Writer writer(out);
    writer.u16(legacy ? id : 0);
    writer.u16(0x8400);   // response, authoritative
    writer.u16(question_size ? 1 : 0);
    const auto at_answers = writer.size();
    writer.u16(0);
    writer.u16(0);
    const auto at_additional = writer.size();
    writer.u16(0);
    if (question_size) {
        writer.bytes(question, question_size);
    }

    uint16_t count_answers = 0;
    for (const auto& record : answers) {
        if (writeRecord(writer, self, record, legacy, goodbye)) {
            count_answers += recordCount(self, record);
        }
    }

    uint16_t count_additional = 0;
    for (const auto& record : additional) {
        if (std::find(answers.begin(), answers.end(), record) != answers.end()) {
            continue;
        }
        if (writeRecord(writer, self, record, legacy, goodbye)) {
            count_additional += recordCount(self, record);
        }
    }

    if (!writer.ok() || !count_answers) {
        return 0;
    }

    writer.count(at_answers, count_answers);
    writer.count(at_additional, count_additional);
    return writer.size();
}

// --- receiving (lwIP's context) --------------------------------------------------------------------------------------

void collect(State& self, const uint8_t* data, size_t size) {
    if (size < 12) {
        return;
    }

    const uint16_t questions = (data[4] << 8) | data[5];
    const uint16_t records = ((data[6] << 8) | data[7]) + ((data[8] << 8) | data[9]) + ((data[10] << 8) | data[11]);
    size_t at = 12;
    std::vector<espurna::string> name;
    for (uint16_t index = 0; index < questions; ++index) {
        if (!readName(data, size, at, name) || (at + 4 > size)) {
            return;
        }
        at += 4;
    }

    std::vector<espurna::string> target;
    for (uint16_t index = 0; index < records; ++index) {
        if (!readName(data, size, at, name) || (at + 10 > size)) {
            return;
        }
        const uint16_t type = (data[at] << 8) | data[at + 1];
        const uint16_t length = (data[at + 8] << 8) | data[at + 9];
        at += 10;
        if (at + length > size) {
            return;
        }

        const auto full = joined(name);
        if ((type == TypePtr) && (full == self.query_type)) {
            size_t cursor = at;
            if (readName(data, size, cursor, target)) {
                self.found.instances.push_back(joined(target));
            }
        } else if ((type == TypeSrv) && (length > 6)) {
            size_t cursor = at + 6;
            if (readName(data, size, cursor, target)) {
                self.found.srv.push_back(Found::Srv{full, joined(target),
                    static_cast<uint16_t>((data[at + 4] << 8) | data[at + 5])});
            }
        } else if ((type == TypeA) && (length == 4)) {
            uint32_t address;
            std::memcpy(&address, &data[at], 4);
            self.found.a.push_back(Found::A{full, address});
        }

        at += length;
    }
}

void onRecv(void*, udp_pcb*, pbuf* buffer, const ip_addr_t* source, u16_t source_port) {
    if (!buffer) {
        return;
    }

    std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[buffer->tot_len]);
    const size_t size = buffer->tot_len;
    if (data) {
        pbuf_copy_partial(buffer, data.get(), size, 0);
    }
    pbuf_free(buffer);
    if (!data || (size < 12)) {
        return;
    }

    auto& self = state();
    std::lock_guard<MDNS_LOCK> lock(self.lock);

    const uint16_t flags = (data[2] << 8) | data[3];
    if (flags & 0x8000) {
        if (self.query_active) {
            collect(self, data.get(), size);
        }
        return;
    }

    if (!self.running || !self.addresses.interface) {
        return;
    }

    const bool legacy = (source_port != Port);
    const uint16_t id = (data[0] << 8) | data[1];
    const uint16_t questions = (data[4] << 8) | data[5];

    std::vector<Record> answers;
    std::vector<Record> additional;
    bool unicast = legacy;
    bool pairing = false;
    size_t at = 12;
    size_t question_at = 0;
    size_t question_size = 0;
    std::vector<espurna::string> name;
    for (uint16_t index = 0; index < questions; ++index) {
        const size_t begin = at;
        if (!readName(data.get(), size, at, name) || (at + 4 > size)) {
            return;
        }
        const uint16_t type = (data[at] << 8) | data[at + 1];
        const uint16_t qclass = (data[at + 2] << 8) | data[at + 3];
        at += 4;
        if (qclass & Unicast) {
            unicast = true;
        }

        const auto before = answers.size();
        answer(self, name, type, answers, additional);

        // the log: the questions of a commissioner looking for a board to pair (_matterc), nothing else. Every
        // question of the LAN (_http, _matter._tcp of the hubs, several a second) filled the WebUI queues of the log
        // and the heap with them (2026-11-15)
        {
            const auto full = joined(name);
            if (full.indexOf("_matterc") >= 0) {
                pairing = true;
                MDNS_LOG("[MDNS] %s:%u asks %s type %u%s: %u answer(s)\n",
                    address_text(source).text, static_cast<unsigned>(source_port), full.c_str(),
                    static_cast<unsigned>(type), (qclass & Unicast) ? " (QU)" : "",
                    static_cast<unsigned>(answers.size() - before));
            }
        }
        if (legacy && (answers.size() != before) && !question_size) {
            // a legacy answer repeats its question (the first one answered; names uncompressed only)
            bool compressed = false;
            for (size_t cursor = begin; cursor < at - 4; cursor += 1 + data[cursor]) {
                if ((data[cursor] & 0xC0) == 0xC0) {
                    compressed = true;
                    break;
                }
                if (!data[cursor]) {
                    break;
                }
            }
            if (!compressed) {
                question_at = begin;
                question_size = at - begin;
            }
        }
    }

    if (answers.empty()) {
        return;
    }

    // known-answer suppression (RFC 6762 7.1): a PTR the asker already holds with at least half of its TTL is not
    // sent again. The speaker hub repeats its _matterc question with our PTR as a known answer; a board answering
    // anyway is not what the stock responders (CHIP, IDF) do, and that hub never started the pairing (2026-11-16)
    if (!legacy) {
        const uint16_t known = (data[6] << 8) | data[7];
        std::vector<espurna::string> owner;
        std::vector<espurna::string> target;
        for (uint16_t index = 0; index < known; ++index) {
            if (!readName(data.get(), size, at, owner) || (at + 10 > size)) {
                break;
            }
            const uint16_t type = (data[at] << 8) | data[at + 1];
            const uint32_t ttl = (static_cast<uint32_t>(data[at + 4]) << 24) | (static_cast<uint32_t>(data[at + 5]) << 16)
                | (static_cast<uint32_t>(data[at + 6]) << 8) | data[at + 7];
            const uint16_t length = (data[at + 8] << 8) | data[at + 9];
            at += 10;
            if (at + length > size) {
                break;
            }
            size_t rdata = at;
            at += length;
            if ((type != TypePtr) || !readName(data.get(), size, rdata, target)) {
                continue;
            }

            const auto owner_name = joined(owner);
            const auto target_name = joined(target);
            answers.erase(std::remove_if(answers.begin(), answers.end(),
                [&](const Record& record) {
                    if (record.service >= self.services.size()) {
                        return false;
                    }
                    const auto& service = self.services[record.service];
                    espurna::string name;
                    switch (record.kind) {
                    case Kind::TypePtr:
                        name = lower(service.type + '.' + service.proto + ".local");
                        break;
                    case Kind::SubtypePtr:
                        name = lower(service.subtypes[record.subtype] + "._sub." + service.type + '.' + service.proto
                            + ".local");
                        break;
                    default:
                        return false;
                    }
                    const auto instance = lower(service.instance + '.' + service.type + '.' + service.proto + ".local");
                    return (owner_name == name) && (target_name == instance)
                        && (ttl * 2 >= ttlOf(self, record, false, false));
                }), answers.end());
        }

        if (answers.empty()) {
            if (pairing) {
                MDNS_LOG("[MDNS] Known answer, no reply\n");
            }
            return;
        }
    }

    std::unique_ptr<uint8_t[]> out(new (std::nothrow) uint8_t[PacketMax]);
    if (!out) {
        return;
    }

    const auto out_size = response(out.get(), self, id, answers, additional, legacy, false,
        question_size ? &data[question_at] : nullptr, question_size);
    if (!out_size) {
        return;
    }

    if (pairing) {
        MDNS_LOG("[MDNS] Reply %u bytes, %u answer(s) + %u more, %s\n", static_cast<unsigned>(out_size),
            static_cast<unsigned>(answers.size()), static_cast<unsigned>(additional.size()),
            unicast ? "unicast" : "multicast");
    }

    if (unicast) {
        sendTo(out.get(), out_size, source, source_port, self.addresses.interface);
        return;
    }

    // a question without QU from port 5353: the group only, like the stock responders (RFC 6762 6). The extra unicast
    // copy to the asker's 5353 (added 2026-11-14) went with every answer to the speaker hub, which never paired since
#if LWIP_IPV6
    if (IP_IS_V6(source)) {
        ip_addr_t group;
        groupV6(group, self.addresses.interface);
        sendTo(out.get(), out_size, &group, Port, self.addresses.interface);
        return;
    }
#endif

    ip_addr_t group;
    groupV4(group);
    sendTo(out.get(), out_size, &group, Port, self.addresses.interface);
}

// --- the interface (lwIP's context) ----------------------------------------------------------------------------------

Addresses current() {
    Addresses out;
    auto* interface = netif_default;
    if (!interface || !netif_is_up(interface) || !netif_is_link_up(interface)) {
        return out;
    }

    out.interface = interface;
#if LWIP_IPV4
    out.v4 = ip4_addr_get_u32(netif_ip4_addr(interface));
#endif
#if LWIP_IPV6
    for (size_t index = 0; index < LWIP_IPV6_NUM_ADDRESSES; ++index) {
        if (ip6_addr_isvalid(netif_ip6_addr_state(interface, index))) {
            ip6_addr_copy(out.v6[out.v6_count], *netif_ip6_addr(interface, index));
            ++out.v6_count;
        }
    }
#endif

    return out;
}

void join(netif* interface) {
    if (!interface) {
        return;
    }

#if LWIP_IGMP
    // the BK7231N port leaves NETIF_FLAG_IGMP off: the join fails with ERR_VAL without it
    if (!(interface->flags & NETIF_FLAG_IGMP)) {
        interface->flags |= NETIF_FLAG_IGMP;
        igmp_start(interface);
    }
    ip_addr_t group;
    groupV4(group);
    const auto v4 = igmp_joingroup_netif(interface, ip_2_ip4(&group));
    if (v4 != ERR_OK) {
        MDNS_LOG("[MDNS] IGMP join: %d\n", static_cast<int>(v4));
    }
#endif

#if LWIP_IPV6 && LWIP_IPV6_MLD
    ip_addr_t group6;
    groupV6(group6, interface);
    const auto v6 = mld6_joingroup_netif(interface, ip_2_ip6(&group6));
    if (v6 != ERR_OK) {
        MDNS_LOG("[MDNS] MLD join: %d\n", static_cast<int>(v6));
    }
#endif
}

// --- the loop --------------------------------------------------------------------------------------------------------

void announce(bool goodbye_only) {
    auto& self = state();
    std::unique_ptr<uint8_t[]> out(new (std::nothrow) uint8_t[PacketMax]);
    if (!out) {
        return;
    }

    struct Packet {
        std::unique_ptr<uint8_t[]> data;
        size_t size;
    };
    std::vector<Packet> packets;
    Addresses addresses;
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        addresses = self.addresses;
        if (!addresses.interface) {
            return;
        }

        const auto add = [&](size_t size) {
            if (!size) {
                return;
            }
            std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[size]);
            if (data) {
                std::memcpy(data.get(), out.get(), size);
                packets.push_back(Packet{std::move(data), size});
            }
        };

        // goodbyes
        for (size_t index = 0; index < self.services.size(); ++index) {
            if (!self.services[index].goodbye) {
                continue;
            }
            std::vector<Record> records;
            push(records, Record{Kind::TypePtr, static_cast<uint8_t>(index), 0});
            for (size_t sub = 0; sub < self.services[index].subtypes.size(); ++sub) {
                push(records, Record{Kind::SubtypePtr, static_cast<uint8_t>(index), static_cast<uint8_t>(sub)});
            }
            add(response(out.get(), self, 0, records, {}, false, true));
        }

        self.services.erase(std::remove_if(self.services.begin(), self.services.end(),
            [](const Service& service) {
                return service.goodbye;
            }), self.services.end());

        if (!goodbye_only) {
            std::vector<Record> host;
            hostRecords(host);
            add(response(out.get(), self, 0, host, {}, false, false));

            for (size_t index = 0; index < self.services.size(); ++index) {
                const auto id = static_cast<uint8_t>(index);
                std::vector<Record> records;
                push(records, Record{Kind::TypePtr, id, 0});
                for (size_t sub = 0; sub < self.services[index].subtypes.size(); ++sub) {
                    push(records, Record{Kind::SubtypePtr, id, static_cast<uint8_t>(sub)});
                }
                push(records, Record{Kind::Srv, id, 0});
                push(records, Record{Kind::Txt, id, 0});
                add(response(out.get(), self, 0, records, {}, false, false));
            }
        }
    }

    ::espurna::lwip::lwipCall([&]() {
        for (const auto& packet : packets) {
            sendGroups(packet.data.get(), packet.size, addresses);
        }
    });
}

void refresh() {
    auto& self = state();
    Addresses now;
    bool changed { false };
    ::espurna::lwip::lwipCall([&]() {
        now = current();
        bool joined_before = false;
        {
            std::lock_guard<MDNS_LOCK> lock(self.lock);
            changed = !now.same(self.addresses);
            joined_before = (self.addresses.interface == now.interface);
            self.addresses = now;
        }
        if (changed && !joined_before) {
            join(now.interface);
        }
    });

    if (changed && now.interface) {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        self.announce = Announcements;
        self.announce_last = espurna::time::ms() - AnnounceInterval;
    }
}

} // namespace

bool begin(const espurna::string& hostname) {
    auto& self = state();
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        self.hostname = hostname;
        if (self.running) {
            self.announce = Announcements;
            return true;
        }
    }

    bool ok { false };
    ::espurna::lwip::lwipCall([&]() {
#if LWIP_IPV6 && LWIP_IPV4
        auto* pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
#else
        auto* pcb = udp_new();
#endif
        if (!pcb) {
            return;
        }
#if LWIP_IPV6 && LWIP_IPV4
        const auto bound = udp_bind(pcb, IP_ANY_TYPE, Port);
#else
        const auto bound = udp_bind(pcb, IP_ADDR_ANY, Port);
#endif
        if (bound != ERR_OK) {
            MDNS_LOG("[MDNS] UDP %u is taken (%d)\n", Port, static_cast<int>(bound));
            udp_remove(pcb);
            return;
        }
#if LWIP_MULTICAST_TX_OPTIONS
        udp_set_multicast_ttl(pcb, 255);
#endif
        udp_recv(pcb, onRecv, nullptr);
        state().pcb = pcb;
        ok = true;
    });

    if (!ok) {
        return false;
    }

    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        self.running = true;
    }
    refresh();
    return true;
}

bool running() {
    return state().running;
}

void update(const char* owner) {
    auto& self = state();
    std::lock_guard<MDNS_LOCK> lock(self.lock);
    for (auto& service : self.services) {
        if (service.owner.equals(owner)) {
            service.stale = true;
        }
    }
}

bool add(const char* owner, const char* instance, const char* type, bool tcp, uint16_t port, uint32_t ttl,
    const char* const* txt, size_t txt_count, const char* const* subtypes, size_t subtypes_count)
{
    if (!owner || !instance || !type) {
        return false;
    }

    auto& self = state();
    std::lock_guard<MDNS_LOCK> lock(self.lock);

    const char* proto = tcp ? "_tcp" : "_udp";
    Service* service = nullptr;
    for (auto& item : self.services) {
        if (item.owner.equals(owner) && item.instance.equals(instance) && item.type.equalsIgnoreCase(type)
            && item.proto.equals(proto) && !item.goodbye)
        {
            service = &item;
            break;
        }
    }

    if (!service) {
        if (self.services.size() >= ServicesMax) {
            return false;
        }
        self.services.push_back(Service{});
        service = &self.services.back();
        service->owner = owner;
        service->instance = instance;
        service->type = type;
        service->proto = proto;
    }

    std::vector<espurna::string> entries;
    for (size_t index = 0; index < txt_count; ++index) {
        entries.push_back(txt[index]);
    }
    std::vector<espurna::string> subs;
    for (size_t index = 0; index < subtypes_count; ++index) {
        subs.push_back(subtypes[index]);
    }

    const bool changed = (service->port != port) || (service->ttl != ttl) || (service->txt != entries)
        || (service->subtypes != subs);
    service->port = port;
    service->ttl = ttl;
    service->txt = std::move(entries);
    service->subtypes = std::move(subs);
    service->stale = false;
    if (changed) {
        self.announce = Announcements;
        self.announce_last = espurna::time::ms() - AnnounceInterval;
    }

    return true;
}

void commit(const char* owner) {
    auto& self = state();
    std::lock_guard<MDNS_LOCK> lock(self.lock);
    bool gone = false;
    for (auto& service : self.services) {
        if (service.owner.equals(owner) && service.stale) {
            service.stale = false;
            service.goodbye = true;
            gone = true;
        }
    }
    if (gone && !self.announce) {
        self.announce = 1;
        self.announce_last = espurna::time::ms() - AnnounceInterval;
    }
}

bool query(const char* service, const char* protocol, uint32_t timeout, QueryCallback callback) {
    auto& self = state();
    if (!self.running) {
        return false;
    }

    espurna::string type = lower(espurna::string('_') + service + "._" + protocol + ".local");
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        self.query_active = true;
        self.query_type = type;
        self.found = Found{};
    }

    // PTR question, the QU bit: the answers come straight back to this socket (port 5353)
    uint8_t packet[128];
    Writer writer(packet);
    writer.u16(0);
    writer.u16(0);
    writer.u16(1);
    writer.u16(0);
    writer.u16(0);
    writer.u16(0);
    writer.name(espurna::string('_') + service, espurna::string('_') + protocol, espurna::string("local"));
    writer.u16(TypePtr);
    writer.u16(ClassIn | Unicast);

    Addresses addresses;
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        addresses = self.addresses;
    }
    if (writer.ok() && addresses.interface) {
        ::espurna::lwip::lwipCall([&]() {
            sendGroups(packet, writer.size(), addresses);
        });
    }

    const auto start = espurna::time::ms();
    while (espurna::time::ms() - start < timeout) {
        espurna::time::sleep(10);
    }

    Found found;
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        self.query_active = false;
        found = std::move(self.found);
    }

    for (const auto& instance : found.instances) {
        for (const auto& srv : found.srv) {
            if (srv.instance != instance) {
                continue;
            }
            for (const auto& a : found.a) {
                if (a.name != srv.target) {
                    continue;
                }
                // network order: the first octet in the lowest byte (no lwIP formatting call: the head of the
                // ESP8266 does not have one)
                const auto* octet = reinterpret_cast<const uint8_t*>(&a.address);
                char text[16];
                snprintf(text, sizeof(text), "%u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
                if (callback(espurna::string(text), srv.port)) {
                    return true;
                }
            }
        }
    }

    return false;
}

void loop() {
    auto& self = state();
    if (!self.running) {
        return;
    }

    const auto now = espurna::time::ms();
    if (now - self.refresh_last >= RefreshInterval) {
        self.refresh_last = now;
        refresh();
    }

    bool send { false };
    bool goodbye_only { false };
    {
        std::lock_guard<MDNS_LOCK> lock(self.lock);
        if (self.announce && (now - self.announce_last >= AnnounceInterval)) {
            self.announce_last = now;
            --self.announce;
            send = true;
        } else {
            for (const auto& service : self.services) {
                if (service.goodbye) {
                    send = true;
                    goodbye_only = true;
                    break;
                }
            }
        }
    }

    if (send) {
        announce(goodbye_only);
    }
}

} // namespace responder
} // namespace mdns
} // namespace espurna

extern "C" {

void espurna_mdns_update(const char* owner) {
    espurna::mdns::responder::update(owner);
}

bool espurna_mdns_add(const char* owner, const char* instance, const char* type, bool tcp, uint16_t port,
    uint32_t ttl, const char* const* txt, size_t txt_count, const char* const* subtypes, size_t subtypes_count)
{
    return espurna::mdns::responder::add(owner, instance, type, tcp, port, ttl, txt, txt_count, subtypes,
        subtypes_count);
}

void espurna_mdns_commit(const char* owner) {
    espurna::mdns::responder::commit(owner);
}

} // extern "C"
