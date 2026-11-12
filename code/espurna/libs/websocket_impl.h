/*

WEBSOCKET - the implementation (libs/websocket.h), part of libs/net_impl.h. No Arduino.

*/

#pragma once

#include "websocket.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <new>
#include <vector>

// BearSSL: our fork (igorufox/bearssl-esp8266, code/libraries.ini `bearssl`) on every chip, the ESP8266 too (its
// core's prebuilt libbearssl.a is taken out of the link, scripts/pio_main.py)
#include <bearssl.h>

namespace espurna {
namespace websocket {
namespace {

constexpr char Guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr uint32_t PingInterval { 30000 };
constexpr uint32_t DeadInterval { 90000 };
constexpr size_t ClientsDefault { 4 };

enum Opcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Client {
    uint32_t id { 0 };
    uint32_t ip { 0 };
    std::unique_ptr<tcp::Connection> conn;
    std::vector<uint8_t> out;
    size_t out_sent { 0 };
    bool closing { false };         // a close frame went out: the connection ends once it is sent
    uint32_t last_in { 0 };
    uint32_t last_ping { 0 };

    // the frame being read
    uint8_t head[14] {};
    size_t head_len { 0 };
    size_t head_need { 2 };
    bool in_payload { false };
    uint8_t opcode { 0 };
    bool fin { false };
    uint8_t mask[4] {};
    uint64_t remaining { 0 };
    uint64_t payload_pos { 0 };

    // the message being joined (data frames), the payload of a control frame
    std::vector<uint8_t> message;
    uint8_t message_opcode { 0 };
    bool dropping { false };
    std::vector<uint8_t> control;

    size_t queued() const {
        return out.size() - out_sent;
    }

    void frame(uint8_t opcode, const uint8_t* data, size_t len) {
        uint8_t header[10];
        size_t header_len { 0 };
        header[header_len++] = 0x80 | opcode;
        if (len < 126) {
            header[header_len++] = static_cast<uint8_t>(len);
        } else if (len < 65536) {
            header[header_len++] = 126;
            header[header_len++] = static_cast<uint8_t>(len >> 8);
            header[header_len++] = static_cast<uint8_t>(len);
        } else {
            header[header_len++] = 127;
            for (int shift = 56; shift >= 0; shift -= 8) {
                header[header_len++] = static_cast<uint8_t>(static_cast<uint64_t>(len) >> shift);
            }
        }

        // what is sent already goes away first
        if (out_sent) {
            out.erase(out.begin(), out.begin() + out_sent);
            out_sent = 0;
        }

        out.insert(out.end(), header, header + header_len);
        if (len) {
            out.insert(out.end(), data, data + len);
        }
    }
};

} // namespace

class Server::Impl {
public:
    void onEvent(EventHandler handler) {
        _handler = std::move(handler);
    }

    bool accept(http::server::Request& request) {
        const char* key = request.header("Sec-WebSocket-Key");
        const char* version = request.header("Sec-WebSocket-Version");
        if (!key || !version || strcmp(version, "13")) {
            request.addHeader("Sec-WebSocket-Version", "13");
            request.send(400, "text/plain", "Bad WebSocket request");
            return false;
        }

        if (_clients.size() >= _clients_max) {
            request.send(503, "text/plain", "Too many WebSocket clients");
            return false;
        }

        // Sec-WebSocket-Accept: base64 of the SHA-1 of the key and the GUID
        br_sha1_context sha;
        br_sha1_init(&sha);
        br_sha1_update(&sha, key, strlen(key));
        br_sha1_update(&sha, Guid, sizeof(Guid) - 1);
        uint8_t digest[br_sha1_SIZE];
        br_sha1_out(&sha, digest);

        struct Out {
            char data[40];
            size_t len { 0 };
            Out& operator+=(char c) {
                if (len + 1 < sizeof(data)) {
                    data[len++] = c;
                    data[len] = '\0';
                }
                return *this;
            }
        } accept_key;
        text::base64Encode(accept_key, digest, sizeof(digest));

        const uint32_t ip = request.remoteIp();
        auto conn = request.takeConnection();
        if (!conn) {
            return false;
        }

        std::unique_ptr<Client> client(new (std::nothrow) Client());
        if (!client) {
            return false;
        }

        char response[160];
        const int len = snprintf(response, sizeof(response),
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key.data);
        if ((len <= 0) || (static_cast<size_t>(len) >= sizeof(response))) {
            return false;
        }

        client->id = ++_next_id;
        client->ip = ip;
        client->conn = std::move(conn);
        client->out.assign(response, response + len);
        client->last_in = tcp::now();
        client->last_ping = client->last_in;

        const auto id = client->id;
        _clients.push_back(std::move(client));
        if (_handler) {
            _handler(Event::Connect, id, ip, nullptr, 0);
        }

        return true;
    }

