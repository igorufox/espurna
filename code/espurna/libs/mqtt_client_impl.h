/*

MQTT CLIENT - the implementation (libs/mqtt_client.h). Included by the one user, the mqtt module (mqtt.cpp); the
stream under it (libs/tls.h) is the head's

*/

#pragma once

#include "mqtt_client.h"
#include "tcp.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace espurna {
namespace mqtt {
namespace {

// MQTT 3.1.1 control packet types (the high nibble of the fixed header)
enum Packet : uint8_t {
    Connect = 1,
    Connack = 2,
    Publish = 3,
    Puback = 4,
    Pubrec = 5,
    Pubrel = 6,
    Pubcomp = 7,
    Subscribe = 8,
    Suback = 9,
    Unsubscribe = 10,
    Unsuback = 11,
    Pingreq = 12,
    Pingresp = 13,
    Disconnect = 14,
};

constexpr uint32_t ConnackTimeout { 10000 };
constexpr size_t TopicMax { 256 };

// a copy of a string, nullptr for none
char* copy(const char* text) {
    if (!text || !*text) {
        return nullptr;
    }

    const auto len = strlen(text);
    auto* out = static_cast<char*>(malloc(len + 1));
    if (out) {
        memcpy(out, text, len + 1);
    }

    return out;
}

void assign(char*& lhs, const char* rhs) {
    free(lhs);
    lhs = copy(rhs);
}

size_t length(const char* text) {
    return text ? strlen(text) : 0;
}

// the bytes of a remaining length
size_t varintSize(size_t value) {
    return (value < 128) ? 1 : (value < 16384) ? 2 : (value < 2097152) ? 3 : 4;
}

} // namespace

const char* reasonText(DisconnectReason reason) {
    switch (reason) {
    case DisconnectReason::User:
        return "disconnected";
    case DisconnectReason::Closed:
        return "the broker closed the connection";
    case DisconnectReason::Resolve:
        return "cannot resolve the broker";
    case DisconnectReason::Connect:
        return "cannot connect to the broker";
    case DisconnectReason::Memory:
        return "out of memory";
    case DisconnectReason::Tls:
        return "TLS handshake failed";
    case DisconnectReason::Untrusted:
        return "the broker certificate is not signed by the CA (or the clock is not set yet)";
    case DisconnectReason::NoCa:
        return "the CA check needs the tls module and a certificate (mqttCA) or the built-in root";
    case DisconnectReason::Fingerprint:
        return "the broker certificate does not match the fingerprint";
    case DisconnectReason::Refused:
        return "the broker refused the connection";
    case DisconnectReason::Protocol:
        return "protocol error";
    case DisconnectReason::Timeout:
        return "the broker does not answer";
    }

    return "";
}

class Client::Impl {
public:
    Impl(size_t message_max, size_t output_max) :
        _message_max(message_max),
        _output_max(output_max)
    {}

