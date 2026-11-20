/*

TELNET MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Implements basic socket server & command interface. Name `TELNET` comes from the original implementation,
we don't implement IAC, there is no special byte escape happening and no buffering outside of line-view parser.

Parts of the code have been borrowed from Thomas Sarlandie's NetServer
- https://github.com/sarfata/kbox-firmware/tree/master/src/esp

Updated to use WiFiServer and support reverse connections by Niek van der Maas <mail at niekvandermaas dot nl>
- https://github.com/xoseperez/espurna/pull/1920

One transport for every chip: the TCP connection and listener of libs/tcp.h (lwIP raw API), everything in loop().
Settings, password prompt, line parsing, terminal commands and the WebUI part are in the Session.

*/

#include "espurna.h"

#if TELNET_SUPPORT

#include "libs/tcp.h"

#if WEB_SUPPORT
#include "ws.h"
#endif

#include "espurna_tasks.h"
#include "mqtt.h"
#include "telnet.h"
#include "terminal.h"
#include "espurna_wifi.h"

#include "libs/URL.h"
#include "libs/Delimiter.h"

#include <list>
#include <memory>
#include <new>
#include <vector>

namespace espurna {
namespace telnet {
namespace {

constexpr bool isEspurnaMinimal() {
#if defined(ESPURNA_MINIMAL_ARDUINO_OTA) || defined(ESPURNA_MINIMAL_WEBUI)
    return true;
#else
    return false;
#endif
}

namespace build {

constexpr size_t LineBufferSize { TELNET_LINE_BUFFER_SIZE };

constexpr size_t ClientsMax { TELNET_MAX_CLIENTS };
static_assert(ClientsMax > 0, "");

constexpr uint16_t port() {
    return TELNET_PORT;
}

constexpr bool station() {
    return 1 == TELNET_STA;
}

constexpr bool authentication() {
    return isEspurnaMinimal() ? false : (1 == TELNET_AUTHENTICATION);
}

} // namespace build

namespace settings {

STRING_VIEW_INLINE(Prefix, "telnet");

namespace keys {

PROGMEM_STRING(Station, "telnetSTA");
PROGMEM_STRING(Authentication, "telnetAuth");
PROGMEM_STRING(Port, "telnetPort");

} // namespace keys

espurna::string password() {
    return systemPassword();
}

bool station() {
    return getSetting(keys::Station, build::station());
}

bool authentication() {
    return password().length() && getSetting(keys::Authentication, build::authentication());
}

uint16_t port() {
    return getSetting(keys::Port, build::port());
}

namespace query {
namespace internal {

#define EXACT_VALUE(NAME, FUNC)\
espurna::string NAME () {\
    return espurna::settings::internal::serialize(FUNC());\
}

EXACT_VALUE(station, settings::station)
EXACT_VALUE(authentication, settings::authentication)
EXACT_VALUE(port, settings::port)

#undef EXACT_VALUE

} // namespace internal

static constexpr std::array<espurna::settings::query::Setting, 3> Settings PROGMEM {{
     {keys::Station, internal::station},
     {keys::Authentication, internal::authentication},
     {keys::Port, internal::port},
}};

bool checkExactPrefix(StringView key) {
    return key.startsWith(settings::Prefix);
}

espurna::settings::query::Result findFrom(StringView key) {
    return espurna::settings::query::findFrom(Settings, key);
}

void setup() {
    settingsRegisterQueryHandler({
        .check = checkExactPrefix,
        .get = findFrom,
    });
}

} // namespace query
} // namespace settings

namespace message {

PROGMEM_STRING(PasswordRequest, "Password (disconnects after 1 failed attempt): ");
PROGMEM_STRING(InvalidPassword, "-ERROR: Invalid password\n");
PROGMEM_STRING(BufferOverflow, "-ERROR: Buffer overflow\n");
PROGMEM_STRING(OkPassword, "+OK\n");

} // namespace message

// clean-up data stream until telnet data ends
bool process_rfc854(const uint8_t*& ptr, size_t& len) {
    static constexpr uint8_t Iac { 0xff };

    static constexpr uint8_t Will { 0xfb };
    static constexpr uint8_t Wont { 0xfc };
    static constexpr uint8_t Do { 0xfd };
    static constexpr uint8_t Dont { 0xfe };

    static constexpr uint8_t Eof { 0xec };

    bool out { true };

    while (out && (len >= 2)) {
        switch (ptr[0]) {
        case Iac:
            switch (ptr[1]) {
            case Will:
            case Wont:
            case Do:
            case Dont:
                if (len >= 3) {
                    ptr += 3;
                    len -= 3;
                    continue;
                }

                break;

            case Eof:
                len = 0;
                out = false;
                ptr = nullptr;
                break;
            }

            break;
        }

        break;
    }

    return out;
}

// Everything between the received data and the response, regardless of the transport:
// optional password prompt, then lines become terminal commands, executed from loop()
class Session {
public:
    struct ProcessLineResult {
        StringView message;
        bool close { false };
    };

protected:
    enum class State {
        Idle,
        Connecting,
        Authenticating,
        Active,
    };

