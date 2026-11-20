/*

ESPOTA - the espota receiver of every chip (espota.py, PlatformIO's upload_protocol = espota), the protocol of the
ESP8266 core's ArduinoOTA. No Arduino and no lwIP sockets: the invitation comes over udp::Socket (libs/udp.h),
the image over tcp::Connection (libs/tcp.h), it is written through the one image receiver (libs/image_receiver.h) into
the head's target, the same as /upgrade. Header only, ota_arduinoota.cpp is its one user.

- UDP invitation "<command> <host port> <size> <md5>\n" (command 0: the firmware; 100, the file system, is refused)
- with a password: "AUTH <nonce>" back, the host answers "200 <cnonce> <md5(md5(password):nonce:cnonce)>"
- "OK", then the board connects to the host's port over TCP, takes the image and acks every part with the number
  of the bytes taken, "OK" at the end; the MD5 of the image is checked before the target's end()
- the whole image is taken inside one handle() call (the progress and the idle callbacks run between the parts). The
  board restarts after the end callback, not here

*/

#pragma once

#include "hash.h"
#include "image_receiver.h"
#include "tcp.h"

#include "udp.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <strings.h>

namespace espurna {
namespace espota {

enum class Error {
    Auth,
    Begin,
    Connect,
    Receive,
    End,
};

class Server {
public:
    // the host waits 10 s for the reply of an authentication and for every ack
    static constexpr uint32_t AuthTimeout { 10000 };
    static constexpr uint32_t ConnectTimeout { 10000 };
    static constexpr uint32_t ReceiveTimeout { 10000 };
    static constexpr size_t Part { 1460 };
    static constexpr size_t LineSize { 128 };

    using Callback = std::function<void()>;
    using Progress = std::function<void(size_t received, size_t size)>;
    using Fail = std::function<void(Error, const char* text)>;
    using Random = uint32_t (*)();