    ~Impl() {
        close();
        for (auto* text : {_host, _client_id, _user, _password, _will_topic, _will_payload}) {
            free(text);
        }
        free(_body);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // --- settings ---------------------------------------------------------------------------------------------------

    void setServer(const char* host, uint16_t port) {
        assign(_host, host);
        _port = port;
    }

    void setClientId(const char* id) {
        assign(_client_id, id);
    }

    void setCredentials(const char* user, const char* password) {
        assign(_user, user);
        assign(_password, password);
    }

    void setKeepAlive(uint16_t seconds) {
        _keepalive = seconds;
    }

    void setCleanSession(bool clean) {
        _clean = clean;
    }

    void setWill(const char* topic, uint8_t qos, bool retain, const char* payload) {
        assign(_will_topic, topic);
        assign(_will_payload, payload);
        _will_qos = std::min<uint8_t>(qos, 2);
        _will_retain = retain;
    }

    void setTls(const tls::Options& options) {
        _tls = options;
    }

    OnConnect on_connect;
    OnDisconnect on_disconnect;
    OnMessage on_message;
    OnPublish on_publish;
    OnSubscribe on_subscribe;

    // --- the connection ---------------------------------------------------------------------------------------------

    bool connect() {
        if (_state != State::Idle) {
            return false;
        }

        if (!_host) {
            lost(DisconnectReason::Resolve, 0);
            return false;
        }

        if (!_body) {
            _body_size = _message_max + TopicMax + 8;
            _body = static_cast<uint8_t*>(malloc(_body_size));
            if (!_body) {
                lost(DisconnectReason::Memory, 0);
                return false;
            }
        }

        _state = State::Opening;
        resetParser();
        _out_len = 0;
        _ping_sent = false;

        if (!_stream.connect(_host, _port, _tls)) {
            failed();
            return false;
        }

        return true;
    }

    void disconnect() {
        if (_state == State::Connected) {
            const uint8_t packet[] { Disconnect << 4, 0 };
            queue(packet, sizeof(packet));
            flush();
        }

        stop();
    }

    void stop() {
        if (_state != State::Idle) {
            lost(DisconnectReason::User, 0);
        }
    }

    bool connected() const {
        return _state == State::Connected;
    }

    bool connecting() const {
        return (_state == State::Opening) || (_state == State::Connack);
    }

    // --- packets out ------------------------------------------------------------------------------------------------

    uint16_t publish(const char* topic, uint8_t qos, bool retain, const char* payload, size_t len) {
        if (!connected() || !topic) {
            return 0;
        }

        qos = std::min<uint8_t>(qos, 2);
        const auto topic_len = strlen(topic);
        const size_t remaining = 2 + topic_len + (qos ? 2 : 0) + len;
        if (!room(1 + varintSize(remaining) + remaining)) {
            return 0;
        }

        const uint16_t pid = qos ? nextPid() : 1;

        putHeader((Publish << 4) | (qos << 1) | (retain ? 1 : 0), remaining);
        putString(topic, topic_len);
        if (qos) {
            putWord(pid);
        }
        put(reinterpret_cast<const uint8_t*>(payload), len);
        flush();

        return pid;
    }

    uint16_t subscribe(const char* topic, uint8_t qos) {
        if (!connected() || !topic) {
            return 0;
        }

        const auto topic_len = strlen(topic);
        const size_t remaining = 2 + 2 + topic_len + 1;
        if (!room(1 + varintSize(remaining) + remaining)) {
            return 0;
        }

        const auto pid = nextPid();
        putHeader((Subscribe << 4) | 2, remaining);
        putWord(pid);
        putString(topic, topic_len);
        putByte(std::min<uint8_t>(qos, 2));
        flush();

        return pid;
    }

    uint16_t unsubscribe(const char* topic) {
        if (!connected() || !topic) {
            return 0;
        }

        const auto topic_len = strlen(topic);
        const size_t remaining = 2 + 2 + topic_len;
        if (!room(1 + varintSize(remaining) + remaining)) {
            return 0;
        }

        const auto pid = nextPid();
        putHeader((Unsubscribe << 4) | 2, remaining);
        putWord(pid);
        putString(topic, topic_len);
        flush();

        return pid;
    }

    // --- the loop ---------------------------------------------------------------------------------------------------

    void loop() {
        switch (_state) {
        case State::Idle:
            return;

        case State::Opening:
            if (_stream.ready()) {
                sendConnect();
                _state = State::Connack;
                _since = tcp::now();
            } else if (!_stream.pending()) {
                failed();
            }
            return;

        case State::Connack:
            if ((tcp::now() - _since) > ConnackTimeout) {
                lost(DisconnectReason::Timeout, 0);
                return;
            }
            break;

        case State::Connected:
            keepalive();
            if (_state != State::Connected) {
                return;
            }
            break;
        }

        flush();
        receive();

        if ((_state != State::Idle) && _stream.drained()) {
            lost(DisconnectReason::Closed, 0);
        }
    }

private:
    enum class State {
        Idle,
        Opening,    // resolve, TCP, TLS
        Connack,    // CONNECT sent
        Connected,
    };

    // --- output -----------------------------------------------------------------------------------------------------

    bool room(size_t size) {
        if (!_out) {
            _out = static_cast<uint8_t*>(malloc(_output_max));
            if (!_out) {
                return false;
            }
        }

        flush();
        return (_out_len + size) <= _output_max;
    }

    void put(const uint8_t* data, size_t len) {
        if (len) {
            memcpy(_out + _out_len, data, len);
            _out_len += len;
        }
    }

    void putByte(uint8_t value) {
        _out[_out_len++] = value;
    }

    void putWord(uint16_t value) {
        putByte(value >> 8);
        putByte(value & 0xff);
    }

    void putString(const char* text, size_t len) {
        putWord(static_cast<uint16_t>(len));
        put(reinterpret_cast<const uint8_t*>(text), len);
    }

    void putHeader(uint8_t first, size_t remaining) {
        putByte(first);
        do {
            uint8_t digit = remaining % 128;
            remaining /= 128;
            if (remaining) {
                digit |= 0x80;
            }
            putByte(digit);
        } while (remaining);
    }

    // a small packet (acks, ping): queued when there is room, else the connection is too far behind
    void queue(const uint8_t* data, size_t len) {
        if (room(len)) {
            put(data, len);
        }
    }

    void queueAck(uint8_t first, uint16_t pid) {
        const uint8_t packet[] {
            first, 2,
            static_cast<uint8_t>(pid >> 8), static_cast<uint8_t>(pid & 0xff),
        };
        queue(packet, sizeof(packet));
    }

    void flush() {
        while (_out && _out_len) {
            const auto sent = _stream.write(_out, _out_len);
            if (!sent) {
                return;
            }

            memmove(_out, _out + sent, _out_len - sent);
            _out_len -= sent;
            _last_out = tcp::now();
        }
    }

    uint16_t nextPid() {
        if (++_pid == 0) {
            _pid = 1;
        }

        return _pid;
    }

    void sendConnect() {
        const bool will = _will_topic != nullptr;
        const bool user = _user != nullptr;
        const bool password = user && (_password != nullptr);

        uint8_t flags = _clean ? 0x02 : 0;
        if (will) {
            flags |= 0x04 | (_will_qos << 3) | (_will_retain ? 0x20 : 0);
        }
        if (user) {
            flags |= 0x80;
        }
        if (password) {
            flags |= 0x40;
        }

        size_t remaining = 10 + 2 + length(_client_id);
        if (will) {
            remaining += 2 + length(_will_topic) + 2 + length(_will_payload);
        }
        if (user) {
            remaining += 2 + length(_user);
        }
        if (password) {
            remaining += 2 + length(_password);
        }

        if (!room(1 + varintSize(remaining) + remaining)) {
            lost(DisconnectReason::Memory, 0);
            return;
        }

        putHeader(Connect << 4, remaining);
        putString("MQTT", 4);
        putByte(4); // 3.1.1
        putByte(flags);
        putWord(_keepalive);
        putString(_client_id ? _client_id : "", length(_client_id));
        if (will) {
            putString(_will_topic, length(_will_topic));
            putString(_will_payload ? _will_payload : "", length(_will_payload));
        }
        if (user) {
            putString(_user, length(_user));
        }
        if (password) {
            putString(_password, length(_password));
        }

        flush();
    }

    void keepalive() {
        if (!_keepalive) {
            return;
        }

        const uint32_t period = static_cast<uint32_t>(_keepalive) * 1000;
        const auto now = tcp::now();

        if (_ping_sent) {
            if ((now - _ping_at) > period) {
                lost(DisconnectReason::Timeout, 0);
            }
            return;
        }

        if ((now - _last_out) >= period) {
            const uint8_t packet[] { Pingreq << 4, 0 };
            queue(packet, sizeof(packet));
            flush();
            _ping_sent = true;
            _ping_at = now;
        }
    }

    // --- input ------------------------------------------------------------------------------------------------------

    enum class Parse {
        Header,
        Length,
        Body,
    };

    void resetParser() {
        _parse = Parse::Header;
        _remaining = 0;
        _multiplier = 1;
        _received = 0;
    }

    void receive() {
        uint8_t buffer[128];
        while (_state != State::Idle) {
            const auto got = _stream.read(buffer, sizeof(buffer));
            if (!got) {
                return;
            }

            for (size_t index = 0; (index < got) && (_state != State::Idle); ++index) {
                feed(buffer[index]);
            }
        }
    }

    void feed(uint8_t byte) {
        switch (_parse) {
        case Parse::Header:
            _first = byte;
            _remaining = 0;
            _multiplier = 1;
            _parse = Parse::Length;
            return;

        case Parse::Length:
            _remaining += (byte & 0x7f) * _multiplier;
            _multiplier *= 128;
            if (byte & 0x80) {
                if (_multiplier > (128 * 128 * 128)) {
                    lost(DisconnectReason::Protocol, 0);
                }
                return;
            }

            _received = 0;
            _parse = Parse::Body;
            if (!_remaining) {
                packet();
            }
            return;

        case Parse::Body:
            // the beginning of a body that is too big (a PUBLISH) is kept: its topic and packet id
            if (_received < _body_size) {
                _body[_received] = byte;
            }
            ++_received;
            if (_received == _remaining) {
                packet();
            }
            return;
        }
    }

    uint16_t word(size_t at) const {
        return (static_cast<uint16_t>(_body[at]) << 8) | _body[at + 1];
    }

    void packet() {
        const auto type = static_cast<uint8_t>(_first >> 4);
        const size_t len = std::min(_remaining, _body_size);
        _parse = Parse::Header;

        switch (type) {
        case Connack:
            if ((_state != State::Connack) || (len < 2)) {
                lost(DisconnectReason::Protocol, 0);
                return;
            }
            if (_body[1] != 0) {
                lost(DisconnectReason::Refused, _body[1]);
                return;
            }
            _state = State::Connected;
            _last_out = tcp::now();
            if (on_connect) {
                on_connect((_body[0] & 1) != 0);
            }
            return;

        case Publish:
            incoming(len);
            return;

        case Puback:
        case Pubcomp:
            if ((len >= 2) && on_publish) {
                on_publish(word(0));
            }
            return;

        case Pubrec:
            if (len >= 2) {
                queueAck((Pubrel << 4) | 2, word(0));
            }
            return;

        case Pubrel:
            if (len >= 2) {
                queueAck(Pubcomp << 4, word(0));
            }
            return;

        case Suback:
            if ((len >= 3) && on_subscribe) {
                on_subscribe(word(0), _body[2]);
            }
            return;

        case Unsuback:
            return;

        case Pingresp:
            _ping_sent = false;
            return;

        default:
            lost(DisconnectReason::Protocol, 0);
            return;
        }
    }

    void incoming(size_t len) {
        const uint8_t qos = (_first >> 1) & 3;
        if ((len < 2) || (qos > 2)) {
            lost(DisconnectReason::Protocol, 0);
            return;
        }

        const size_t topic_len = word(0);
        const size_t head = 2 + topic_len + (qos ? 2 : 0);
        if ((topic_len >= TopicMax) || (head > len)) {
            lost(DisconnectReason::Protocol, 0);
            return;
        }

        const uint16_t pid = qos ? word(2 + topic_len) : 0;
        const size_t payload_len = _remaining - head;

        // the whole message fits (with a byte to spare for the payload's NUL): handed over; else dropped, acknowledged
        if (_remaining < _body_size) {
            // the topic and the payload, each followed by a NUL: the topic moves one byte to the left over its
            // length (the packet id after it was read already), the payload's NUL is the byte after the body
            char* topic = reinterpret_cast<char*>(_body + 1);
            memmove(topic, _body + 2, topic_len);
            topic[topic_len] = '\0';

            char* payload = reinterpret_cast<char*>(_body + head);
            payload[payload_len] = '\0';
            if (on_message) {
                on_message(topic, payload, payload_len);
            }
        } else if (on_message) {
            // the payload length only: the user logs it
            on_message(nullptr, nullptr, payload_len);
        }

        if (qos == 1) {
            queueAck(Puback << 4, pid);
        } else if (qos == 2) {
            queueAck(Pubrec << 4, pid);
        }
    }

    // --- the end ----------------------------------------------------------------------------------------------------

    void failed() {
        DisconnectReason reason { DisconnectReason::Connect };
        switch (_stream.failure()) {
        case tls::Failure::Memory:
            reason = DisconnectReason::Memory;
            break;
        case tls::Failure::Resolve:
            reason = DisconnectReason::Resolve;
            break;
        case tls::Failure::Tls:
            reason = DisconnectReason::Tls;
            break;
        case tls::Failure::Fingerprint:
            reason = DisconnectReason::Fingerprint;
            break;
        case tls::Failure::Untrusted:
            reason = DisconnectReason::Untrusted;
            break;
        case tls::Failure::NoCa:
            reason = DisconnectReason::NoCa;
            break;
        case tls::Failure::None:
        case tls::Failure::Connect:
            break;
        }

        lost(reason, _stream.tlsError());
    }

    void lost(DisconnectReason reason, int code) {
        close();
        if (on_disconnect) {
            on_disconnect(reason, code);
        }
    }

    void close() {
        _stream.close();
        _state = State::Idle;
        _out_len = 0;
        free(_out);
        _out = nullptr;
        resetParser();
    }

    const size_t _message_max;
    const size_t _output_max;

    char* _host { nullptr };
    uint16_t _port { 1883 };
    char* _client_id { nullptr };
    char* _user { nullptr };
    char* _password { nullptr };
    uint16_t _keepalive { 60 };
    bool _clean { true };
    char* _will_topic { nullptr };
    char* _will_payload { nullptr };
    uint8_t _will_qos { 0 };
    bool _will_retain { false };
    tls::Options _tls;

    State _state { State::Idle };
    tls::Stream _stream;
    uint32_t _since { 0 };
    uint16_t _pid { 0 };

    uint8_t* _out { nullptr };
    size_t _out_len { 0 };
    uint32_t _last_out { 0 };
    bool _ping_sent { false };
    uint32_t _ping_at { 0 };

    Parse _parse { Parse::Header };
    uint8_t _first { 0 };
    size_t _remaining { 0 };
    size_t _multiplier { 1 };
    size_t _received { 0 };
    uint8_t* _body { nullptr };
    size_t _body_size { 0 };
};

Client::Client(size_t message_max, size_t output_max) :
    _impl(new (std::nothrow) Impl(message_max, output_max))
{}

Client::~Client() = default;

void Client::setServer(const char* host, uint16_t port) {
    if (_impl) {
        _impl->setServer(host, port);
    }
}

void Client::setClientId(const char* id) {
    if (_impl) {
        _impl->setClientId(id);
    }
}

void Client::setCredentials(const char* user, const char* password) {
    if (_impl) {
        _impl->setCredentials(user, password);
    }
}

void Client::setKeepAlive(uint16_t seconds) {
    if (_impl) {
        _impl->setKeepAlive(seconds);
    }
}

void Client::setCleanSession(bool clean) {
    if (_impl) {
        _impl->setCleanSession(clean);
    }
}

void Client::setWill(const char* topic, uint8_t qos, bool retain, const char* payload) {
    if (_impl) {
        _impl->setWill(topic, qos, retain, payload);
    }
}

void Client::setTls(const tls::Options& options) {
    if (_impl) {
        _impl->setTls(options);
    }
}

void Client::onConnect(OnConnect callback) {
    if (_impl) {
        _impl->on_connect = std::move(callback);
    }
}

void Client::onDisconnect(OnDisconnect callback) {
    if (_impl) {
        _impl->on_disconnect = std::move(callback);
    }
}

void Client::onMessage(OnMessage callback) {
    if (_impl) {
        _impl->on_message = std::move(callback);
    }
}

void Client::onPublish(OnPublish callback) {
    if (_impl) {
        _impl->on_publish = std::move(callback);
    }
}

void Client::onSubscribe(OnSubscribe callback) {
    if (_impl) {
        _impl->on_subscribe = std::move(callback);
    }
}

bool Client::connect() {
    return _impl && _impl->connect();
}

void Client::disconnect() {
    if (_impl) {
        _impl->disconnect();
    }
}

void Client::stop() {
    if (_impl) {
        _impl->stop();
    }
}

bool Client::connected() const {
    return _impl && _impl->connected();
}

bool Client::connecting() const {
    return _impl && _impl->connecting();
}

uint16_t Client::publish(const char* topic, uint8_t qos, bool retain, const char* payload, size_t len) {
    return _impl ? _impl->publish(topic, qos, retain, payload, len) : 0;
}

uint16_t Client::subscribe(const char* topic, uint8_t qos) {
    return _impl ? _impl->subscribe(topic, qos) : 0;
}

uint16_t Client::unsubscribe(const char* topic) {
    return _impl ? _impl->unsubscribe(topic) : 0;
}

void Client::loop() {
    if (_impl) {
        _impl->loop();
    }
}

} // namespace mqtt
} // namespace espurna
