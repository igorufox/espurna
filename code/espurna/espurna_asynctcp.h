/*

ASYNC TCP

The async TCP library of the chip, the one include for every user (api, mqtt, ota, telnet, thingspeak): ESPAsyncTCP on
the esp8266, AsyncTCP on ESP32 and LibreTiny. Both have the same AsyncClient (connect(), close(), onDisconnect() ...).

Also the one way to reach a private member of a library class (the AsyncClient of AsyncMqttClient, the client list
lock of AsyncWebSocket), for what the library has no public call for. Standard C++: an explicit template instantiation
may name a private member, the pointer to it is then kept by a friend function of the tag. The tag is declared in this
namespace, so the friend it declares and the one Member defines are the same function:

    namespace espurna {
    namespace async {
    struct MqttClient {
        using type = AsyncClient AsyncMqttClient::*;
        friend type member(MqttClient);
    };
    template struct Member<MqttClient, &AsyncMqttClient::_client>;
    } // namespace async
    } // namespace espurna

    (object.*member(espurna::async::MqttClient{})).close(true);

*/

#pragma once

#if defined(ESP8266)
#include <ESPAsyncTCP.h>
#else
#include <AsyncTCP.h>
#endif

namespace espurna {
namespace async {

template <typename Tag, typename Tag::type Pointer>
struct Member {
    friend typename Tag::type member(Tag) {
        return Pointer;
    }
};

} // namespace async
} // namespace espurna
