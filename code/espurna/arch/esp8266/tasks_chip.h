/*

TASKS: what the network callbacks share with loop() (espurna_tasks.h): the part of the esp8266 NONOS SDK (the chip's folder, -I of its ini)

*/

#pragma once

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