    bool hasClient(uint32_t id) const {
        return find(id) != nullptr;
    }

    size_t count() const {
        return _clients.size();
    }

    bool pending() const {
        for (const auto& client : _clients) {
            if (client->queued()) {
                return true;
            }
        }
        return false;
    }

    size_t availableForWrite(uint32_t id) const {
        const auto* client = find(id);
        if (!client || client->closing) {
            return 0;
        }
        const size_t queued = client->queued();
        return (queued < QueueMax) ? (QueueMax - queued) : 0;
    }

    bool text(uint32_t id, const char* data, size_t len) {
        auto* client = find(id);
        if (!client || client->closing || (client->queued() + len + 10 > QueueMax)) {
            return false;
        }

        client->frame(Opcode::Text, reinterpret_cast<const uint8_t*>(data), len);
        flush(*client);
        return true;
    }

    void textAll(const char* data, size_t len) {
        for (auto& client : _clients) {
            text(client->id, data, len);
        }
    }

    void close(uint32_t id) {
        auto* client = find(id);
        if (client && !client->closing) {
            client->frame(Opcode::Close, nullptr, 0);
            client->closing = true;
            flush(*client);
        }
    }

    void closeAll() {
        for (auto& client : _clients) {
            close(client->id);
        }
    }

    size_t clientsMax() const {
        return _clients_max;
    }

    void clientsMax(size_t value) {
        _clients_max = value ? value : 1;
    }

    void loop() {
        const auto now = tcp::now();
        for (auto& client : _clients) {
            step(*client, now);
        }

        for (auto it = _clients.begin(); it != _clients.end();) {
            auto& client = **it;
            const bool sent = (client.queued() == 0);
            const bool gone = client.conn->closed() && !client.conn->available();
            if (gone || (client.closing && sent)) {
                const auto id = client.id;
                const auto ip = client.ip;
                client.conn->close();
                it = _clients.erase(it);
                if (_handler) {
                    _handler(Event::Disconnect, id, ip, nullptr, 0);
                }
                continue;
            }
            ++it;
        }
    }

private:
    Client* find(uint32_t id) const {
        for (const auto& client : _clients) {
            if (client->id == id) {
                return client.get();
            }
        }
        return nullptr;
    }

    void flush(Client& client) {
        while (client.out_sent < client.out.size()) {
            const auto sent = client.conn->write(client.out.data() + client.out_sent,
                client.out.size() - client.out_sent);
            if (!sent) {
                break;
            }
            client.out_sent += sent;
        }

        if (client.out_sent == client.out.size()) {
            client.out.clear();
            client.out_sent = 0;
        }
    }

    void step(Client& client, uint32_t now) {
        flush(client);

        uint8_t buffer[512];
        for (;;) {
            const size_t want = client.in_payload
                ? static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), client.remaining))
                : (client.head_need - client.head_len);
            if (!want && client.in_payload) {
                payloadDone(client);
                continue;
            }

            const auto got = client.conn->read(client.in_payload ? buffer : (client.head + client.head_len), want);
            if (!got) {
                break;
            }

            client.last_in = now;
            if (!client.in_payload) {
                client.head_len += got;
                headerStep(client);
                continue;
            }

            for (size_t index = 0; index < got; ++index) {
                buffer[index] ^= client.mask[(client.payload_pos + index) % 4];
            }
            client.payload_pos += got;
            client.remaining -= got;
            payloadData(client, buffer, got);
            if (!client.remaining) {
                payloadDone(client);
            }
        }

        if (client.closing) {
            return;
        }

        if (now - client.last_in > DeadInterval) {
            client.conn->close();
            return;
        }

