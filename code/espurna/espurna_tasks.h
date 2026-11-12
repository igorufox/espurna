/*

EXECUTION MODEL

Common code is written for the esp8266 cooperative model, where network callbacks (lwIP, AsyncTCP,
SDK events, SNTP, Matter) never overlap with loop(). Chips running those in tasks of their own provide the
synchronization through the same few names, and on esp8266 every one of them is a direct call or nothing:

- ParallelTasks          true when network callbacks run in parallel with loop()
- loopTask()             whether the caller is the loop task
- Mutex, MutexLock       recursive lock between tasks (FreeRTOS mutex, or nothing)
- Shared<T>              small value read by one task and written by another (critical section, or plain T)
- EventQueue<T, Size>    fixed number of events of another task for loop(), in order (FreeRTOS queue, or a ring
                         buffer filled from SYS). T is copied byte-wise, the full queue drops and counts
- runInLoop()            any callable for loop(), in order, at most RunInLoopMax of them and RunInLoopBytesMax of their
                         copied data (drops and counts the rest)
- Deferred               ordered hand-over of network events to loop(), when the loop lock could not
                         be taken in time (ref. AsyncGuard in system.h). Once anything was deferred,
                         every following event is deferred too until loop() catches up, so a later
                         event handled right away never overtakes an earlier deferred one:
                             AsyncGuard guard;
                             if (deferred.defer(guard)) { deferred.run(callback); return; }
                         Handlers that only learn the guard state from their caller use
                         `auto scope = deferred.scope(guard);` around the call and `defer()` inside.
- registerPendingOutput  restart() waits for the output some module did not send out yet
- TaskQueue<T, Max>      items from other tasks for loop(), at most Max bytes of them

The model is the SDK's, not a choice: the esp8266 NONOS SDK runs network callbacks in SYS, between loop() calls,
the FreeRTOS SDKs (ESP32, LibreTiny) run them in tasks of their own. Below, one part per model. What the chip itself
forces (the critical section: a spinlock of both ESP32 cores, the interrupt mask of the single core BK72xx) is in
arch/rtos/system_rtos.cpp

*/

#pragma once

#include "system.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace espurna {
namespace system {

// Run the callback from loop(), in the order of calls. Safe to call from any task, e.g. from network handlers on
// ESP32 / Beken when the loop lock could not be taken in time (ref. AsyncGuard). `bytes` is the data the callback
// carries (a copied message): when loop() falls behind, callbacks are dropped instead of running out of heap (the
// first one queued always goes in). false when dropped, which is counted and reported once per burst
constexpr size_t RunInLoopMax { 64 };
constexpr size_t RunInLoopBytesMax { 8192 };

bool runInLoop(std::function<void()> callback, size_t bytes = 0);

} // namespace system
} // namespace espurna

#if defined(ESP8266)

// Network callbacks run in SYS, which never overlaps with loop(). Nothing has to be synchronized,
// every type below is empty and every check is a constant, so none of it ends up in the image.

namespace espurna {
namespace system {

constexpr bool ParallelTasks { false };

constexpr bool loopTask() {
    return true;
}

// the same calls as the Shared of the FreeRTOS chips; one task, nothing to lock
template <typename T>
class Shared {
public:
    constexpr Shared() = default;
    constexpr Shared(T value) :
        _value(value)
    {}

    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    T load() const {
        return _value;
    }

    void store(T value) {
        _value = value;
    }

    T exchange(T value) {
        const auto out = _value;
        _value = value;
        return out;
    }

    T fetch_add(T value) {
        const auto out = _value;
        _value = out + value;
        return out;
    }

    T fetch_sub(T value) {
        const auto out = _value;
        _value = out - value;
        return out;
    }

    T fetch_or(T value) {
        const auto out = _value;
        _value = out | value;
        return out;
    }

    T fetch_and(T value) {
        const auto out = _value;
        _value = out & value;
        return out;
    }

    operator T() const {
        return _value;
    }

    Shared& operator=(T value) {
        _value = value;
        return *this;
    }

private:
    T _value {};
};

struct Mutex {
    void lock() {
    }

    bool try_lock() {
        return true;
    }

    void unlock() {
    }
};

struct MutexLock {
    explicit MutexLock(Mutex&) {
    }
};

// Filled from SYS (SDK events), emptied by loop(): the two never overlap
template <typename T, size_t Size>
class EventQueue {
public:
    static_assert(std::is_trivially_copyable<T>::value, "events are copied byte-wise");
    static_assert(Size > 0, "");