    Session() = default;
    explicit Session(bool request_auth) :
        _request_auth(request_auth)
    {}

#if TERMINAL_SUPPORT
    void process(::espurna::Output& print) {
        while (!_cmds.empty()) {
            auto cmd = std::move(_cmds.front());
            _cmds.pop_front();

            if (!espurna::terminal::api_find_and_call(cmd, print)) {
                _cmds.clear();
                break;
            }
        }
    }

    void process(espurna::string cmd) {
        _cmds.push_back(std::move(cmd));
    }
#endif

    void reset_session() {
#if TERMINAL_SUPPORT
        _cmds.clear();
#endif
    }

    auto process_line(StringView line) -> ProcessLineResult {
        ProcessLineResult out;
        if (!line.length()) {
            return out;
        }

        switch (_state) {
        case State::Idle:
        case State::Connecting:
            break;
        case State::Active:
#if TERMINAL_SUPPORT
            process(line.toString());
#endif
            break;
        case State::Authenticating:
            if (!systemPasswordEquals(stripNewline(line))) {
                out.message = StringView{message::InvalidPassword};
                out.close = true;
                return out;
            }

            out.message = StringView{message::OkPassword};
            _state = State::Active;
            break;
        }

        return out;
    }

    State _state { State::Idle };
    bool _request_auth { false };

    LineBuffer<build::LineBufferSize> _line_buffer;

#if TERMINAL_SUPPORT
    std::list<espurna::string> _cmds;
#endif
};

// One transport for every chip: the TCP connection and listener of libs/tcp.h (lwIP raw API, the same code on
// ESP8266, ESP32 and Beken). lwIP's callbacks only fill the connection's receive ring; accepting, reading, writing
// and closing all happen in loop(), so nothing here is ever touched from another task.

// Output waiting for the connection's send buffer (the terminal prints faster than TCP takes it)
constexpr size_t OutputMax { 4096 };

struct Address {
    espurna::Ip4 ip;
};

[[gnu::unused]]
espurna::string address_string(Address address) {
    return ipToString(address.ip);
}

class Client : public Session {
public:
    // accepted by the listener
    explicit Client(std::unique_ptr<tcp::Connection> connection) :
        _conn(std::move(connection)),
        _remote(Address{
            .ip = espurna::Ip4(_conn ? _conn->remoteIp() : 0),
        })
    {}

