/*

TELNET MODULE

Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
Copyright (C) 2019-2022 by Maxim Prokhorov <prokhorov dot max at outlook dot com>

Implements basic socket server & command interface. Name `TELNET` comes from the original implementation,
we don't implement IAC, there is no special byte escape happening and no buffering outside of line-view parser.

Parts of the code have been borrowed from Thomas Sarlandie's NetServer
- https://github.com/sarfata/kbox-firmware/tree/master/src/esp

Buffered client class based on ESPAsyncTCPbuffer, distributed with the ESPAsyncTCP
- https://github.com/me-no-dev/ESPAsyncTCP/blob/master/src/ESPAsyncTCPbuffer.cpp

Updated to use WiFiServer and support reverse connections by Niek van der Maas <mail at niekvandermaas dot nl>
- https://github.com/xoseperez/espurna/pull/1920

One transport for every chip, AsyncClient / AsyncServer of the chip's async TCP library.
Where network callbacks run differs (ref. ParallelTasks in espurna_tasks.h), the transport
follows the loop lock rules and works the same in both models, see the Client class below.
Settings, password prompt, line parsing, terminal commands and the WebUI part are in the Session.

*/

#include "espurna.h"

#if TELNET_SUPPORT

// the async TCP library of the chip, same AsyncClient / AsyncServer API
#include "espurna_asynctcp.h"
#if !defined(ESP8266)
#include <lwip/opt.h>
#endif

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

// One transport for every chip, on top of the AsyncClient / AsyncServer API
// (ESPAsyncTCP on esp8266, AsyncTCP on ESP32 and LibreTiny)
//
// Network callbacks either never overlap with loop() (esp8266) or run in the async_tcp task in parallel
// with it (ESP32, LibreTiny), ref. ParallelTasks in espurna_tasks.h. The rules below are written for the
// second model, on esp8266 every guard is always locked and nothing is ever deferred.
// - clients list and Client state are only touched with the loop lock held, i.e. from loop()
//   or from a callback that got the lock in time. Otherwise the event (with a copy of its data)
//   is handed over to loop(), and every following one is too until those are handled, so
//   everything is still processed in order (ref. Deferred in espurna_tasks.h)
// - AsyncClient is only ever closed from the network side (poll / ack timeout callbacks),
//   the library may be inside one of its handlers for the same client when loop() would close it.
//   loop() only requests it, see Client::request_close()
// - AsyncClient is deleted after its discard (onDisconnect) callback, loop() erases the Client
//   once the handle is gone. Nothing is called for the AsyncClient after discard, so `Client*`
//   given to the library as the callback arg outlives all of the callbacks and deferred events
// - loop() sleeping inside of a blocking delay lets the network side run (esp8266 SYS tasks,
//   ESP32 / LibreTiny loop lock is released). Client object stays valid, its handle may go away
::espurna::system::Deferred deferred;

// Generic TCP interface assumes we only have the network buffer available to us.
// Since we can't chain-in additional backing storage through the provided API,
// force every write to either go through the usual means or cache it using extra N
// u8 arrays that are created when writing data and then destroyed when it is sent
struct ClientWriter {
    size_t write(AsyncClient* handle, const uint8_t* data, size_t size) {
        if (!size || !handle->connected()) {
            return 0;
        }

        // first, trying to write directly into the network stack buffers
        size_t written = 0;
        if (_list.empty()) {
            const size_t available = handle->space();
            written = std::min(available, size);

            if (written) {
                const auto added = handle->add(
                    reinterpret_cast<const char*>(data), written, ASYNC_WRITE_FLAG_COPY);
                if (added != written) {
                    return 0;
                }
            }

            if (written == size) {
                return size;
            }
        }

        // second, cache the data on the app level
        size_t left { size - written };
        const auto* ptr = data + written;

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
        // stall the output until there's a large chunk of data available
        // plus, lwip loop & poll will dump things periodically
        if (!_list.empty()) {
            handle->send();
        }

        return written;
    }

    size_t write(AsyncClient* handle, StringView data) {
        return write(handle, reinterpret_cast<const uint8_t*>(data.c_str()), data.length());
    }

    void flush(AsyncClient* handle) {
        if (!handle->connected()) {
            return;
        }

        bool wrote { false };
        while (!_list.empty()) {
            auto& chunk = _list.front();

            const size_t available = handle->space();
            if (available < chunk.size()) {
                break;
            }

            const auto added = handle->add(
                reinterpret_cast<const char*>(chunk.data()), chunk.size(), ASYNC_WRITE_FLAG_COPY);
            if (added != chunk.size()) {
                break;
            }

            wrote = true;
            _list.pop_front();
        }

        if (wrote) {
            handle->send();
        }
    }

    bool empty() const {
        return _list.empty();
    }

    bool writable(AsyncClient* handle) const {
        return _list.empty() && (handle->space() > 0);
    }