        if ((now - client.last_in > PingInterval) && (now - client.last_ping > PingInterval)) {
            client.last_ping = now;
            client.frame(Opcode::Ping, nullptr, 0);
            flush(client);
        }
    }

    // the frame header as far as it came: 2 bytes, the extended length, the mask
    void headerStep(Client& client) {
        if (client.head_len < client.head_need) {
            return;
        }

        const uint8_t len7 = client.head[1] & 0x7f;
        const bool masked = client.head[1] & 0x80;
        size_t need = 2 + ((len7 == 126) ? 2 : (len7 == 127) ? 8 : 0) + (masked ? 4 : 0);
        if (client.head_need < need) {
            client.head_need = need;
            return;
        }

        client.fin = client.head[0] & 0x80;
        client.opcode = client.head[0] & 0x0f;

        size_t at = 2;
        uint64_t length = len7;
        if (len7 == 126) {
            length = (static_cast<uint64_t>(client.head[2]) << 8) | client.head[3];
            at = 4;
        } else if (len7 == 127) {
            length = 0;
            for (size_t index = 0; index < 8; ++index) {
                length = (length << 8) | client.head[2 + index];
            }
            at = 10;
        }

        if (masked) {
            memcpy(client.mask, client.head + at, 4);
        } else {
            memset(client.mask, 0, 4);
        }

        client.remaining = length;
        client.payload_pos = 0;
        client.in_payload = true;
        client.head_len = 0;
        client.head_need = 2;

        if (client.opcode >= Opcode::Close) {
            client.control.clear();
            return;
        }

        if (client.opcode != Opcode::Continuation) {
            client.message.clear();
            client.message_opcode = client.opcode;
            client.dropping = false;
        }

        if (client.message.size() + length > MessageMax) {
            client.dropping = true;
            client.message.clear();
        }
    }

    void payloadData(Client& client, const uint8_t* data, size_t len) {
        if (client.opcode >= Opcode::Close) {
            if (client.control.size() + len <= 125) {
                client.control.insert(client.control.end(), data, data + len);
            }
            return;
        }

        if (!client.dropping) {
            client.message.insert(client.message.end(), data, data + len);
        }
    }

    void payloadDone(Client& client) {
        client.in_payload = false;

        switch (client.opcode) {
        case Opcode::Close:
            if (!client.closing) {
                client.frame(Opcode::Close, client.control.data(), std::min<size_t>(client.control.size(), 2));
                client.closing = true;
                flush(client);
            }
            return;

        case Opcode::Ping:
            client.frame(Opcode::Pong, client.control.data(), client.control.size());
            flush(client);
            return;

        case Opcode::Pong:
            return;

        default:
            break;
        }

        if (!client.fin) {
            return;
        }

        if (!client.dropping && _handler) {
            const size_t len = client.message.size();
            client.message.push_back(0);
            _handler(Event::Message, client.id, client.ip, client.message.data(), len);
        }

        client.message.clear();
        client.message.shrink_to_fit();
        client.dropping = false;
    }

    EventHandler _handler;
    std::vector<std::unique_ptr<Client>> _clients;
    uint32_t _next_id { 0 };
    size_t _clients_max { ClientsDefault };
};

Server::Server() :
    _impl(new (std::nothrow) Impl())
{}

Server::~Server() = default;

void Server::onEvent(EventHandler handler) { if (_impl) { _impl->onEvent(std::move(handler)); } }
bool Server::accept(http::server::Request& request) { return _impl && _impl->accept(request); }
bool Server::hasClient(uint32_t id) const { return _impl && _impl->hasClient(id); }
size_t Server::count() const { return _impl ? _impl->count() : 0; }
bool Server::pending() const { return _impl && _impl->pending(); }
size_t Server::availableForWrite(uint32_t id) const { return _impl ? _impl->availableForWrite(id) : 0; }
bool Server::text(uint32_t id, const char* data, size_t len) { return _impl && _impl->text(id, data, len); }
void Server::textAll(const char* data, size_t len) { if (_impl) { _impl->textAll(data, len); } }
void Server::close(uint32_t id) { if (_impl) { _impl->close(id); } }
void Server::closeAll() { if (_impl) { _impl->closeAll(); } }
void Server::loop() { if (_impl) { _impl->loop(); } }
size_t Server::clientsMax() const { return _impl ? _impl->clientsMax() : 0; }
void Server::clientsMax(size_t value) { if (_impl) { _impl->clientsMax(value); } }

} // namespace websocket
} // namespace espurna