    // an outgoing connection (TELNET.REVERSE, MQTT): connect() resolves the name itself
    Client(espurna::string host, uint16_t port) :
        _conn(new (std::nothrow) tcp::Connection()),
        _host(std::move(host)),
        _port(port)
    {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect() {
        _state = State::Connecting;
        if (_conn && _conn->connect(_host.c_str(), _port)) {
            return true;
        }

        DEBUG_MSG_P(PSTR("[TELNET] Cannot connect to %s:%hu\n"), _host.c_str(), _port);
        _state = State::Idle;
        _conn.reset();
        return false;
    }

    void start(bool auth) {
        _request_auth = auth;
        activate();
    }

    tcp::Connection* connection() const {
        return _conn.get();
    }

    Address remote() const {
        return _remote;
    }

    bool connected() const {
        return _conn && _conn->connected() && !_conn->closed() && !_close_requested;
    }

    void request_close() {
        _close_requested = true;
    }

    bool close_requested() const {
        return _close_requested;
    }

    // nothing more will happen on this connection
    bool closed() const {
        return !_conn;
    }

    size_t write(const uint8_t* data, size_t size) {
        if (!connected() || (_state != State::Active)) {
            return 0;
        }

        return append(data, size);
    }

    void flush() {
        if (!_conn || _output.empty()) {
            return;
        }

        const auto sent = _conn->write(_output.data(), _output.size());
        if (sent) {
            _output.erase(_output.begin(), _output.begin() + sent);
        }
    }

    // our buffer is empty and the connection takes more
    bool writable() {
        return connected() && _output.empty() && (_conn->writable() > 0);
    }

    // something was not handed to TCP yet
    bool pending_output() {
        flush();
        return connected() && !_output.empty();
    }

    void loop() {
        if (!_conn) {
            return;
        }

        if (_state == State::Connecting) {
            if (_conn->connected()) {
                _remote.ip = espurna::Ip4(_conn->remoteIp());
                activate();
            } else if (_conn->closed()) {
                DEBUG_MSG_P(PSTR("[TELNET] Cannot connect to %s:%hu\n"), _host.c_str(), _port);
                drop();
                return;
            }
            return;
        }

        uint8_t buffer[128];
        for (;;) {
            const auto got = _conn->read(buffer, sizeof(buffer));
            if (!got) {
                break;
            }

            on_data(buffer, got);
        }

        flush();
        if (!_close_requested) {
            process();
        }

        // after the last of the output went out (the password error, the overflow message)
        if (_close_requested && _output.empty()) {
            drop();
            return;
        }

        if (_conn->closed() && !_conn->available()) {
            drop();
        }
    }

    void process() {
#if TERMINAL_SUPPORT
        ExhaustingPrint print(this);
        Session::process(print);
#endif
    }

private:
    // Terminal output (espurna::Output): wait (up to 3 s) for the connection to take what was printed
    // before taking more, the loop sleeps in between and lwIP sends meanwhile
    struct ExhaustingPrint : public Output {
        explicit ExhaustingPrint(Client* client) :
            _client(client)
        {}

        using Output::write;

        void write(const char* ptr, size_t length) override {
            flush();
            _client->write(reinterpret_cast<const uint8_t*>(ptr), length);
        }

        void flush() override {
            auto flag = PolledFlag<time::CoreClock>();
            while (!flag.wait(duration::Seconds(3))
                && _client->connected()
                && !_client->writable())
            {
                _client->flush();
                time::blockingDelay(duration::Milliseconds(10));
            }
        }

    private:
        Client* _client;
    };

    void activate() {
        if (!connected()) {
            return;
        }

        _state = _request_auth
            ? State::Authenticating
            : State::Active;
        DEBUG_MSG_P(PSTR("[TELNET] Connected %s\n"), address_string(_remote).c_str());
        if (_request_auth) {
            write_message(message::PasswordRequest);
        }
    }

    void drop() {
        if (_state != State::Idle) {
            DEBUG_MSG_P(PSTR("[TELNET] Disconnected %s\n"), address_string(_remote).c_str());
        }

        _state = State::Idle;
        _output.clear();
        reset_session();
        _conn.reset();
    }

    size_t append(const uint8_t* data, size_t size) {
        const auto room = (_output.size() < OutputMax) ? (OutputMax - _output.size()) : 0;
        const auto take = std::min(room, size);
        if (take) {
            _output.insert(_output.end(), data, data + take);
            flush();
        }

        return take;
    }

    // a flash string is copied out first (espurna::string), messages are rare: the prompt, the password result, errors
    void write_message(StringView message) {
        if (_conn) {
            const auto blob = espurna::string(message);
            append(reinterpret_cast<const uint8_t*>(blob.c_str()), blob.length());
        }
    }

    bool handle_line(StringView line) {
        auto result = process_line(line);
        if (result.message.length()) {
            write_message(result.message);
        }

        if (result.close) {
            request_close();
            return false;
        }

        return true;
    }

    void on_data(const uint8_t* data, size_t len) {
        if (_close_requested) {
            return;
        }

        // In case connection happens from an actual telnet client
        if (!process_rfc854(data, len)) {
            request_close();
            return;
        }

        // We always attempt to parse the received data directly first,
        // socat, netcat, etc. usually send a single packet per line.
        // Otherwise (and char-by-char from an actual telnet client), buffer it
        const auto* begin = reinterpret_cast<const char*>(data);
        auto view = LineView{StringView{begin, begin + len}};

        if (!_line_buffer.size()) {
            for (auto line = view.next(); line.length() > 0; line = view.next()) {
                if (!handle_line(line)) {
                    return;
                }
            }
        }

        if (view.length()) {
            _line_buffer.append(view.get());
        }

        if (_line_buffer.overflow()) {
            write_message(message::BufferOverflow);
            request_close();
            return;
        }

        for (;;) {
            const auto line_result = _line_buffer.next();
            if (line_result.overflow) {
                write_message(message::BufferOverflow);
                request_close();
                return;
            }

            if (!line_result.value.length()) {
                break;
            }

            if (!handle_line(line_result.value)) {
                return;
            }
        }
    }

    std::unique_ptr<tcp::Connection> _conn;
    Address _remote {};
    espurna::string _host;
    uint16_t _port { 0 };

    bool _close_requested { false };
    std::vector<uint8_t> _output;
};

std::list<std::unique_ptr<Client>> clients;
tcp::Listener server;

// closing ones are not counted
size_t active() {
    size_t out = 0;
    for (const auto& client : clients) {
        if (!client->closed() && !client->close_requested()) {
            ++out;
        }
    }

    return out;
}

// Connections through STA are only allowed when telnetSTA is set
bool accepted(Client& client) {
    auto* conn = client.connection();
    if (!conn || !client.connected()) {
        return false;
    }

    if (!settings::station() && wifiConnected() && (espurna::Ip4(conn->localIp()) == wifiStaIp())) {
        return false;
    }

    return active() < build::ClientsMax;
}

void attach(std::unique_ptr<Client> client) {
    if (accepted(*client)) {
        client->start(settings::authentication());
    } else {
        DEBUG_MSG_P(PSTR("[TELNET] Rejecting %s\n"), address_string(client->remote()).c_str());
        client->request_close();
    }

    clients.push_back(std::move(client));
}

bool connected() {
    return !clients.empty();
}

bool write(const char* data, size_t len) {
    size_t out = 0;
    for (auto& client : clients) {
        out += client->write(reinterpret_cast<const uint8_t*>(data), len);
    }

    return out > 0;
}

void loop() {
    for (;;) {
        auto conn = server.accept();
        if (!conn) {
            break;
        }

        auto client = std::make_unique<Client>(std::move(conn));
        attach(std::move(client));
    }

    auto it = clients.begin();
    while (it != clients.end()) {
        (*it)->loop();
        if ((*it)->closed()) {
            it = clients.erase(it);
            continue;
        }

        ++it;
    }
}

bool listen() {
    server.end();

    const auto port = settings::port();
    if (!server.begin(port, build::ClientsMax)) {
        DEBUG_MSG_P(PSTR("[TELNET] Cannot listen on port %hu\n"), port);
        return false;
    }

    DEBUG_MSG_P(PSTR("[TELNET] Listening on port %hu\n"), port);

    // e.g. RESET command reply, don't restart before it reaches the client
    ::espurna::system::registerPendingOutput([]() {
        bool out = false;
        for (auto& client : clients) {
            out = client->pending_output() || out;
        }
        return out;
    });

    return true;
}

namespace reverse {

// An outgoing session (the telnet_reverse module: TELNET.REVERSE, MQTT <root>/telnet_reverse): shares the client limit
// with the accepted ones; the connection resolves the name itself
bool connect(espurna::string host, uint16_t port) {
    if (active() >= build::ClientsMax) {
        return false;
    }

    auto client = std::make_unique<Client>(std::move(host), port);
    if (!client->connect()) {
        return false;
    }

    clients.push_back(std::move(client));
    return true;
}

} // namespace reverse

#if WEB_SUPPORT
namespace web {

bool onKeyCheck(espurna::StringView key, espurna::json::Value) {
    return settings::query::checkExactPrefix(key);
}

void onVisible(espurna::json::Writer& root) {
    wsPayloadModule(root, settings::Prefix);
}

void onConnected(espurna::json::Writer& root) {
    for (auto setting : settings::query::Settings) {
        root.field(setting.key(), setting.value());
    }
}

void setup() {
    wsRegister()
        .onKeyCheck(onKeyCheck)
        .onVisible(onVisible)
        .onConnected(onConnected);
}

} // namespace web
#endif

void setup() {
#if WEB_SUPPORT
    web::setup();
#endif

    listen();

    settings::query::setup();

    ::espurnaRegisterLoop(loop);
}

} // namespace
} // namespace telnet
} // namespace espurna

uint16_t telnetPort() {
    return espurna::telnet::settings::port();
}

// the telnet_reverse module's: a session out to a listener (a board behind NAT)
bool telnetReverse(const char* host, uint16_t port) {
    return host && *host && port && espurna::telnet::reverse::connect(espurna::string(host), port);
}

bool telnetConnected() {
    return espurna::telnet::connected();
}

bool telnetDebugSend(const DebugPrefix& prefix, const char* message, size_t length) {
    size_t out = 0;

    if (telnetConnected()) {
        if (debugWithPrefix(prefix)) {
            out += espurna::telnet::write(prefix, debugPrefixLength(prefix));
        }

        out += espurna::telnet::write(message, length);
    }

    return out > 0;
}

void telnetSetup() {
    espurna::telnet::setup();
}

#endif