    bool push(const T& value, duration::Milliseconds = duration::Milliseconds{}) {
        if ((_head - _tail) >= Size) {
            ++_dropped;
            return false;
        }

        _items[_head % Size] = value;
        ++_head;
        return true;
    }

    bool pop(T& out) {
        if (_head == _tail) {
            return false;
        }

        out = _items[_tail % Size];
        ++_tail;
        return true;
    }

    void clear() {
        _tail = _head;
    }

    // since the last call
    size_t dropped() {
        const auto out = _dropped;
        _dropped = 0;
        return out;
    }

private:
    T _items[Size];
    size_t _head { 0 };
    size_t _tail { 0 };
    size_t _dropped { 0 };
};

struct Deferred {
    struct Scope {
        ~Scope() {
        }
    };

    Scope scope(const AsyncGuard&) {
        return Scope{};
    }

    constexpr bool defer() const {
        return false;
    }

    constexpr bool defer(const AsyncGuard&) const {
        return false;
    }

    template <typename T>
    void run(T&& callback, size_t bytes = 0) {
        runInLoop(std::forward<T>(callback), bytes);
    }
};

// ESP.restart() only happens once the SDK gets to run again, the output is sent out by then
using PendingOutput = bool (*)();

inline void registerPendingOutput(PendingOutput) {
}

template <typename T, size_t BytesMax>
struct TaskQueue {
    bool push(T&&, size_t) {
        return false;
    }

    constexpr bool empty() const {
        return true;
    }

    std::vector<T> take() {
        return {};
    }
};

} // namespace system
} // namespace espurna

#elif defined(ESP32) || defined(LIBRETINY)

// AsyncTCP, WiFi events, SNTP and Matter run in tasks of their own, in parallel with loop() and
// on ESP32 possibly on the other core. The loop task holds the loop lock for the whole
// iteration (ref. LoopGuard), network handlers take it with AsyncGuard, everything they
// could not handle in time is handed over to loop().

#include <Arduino.h>

#include <algorithm>

// Arduino core main.cpp (ESP32), arch/beken/system_beken.cpp (LibreTiny)
extern TaskHandle_t loopTaskHandle;

namespace espurna {
namespace system {

constexpr bool ParallelTasks { true };

inline bool loopTask() {
    return xTaskGetCurrentTaskHandle() == loopTaskHandle;
}

// Short section no other task (and no interrupt) gets into, the chip's part (arch/rtos/system_rtos.cpp): ESP32 a
// spinlock of both cores, BK72xx (one core) the interrupt mask. Never around anything that may wait or allocate
uint32_t criticalEnter();
void criticalExit(uint32_t);

struct Critical {
    Critical() :
        _state(criticalEnter())
    {}

    ~Critical() {
        criticalExit(_state);
    }

    Critical(const Critical&) = delete;
    Critical& operator=(const Critical&) = delete;

private:
    uint32_t _state;
};

// A word or two shared between tasks (flags, counters, a state enum). No std::atomic: the BK72xx (ARMv5) has no
// atomic instructions and every std::atomic there went through our own libatomic, with the interrupts masked all
// the same (ref. arch/beken/compat_beken.cpp)
template <typename T>
class Shared {
public:
    static_assert(std::is_trivially_copyable<T>::value, "");

    constexpr Shared() = default;
    constexpr Shared(T value) :
        _value(value)
    {}

    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    T load() const {
        Critical critical;
        return _value;
    }

    void store(T value) {
        Critical critical;
        _value = value;
    }

    T exchange(T value) {
        Critical critical;
        const auto out = _value;
        _value = value;
        return out;
    }

    T fetch_add(T value) {
        Critical critical;
        const auto out = _value;
        _value = out + value;
        return out;
    }

    T fetch_sub(T value) {
        Critical critical;
        const auto out = _value;
        _value = out - value;
        return out;
    }

    T fetch_or(T value) {
        Critical critical;
        const auto out = _value;
        _value = out | value;
        return out;
    }

    T fetch_and(T value) {
        Critical critical;
        const auto out = _value;
        _value = out & value;
        return out;
    }

    operator T() const {
        return load();
    }

    Shared& operator=(T value) {
        store(value);
        return *this;
    }

private:
    T _value {};
};

// Recursive lock between tasks. The FreeRTOS mutex (priority inheritance) is made on the first use, mutexes are
// used during the static init already. The recursion is counted here: the BK72xx SDK is built without
// configUSE_RECURSIVE_MUTEXES. arch/rtos/system_rtos.cpp
class Mutex {
public:
    constexpr Mutex() = default;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        take(portMAX_DELAY);
    }

    bool try_lock() {
        return take(0);
    }

    bool try_lock_for(duration::Milliseconds timeout) {
        return take(pdMS_TO_TICKS(timeout.count()));
    }