    void reset() {
        _list.clear();
    }

private:
    using Buffer = std::vector<uint8_t>;
    using List = std::list<Buffer>;

    constexpr static size_t BufferSize = TCP_MSS;
    constexpr static size_t BuffersMax =
        (BufferSize >= 1024)
            ? 2  // e.g. MSS is 1460 bytes, additional 2920 bytes
            : 5; // e.g. MSS is 536 bytes, additional 2680 bytes

    bool make_buffer() {
        if (_list.size() < BuffersMax) {
            Buffer buffer;
            buffer.reserve(BufferSize);
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
    // (gives up after 3s of not being able to send)
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
    IPAddress ip;
    uint16_t port;
};

[[gnu::unused]]
String address_string(Address address) {
    return ipToString(address.ip) + ':' + String(address.port, 10);
}

class Client : public Session {
public:
    // Only installs the callbacks, start() / connect() or request_close() decide what happens next
    Client(AsyncClient* handle, Address remote) :
        _handle(handle),
        _remote(remote)
    {
        _handle->onConnect(on_connect_async, this);
        _handle->onData(on_data_async, this);
        _handle->onAck(on_ack_async, this);
        _handle->onError(on_error_async, this);
        _handle->onDisconnect(on_disconnect_async, this);
        _handle->onPoll(on_poll_async, this);
        _handle->onTimeout(
            [](void* arg, AsyncClient* ptr, uint32_t) {
                on_poll_async(arg, ptr);
            }, this);
    }

    // Accepted by the server, remote is already known
    explicit Client(AsyncClient* handle) :
        Client(handle, Address{
            .ip = handle->remoteIP(),
            .port = handle->remotePort(),
        })
    {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Loop lock held. Accepted connection
    void start(bool auth) {
        _request_auth = auth;
        if (connected()) {
            _state = _request_auth
                ? State::Authenticating
                : State::Active;
            DEBUG_MSG_P(PSTR("[TELNET] Connected %s\n"),
                address_string(_remote).c_str());
            maybe_ask_auth();
        }
    }

    // Loop lock held. Outgoing connection, see on_connected()
    // Handle is gone when this fails, loop() erases the Client
    bool connect() {
        _state = State::Connecting;
        if (_handle->connect(_remote.ip, _remote.port)) {
            return true;
        }

        _state = State::Idle;
        release(_handle);
        _handle = nullptr;

        return false;
    }

    AsyncClient* handle() const {
        return _handle;
    }

    Address remote() const {
        return _remote;
    }

    bool connected() const {
        return _handle && !_close_requested && _handle->connected();
    }

    // Actual close() happens on the network side, see on_poll_async()
    void request_close() {
        _close_requested = true;
    }

    bool close_requested() const {
        return _close_requested;
    }

    // Handle was disconnected and deleted
    bool closed() const {
        return _handle == nullptr;
    }

    size_t write(const uint8_t* data, size_t size) {
        if (connected() && (_state == State::Active)) {
            return _writer.write(_handle, data, size);
        }

        return 0;
    }

    void flush() {
        if (connected()) {
            _writer.flush(_handle);
        }
    }

    // our network & internal buffers are free
    bool writable() const {
        return connected() && _writer.writable(_handle);
    }

    // Loop lock held. Something was not sent out or acknowledged yet
    bool pending_output() {
        if (!connected()) {
            return false;
        }

        flush();
        if (!_writer.empty()) {
            return true;
        }

        // space only goes back to its initial size once all of the sent data is acked
        if (_handle->space() < TCP_SND_BUF) {
            if (_handle->canSend()) {
                _handle->send();
            }

            return true;
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
    // can't use an arbitrary pointer with the network buffer, since this may
    // be a flash string and without `memcpy_P` we can't read it.
    // messages are rare (prompt, password result, errors), sent right away
    void write_message(StringView message) {
        if (connected()) {
            const auto blob = String(message);
            _writer.write(_handle, blob);
            if (_handle->connected()) {
                _handle->send();
            }
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

    // Closing right here would delete AsyncClient while the library is still inside its
    // recv handler, it is closed later from the poll callback (see request_close())
    // Loop lock held, called either from the network side or from loop()
    void on_data(const uint8_t* data, size_t len) {
        if (_close_requested) {
            return;
        }

        // In case connection happens from an actual telnet client
        if (!process_rfc854(data, len)) {
            request_close();
            return;
        }

        // We always attempt to parse the network buffer directly first,
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

    // Loop lock held
    void on_connected() {
        if (_close_requested) {
            return;
        }

        _state = _request_auth
            ? State::Authenticating
            : State::Active;
        DEBUG_MSG_P(PSTR("[TELNET] Connected %s\n"),
            address_string(_remote).c_str());
        maybe_ask_auth();
    }

    // Loop lock held. AsyncClient is disconnected, nothing else is called for it after this
    void on_disconnect(AsyncClient* ptr) {
        if (_handle == ptr) {
            _handle = nullptr;
        }

        release(ptr);

        _state = State::Idle;
        _writer.reset();
        reset_session();

        DEBUG_MSG_P(PSTR("[TELNET] Disconnected %s\n"),
            address_string(_remote).c_str());
    }

    // ~AsyncClient() would call discard once again if it still had a pcb
    static void release(AsyncClient* ptr) {
        ptr->onConnect(nullptr, nullptr);
        ptr->onData(nullptr, nullptr);
        ptr->onAck(nullptr, nullptr);
        ptr->onError(nullptr, nullptr);
        ptr->onDisconnect(nullptr, nullptr);
        ptr->onPoll(nullptr, nullptr);
        ptr->onTimeout(nullptr, nullptr);
        delete ptr;
    }

    static void on_connect_async(void* arg, AsyncClient*) {
        auto* client = static_cast<Client*>(arg);

        ::espurna::system::AsyncGuard guard;
        if (deferred.defer(guard)) {
            deferred.run([client]() {
                client->on_connected();
            });
            return;
        }

        client->on_connected();
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

    // Without parallel tasks, push the cached output as soon as the network buffer frees up.
    // Otherwise loop() does that on every iteration, waiting for the lock here is not worth it
    static void on_ack_async(void* arg, AsyncClient*, size_t, uint32_t) {
        if (!::espurna::system::ParallelTasks) {
            static_cast<Client*>(arg)->flush();
        }
    }

    // Only reads the address, which never changes. Discard callback follows
    static void on_error_async(void* arg, AsyncClient*, err_t err) {
        auto* client = static_cast<Client*>(arg);
        DEBUG_MSG_P(PSTR("[TELNET] %s ERROR %d\n"),
            address_string(client->_remote).c_str(), int(err));
    }

    // Loop may still write into the AsyncClient, delete it only with the lock held
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
            // same as on_ack_async()
            if (!::espurna::system::ParallelTasks) {
                client->flush();
            }
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
    Address _remote;

    ::espurna::system::Shared<bool> _close_requested { false };

    ClientWriter _writer;
};

std::list<std::unique_ptr<Client>> clients;
AsyncServer* server { nullptr };

// Loop lock held. Closing ones are not counted
size_t active() {
    size_t out = 0;
    for (const auto& client : clients) {
        if (!client->closed() && !client->close_requested()) {
            ++out;
        }
    }

    return out;
}

// Loop lock held. Connections through STA are only allowed when telnetSTA is set
bool accepted(const Client& client) {
    auto* handle = client.handle();
    if (!handle || !client.connected()) {
        return false;
    }

    if (!settings::station() && wifiConnected() && (handle->localIP() == wifiStaIp())) {
        return false;
    }

    return active() < build::ClientsMax;
}

// Loop lock held. Rejected client stays in the list until it is closed
void attach(std::unique_ptr<Client> client) {
    if (accepted(*client)) {
        client->start(settings::authentication());
    } else {
        DEBUG_MSG_P(PSTR("[TELNET] Rejecting %s\n"),
            address_string(client->remote()).c_str());
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

// Closing clients are counted as well, until the library is done with them
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
    auto it = clients.begin();
    while (it != clients.end()) {
        if ((*it)->closed()) {
            it = clients.erase(it);
            continue;
        }

        // closing ones are left to the network side, see Client::on_poll_async()
        if (!(*it)->close_requested()) {
            (*it)->flush();
            (*it)->process();
        }

        ++it;
    }
}

bool listen() {
    if (server) {
        delete server;
    }

    const auto port = settings::port();

    server = new AsyncServer(port);
    server->onClient([](void*, AsyncClient* client) { add(client); }, nullptr);
    server->begin();

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

#if TELNET_REVERSE_SUPPORT
namespace reverse {

// Loop lock held. Outgoing connections share the client limit with the accepted ones
bool connect(Address address) {
    if (active() >= build::ClientsMax) {
        return false;
    }

    auto* handle = new (std::nothrow) AsyncClient();
    if (!handle) {
        return false;
    }

    // keep it in the list before connecting, callbacks may arrive right away
    clients.push_back(std::make_unique<Client>(handle, address));
    return clients.back()->connect();
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

void connect_host(network::dns::HostPtr host, uint16_t port) {
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
}

void connect_url(String url) {
    URL parsed(std::move(url));
    if (!parsed.host.length() || !parsed.port) {
        DEBUG_MSG_P(PSTR("[TELNET] Cannot parse the url\n"));
        return;
    }

    // DNS callback may come from the network task, clients list belongs to loop()
    const auto port = parsed.port;
    network::dns::resolve(
        std::move(parsed.host),
        [port](network::dns::HostPtr host) {
            if (::espurna::system::loopTask()) {
                connect_host(host, port);
                return;
            }

            ::espurna::system::runInLoop([host, port]() {
                connect_host(host, port);
            });
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
