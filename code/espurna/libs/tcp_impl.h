/*

TCP - the implementation (libs/tcp.h), part of libs/net_impl.h. Needs, defined before:
    NET_LOCK    the lock between lwIP's callbacks and the loop (lock(), unlock()); a no-op one without an OS

*/

#pragma once

#include "tcp.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <type_traits>

#include <lwip/opt.h>
#include <lwip/dns.h>
#include <lwip/pbuf.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#if !NO_SYS
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#endif

namespace espurna {
namespace tcp {

uint32_t now() {
    return sys_now();
}

namespace {

// --- lwIP: the pcb only in lwIP's own context ------------------------------------------------------------------------

#if NO_SYS

// no OS: the loop is lwIP's context
template <typename F>
void lwipCall(F&& func) {
    func();
}

#else

template <typename F>
struct LwipCall {
    tcpip_api_call_data base;   // first: lwIP passes this pointer back
    F* func;
};

// runs `func` in the tcpip thread and waits for it
template <typename F>
void lwipCall(F&& func) {
    using Func = typename std::remove_reference<F>::type;
    LwipCall<Func> call {};
    call.func = &func;
    tcpip_api_call([](tcpip_api_call_data* data) -> err_t {
        (*reinterpret_cast<LwipCall<Func>*>(data)->func)();
        return ERR_OK;
    }, &call.base);
}

#endif

} // namespace

// --- a connection --------------------------------------------------------------------------------------------------

class Connection::Impl {
public:
    // more than a whole TCP window (ESP8266 lwIP2 4 x MSS, ESP32 5744, BK7231N lwIP 4 x MSS): the peer sends no more
    // than the window until the loop takes it; a segment that does not fit waits in lwIP (refused, given again)
    static constexpr size_t RingSize { 8192 };

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        close();
    }

    bool allocate() {
        if (!_ring) {
            _ring.reset(new (std::nothrow) uint8_t[RingSize]);
        }
        return static_cast<bool>(_ring);
    }

    // lwIP's context: an accepted pcb
    void attach(tcp_pcb* pcb) {
        _pcb = pcb;
        _connected = true;
        tcp_arg(pcb, this);
        tcp_err(pcb, onErr);
        tcp_recv(pcb, onRecv);
    }

    bool connect(const char* host, uint16_t port) {
        if (!allocate()) {
            return false;
        }

        _port = port;
        _connected = false;
        _closed = false;
        _resolve_failed = false;

        bool started { false };
        lwipCall([&]() {
            size_t index = 0;
            while ((index < DnsSlots) && dnsOwner(index)) {
                ++index;
            }
            if (index == DnsSlots) {
                return;
            }

            ip_addr_t addr;
            const auto result = dns_gethostbyname(host, &addr, onDns,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)));
            if (result == ERR_OK) {
                started = true;
                tcpConnect(addr);
            } else if (result == ERR_INPROGRESS) {
                started = true;
                dnsOwner(index) = this;
                _dns_slot = index;
            }
        });

        if (!started) {
            _closed = true;
            _resolve_failed = true;
        }

        return started;
    }

    bool connected() const {
        return _connected;
    }

    bool closed() const {
        return _closed;
    }

    bool resolveFailed() const {
        return _resolve_failed;
    }

    size_t available() {
        if (!_ring) {
            return 0;
        }
        std::lock_guard<NET_LOCK> lock(_lock);
        return _ring_head - _ring_tail;
    }

    size_t read(uint8_t* out, size_t len) {
        if (!_ring) {
            return 0;
        }

        size_t size;
        {
            std::lock_guard<NET_LOCK> lock(_lock);
            size = std::min(len, _ring_head - _ring_tail);
            for (size_t index = 0; index < size; ++index) {
                out[index] = _ring[(_ring_tail + index) % RingSize];
            }
            _ring_tail += size;
        }

        // the loop took them: the window opens again, in steps (one call into lwIP per ~half a KB)
        _unacked += size;
        if (_unacked && ((_unacked >= 512) || !available())) {
            const auto ack = _unacked;
            _unacked = 0;
            lwipCall([&]() {
                if (_pcb) {
                    tcp_recved(_pcb, ack);
                }
            });
        }

        return size;
    }

    size_t writable() {
        size_t out { 0 };
        lwipCall([&]() {
            if (_pcb) {
                out = tcp_sndbuf(_pcb);
            }
        });
        return out;
    }

    size_t write(const uint8_t* data, size_t len) {
        size_t out { 0 };
        lwipCall([&]() {
            if (!_pcb) {
                return;
            }

            const size_t size = std::min<size_t>(len, tcp_sndbuf(_pcb));
            if (size && (tcp_write(_pcb, data, size, TCP_WRITE_FLAG_COPY) == ERR_OK)) {
                tcp_output(_pcb);
                out = size;
            }
        });

        return out;
    }

    uint32_t remoteIp() const {
        return address(false);
    }

    uint32_t localIp() const {
        return address(true);
    }

    uint32_t address(bool local) const {
        uint32_t out { 0 };
        const_cast<Impl*>(this)->call([&]() {
#if LWIP_IPV4
            if (_pcb) {
                const auto& ip = local ? _pcb->local_ip : _pcb->remote_ip;
                if (IP_IS_V4_VAL(ip)) {
                    out = ip_2_ip4(&ip)->addr;
                }
            }
#endif
        });
        return out;
    }

    // nothing to do in lwIP's context when there is no pcb and no DNS answer pending: also what lets a connection
    // that was never attached be deleted from lwIP's own context (onAccept)
    void close() {
        if (!_pcb && (_dns_slot < 0)) {
            _closed = true;
            return;
        }

        lwipCall([&]() {
            if (_dns_slot >= 0) {
                dnsOwner(_dns_slot) = nullptr;
                _dns_slot = -1;
            }

            if (_pcb) {
                tcp_arg(_pcb, nullptr);
                tcp_recv(_pcb, nullptr);
                tcp_err(_pcb, nullptr);
                if (tcp_close(_pcb) != ERR_OK) {
                    tcp_abort(_pcb);
                }
                _pcb = nullptr;
            }
        });

        _closed = true;
    }