    void unlock();

    // held by the calling task
    bool owned() const;

    // every level the calling task holds is let go, the count is what reacquire() takes back (ref. delay_unlocked())
    size_t release();
    void reacquire(size_t depth);

private:
    bool take(TickType_t ticks);
    void held(size_t depth);
    SemaphoreHandle_t handle();

    SemaphoreHandle_t _handle { nullptr };

    // only ever set by the holder, read in the critical section
    TaskHandle_t _owner { nullptr };
    size_t _depth { 0 };
};

class MutexLock {
public:
    explicit MutexLock(Mutex& mutex) :
        _mutex(mutex)
    {
        _mutex.lock();
    }

    ~MutexLock() {
        _mutex.unlock();
    }

    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;

private:
    Mutex& _mutex;
};

// FreeRTOS queue of Size events, made on the first use (either side). Producer is any task, the consumer is loop()
template <typename T, size_t Size>
class EventQueue {
public:
    static_assert(std::is_trivially_copyable<T>::value, "events are copied byte-wise");
    static_assert(Size > 0, "");

    constexpr EventQueue() = default;

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    // waits up to `wait` for loop() to make room, drops (and counts) the event after that
    bool push(const T& value, duration::Milliseconds wait = duration::Milliseconds{}) {
        auto* queue = handle();
        if (queue && (xQueueSend(queue, &value, pdMS_TO_TICKS(wait.count())) == pdTRUE)) {
            return true;
        }

        _dropped.fetch_add(1);
        return false;
    }

    bool pop(T& out) {
        auto* queue = handle();
        return queue && (xQueueReceive(queue, &out, 0) == pdTRUE);
    }

    void clear() {
        auto* queue = handle();
        if (queue) {
            xQueueReset(queue);
        }
    }

    // since the last call
    size_t dropped() {
        return _dropped.exchange(0);
    }

private:
    // made outside of the critical section (allocates), the first one installed wins
    QueueHandle_t handle() {
        QueueHandle_t out;
        {
            Critical critical;
            out = _handle;
        }

        if (!out) {
            auto* made = xQueueCreate(Size, sizeof(T));
            {
                Critical critical;
                if (!_handle) {
                    _handle = made;
                    made = nullptr;
                }
                out = _handle;
            }

            if (made) {
                vQueueDelete(made);
            }
        }

        return out;
    }

    QueueHandle_t _handle { nullptr };
    Shared<size_t> _dropped { 0 };
};

// Pending counter is only checked either unlocked, which always defers, or under the loop lock,
// which loop() also holds while running the deferred callbacks
class Deferred {
public:
    class Scope {
    public:
        Scope(Deferred& deferred, bool locked) :
            _deferred(deferred)
        {
            _deferred._unlocked = !locked;
        }

        ~Scope() {
            _deferred._unlocked = false;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Deferred& _deferred;
    };

    // Flag is only ever touched by the network task, while inside of its handler
    Scope scope(const AsyncGuard& guard) {
        return Scope(*this, guard.locked());
    }

    bool defer() const {
        return _unlocked || (_pending.load() > 0);
    }

    bool defer(const AsyncGuard& guard) const {
        return !guard.locked() || (_pending.load() > 0);
    }

    // a callback runInLoop() dropped is gone, later events are not held back by it
    template <typename T>
    void run(T&& callback, size_t bytes = 0) {
        _pending.fetch_add(1);
        if (!runInLoop(Callback<typename std::decay<T>::type>{
                this, std::forward<T>(callback) }, bytes))
        {
            _pending.fetch_sub(1);
        }
    }

private:
    template <typename T>
    struct Callback {
        void operator()() {
            callback();
            deferred->_pending.fetch_sub(1);
        }

        Deferred* deferred;
        T callback;
    };

    Shared<size_t> _pending { 0 };
    bool _unlocked { false };
};

template <typename T, size_t BytesMax>
class TaskQueue {
public:
    // dropped when there are already too many
    bool push(T&& value, size_t size) {
        MutexLock lock(_mutex);
        if ((_bytes + size) > BytesMax) {
            return false;
        }

        _items.push_back(std::move(value));
        _bytes += size;
        return true;
    }

    bool empty() const {
        MutexLock lock(_mutex);
        return _items.empty();
    }

    std::vector<T> take() {
        std::vector<T> out;

        MutexLock lock(_mutex);
        out.swap(_items);
        _bytes = 0;

        return out;
    }

private:
    mutable Mutex _mutex;
    std::vector<T> _items;
    size_t _bytes { 0 };
};

} // namespace system
} // namespace espurna

#endif
