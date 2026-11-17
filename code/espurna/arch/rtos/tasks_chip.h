/*

TASKS: what the network callbacks share with loop() (espurna_tasks.h): the part of the FreeRTOS SDKs (ESP32, Beken) (the chip's folder, -I of its ini)

*/

#pragma once

// WiFi events, SNTP and Matter run in tasks of their own, in parallel with loop() and on ESP32 possibly on the
// other core; what they have for loop() goes through runInLoop() / EventQueue. The loop task holds the loop lock for
// the whole iteration (ref. LoopGuard) and lets it go only while sleeping (delay_unlocked()).

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

    // the FreeRTOS mutex goes with the object: every TCP connection has one, ~88 bytes of heap were lost with each
    // connection on Beken and ESP32 (leak regression 11-13)
    ~Mutex();

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

} // namespace system
} // namespace espurna