private:
    template <typename F>
    void call(F&& func) {
        lwipCall(func);
    }

    // DNS answers come to a slot, never to the connection object: a connection given up before the answer leaves
    // nothing the callback could still point to. Only touched in lwIP's context
    static constexpr size_t DnsSlots { 4 };

    static Impl*& dnsOwner(size_t index) {
        static Impl* owners[DnsSlots] {};
        return owners[index];
    }

    static void onDns(const char*, const ip_addr_t* addr, void* arg) {
        const auto index = reinterpret_cast<uintptr_t>(arg);
        auto*& owner = dnsOwner(index);
        auto* self = owner;
        owner = nullptr;
        if (!self) {
            return;
        }

        self->_dns_slot = -1;
        if (!addr) {
            self->_resolve_failed = true;
            self->_closed = true;
            return;
        }

        self->tcpConnect(*addr);
    }

    static err_t onConnected(void* arg, tcp_pcb*, err_t) {
        if (arg) {
            static_cast<Impl*>(arg)->_connected = true;
        }
        return ERR_OK;
    }

    static err_t onRecv(void* arg, tcp_pcb*, pbuf* p, err_t) {
        auto* self = static_cast<Impl*>(arg);
        if (!self) {
            if (p) {
                pbuf_free(p);
            }
            return ERR_OK;
        }

        if (!p) {
            self->_closed = true;
            return ERR_OK;
        }

        {
            std::lock_guard<NET_LOCK> lock(self->_lock);
            const size_t used = self->_ring_head - self->_ring_tail;
            const size_t len = p->tot_len;
            if (len > RingSize - used) {
                return ERR_MEM; // refused: lwIP keeps it and gives it again
            }

            const size_t at = self->_ring_head % RingSize;
            const size_t first = std::min(len, RingSize - at);
            pbuf_copy_partial(p, self->_ring.get() + at, first, 0);
            if (first < len) {
                pbuf_copy_partial(p, self->_ring.get(), len - first, first);
            }
            self->_ring_head += len;
        }

        pbuf_free(p);
        return ERR_OK;
    }

    // the pcb is gone already
    static void onErr(void* arg, err_t) {
        auto* self = static_cast<Impl*>(arg);
        if (self) {
            self->_pcb = nullptr;
            self->_closed = true;
        }
    }

    // lwIP's context
    void tcpConnect(const ip_addr_t& addr) {
        auto* pcb = tcp_new();
        if (!pcb) {
            _closed = true;
            return;
        }

        tcp_arg(pcb, this);
        tcp_err(pcb, onErr);
        tcp_recv(pcb, onRecv);
        if (tcp_connect(pcb, &addr, _port, onConnected) != ERR_OK) {
            tcp_arg(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_abort(pcb);
            _closed = true;
            return;
        }

        _pcb = pcb;
    }

    tcp_pcb* _pcb { nullptr };          // lwIP's context only
    int _dns_slot { -1 };               // lwIP's context only
    uint16_t _port { 0 };
    std::unique_ptr<uint8_t[]> _ring;
    NET_LOCK _lock;
    size_t _ring_head { 0 };            // written by lwIP's callback, under the lock
    size_t _ring_tail { 0 };            // written by the loop, under the lock
    size_t _unacked { 0 };              // taken by the loop, not given back to the window yet
    volatile bool _connected { false };
    volatile bool _closed { false };
    volatile bool _resolve_failed { false };
};