    Server() = default;
    ~Server() {
        end();
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void onStart(Callback callback) {
        _on_start = std::move(callback);
    }

    void onEnd(Callback callback) {
        _on_end = std::move(callback);
    }

    void onError(Fail callback) {
        _on_error = std::move(callback);
    }

    void onProgress(Progress callback) {
        _on_progress = std::move(callback);
    }

    // between the waits of a transfer (the watchdogs, a short sleep)
    void onIdle(Callback callback) {
        _on_idle = std::move(callback);
    }

    // empty - no authentication. The protocol takes the MD5 of the password
    void password(const char* password) {
        _password[0] = '\0';
        if (password && *password) {
            md5_hex(_password, password, std::strlen(password));
        }
    }

    // `random` makes the nonce of the authentication
    bool begin(uint16_t port, image::Target& target, Random random) {
        end();

        _target = &target;
        _random = random;
        _udp.onPacket([this](espurna::udp::Packet& packet) {
            invitation(packet);
        });

        return _udp.listen(port);
    }

    void end() {
        _udp.close();
        _pending.store(false, std::memory_order_release);
        _state = State::Idle;
    }

    void handle() {
        if (!_udp.connected()) {
            return;
        }

        if ((_state == State::WaitAuth) && ((tcp::now() - _auth_at) > AuthTimeout)) {
            _state = State::Idle;
        }

        if (!_pending.load(std::memory_order_acquire)) {
            return;
        }

        char line[LineSize];
        std::memcpy(line, _line, sizeof(line));
        const uint32_t ip = _line_ip;
        const uint16_t port = _line_port;
        _pending.store(false, std::memory_order_release);

        if (_state == State::Idle) {
            invited(line, ip, port);
        } else {
            authenticated(line, ip);
        }
    }

private:
    enum class State {
        Idle,
        WaitAuth,
    };

    static constexpr size_t Md5Hex { 33 };

    static void md5_hex(char (&out)[Md5Hex], const void* data, size_t size) {
        hash::Md5 md5;
        md5.update(data, size);

        uint8_t digest[hash::Md5::Size];
        md5.finish(digest);

        for (size_t index = 0; index < sizeof(digest); ++index) {
            std::snprintf(&out[index * 2], 3, "%02x", digest[index]);
        }
    }

    // "<field> <field> ..." of the invitation and of the answer; false when there is no such field
    static bool field(const char* line, int index, char* out, size_t size) {
        const char* start = line;
        for (int current = 0; current < index; ++current) {
            start = std::strchr(start, ' ');
            if (!start) {
                return false;
            }
            ++start;
        }

        size_t len = std::strcspn(start, " \r\n");
        if (!len || (len >= size)) {
            return false;
        }

        std::memcpy(out, start, len);
        out[len] = '\0';
        return true;
    }

    // lwIP's context: copied for handle(), one at a time
    void invitation(espurna::udp::Packet& packet) {
        if (_pending.load(std::memory_order_acquire)) {
            return;
        }

        const size_t len = (packet.length() < (LineSize - 1)) ? packet.length() : (LineSize - 1);
        std::memcpy(_line, packet.data(), len);
        _line[len] = '\0';
        _line_ip = packet.remoteIP();
        _line_port = packet.remotePort();

        _pending.store(true, std::memory_order_release);
    }

    void reply(const char* text) {
        const size_t len = std::strlen(text);
        espurna::udp::Message message(len);
        if (message) {
            message.write(reinterpret_cast<const uint8_t*>(text), len);
            _udp.sendTo(message, _remote_ip, _remote_udp_port);
        }
    }

    void fail(Error error, const char* text) {
        _state = State::Idle;
        if (_on_error) {
            _on_error(error, text);
        }
    }

    void idle() {
        if (_on_idle) {
            _on_idle();
        }
    }

    void invited(const char* line, uint32_t ip, uint16_t port) {
        char text[40];
        if (!field(line, 0, text, sizeof(text)) || (std::strtol(text, nullptr, 10) != 0)) {
            return;
        }

        if (!field(line, 1, text, sizeof(text))) {
            return;
        }
        const auto remote_port = static_cast<uint16_t>(std::strtoul(text, nullptr, 10));

        if (!field(line, 2, text, sizeof(text))) {
            return;
        }
        const auto size = static_cast<size_t>(std::strtoul(text, nullptr, 10));

        if (!field(line, 3, _md5, sizeof(_md5)) || (std::strlen(_md5) != (Md5Hex - 1))) {
            return;
        }

        if (!remote_port || !size) {
            return;
        }

        _remote_ip = ip;
        _remote_udp_port = port;
        _remote_port = remote_port;
        _size = size;

        if (_password[0]) {
            char seed[48];
            const int len = std::snprintf(seed, sizeof(seed), "%u:%u:%u",
                static_cast<unsigned>(tcp::now()), static_cast<unsigned>(_random ? _random() : 0),
                static_cast<unsigned>(ip));
            md5_hex(_nonce, seed, static_cast<size_t>(len));

            char challenge[5 + Md5Hex];
            std::snprintf(challenge, sizeof(challenge), "AUTH %s", _nonce);
            reply(challenge);

            _state = State::WaitAuth;
            _auth_at = tcp::now();
            return;
        }

        reply("OK");
        transfer();
    }

    // "200 <cnonce> <response>"
    void authenticated(const char* line, uint32_t ip) {
        char text[8];
        if ((ip != _remote_ip) || !field(line, 0, text, sizeof(text)) || (std::strcmp(text, "200") != 0)) {
            return;
        }

        char cnonce[Md5Hex];
        char response[Md5Hex];
        if (!field(line, 1, cnonce, sizeof(cnonce)) || !field(line, 2, response, sizeof(response))) {
            return;
        }

        char challenge[3 * Md5Hex];
        const int len = std::snprintf(challenge, sizeof(challenge), "%s:%s:%s", _password, _nonce, cnonce);

        char expected[Md5Hex];
        md5_hex(expected, challenge, static_cast<size_t>(len));
        if (std::strcmp(response, expected) != 0) {
            reply("Authentication Failed");
            fail(Error::Auth, "authentication failed");
            return;
        }

        reply("OK");
        transfer();
    }

    // the text to the host, then the connection closes (what was written still goes out)
    void finish(tcp::Connection& connection, const char* text) {
        const size_t len = std::strlen(text);
        const auto start = tcp::now();
        while ((connection.writable() < len) && !connection.closed() && ((tcp::now() - start) < ReceiveTimeout)) {
            idle();
        }

        connection.write(reinterpret_cast<const uint8_t*>(text), len);
        connection.close();
    }

    void transfer() {
        _state = State::Idle;

        if (_on_start) {
            _on_start();
        }

        const auto* ip = reinterpret_cast<const uint8_t*>(&_remote_ip);
        char host[16];
        std::snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

        tcp::Connection connection;
        if (!connection.connect(host, _remote_port)) {
            fail(Error::Connect, "no connection");
            return;
        }

        auto start = tcp::now();
        while (!connection.connected() && !connection.closed() && ((tcp::now() - start) < ConnectTimeout)) {
            idle();
        }

        if (!connection.connected()) {
            connection.close();
            fail(Error::Connect, "the host did not answer");
            return;
        }

        auto* data = static_cast<uint8_t*>(std::malloc(Part));
        if (!data) {
            connection.close();
            fail(Error::Begin, "no memory");
            return;
        }

        image::Receiver receiver(*_target);
        receiver.expect(_size);

        hash::Md5 md5;
        const char* error { nullptr };

        size_t received { 0 };
        start = tcp::now();
        while (received < _size) {
            const size_t want = ((_size - received) < Part) ? (_size - received) : Part;
            const size_t got = connection.available()
                ? connection.read(data, want)
                : 0;

            if (!got) {
                if (connection.closed() || ((tcp::now() - start) > ReceiveTimeout)) {
                    break;
                }

                idle();
                continue;
            }

            md5.update(data, got);
            error = receiver.push(data, got);
            if (error) {
                break;
            }

            received += got;
            start = tcp::now();

            char ack[12];
            const int len = std::snprintf(ack, sizeof(ack), "%u", static_cast<unsigned>(got));
            connection.write(reinterpret_cast<const uint8_t*>(ack), static_cast<size_t>(len));

            if (_on_progress) {
                _on_progress(received, _size);
            }
        }

        std::free(data);

        if (error) {
            receiver.abort();
            finish(connection, error);
            fail((received < image::Receiver::FirstPiece) ? Error::Begin : Error::Receive, error);
            return;
        }

        if (received != _size) {
            const bool closed = connection.closed();
            receiver.abort();
            connection.close();
            fail(Error::Receive, closed ? "the host closed the connection" : "timeout");
            return;
        }

        uint8_t digest[hash::Md5::Size];
        md5.finish(digest);

        char md5_text[Md5Hex];
        for (size_t index = 0; index < sizeof(digest); ++index) {
            std::snprintf(&md5_text[index * 2], 3, "%02x", digest[index]);
        }

        if (strcasecmp(md5_text, _md5) != 0) {
            receiver.abort();
            finish(connection, "MD5 Mismatch");
            fail(Error::End, "MD5 mismatch");
            return;
        }

        error = receiver.finish();
        if (error) {
            finish(connection, error);
            fail(Error::End, error);
            return;
        }

        finish(connection, "OK");

        if (_on_end) {
            _on_end();
        }
    }

    espurna::udp::Socket _udp;
    image::Target* _target { nullptr };
    Random _random { nullptr };

    char _password[Md5Hex] {};
    State _state { State::Idle };
    uint32_t _auth_at { 0 };
    char _nonce[Md5Hex] {};

    // the invitation, from lwIP's context
    std::atomic<bool> _pending { false };
    char _line[LineSize] {};
    uint32_t _line_ip { 0 };
    uint16_t _line_port { 0 };

    uint32_t _remote_ip { 0 };
    uint16_t _remote_udp_port { 0 };
    uint16_t _remote_port { 0 };
    size_t _size { 0 };
    char _md5[Md5Hex] {};

    Callback _on_start;
    Callback _on_end;
    Fail _on_error;
    Progress _on_progress;
    Callback _on_idle;
};

} // namespace espota
} // namespace espurna
