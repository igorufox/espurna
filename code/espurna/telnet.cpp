/*

TELNET MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Implements basic socket server & command interface. Name `TELNET` comes from the original implementation,
we don't implement IAC, there is no special byte escape happening and no buffering outside of line-view parser.

Updated to use LWIP APIs directly and avoid ESPAsyncTCP dependency altogether.
Given a chance, may get ported to non-blocking POSIX socket code.

Parts of the code have been borrowed from Thomas Sarlandie's NetServer
- https://github.com/sarfata/kbox-firmware/tree/master/src/esp

Buffered client class based on ESPAsyncTCPbuffer, distributed with the ESPAsyncTCP
- https://github.com/me-no-dev/ESPAsyncTCP/blob/master/src/ESPAsyncTCPbuffer.cpp

Updated to use WiFiServer and support reverse connections by Niek van der Maas <mail at niekvandermaas dot nl>
- https://github.com/xoseperez/espurna/pull/1920

Transport depends on where network callbacks run (ref. ParallelTasks in espurna_tasks.h)
- esp8266, lwip callbacks never overlap with loop(), raw LWIP APIs are used directly
- ESP32 and LibreTiny run lwip in a task of its own, raw LWIP calls from loop() would race with it.
  AsyncTCP does the hand-over to the tcpip task, its callbacks are deferred to loop() (ref. AsyncGuard)
Settings, password prompt, line parsing, terminal commands and the WebUI part are common.

*/

#include "espurna.h"

#if TELNET_SUPPORT

// raw LWIP transport (esp8266) or AsyncTCP (lwIP in a task of its own), see above
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <AsyncTCP.h>
#include <lwip/opt.h>
#endif

#if WEB_SUPPORT
#include "ws.h"
#endif

#include "espurna_tasks.h"
#include "mqtt.h"
#include "telnet.h"
#include "terminal.h"
#include "wifi_orch.h"

#include "libs/URL.h"
#include "libs/Delimiter.h"

#include <atomic>
#include <forward_list>
#include <list>
#include <memory>
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

