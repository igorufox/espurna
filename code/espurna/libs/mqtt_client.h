/*

MQTT CLIENT - the one MQTT 3.1.1 client of ESPurna (the mqtt module), every chip alike: on the byte stream of
libs/tls.h (lwIP's raw TCP, plain or TLS by BearSSL). The loop drives it, nothing waits, nothing runs in another task:
lwIP only fills the TCP receive ring, the client reads, parses and writes from loop().

- CONNECT with the client id, the will, the user and password, keep alive, clean session
- PUBLISH out with QoS 0, 1, 2 (the packet id, onPublish() once the broker acknowledged it: PUBACK, or PUBREC ->
  PUBREL -> PUBCOMP), in with QoS 0, 1, 2 (PUBACK, PUBREC / PUBCOMP sent); a message is handed over whole with the
  topic and the payload NUL-terminated, up to `message_max` bytes of payload; a longer one is acknowledged and
  dropped (onMessage(nullptr, nullptr, its length))
- SUBSCRIBE / UNSUBSCRIBE (onSubscribe() with the granted QoS)
- PINGREQ when nothing went out for the keep alive time, the connection given up when the broker stays silent
- the output is one buffer of `OutputMax` bytes: a packet that does not fit is refused (0), never split

    espurna::mqtt::Client client;
    client.setServer("broker", 1883);
    client.onMessage([](const char* topic, const char* payload, size_t len) { ... });
    client.connect();
    ... client.loop() from the loop ...

The implementation is libs/mqtt_client_impl.h, included by the mqtt module (mqtt.cpp).

*/

#pragma once

#include "tls.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace espurna {
namespace mqtt {

enum class DisconnectReason : uint8_t {
    User,           // disconnect() / stop()
    Closed,         // the broker closed the connection
    Resolve,
    Connect,
    Memory,
    Tls,
    Fingerprint,
    Untrusted,      // the chain did not lead to the CA (or the clock is not set yet)
    NoCa,           // the CA check is not available: no tls module, or no certificate in mqttCA
    NoTls,          // a secure connection, and the tls module is not installed
    Refused,        // CONNACK with an error code (code())
    Protocol,       // something the client could not parse
    Timeout,        // no CONNACK, or no answer to PINGREQ
};

const char* reasonText(DisconnectReason reason);

class Client {
public:
    using OnConnect = std::function<void(bool session_present)>;
    using OnDisconnect = std::function<void(DisconnectReason reason, int code)>;
    using OnMessage = std::function<void(const char* topic, const char* payload, size_t len)>;
    using OnPublish = std::function<void(uint16_t pid)>;
    using OnSubscribe = std::function<void(uint16_t pid, uint8_t qos)>;

    explicit Client(size_t message_max = 1024, size_t output_max = 4096);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // the strings are copied
    void setServer(const char* host, uint16_t port);
    void setClientId(const char* id);
    void setCredentials(const char* user, const char* password);
    void setKeepAlive(uint16_t seconds);
    void setCleanSession(bool clean);
    void setWill(const char* topic, uint8_t qos, bool retain, const char* payload);
    void setTls(const tls::Options& options);

    void onConnect(OnConnect callback);
    void onDisconnect(OnDisconnect callback);
    void onMessage(OnMessage callback);
    void onPublish(OnPublish callback);
    void onSubscribe(OnSubscribe callback);

    // starts the attempt (resolve, TCP, TLS, CONNECT). false - it could not even start (reported through onDisconnect)
    bool connect();
    // DISCONNECT to the broker (the will is not sent), then closed
    void disconnect();
    // closed right away (an attempt too); onDisconnect(User) when there was anything to close
    void stop();

    bool connected() const;
    bool connecting() const;

    // the packet id (1 for QoS 0), 0 - not connected or no room
    uint16_t publish(const char* topic, uint8_t qos, bool retain, const char* payload, size_t len);
    uint16_t subscribe(const char* topic, uint8_t qos);
    uint16_t unsubscribe(const char* topic);

    // from the loop
    void loop();

    class Impl;

private:
    std::unique_ptr<Impl> _impl;
};

} // namespace mqtt
} // namespace espurna
