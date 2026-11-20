/*

The C++ runtime of the ESP8266 build without exceptions (CONFIG_COMPILER_CXX_EXCEPTIONS off, as the esp8266 Arduino
core's abi.cpp had it on the NONOS build).

libstdc++ of the toolchain is built with exceptions: its std::__throw_* (std::string, std::vector length checks), the
throwing operator new and __cxa_pure_virtual pull logic_error, the copy-on-write std::string of the old ABI, the
exception allocator, the personality routine and the DWARF unwinder into the head (about 25 KB) for paths that only
ever end in abort() here. These definitions stop the program the same way with the name of the failure, and the
linker takes none of that from libstdc++.

The nothrow operator new: arch/rtos/system_rtos.cpp (every FreeRTOS chip).

*/

#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <ext/atomicity.h>

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>

namespace {

[[noreturn]] void fail(const char* what, const char* detail) {
    ets_printf("C++ %s: %s\n", what, detail ? detail : "");
    abort();
}

} // namespace

// the throwing operator new: no exception to throw, the program stops (its callers do not check for nullptr)
void* operator new(size_t size) {
    void* out = malloc(size ? size : 1);
    if (!out) {
        fail("operator new", "out of memory");
    }

    return out;
}

void* operator new[](size_t size) {
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    free(ptr);
}

// The guard of a function's static variable: libstdc++'s guard.o throws recursive_init_error and pulled the exception
// runtime in too. One recursive mutex for every guard, as ESP-IDF's cxx_guards: the first task to come initializes,
// another waits for it; before the scheduler runs there is only the one task. The guard's first byte is the "done" of
// the Itanium C++ ABI
namespace {

SemaphoreHandle_t guard_lock { nullptr };

bool guard_scheduler() {
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

SemaphoreHandle_t guard_mutex() {
    if (!guard_lock) {
        taskENTER_CRITICAL();
        if (!guard_lock) {
            guard_lock = xSemaphoreCreateRecursiveMutex();
        }
        taskEXIT_CRITICAL();
    }

    return guard_lock;
}

bool guard_done(const uint32_t* guard) {
    return *reinterpret_cast<const volatile uint8_t*>(guard) != 0;
}

} // namespace

extern "C" {

int __cxa_guard_acquire(uint32_t* guard) {
    if (guard_done(guard)) {
        return 0;
    }

    if (!guard_scheduler()) {
        return 1;
    }

    auto* mutex = guard_mutex();
    if (!mutex) {
        fail("static init", "no mutex");
    }

    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    if (guard_done(guard)) {
        xSemaphoreGiveRecursive(mutex);
        return 0;
    }

    return 1;
}

void __cxa_guard_release(uint32_t* guard) {
    *reinterpret_cast<volatile uint8_t*>(guard) = 1;
    if (guard_scheduler() && guard_lock) {
        xSemaphoreGiveRecursive(guard_lock);
    }
}

void __cxa_guard_abort(uint32_t*) {
    if (guard_scheduler() && guard_lock) {
        xSemaphoreGiveRecursive(guard_lock);
    }
}

void __cxa_pure_virtual() {
    fail("pure virtual call", nullptr);
}

void __cxa_deleted_virtual() {
    fail("deleted virtual call", nullptr);
}

} // extern "C"

// The reference counts of std::shared_ptr: the lx106 has no atomic instructions, libstdc++'s atomicity.o counts under a
// mutex of its own that throws __concurrence_lock_error (the exception runtime again). A critical section instead
namespace __gnu_cxx _GLIBCXX_VISIBILITY(default) {
_GLIBCXX_BEGIN_NAMESPACE_VERSION

_Atomic_word __exchange_and_add(volatile _Atomic_word* mem, int value) _GLIBCXX_NOTHROW {
    taskENTER_CRITICAL();
    const _Atomic_word out = *mem;
    *mem = out + value;
    taskEXIT_CRITICAL();
    return out;
}

void __atomic_add(volatile _Atomic_word* mem, int value) _GLIBCXX_NOTHROW {
    taskENTER_CRITICAL();
    *mem += value;
    taskEXIT_CRITICAL();
}

_GLIBCXX_END_NAMESPACE_VERSION
} // namespace __gnu_cxx

namespace std {

// the tag of new (std::nothrow): libstdc++ has it in new_handler.o next to the throwing new handler, which pulled the
// exception runtime in for this empty object alone
const nothrow_t nothrow {};

void __throw_bad_alloc() {
    fail("bad_alloc", nullptr);
}

// GCC 11+ (14.2 here): new[] of a too large count; from libstdc++ it pulled functexcept.o and the exception runtime into
// a module (garland, 19.11)
void __throw_bad_array_new_length() {
    fail("bad_array_new_length", nullptr);
}

void __throw_bad_cast() {
    fail("bad_cast", nullptr);
}

void __throw_bad_typeid() {
    fail("bad_typeid", nullptr);
}

void __throw_bad_exception() {
    fail("bad_exception", nullptr);
}

void __throw_bad_function_call() {
    fail("bad_function_call", nullptr);
}

void __throw_logic_error(const char* what) {
    fail("logic_error", what);
}

void __throw_domain_error(const char* what) {
    fail("domain_error", what);
}

void __throw_invalid_argument(const char* what) {
    fail("invalid_argument", what);
}

void __throw_length_error(const char* what) {
    fail("length_error", what);
}

void __throw_out_of_range(const char* what) {
    fail("out_of_range", what);
}

void __throw_out_of_range_fmt(const char* what, ...) {
    fail("out_of_range", what);
}

void __throw_runtime_error(const char* what) {
    fail("runtime_error", what);
}

void __throw_range_error(const char* what) {
    fail("range_error", what);
}

void __throw_overflow_error(const char* what) {
    fail("overflow_error", what);
}

void __throw_underflow_error(const char* what) {
    fail("underflow_error", what);
}

} // namespace std

// the destructors of function-local statics are registered by atexit(); the firmware never exits, nothing would call
// them: no newlib atexit / __register_exitproc / its recursive mutex in the head
extern "C" int atexit(void (*)()) {
    return 0;
}