String password() {
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
String NAME () {\
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
    void process(Print& print) {
        while (!_cmds.empty()) {
            auto cmd = std::move(_cmds.front());
            _cmds.pop_front();

            if (!espurna::terminal::api_find_and_call(cmd, print)) {
                _cmds.clear();
                break;
            }
        }
    }

    void process(String cmd) {
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
    std::list<String> _cmds;
#endif
};

#if defined(ESP8266)

// Generic TCP interface assumes we only have the network buffer available to us.
// Since we can't chain-in additional backing storage through the provided LWIP API,
// force every write to either go through the usual means or cache it using extra N
// u8 arrays that are created when writing data and then destroyed when it is sent
struct ClientWriter {
    size_t write(tcp_pcb* pcb, const uint8_t* data, size_t size) {
        if (!size || (pcb->state != ESTABLISHED)) {
            return 0;
        }

        int8_t flags = TCP_WRITE_FLAG_COPY;

        // first, trying to write directly into the network stack buffers
        size_t written = 0;
        if (_list.empty()) {
            size_t available = tcp_sndbuf(pcb);
            written = std::min(available, size);

            auto err = tcp_write(pcb, data, written, flags);
            if (err != ERR_OK) {
                return 0;
            }

            if (written == size) {
                return size;
            }
        }

        // second, cache the data on the app level
        size_t left { size - written };

        const auto* ptr = data + written;
        flags |= TCP_WRITE_FLAG_MORE;

        while (left) {
            if (_list.empty() && !make_buffer()) {
                break;
            }

            auto& current = _list.back();
            const auto have = current.capacity() - current.size();
            if (have < left) {
                current.insert(current.end(), ptr, ptr + have);
                if (!make_buffer()) {
                    break;
                }

                ptr += have;
                written += have;
                left -= have;
            } else {
                current.insert(current.end(), ptr, ptr + left);
                written = size;
                break;
            }
        }

        // to avoid dumping sources like printf("%02X ") in a loop immediately,
        // stall calling tcp_output until there's a large chunk of data available
        // plus, lwip loop & poll will dump things periodically
        if (!_list.empty()) {
            tcp_output(pcb);
        }

        return written;
    }

    size_t write(tcp_pcb* pcb, StringView data) {
        return write(pcb, reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    }

    void flush(tcp_pcb* pcb) {
        bool wrote { false };
        while (!_list.empty()) {
            auto& chunk = _list.front();

            const size_t available = tcp_sndbuf(pcb);
            if (available < chunk.size()) {
                break;
            }

            auto err = tcp_write(pcb, chunk.data(), chunk.size(), TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) {
                break;
            }

            wrote = true;
            _list.pop_front();
        }

        if (wrote) {
            tcp_output(pcb);
        }
    }

    size_t writable(tcp_pcb* pcb) const {
        return _list.empty() && (tcp_sndbuf(pcb) > 0);
    }

    void reset() {
        _list.clear();
    }

private:
    using Buffer = std::vector<uint8_t>;
    using List = std::list<Buffer>;

    constexpr static size_t BufferSize = TCP_MSS;
    constexpr static size_t BuffersMax =
        (BufferSize == 1460)
            ? 2  // MSS is 1460 bytes, additional 2920 bytes
            : 5; // MSS is 536 bytes, additional 2680 bytes

    bool make_buffer() {
        if (_list.size() < BuffersMax) {
            Buffer buffer;
            buffer.reserve(TCP_MSS);
            _list.push_back(std::move(buffer));
            return true;
        }

        return false;
    }

    List _list;
};

// Terminal output works through Arduino `Print` interface, and everything there uses basic `write`.
// Really really flush both network and internal buffer(s), or we stall our `loop` with the arbitrary
// timeout option it selects for us. (ref. Core files for more)
// Plus, we don't force either `Print` or `Stream` class inheritance for the `T`.
template <typename T>
struct ExhaustingPrint : public Print {
    ExhaustingPrint() = delete;
    explicit ExhaustingPrint(T* client) :
        _client(client)
    {}

    size_t write(const uint8_t* ptr, size_t length) override {
        flush();
        return _client->write(ptr, length);
    }

    size_t write(uint8_t c) override {
        return write(&c, 1);
    }

    // while in the main loop, attempt to flush everything gathered so far as soon as possible
    // currently, neither client state or flush result are checked by the caller
    void flush() override {
        auto flag = PolledFlag<time::CoreClock>();

        while (!flag.wait(duration::Seconds(3))
            && _client->connected()
            && !_client->writable())
        {
            _client->flush();
            time::blockingDelay(
                duration::Milliseconds(10));
        }
    }

private:
    T* _client;
};

struct Address {
    ip_addr_t ip;
    uint16_t port;
};

LineView line_view(pbuf* pb) {
    auto* payload = reinterpret_cast<const char*>(pb->payload);
    return LineView{
        StringView{payload, payload + pb->len}};
}

Address address(tcp_pcb* pcb) {
    Address out;
    ip_addr_copy(out.ip, pcb->remote_ip);
    out.port = pcb->remote_port;
    return out;
}

[[gnu::unused]]
String address_string(Address address) {
    return IPAddress(address.ip).toString() + ':' + String(address.port, 10);
}

// clean-up pcb data stream until telnet data ends
bool process_rfc854(pbuf* pb) {
    const auto* ptr = reinterpret_cast<const uint8_t*>(pb->payload);
    size_t len { pb->len };

    const auto out = process_rfc854(ptr, len);

    pb->len = len;
    pb->payload = const_cast<uint8_t*>(ptr);

    return out;
}

// tracks the provided TCP `pcb`, cannot instantiate one by itself
class Client : public Session {
public:
    Client() = delete;
    Client(tcp_pcb* pcb, bool auth) :
        Session(auth),
        _pcb(pcb),
        _remote(address(pcb))
    {
        // we could 'reap' certain low priority pcbs from
        // the active list by calling `tcp_kill_prio(LEVEL)`
        tcp_setprio(_pcb, TCP_PRIO_MIN);

        // TODO: any reason *not* to enable no-delay?
        tcp_nagle_enable(_pcb);

        // just like netconn aka socket api provided by lwip, we cross-reference
        // these two objects; pcb knows of `this` as `callback_arg`, we track the
        // `pcb` pointer and only remove it when connection closes or errors out
        tcp_arg(_pcb, this);
        tcp_sent(_pcb, s_on_tcp_sent);
        tcp_recv(_pcb, s_on_tcp_recv);
        tcp_err(_pcb, s_on_tcp_error);
        tcp_poll(_pcb, s_on_tcp_poll, 1);

        // sometimes we are already connected. e.g. server accept callback
        if (connected()) {
            _state = _request_auth
                ? State::Authenticating
                : State::Active;
        }
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&& other) = delete;
    Client& operator=(Client&&) = delete;

    ~Client() {
        close();
    }

    bool connect(Address address) {
        if (_pcb) {
            auto err = tcp_connect(
                _pcb,
                &address.ip,
                address.port,
                s_on_tcp_connected);

            if (err != ERR_OK) {
                return false;
            }

            _state = State::Connecting;
            return true;
        }

        return false;
    }

    bool connected() const {
        return _pcb && (_pcb->state == ESTABLISHED);
    }

    err_t abort() {
        return abort(ERR_ABRT);
    }

    err_t close() {
        err_t err = ERR_OK;
        if (_pcb) {
            detach();
            err = tcp_close(_pcb);
            if (err != ERR_OK) {
                err = abort();
            }

            DEBUG_MSG_P(PSTR("[TELNET] Disconnected %s\n"),
                address_string(_remote).c_str());

            _pcb = nullptr;
        }

        return err;
    }

    err_t error() const {
        return _last_err;
    }

    Address remote() const {
        return _remote;
    }

    size_t write(const uint8_t* data, size_t size) {
        if (_pcb && (_state == State::Active)) {
            return _writer.write(_pcb, data, size);
        }

        return 0;
    }

    void flush() {
        if (_pcb) {
            _writer.flush(_pcb);
        }
    }

    // whenever client was disconnected / left hanging
    bool closed() const {
        return _pcb == nullptr;
    }

    // our network & internal buffers are free
    bool writable() {
        if (_pcb) {
            return _writer.writable(_pcb);
        }

        return false;
    }

    void maybe_ask_auth() {
        if (_request_auth) {
            write_message(message::PasswordRequest);
        }
    }

    void process() {
#if TERMINAL_SUPPORT
        ExhaustingPrint<Client> print(this);
        Session::process(print);
#endif
    }

private:
    // can't use an arbitrary `pbuf` with `tcp_write`, since this may
    // be a flash string and without `memcpy_P` we can't read it.
    void write_message(StringView message) {
        if (_pcb) {
            const auto blob = String(message);
            _writer.write(_pcb, blob);
        }
    }

    err_t abort(err_t err) {
        detach();
        _pcb = nullptr;
        _state = State::Idle;
        _last_err = err;
        _writer.reset();
        reset_session();

        DEBUG_MSG_P(PSTR("[TELNET] %s ERROR %s\n"),
            address_string(_remote).c_str(),
#if LWIP_DEBUG
            lwip_strerr(err));
#else
            String(int(err), 10).c_str());
#endif

        return ERR_ABRT;
    }

    void detach() {
        if (_pcb) {
            tcp_arg(_pcb, nullptr);
            tcp_sent(_pcb, nullptr);
            tcp_recv(_pcb, nullptr);
            tcp_err(_pcb, nullptr);
            tcp_poll(_pcb, nullptr, 0);
        }
    }

    static void s_on_tcp_error(void* arg, err_t err) {
        reinterpret_cast<Client*>(arg)->abort(err);
    }

    // TODO: timeout when buffers are filled for a long time?
    static err_t s_on_tcp_poll(void* arg, tcp_pcb*) {
        reinterpret_cast<Client*>(arg)->flush();
        return ERR_OK;
    }

    // no need to check len, since write() copied the data and already advanced the buffer
    // (COPY flag is implicitly enabled, so *not* passing it / flagging as 0 is irrelevant)
    static err_t s_on_tcp_sent(void* arg, tcp_pcb*, uint16_t) {
        reinterpret_cast<Client*>(arg)->flush();
        return ERR_OK;
    }

    err_t on_tcp_recv(pbuf* pb, err_t err) {
        if (!pb || (err != ERR_OK)) {
            return close();
        }

        // We always attempt to parse the network buffer directly first,
        // socat, netcat, etc. usually send a single packet per line.
        // Otherwise, try to buffer it and everything else in the chain.
        for (auto it = pb; it != nullptr; it = it->next) {
            // In case connection happens from an actual telnet client
            if (!process_rfc854(pb)) {
                return close();
            }

            auto view = line_view(it);
            if (_line_buffer.size()) {
                goto next;
            }

            for (auto line = view.next(); line.length() > 0; line = view.next()) {
                auto result = process_line(line);
                if (result.message.length()) {
                    write_message(result.message);
                }

                if (result.close) {
                    return close();
                }
            }

next:
            if (view.length()) {
                _line_buffer.append(view.get());
            }
        }

        if (_line_buffer.overflow()) {
            write_message(message::BufferOverflow);
            return close();
        }

        for (;;) {
            const auto line_result = _line_buffer.next();
            if (line_result.overflow) {
                write_message(message::BufferOverflow);
                return close();
            }

            if (!line_result.value.length()) {
                break;
            }

            auto result = process_line(line_result.value);
            if (result.message.length()) {
                write_message(result.message);
            }

            if (result.close) {
                return close();
            }
        }

        // expect everything to be handled above, we don't allow lingering pbufs
        // (as extra buffers, for retries, or anything else)
        tcp_recved(_pcb, pb->tot_len);
        pbuf_free(pb);

        return ERR_OK;
    }

    static err_t s_on_tcp_recv(void* arg, tcp_pcb*, pbuf* pb, err_t err) {
        return reinterpret_cast<Client*>(arg)->on_tcp_recv(pb, err);
    }

    err_t on_tcp_connected() {
        _state = _request_auth
            ? State::Authenticating
            : State::Active;
        maybe_ask_auth();
        return ERR_OK;
    }

    static err_t s_on_tcp_connected(void* arg, tcp_pcb*, err_t) {
        return reinterpret_cast<Client*>(arg)->on_tcp_connected();
    }

    tcp_pcb* _pcb;
    Address _remote;

    err_t _last_err { ERR_OK };

    ClientWriter _writer;
};

using ClientPtr = std::unique_ptr<Client>;

template <size_t Size>
struct Clients {
    using List = std::forward_list<ClientPtr>;

    bool connected() {
        const auto it = std::find_if(
            std::begin(_clients),
            std::end(_clients),
            [](const ClientPtr& ptr) {
                return static_cast<bool>(ptr);
            });

        return it != std::end(_clients);
    }

    Client* add(ClientPtr&& client) {
        for (auto& entry : _clients) {
            if (!entry || entry->closed()) {
                entry = std::move(client);
                return entry.get();
            }
        }

        return nullptr;
    }

    bool write(const uint8_t* data, size_t len) {
        size_t out = 0;
        for (auto& client : _clients) {
            if (client && client->connected()) {
                out += client->write(data, len);
            }
        }

        return out > 0;
    }

    bool write(const char* data, size_t len) {
        return write(reinterpret_cast<const uint8_t*>(data), len);
    }

    bool write(StringView data) {
        return write(data.data(), data.length());
    }

    void process() {
        for (auto& client : _clients) {
            if (client) {
                client->process();
            }
        }
    }

    void flush() {
        for (auto& client : _clients) {
            if (client) {
                client->flush();
            }
        }
    }

    template <typename T>
    void foreach(T&& callback) {
        for (auto& client : _clients) {
            if (client) {
                callback(client);
            }
        }
    }

private:
    ClientPtr _clients[Size];
};

template <>
struct Clients<1> {
    bool connected() {
        return static_cast<bool>(_client);
    }

    Client* add(ClientPtr&& client) {
        if (!_client || _client->closed()) {
            _client = std::move(client);
            return _client.get();
        }

        return nullptr;
    }

    bool write(const uint8_t* data, size_t len) {
        if (_client && _client->connected()) {
            return _client->write(data, len) > 0;
        }

        return false;
    }

    bool write(const char* data, size_t len) {
        return write(reinterpret_cast<const uint8_t*>(data), len);
    }

    bool write(StringView data) {
        return write(data.data(), data.length());
    }

    void process() {
        if (_client) {
            _client->process();
        }
    }

    void flush() {
        if (_client) {
            _client->flush();
        }
    }

    template <typename T>
    void foreach(T&& callback) {
        if (_client) {
            callback(_client);
        }
    }

private:
    ClientPtr _client;
};

namespace internal {

Clients<build::ClientsMax> clients;

} // namespace internal

bool add(ClientPtr client) {
    auto result = internal::clients.add(std::move(client));
    if (result) {
        DEBUG_MSG_P(PSTR("[TELNET] Connected %s\n"),
            address_string(result->remote()).c_str());
        result->maybe_ask_auth();
        return true;
    }

    client->abort();
    DEBUG_MSG_P(PSTR("[TELNET] Rejecting %s\n"),
        address_string(client.get()->remote()).c_str());

    return false;
}

bool connected() {
    return internal::clients.connected();
}

bool write(const char* data, size_t len) {
    return internal::clients.write(data, len);
}

void flush() {
    internal::clients.flush();
}

void process() {
    internal::clients.process();
}

void loop() {
    flush();
    process();
}

// lwip backlog allows to limit the amount of connected pcbs,
// and needs to be explicitly delayed / accepted:
// > tcp_backlog_delayed(pcb);
// > tcp_backlog_accepted(pcb);
//
// note that b/c of the reverse use-case, this cannot be the only
// way to count the clients
// (however, it may be useful to remove restriction on the reverse)

ClientPtr make_client(tcp_pcb* pcb, bool request_auth) {
    return std::make_unique<Client>(pcb, request_auth);
}

err_t accept(void*, tcp_pcb* pcb, err_t err) {
    if (!pcb || (err != ERR_OK)) {
        return ERR_VAL;
    }

    auto sta = settings::station();
    if (!sta && wifiConnected()) {
        auto ip = wifiStaIp();
        if (ip == pcb->local_ip) {
            return ERR_VAL;
        }
    }

    if (add(make_client(pcb, settings::authentication()))) {
        return ERR_OK;
    }

    return ERR_MEM;
}

bool listen() {
    auto* pcb = tcp_new();
    if (!pcb) {
        return false;
    }

    pcb->so_options |= SOF_REUSEADDR;
    if (tcp_bind(pcb, IP_ADDR_ANY, settings::port()) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        tcp_close(pcb);
        return false;
    }

    tcp_accept(pcb, accept);
    DEBUG_MSG_P(PSTR("[TELNET] Listening on port %hu\n"), pcb->local_port);

    return true;
}

#if TELNET_REVERSE_SUPPORT
namespace reverse {

bool connect(Address address) {
    auto* pcb = tcp_new();
    if (!pcb) {
        return false;
    }

    // wait until the connection attempt happens
    // or, we could also fail right here as well
    auto client = make_client(pcb, false);
    if (!client->connect(address)) {
        return false;
    }

    // we can't stall here, make use of the storage
    if (add(std::move(client))) {
        return true;
    }

    return false;
}

#if TERMINAL_SUPPORT
namespace terminal {
namespace commands {

PROGMEM_STRING(Reverse, "TELNET.REVERSE");

void reverse(::terminal::CommandContext&& ctx) {
    if (ctx.argv.size() != 3) {
        terminalError(ctx, F("TELNET.REVERSE <HOST> <PORT>"));
        return;
    }

    const auto ip = network::dns::gethostbyname(ctx.argv[1]);
    if (!ip.isSet()) {
        terminalError(ctx, F("Host not found"));
        return;
    }

    const auto convert_port = espurna::settings::internal::convert<uint16_t>;
    auto port = convert_port(ctx.argv[2]);
    if (!port) {
        terminalError(ctx, F("Invalid port"));
        return;
    }

    const auto address = Address{
        .ip = ip,
        .port = port,
    };

    if (connect(address)) {
        terminalOK(ctx);
        return;
    }

    terminalError(ctx, F("Unable to connect"));
}

static constexpr ::terminal::Command List[] PROGMEM {
    {Reverse, commands::reverse},
};

} // namespace commands

void setup() {
    espurna::terminal::add(commands::List);
}

} // namespace terminal
#endif

#if MQTT_SUPPORT
namespace mqtt {

void connect_url(String url) {
    URL parsed(std::move(url));
    if (!parsed.host.length() || !parsed.port) {
        DEBUG_MSG_P(PSTR("[TELNET] Cannot parse the url\n"));
        return;
    }

    const auto port = parsed.port;
    network::dns::resolve(
        std::move(parsed.host),
        [port](network::dns::HostPtr host) {
            if (host->err == ERR_OK) {
                const auto addr = Address{
                    .ip = host->addr,
                    .port = port,
                };

                if (connect(addr)) {
                    return;
                }
            }

            DEBUG_MSG_P(PSTR("[TELNET] Cannot connect to %s:%hu\n"),
                host->name.c_str(), port);
        });
}

void setup() {
    mqttRegister([](unsigned int type, StringView topic, StringView payload) {
        switch (type) {
        case MQTT_CONNECT_EVENT:
            mqttSubscribe(MQTT_TOPIC_TELNET_REVERSE);
            break;

        case MQTT_MESSAGE_EVENT: {
            auto t = mqttMagnitude(topic);
            if (t.equals(MQTT_TOPIC_TELNET_REVERSE)) {
                connect_url(payload.toString());
            }
            break;
        }

        }
    });
}

} // namespace mqtt
#endif // MQTT_SUPPORT

void setup() {
#if TERMINAL_SUPPORT
    terminal::setup();
#endif
#if MQTT_SUPPORT
    mqtt::setup();
#endif
}

} // namespace reverse
#endif // TELNET_REVERSE_SUPPORT

#else // defined(ESP8266)

#if TELNET_REVERSE_SUPPORT
#error "TELNET_REVERSE_SUPPORT is only implemented for the esp8266 (raw LWIP) transport"
#endif

// AsyncTCP callbacks run in the async_tcp task, in parallel with loop() (ref. AsyncGuard)
// - clients list and Client state are only touched with the loop lock held, i.e. from loop()
//   or from a callback that got the lock in time. Otherwise the event (with a copy of its data)
//   is handed over to loop(), and every following one is too until those are handled, so
//   everything is still processed in order (ref. Deferred in espurna_tasks.h)
// - AsyncClient is only ever closed from the async_tcp task (poll / ack timeout callbacks),
//   AsyncTCP may be inside one of its handlers for the same client when loop() would close it.
//   loop() only requests it, see Client::request_close()
// - AsyncClient is deleted after its discard (onDisconnect) callback, loop() erases the Client
//   once the handle is gone. Nothing is called for the AsyncClient after discard, so `Client*`
//   given to AsyncTCP as the callback arg outlives all of the callbacks and deferred events
::espurna::system::Deferred deferred;

// Terminal output goes through Arduino `Print`, make sure everything reaches the
// client even when the command prints more than AsyncClient buffers can hold.
// (gives up after 3s of not being able to send, same as the raw LWIP version)
template <typename T>
struct ExhaustingPrint : public Print {
    ExhaustingPrint() = delete;
    explicit ExhaustingPrint(T* client) :
        _client(client)
    {}

    size_t write(const uint8_t* ptr, size_t length) override {
        size_t written = 0;
        const auto start = time::CoreClock::now();

        while (written < length) {
            written += _client->write(ptr + written, length - written);
            if (written >= length) {
                break;
            }

            if (!_client->connected()
                || (time::CoreClock::now() - start) > duration::Seconds(3))
            {
                break;
            }

            _client->flush();
            time::blockingDelay(duration::Milliseconds(10));
        }

        return written;
    }

    size_t write(uint8_t c) override {
        return write(&c, 1);
    }

private:
    T* _client;
};

class Client : public Session {
public:
    // Only installs the callbacks, start() or request_close() decide what happens next
    explicit Client(AsyncClient* handle) :
        _handle(handle)
    {
        _remote_ip = _handle->remoteIP();
        _remote_port = _handle->remotePort();

        _handle->onData(on_data_async, this);
        _handle->onDisconnect(on_disconnect_async, this);
        _handle->onPoll(on_poll_async, this);
        _handle->onTimeout(
            [](void* arg, AsyncClient* ptr, uint32_t) {
                on_poll_async(arg, ptr);
            }, this);
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Loop lock held
    void start(bool auth) {
        _request_auth = auth;
        if (connected()) {
            _state = _request_auth
                ? State::Authenticating
                : State::Active;
            maybe_ask_auth();
        }
    }

    AsyncClient* handle() const {
        return _handle;
    }

    bool connected() const {
        return _handle && !_close_requested.load() && _handle->connected();
    }

    // Actual close() happens in the async_tcp task, see on_poll_async()
    void request_close() {
        _close_requested = true;
    }

    bool close_requested() const {
        return _close_requested.load();
    }

    bool closed() const {
        return _handle == nullptr;
    }

    // AsyncClient::add() may only take a part of the data when its buffers are full
    size_t write(const uint8_t* data, size_t size) {
        if (!connected() || (_state != State::Active)) {
            return 0;
        }

        size_t written = 0;
        while (written < size) {
            const auto added = _handle->add(
                reinterpret_cast<const char*>(data + written), size - written);
            if (!added) {
                break;
            }

            written += added;
        }

        return written;
    }

    void flush() {
        if (connected() && _handle->canSend()) {
            _handle->send();
        }
    }

    bool writable() const {
        return connected() && _handle->space();
    }

    void maybe_ask_auth() {
        if (_request_auth) {
            write_message(message::PasswordRequest);
        }
    }

    void process() {
#if TERMINAL_SUPPORT
        ExhaustingPrint<Client> print(this);
        Session::process(print);
#endif
    }

private:
    void write_message(StringView message) {
        if (connected()) {
            const auto blob = String(message);
            _handle->add(blob.c_str(), blob.length());
            _handle->send();
        }
    }

    // Lines may arrive split between multiple packets (and char-by-char from an actual telnet client)
    // Closing right here would delete AsyncClient while AsyncTCP is still inside its
    // recv handler, it is closed later from the poll callback (see request_close())
    // Loop lock held, called either from the async_tcp task or from loop()
    void on_data(const uint8_t* data, size_t len) {
        if (_close_requested.load()) {
            return;
        }

        if (!process_rfc854(data, len)) {
            request_close();
            return;
        }

        _line_buffer.append(reinterpret_cast<const char*>(data), len);

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

            auto result = process_line(line_result.value);
            if (result.message.length()) {
                write_message(result.message);
            }

            if (result.close) {
                request_close();
                return;
            }
        }
    }

    // Loop lock held. AsyncClient is disconnected, nothing else is called for it after this
    void on_disconnect(AsyncClient* ptr) {
        if (_handle == ptr) {
            _handle = nullptr;
        }

        // ~AsyncClient() would call discard once again if it still had a pcb
        ptr->onData(nullptr, nullptr);
        ptr->onDisconnect(nullptr, nullptr);
        ptr->onPoll(nullptr, nullptr);
        ptr->onTimeout(nullptr, nullptr);
        delete ptr;
    }

    static void on_data_async(void* arg, AsyncClient*, void* data, size_t len) {
        auto* client = static_cast<Client*>(arg);

        ::espurna::system::AsyncGuard guard;
        if (deferred.defer(guard)) {
            // data is only valid during this call
            const auto* ptr = static_cast<const uint8_t*>(data);
            std::vector<uint8_t> copy(ptr, ptr + len);
            deferred.run([client, copy]() {
                client->on_data(copy.data(), copy.size());
            });
            return;
        }

        client->on_data(static_cast<const uint8_t*>(data), len);
    }

    // AsyncClient may still be written to by loop(), delete it only with the lock held
    static void on_disconnect_async(void* arg, AsyncClient* ptr) {
        auto* client = static_cast<Client*>(arg);

        ::espurna::system::AsyncGuard guard;
        if (deferred.defer(guard)) {
            deferred.run([client, ptr]() {
                client->on_disconnect(ptr);
            });
            return;
        }

        client->on_disconnect(ptr);
    }

    // Periodic (and ack timeout) callback, the only place AsyncClient gets closed from.
    // Discard callback is called right from close(), neither `client` nor `ptr` may be
    // touched after it. Without the lock, retry on the next poll.
    static void on_poll_async(void* arg, AsyncClient* ptr) {
        auto* client = static_cast<Client*>(arg);
        if (!client->close_requested()) {
            return;
        }

        ::espurna::system::AsyncGuard guard;
        if (!guard.locked()) {
            return;
        }

        if (client->_handle == ptr) {
            ptr->close(true);
        }
    }

    AsyncClient* _handle;
    IPAddress _remote_ip;
    uint16_t _remote_port;
    std::atomic<bool> _close_requested { false };
};

std::list<std::unique_ptr<Client>> clients;
AsyncServer* server { nullptr };

// Loop lock held. Connections through STA are only allowed when telnetSTA is set
bool accepted(const Client& client) {
    auto* handle = client.handle();
    if (!handle || !client.connected()) {
        return false;
    }

    if (!settings::station() && wifiConnected() && (handle->localIP() == wifiStaIp())) {
        return false;
    }

    size_t active = 0;
    for (const auto& other : clients) {
        if (!other->close_requested()) {
            ++active;
        }
    }

    return active < build::ClientsMax;
}

// Loop lock held. Rejected client stays in the list until it is closed
void attach(std::unique_ptr<Client> client) {
    if (accepted(*client)) {
        client->start(settings::authentication());
    } else {
        client->request_close();
    }

    clients.push_back(std::move(client));
}

// AsyncServer allocates accepted clients and never frees them, Client callbacks do.
// Client only installs its callbacks here, events arriving before attach() are deferred as well
void add(AsyncClient* handle) {
    auto client = std::make_unique<Client>(handle);

    ::espurna::system::AsyncGuard guard;
    if (deferred.defer(guard)) {
        auto* ptr = client.release();
        deferred.run([ptr]() {
            attach(std::unique_ptr<Client>(ptr));
        });
        return;
    }

    attach(std::move(client));
}

// Closing clients are counted as well, until AsyncTCP is done with them
bool connected() {
    return !clients.empty();
}

bool write(const char* data, size_t len) {
    bool out = false;
    for (auto& client : clients) {
        if (client->connected()) {
            client->write(reinterpret_cast<const uint8_t*>(data), len);
            out = true;
        }
    }

    return out;
}

void loop() {
    auto it = clients.begin();
    while (it != clients.end()) {
        if ((*it)->closed()) {
            it = clients.erase(it);
            continue;
        }

        // closing ones are left to the async_tcp task, see Client::on_poll_async()
        if (!(*it)->close_requested()) {
            (*it)->process();
            (*it)->flush();
        }

        ++it;
    }
}

bool listen() {
    if (server) {
        delete server;
    }

    server = new AsyncServer(settings::port());
    server->onClient([](void*, AsyncClient* client) { add(client); }, nullptr);
    server->begin();

    // e.g. RESET command reply, don't restart before it reaches the client.
    // tcp_sndbuf() only goes back to its initial size once all of the sent data is acked
    ::espurna::system::registerPendingOutput([]() {
        bool out = false;
        for (auto& client : clients) {
            if (client->connected()) {
                client->flush();
                out = out || (client->handle()->space() < TCP_SND_BUF);
            }
        }
        return out;
    });

    return true;
}

#endif // defined(ESP8266)

#if WEB_SUPPORT
namespace web {

bool onKeyCheck(espurna::StringView key, const JsonVariant&) {
    return settings::query::checkExactPrefix(key);
}

void onVisible(JsonObject& root) {
    wsPayloadModule(root, settings::Prefix);
}

void onConnected(JsonObject& root) {
    for (auto setting : settings::query::Settings) {
        root[setting.key()] = setting.value();
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
#if TELNET_REVERSE_SUPPORT
    reverse::setup();
#endif
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
