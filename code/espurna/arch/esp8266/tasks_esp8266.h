/*

EXECUTION MODEL, esp8266 part (ref. espurna_tasks.h)

Network callbacks run in SYS, which never overlaps with loop(). Nothing has to be synchronized,
every type below is empty and every check is a constant, so none of it ends up in the image.

*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace espurna {
namespace system {

constexpr bool ParallelTasks { false };

constexpr bool loopTask() {
    return true;
}

template <typename T>
using Shared = T;

struct Mutex {
};

struct MutexLock {
    explicit MutexLock(Mutex&) {
    }
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
    void run(T&& callback) {
        ::systemRunInLoop(std::forward<T>(callback));
    }
};

// ESP.restart() only happens once the SDK gets to run again, the output is sent out by then
using PendingOutput = bool (*)();

inline void registerPendingOutput(PendingOutput) {
}

struct ConnectionIds {
    void add(uint32_t) {
    }

    void remove(uint32_t) {
    }

    std::vector<uint32_t> copy() const {
        return {};
    }
};

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
