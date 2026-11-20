/*

MDNS RESPONDER - the one mDNS of ESPurna on every chip (U28, W38): ESPurna's own services and Matter's (the Matter
library publishes through the C calls below instead of a responder of its own). On lwIP's raw UDP API, IPv4 and IPv6
(where the lwIP of the chip has it), UDP 5353 held by this responder alone.

- the host: <hostname>.local, A and AAAA of the station interface
- services: instance, type, protocol, port, TXT, subtypes (Matter's commissionable `_matterc._udp` carries
  `_L<discriminator>`, `_S<short>`, `_V<vendor>`, `_CM`: a commissioner finds the board by them; lwIP's own mDNS app has
  no subtypes, hence this one)
- answers: multicast, or unicast for a question with the QU bit and for a legacy query (source port not 5353); the
  service enumeration `_services._dns-sd._udp.local`
- announcements when the address or a service changes (three, a second apart), goodbyes (TTL 0) for removed ones
- a query (blocking, for the MQTT broker discovery): PTR of a service type, then SRV and A of its answers

Not done (a device does not need them): probing / conflict resolution, known-answer suppression, browsing for
Matter (the controller looks the board up, not the other way round)

The implementation: libs/mdns_responder_impl.h, included by the mdns module (mdns.cpp) alone

*/

#pragma once


#include <cstddef>
#include <cstdint>

namespace espurna {
namespace mdns {
namespace responder {

bool begin(const espurna::string& hostname);
bool running();

// the services of one owner ("espurna", "matter") are replaced as a whole: update(owner), add() each, commit(owner);
// what the owner did not add again goes (with a goodbye). Safe from any task: the loop sends
void update(const char* owner);
bool add(const char* owner, const char* instance, const char* type, bool tcp, uint16_t port, uint32_t ttl,
    const char* const* txt, size_t txt_count, const char* const* subtypes, size_t subtypes_count);
void commit(const char* owner);

// a blocking query from the loop: `callback(address, port)` for every instance of `_<service>._<protocol>.local`
// found within `timeout` ms, until it returns true
using QueryCallback = bool(*)(espurna::string&& address, uint16_t port);
bool query(const char* service, const char* protocol, uint32_t timeout, QueryCallback callback);

void loop();

} // namespace responder
} // namespace mdns
} // namespace espurna

// for the Matter library (its platform DNS-SD, matter-espurna gn/platform/EspurnaDnssd.cpp): the same calls in C
extern "C" {
void espurna_mdns_update(const char* owner);
bool espurna_mdns_add(const char* owner, const char* instance, const char* type, bool tcp, uint16_t port,
    uint32_t ttl, const char* const* txt, size_t txt_count, const char* const* subtypes, size_t subtypes_count);
void espurna_mdns_commit(const char* owner);
}