Connection::Connection() :
    _impl(new (std::nothrow) Impl())
{}

Connection::~Connection() = default;

bool Connection::connect(const char* host, uint16_t port) {
    return _impl && _impl->connect(host, port);
}

bool Connection::connected() const {
    return _impl && _impl->connected();
}

bool Connection::closed() const {
    return !_impl || _impl->closed();
}

bool Connection::resolveFailed() const {
    return _impl && _impl->resolveFailed();
}

size_t Connection::available() {
    return _impl ? _impl->available() : 0;
}

size_t Connection::read(uint8_t* out, size_t len) {
    return _impl ? _impl->read(out, len) : 0;
}

size_t Connection::writable() {
    return _impl ? _impl->writable() : 0;
}

size_t Connection::write(const uint8_t* data, size_t len) {
    return _impl ? _impl->write(data, len) : 0;
}

uint32_t Connection::remoteIp() const {
    return _impl ? _impl->remoteIp() : 0;
}

uint32_t Connection::localIp() const {
    return _impl ? _impl->localIp() : 0;
}

void Connection::close() {
    if (_impl) {
        _impl->close();
    }
}

// --- a listening port ----------------------------------------------------------------------------------------------

class Listener::Impl {
public:
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        end();
    }

    bool begin(uint16_t port, size_t pending) {
        _pending_max = std::min<size_t>(pending, QueueMax);
        bool ok { false };
        lwipCall([&]() {
            auto* pcb = tcp_new();
            if (!pcb) {
                return;
            }

            ip_set_option(pcb, SOF_REUSEADDR);
            if (tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) {
                tcp_close(pcb);
                return;
            }

            auto* listen = tcp_listen_with_backlog(pcb, static_cast<u8_t>(_pending_max));
            if (!listen) {
                tcp_close(pcb);
                return;
            }

            _pcb = listen;
            tcp_arg(_pcb, this);
            tcp_accept(_pcb, onAccept);
            ok = true;
        });

        return ok;
    }

    void end() {
        lwipCall([&]() {
            if (_pcb) {
                tcp_arg(_pcb, nullptr);
                tcp_accept(_pcb, nullptr);
                tcp_close(_pcb);
                _pcb = nullptr;
            }
        });

        Connection::Impl* left[QueueMax];
        size_t count { 0 };
        {
            std::lock_guard<NET_LOCK> lock(_lock);
            while (_count) {
                left[count++] = pop();
            }
        }

        for (size_t index = 0; index < count; ++index) {
            delete left[index];
        }
    }

    std::unique_ptr<Connection> accept() {
        Connection::Impl* impl { nullptr };
        {
            std::lock_guard<NET_LOCK> lock(_lock);
            if (_count) {
                impl = pop();
            }
        }

        if (!impl) {
            return nullptr;
        }

        std::unique_ptr<Connection> out(new (std::nothrow) Connection());
        if (!out) {
            delete impl;
            return nullptr;
        }

        out->_impl.reset(impl);
        return out;
    }

private:
    static constexpr size_t QueueMax { 8 };

    Connection::Impl* pop() {
        auto* out = _queue[_first];
        _first = (_first + 1) % QueueMax;
        --_count;
        return out;
    }

    // lwIP's context
    static err_t onAccept(void* arg, tcp_pcb* pcb, err_t err) {
        auto* self = static_cast<Listener::Impl*>(arg);
        if (!self || !pcb || (err != ERR_OK)) {
            return ERR_VAL;
        }

        tcp_backlog_accepted(pcb);

        auto* impl = new (std::nothrow) Connection::Impl();
        bool queued { false };
        if (impl && impl->allocate()) {
            std::lock_guard<NET_LOCK> lock(self->_lock);
            if (self->_count < self->_pending_max) {
                impl->attach(pcb);
                self->_queue[(self->_first + self->_count) % QueueMax] = impl;
                ++self->_count;
                queued = true;
            }
        }

        if (!queued) {
            delete impl;
            tcp_abort(pcb);
            return ERR_ABRT;
        }

        return ERR_OK;
    }

    tcp_pcb* _pcb { nullptr };
    NET_LOCK _lock;
    Connection::Impl* _queue[QueueMax] {};
    size_t _first { 0 };
    size_t _count { 0 };
    size_t _pending_max { 4 };
};

Listener::Listener() = default;

Listener::~Listener() = default;

bool Listener::begin(uint16_t port, size_t pending) {
    if (!_impl) {
        _impl.reset(new (std::nothrow) Impl());
    }
    return _impl && _impl->begin(port, pending);
}

void Listener::end() {
    if (_impl) {
        _impl->end();
    }
}

std::unique_ptr<Connection> Listener::accept() {
    return _impl ? _impl->accept() : nullptr;
}

} // namespace tcp
} // namespace espurna
