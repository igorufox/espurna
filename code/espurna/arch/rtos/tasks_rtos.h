/*

EXECUTION MODEL, FreeRTOS part: ESP32 and LibreTiny (ref. espurna_tasks.h)

AsyncTCP, WiFi events and SNTP run in tasks of their own, in parallel with loop() and
on ESP32 possibly on the other core. The loop task holds the loop lock for the whole
iteration (ref. LoopGuard), network handlers take it with AsyncGuard, everything they
could not handle in time is handed over to loop().

*/

#pragma once

#include <Arduino.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

// Arduino core main.cpp (ESP32), arch/beken/system_beken.cpp (LibreTiny)
extern TaskHandle_t loopTaskHandle;

namespace espurna {
namespace system {

constexpr bool ParallelTasks { true };

inline bool loopTask() {
    return xTaskGetCurrentTaskHandle() == loopTaskHandle;
}

template <typename T>
using Shared = std::atomic<T>;

using Mutex = std::mutex;
using MutexLock = std::lock_guard<std::mutex>;

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

    template <typename T>
    void run(T&& callback) {
        ++_pending;
        ::systemRunInLoop(Callback<typename std::decay<T>::type>{
            this, std::forward<T>(callback) });
    }

private:
    template <typename T>
    struct Callback {
        void operator()() {
            callback();
            --deferred->_pending;
        }

        Deferred* deferred;
        T callback;
    };

    std::atomic<size_t> _pending { 0 };
    bool _unlocked { false };
};

class ConnectionIds {
public:
    void add(uint32_t id) {
        MutexLock lock(_mutex);
        if (std::find(_ids.begin(), _ids.end(), id) == _ids.end()) {
            _ids.push_back(id);
        }
    }

    void remove(uint32_t id) {
        MutexLock lock(_mutex);
        auto it = std::find(_ids.begin(), _ids.end(), id);
        if (it != _ids.end()) {
            _ids.erase(it);
        }
    }

    std::vector<uint32_t> copy() const {
        MutexLock lock(_mutex);
        return _ids;
    }

private:
    mutable Mutex _mutex;
    std::vector<uint32_t> _ids;
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
