/*

RTMEM MODULE FOR BEKEN (BDK)

*/

#include "../../espurna.h"
#include "../../rtcmem.h"
#include "../../crash.h"
#include "../../build.h"
#include "layout_beken.h"
#include "beken_api.h"

#if MQTT_SUPPORT
#include "../../mqtt.h"
#endif

#include <lwip/tcpip.h>

#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>

// No RAM survives the reset, bootloader clears all of it (0x00400000 and up, checked on BK7231N).
// Contents only live until the next reboot, relay state is restored from settings instead.
static RtcmemData _rtcmem_storage;
volatile RtcmemData* Rtcmem = &_rtcmem_storage;

// only the system word survives, the rest never (rtcmem.cpp)
bool rtcmemChipKept() {
    return false;
}

namespace {

// Except for the system word (custom reset reason and the stability counter), kept in the SDK retention
// register instead of the start type. Register only has 16 bits (checked on BK7231N), hardware keeps
// them across watchdog resets and clears them on power-on. SDK start types are 0x0..0xc and 0xad0x,
// ours are 0x5rc0 (reason, counter). SDK reads them as a watchdog reset, system_beken.cpp knows better.
// SDK rewrites the register on every watchdog feed, its own reboots and crashes, so ours is only
// written by restart() right before the reset, and by the crash handlers below (0x5rck, k = crash kind).
// Otherwise the stability counter would be lost on every crash and a crash loop never detected.
// A hang still loses it, the start type tells nothing more than 'watchdog' then
constexpr uintptr_t RegRetention { 0x00800150 };

constexpr uint32_t RetentionMarkerMask { 0xf000 };
constexpr uint32_t RetentionMarker { 0x5000 };

constexpr int RetentionCounterShift { 4 };
constexpr int RetentionReasonShift { 8 };
constexpr uint32_t RetentionFieldMask { 0xf };
constexpr uint32_t RetentionCrashMask { 0xf };

volatile uint32_t& retention() {
    return *reinterpret_cast<volatile uint32_t*>(RegRetention);
}

// Same layout as system.cpp Data, low byte is the counter and the next one is the reason
uint32_t pack(uint32_t sys) {
    const auto counter = std::min(sys & 0xff, RetentionFieldMask);
    const auto reason = std::min((sys >> 8) & 0xff, RetentionFieldMask);

    return RetentionMarker
        | (counter << RetentionCounterShift)
        | (reason << RetentionReasonShift);
}

uint32_t unpack(uint32_t value) {
    const auto counter = (value >> RetentionCounterShift) & RetentionFieldMask;
    const auto reason = (value >> RetentionReasonShift) & RetentionFieldMask;

    return counter | (reason << 8);
}

// Watchdog driver writes RESET_SOURCE_WATCHDOG over the whole register on every feed, the first one
// happens before setup(). Read it as early as possible, while running the static initializers
const uint32_t _rtcmem_retention_at_boot = retention();

// RAM does not survive the reset either, so the crash handlers below also write the registers and the top of the stack
// into the flash sector right above the settings pool (storage_flash_beken.cpp) and below the Tuya config, blank
// on every stock dump seen. Its place comes from the layout of the bootloader (layout_beken.cpp): an unknown one,
// or one whose dumps were not checked, gets no crash record. Picked in rtcmemSetup(), before that nothing is
// written. The record stays until the next crash, 'pending' goes from all ones to zero (no erase needed) once the
// boot has reported it
uint32_t CrashRecordOffset { 0 };
constexpr uint32_t CrashRecordMagic { 0x32535243 }; // CRS2, CRSH kept the exception mode sp lr
constexpr uint32_t CrashRecordPending { 0xffffffff };

// BK7231N RAM, a stack pointer outside of it is not read
constexpr uint32_t RamBegin { 0x00400000 };
constexpr uint32_t RamEnd { 0x00440000 };

// Code runs from the flash mapped at 0, an instruction outside of both is not read either
constexpr uint32_t FlashEnd { 0x00200000 };

// struct arm_registers of the SDK, r0..r12 sp lr pc spsr cpsr. Filled by the exception vector in the exception
// mode, so its sp lr and pc are that mode's own: sp points to this very struct and lr = pc = return address.
// Record keeps the interrupted mode's sp and lr instead (from the SDK backup area below) and the faulting pc
constexpr size_t CrashRegisters { 18 };
constexpr size_t CrashRegisterSp { 13 };
constexpr size_t CrashRegisterLr { 14 };
constexpr size_t CrashRegisterPc { 15 };
constexpr size_t CrashRegisterSpsr { 16 };
constexpr size_t CrashRegisterCpsr { 17 };

constexpr size_t CrashStackWords { 128 };

constexpr uint32_t PsrModeMask { 0x1f };
constexpr uint32_t PsrThumb { 1 << 5 };

// Vectors (boot_handlers.S PUSH_ALL_ARM_REG) store the banked r8..r14 of every mode at fixed addresses, the same
// ones bk_show_register prints. Address of the banked r13, r14 follows it
uintptr_t crash_banked_sp(uint32_t mode) {
    switch (mode) {
    case 0x10: // usr
    case 0x1f: // sys
        return 0x0040003c;
    case 0x12: // irq
        return 0x00400060;
    case 0x11: // fiq
        return 0x00400084;
    case 0x17: // abt
        return 0x004000a8;
    case 0x1b: // und
        return 0x004000cc;
    case 0x13: // svc
        return 0x004000f0;
    }

    return 0;
}

struct CrashRecord {
    uint32_t magic;
    uint32_t kind;
    uint32_t uptime;
    uint32_t regs[CrashRegisters];
    uint32_t insn;
    uint32_t stack[CrashStackWords];
    char task[16];
    char build[32];
    uint32_t pending;
};

static_assert(sizeof(CrashRecord) <= FLASH_SECTOR_SIZE, "");

// Handlers can not ask for it, copied at boot
char _crash_build[sizeof(CrashRecord::build)] {};

const char* crash_name(uint32_t kind) {
    switch (static_cast<RtcmemCrash>(kind)) {
    case RtcmemCrash::Undefined:
        return "undefined instruction";
    case RtcmemCrash::PrefetchAbort:
        return "prefetch abort";
    case RtcmemCrash::DataAbort:
        return "data abort";
    case RtcmemCrash::Reserved:
        return "not used";
    case RtcmemCrash::MallocFailed:
        return "malloc failed";
    case RtcmemCrash::Stall:
        return "watchdog not fed";
    case RtcmemCrash::Abort:
        return "abort";
    case RtcmemCrash::None:
        break;
    }

    return "unknown";
}

// Faulting instruction, the one lr points after
uint32_t crash_pc(uint32_t lr, uint32_t spsr, RtcmemCrash kind) {
    switch (kind) {
    case RtcmemCrash::Undefined:
        return lr - (((spsr & PsrThumb) != 0) ? 2 : 4);
    case RtcmemCrash::PrefetchAbort:
        return lr - 4;
    case RtcmemCrash::DataAbort:
        return lr - 8;
    case RtcmemCrash::Reserved:
    case RtcmemCrash::MallocFailed:
    case RtcmemCrash::Stall:
    case RtcmemCrash::Abort:
    case RtcmemCrash::None:
        break;
    }

    return lr;
}

bool crash_readable(uint32_t address, size_t size) {
    return ((address + size) <= FlashEnd)
        || ((address >= RamBegin) && ((address + size) <= RamEnd));
}

void crash_flash_write(size_t offset, const void* data, size_t size) {
    if (!CrashRecordOffset) {
        return;
    }
    ::espurna::beken::flash_write(CrashRecordOffset + offset, static_cast<const uint8_t*>(data), size);
}

// Written field by field straight from where the data is, no copy of the whole record in RAM. What is not
// written stays erased: stack words past the end of RAM read 0xffffffff and 'pending' is already all ones
void crash_record_write(const uint32_t* frame, RtcmemCrash kind) {
    uint32_t regs[CrashRegisters];
    std::memcpy(regs, frame, sizeof(regs));

    const auto spsr = regs[CrashRegisterSpsr];
    const auto banked = crash_banked_sp(spsr & PsrModeMask);
    if (banked) {
        regs[CrashRegisterSp] = *reinterpret_cast<const volatile uint32_t*>(banked);
        regs[CrashRegisterLr] = *reinterpret_cast<const volatile uint32_t*>(banked + sizeof(uint32_t));
    }

    const auto pc = crash_pc(regs[CrashRegisterPc], spsr, kind);
    regs[CrashRegisterPc] = pc;

    // tells a jump into data (heap, zeroes, erased flash) from a real undefined opcode
    uint32_t insn { 0 };
    if (crash_readable(pc & ~3u, sizeof(uint32_t))) {
        insn = *reinterpret_cast<const volatile uint32_t*>(pc & ~3u);
    }

    // no stack high water mark, the SDK builds FreeRTOS without it
    char task[sizeof(CrashRecord::task)] {};
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        std::strncpy(task, pcTaskGetName(nullptr), sizeof(task) - 1);
    }

    const uint32_t head[] {
        CrashRecordMagic,
        static_cast<uint32_t>(kind),
        espurna::time::ms(),
    };

    static_assert(offsetof(CrashRecord, magic) == 0, "");
    static_assert(offsetof(CrashRecord, uptime) == sizeof(head) - sizeof(uint32_t), "");

    if (!CrashRecordOffset) {
        return;
    }
    ::espurna::beken::flash_erase(CrashRecordOffset, FLASH_SECTOR_SIZE);
    crash_flash_write(offsetof(CrashRecord, magic), head, sizeof(head));
    crash_flash_write(offsetof(CrashRecord, regs), regs, sizeof(regs));
    crash_flash_write(offsetof(CrashRecord, insn), &insn, sizeof(insn));

    const auto sp = regs[CrashRegisterSp];
    if (((sp & 3) == 0) && (sp >= RamBegin) && (sp < RamEnd)) {
        const auto words = std::min<size_t>(CrashStackWords, (RamEnd - sp) / sizeof(uint32_t));
        crash_flash_write(offsetof(CrashRecord, stack), reinterpret_cast<const void*>(sp), words * sizeof(uint32_t));
    }

    crash_flash_write(offsetof(CrashRecord, task), task, sizeof(task));
    crash_flash_write(offsetof(CrashRecord, build), _crash_build, sizeof(_crash_build));
}

bool crash_record_read(CrashRecord& out) {
    return CrashRecordOffset && (::espurna::beken::flash_read(CrashRecordOffset, reinterpret_cast<uint8_t*>(&out), sizeof(out)) == sizeof(out))
        && (out.magic == CrashRecordMagic);
}

espurna::string crash_summary(const CrashRecord& record) {
    char buffer[224];
    if (record.kind == static_cast<uint32_t>(RtcmemCrash::Stall)) {
        snprintf_P(buffer, sizeof(buffer),
            PSTR("%s for %u ms of %u, loop in 0x%08x, %u tasks, free=%u uptime=%u build=%.32s"),
            crash_name(record.kind), record.regs[0], record.regs[1], record.regs[CrashRegisterPc], record.regs[2],
            record.regs[3], record.uptime, record.build);
        return espurna::string(buffer);
    }

    if (record.kind == static_cast<uint32_t>(RtcmemCrash::MallocFailed)) {
        snprintf_P(buffer, sizeof(buffer),
            PSTR("%s free=%u lowest=%u in=0x%08x sp=0x%08x task=%.16s uptime=%u build=%.32s"),
            crash_name(record.kind), record.regs[0], record.regs[1], record.regs[CrashRegisterPc],
            record.regs[CrashRegisterSp], record.task, record.uptime, record.build);
        return espurna::string(buffer);
    }

    snprintf_P(buffer, sizeof(buffer),
        PSTR("%s pc=0x%08x insn=0x%08x lr=0x%08x sp=0x%08x spsr=0x%02x task=%.16s uptime=%u build=%.32s"),
        crash_name(record.kind),
        record.regs[CrashRegisterPc], record.insn, record.regs[CrashRegisterLr], record.regs[CrashRegisterSp],
        record.regs[CrashRegisterSpsr] & 0xff, record.task, record.uptime, record.build);

    return espurna::string(buffer);
}

#if MQTT_SUPPORT
espurna::string _crash_mqtt;
#endif

} // namespace

// Same as the SDK handlers (the SDK glue fixups/intc.c), except for the value left in the register and the record.
// Called from the exception vectors (boot_handlers.S), linked with --wrap (ref. platformio_beken.ini).
// Vectors run them in the exception mode, whose stack is only 16 bytes (_UND_STACK_SIZE_, _ABT_STACK_SIZE_) and
// already overflown by the 72-byte register frame. Anything deeper, like printf or the flash driver, writes over
// the .bss below it (wifi globals, then whatever comes next) and never finishes: the watchdog resets the chip with
// an empty record and no output. So the C part runs on a borrowed stack, with both interrupts off: the FIQ one
// (_FIQ_STACK_SIZE_ 0x7f0), unused once FIQ is masked, or the IRQ one (0xff0) when the crash happened in FIQ mode
// and the FIQ stack is the one being dumped. Nothing returns from here, so their old contents do not matter.
// No buffer of our own: interrupt stacks and heap are the RAM that Matter boards are short of
extern "C" {

struct arm_registers;
void bk_show_register(struct arm_registers*);
void bk_cpu_shutdown(void);
void bk_printf(const char*, ...);

[[noreturn]] void _rtcmem_crash(const uint32_t* frame, uint32_t kind) {
    // frame lives below the exception mode stack, the borrowed one is elsewhere (boot_handlers.S .bss order:
    // und, abt, fiq, irq), so it stays as it is
    retention() = rtcmemRetention() | kind;
    crash_record_write(frame, static_cast<RtcmemCrash>(kind));

    bk_printf("%s\n", crash_name(kind));
    bk_show_register(reinterpret_cast<struct arm_registers*>(const_cast<uint32_t*>(frame)));
    bk_cpu_shutdown();
    for (;;) {
    }
}

// r0 is the frame, r1 the kind. Frame spsr (word 16) is the interrupted cpsr, mode 0x11 is FIQ
#define RTCMEM_CRASH_HANDLER(NAME, KIND)\
    __attribute__((naked, target("arm"))) void NAME(struct arm_registers*) {\
        asm volatile(\
            "mrs r2, cpsr\n"\
            "orr r2, r2, #0xc0\n"\
            "msr cpsr_c, r2\n"\
            "mov r1, #" #KIND "\n"\
            "ldr r2, [r0, #64]\n"\
            "and r2, r2, #0x1f\n"\
            "cmp r2, #0x11\n"\
            "ldreq sp, =irq_stack_start + 0xff0\n"\
            "ldrne sp, =fiq_stack_start + 0x7f0\n"\
            "ldr r2, =_rtcmem_crash\n"\
            "bx r2\n"\
            ".ltorg\n");\
    }

static_assert(static_cast<uint32_t>(RtcmemCrash::Undefined) == 1, "");
static_assert(static_cast<uint32_t>(RtcmemCrash::PrefetchAbort) == 2, "");
static_assert(static_cast<uint32_t>(RtcmemCrash::DataAbort) == 3, "");
static_assert(static_cast<uint32_t>(RtcmemCrash::Reserved) == 4, "");

RTCMEM_CRASH_HANDLER(__wrap_bk_trap_udef, 1)
RTCMEM_CRASH_HANDLER(__wrap_bk_trap_pabt, 2)
RTCMEM_CRASH_HANDLER(__wrap_bk_trap_dabt, 3)
RTCMEM_CRASH_HANDLER(__wrap_bk_trap_resv, 4)

#undef RTCMEM_CRASH_HANDLER

// FreeRTOS calls it from pvPortMalloc() (heap_4, scheduler suspended) when a block can not be found, the SDK one
// is empty. Nothing faults afterwards: the caller gets NULL, writes near 0 are ignored on BK7231N and the chip
// usually hangs until the watchdog, without any trace. So the first failure of a boot leaves a record, reported at
// the next boot like a crash. The hook has no arguments: r0 = free heap, r1 = lowest free heap ever, pc = lr =
// the return address inside pvPortMalloc() and the stack words hold its caller and the requested size.
// Linked with --wrap (ref. platformio_beken.ini). Runs on the stack of the task that asked, keep it small
namespace {

bool _rtcmem_malloc_failed { false };

} // namespace

size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);

void __wrap_vApplicationMallocFailedHook(void) {
    if (_rtcmem_malloc_failed) {
        return;
    }

    _rtcmem_malloc_failed = true;

    // a failed allocation is a note, a fault or a stall is the crash: the record of one is not written over. A new
    // (std::nothrow) the caller handles came 37 s after a watchdog reset and took the place of its stall record
    // (10.0.5.139 2026-11-15, the regress round 49)
    if (CrashRecordOffset) {
        uint32_t head[2] {};
        ::espurna::beken::flash_read(CrashRecordOffset, reinterpret_cast<uint8_t*>(head), sizeof(head));
        if ((head[0] == CrashRecordMagic) && (head[1] != static_cast<uint32_t>(RtcmemCrash::MallocFailed))) {
            return;
        }
    }

    uint32_t regs[CrashRegisters] {};
    regs[0] = xPortGetFreeHeapSize();
    regs[1] = xPortGetMinimumEverFreeHeapSize();

    uint32_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    regs[CrashRegisterSp] = sp;

    const auto ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    regs[CrashRegisterLr] = ret;
    regs[CrashRegisterPc] = ret;

    // spsr 0 is no mode, the record keeps the sp and lr above
    crash_record_write(regs, RtcmemCrash::MallocFailed);
}

// The head's own abort() in place of newlib's (whose _exit is libnosys' endless loop): the modules link against this
// one too. The caller's return address as pc and lr, the stack it had, then the crash path of the trap handlers
[[noreturn]] void abort(void) {
    uint32_t regs[CrashRegisters] {};
    regs[0] = xPortGetFreeHeapSize();
    regs[1] = xPortGetMinimumEverFreeHeapSize();

    uint32_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    regs[CrashRegisterSp] = sp;

    const auto ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    regs[CrashRegisterLr] = ret;
    regs[CrashRegisterPc] = ret;

    _rtcmem_crash(regs, static_cast<uint32_t>(RtcmemCrash::Abort));
}

} // extern "C"

// A chip that hangs without a fault (two tasks waiting for each other, a task that never gives the CPU back) is only
// reset by the watchdog, and the reset leaves nothing behind. The common stall watcher (arch/rtos/system_rtos.cpp) keeps
// an eye on the feeding (beken::wdt_* below tell it). When the watchdog is about to
// fire, the record here gets the saved context of every task: name, state, priority and the words at its saved stack
// pointer, i.e. the registers the scheduler saved (pc and lr included) and the frames of the calls it waits in; pc of
// the record is the loop callback the loop task stood in. The watchdog resets the chip right after
namespace {

namespace stall {

constexpr size_t TableOffset { 1024 };
constexpr size_t Tasks { 16 };
constexpr size_t Words { 40 };

struct Entry {
    char name[8];
    uint8_t state;
    uint8_t priority;
    uint8_t base;
    uint8_t number;
    uint32_t top;
    // what a blocked task waits on: the event list of its TCB (xEventListItem.pvContainer), the mutex behind it
    // (a mutex has pcHead 0, its holder in u.xSemaphore.xMutexHolder) and the number of the holding task (0: none, or not a mutex)
    uint32_t waits;
    uint32_t mutex;
    uint8_t holder;
    uint8_t reserved[3];
    uint32_t words[Words];
};

// FreeRTOS-Kernel V11.1.0 (the offsets are checked by the kernel package, portable/GCC/ARM968/freertos_tasks_c_additions.h): TCB {
// pxTopOfStack; ListItem_t xStateListItem; ListItem_t xEventListItem; ... }, a ListItem_t is 5 words (no integrity
// check), pxContainer the last one; Queue_t { pcHead (NULL for a mutex: queueQUEUE_IS_MUTEX), pcWriteTo, u { xQueue,
// xSemaphore { xMutexHolder, uxRecursiveCallCount } }, List_t xTasksWaitingToSend, List_t xTasksWaitingToReceive, ... },
// a List_t is 7 words (its end is a full ListItem_t: configUSE_MINI_LIST_ITEM 0, GCC 14 strict aliasing). A task taking a
// mutex waits in xTasksWaitingToReceive
constexpr size_t TcbEventContainer { 4 + 20 + 16 };
constexpr size_t QueueReceiveList { 16 + 28 };
constexpr size_t QueueMutexHolder { 8 };

// the handle of the lwIP core lock (tcpip.c, LWIP_TCPIP_CORE_LOCKING): created once at the start of lwIP, the
// same every boot, compared against the mutex a task waited for
uint32_t lwip_core_lock() {
    return *reinterpret_cast<const uint32_t*>(&lock_tcpip_core);
}

static_assert((TableOffset + Tasks * sizeof(Entry)) <= FLASH_SECTOR_SIZE, "");
static_assert(sizeof(CrashRecord) <= TableOffset, "");

TaskStatus_t statuses[24];

void write(const espurna::system::stall::Report& report) {
    const auto count = uxTaskGetSystemState(statuses, sizeof(statuses) / sizeof(statuses[0]), nullptr);

    uint32_t regs[CrashRegisters] {};
    regs[0] = report.since;
    regs[1] = report.period;
    regs[2] = count;
    regs[3] = xPortGetFreeHeapSize();
    regs[CrashRegisterPc] = report.callback;

    const uint32_t head[] {
        CrashRecordMagic,
        static_cast<uint32_t>(RtcmemCrash::Stall),
        espurna::time::ms(),
    };

    char task[sizeof(CrashRecord::task)] {};
    std::strncpy(task, "stall", sizeof(task) - 1);

    if (!CrashRecordOffset) {
        return;
    }
    ::espurna::beken::flash_erase(CrashRecordOffset, FLASH_SECTOR_SIZE);
    crash_flash_write(offsetof(CrashRecord, magic), head, sizeof(head));
    crash_flash_write(offsetof(CrashRecord, regs), regs, sizeof(regs));
    crash_flash_write(offsetof(CrashRecord, task), task, sizeof(task));
    crash_flash_write(offsetof(CrashRecord, build), _crash_build, sizeof(_crash_build));

    for (size_t index = 0; (index < count) && (index < Tasks); ++index) {
        const auto& status = statuses[index];

        Entry entry {};
        std::strncpy(entry.name, status.pcTaskName, sizeof(entry.name));
        entry.state = static_cast<uint8_t>(status.eCurrentState);
        entry.priority = static_cast<uint8_t>(status.uxCurrentPriority);
        entry.base = static_cast<uint8_t>(status.uxBasePriority);
        entry.number = static_cast<uint8_t>(status.xTaskNumber);

        // first member of the TCB, where the context switch left the registers
        const auto top = *reinterpret_cast<const volatile uint32_t*>(status.xHandle);
        entry.top = top;

        const auto container = *reinterpret_cast<const volatile uint32_t*>(
            reinterpret_cast<uintptr_t>(status.xHandle) + TcbEventContainer);
        if (((container & 3) == 0) && (container >= (RamBegin + QueueReceiveList)) && (container < RamEnd)) {
            entry.waits = container;
            const auto queue = container - QueueReceiveList;
            const auto head = *reinterpret_cast<const volatile uint32_t*>(queue);
            const auto holder = *reinterpret_cast<const volatile uint32_t*>(queue + QueueMutexHolder);
            if (head == 0) {
                entry.mutex = queue;
                for (size_t other = 0; other < count; ++other) {
                    if (reinterpret_cast<uintptr_t>(statuses[other].xHandle) == holder) {
                        entry.holder = static_cast<uint8_t>(statuses[other].xTaskNumber);
                        break;
                    }
                }
            }
        }

        const auto offset = TableOffset + index * sizeof(Entry);
        crash_flash_write(offset, &entry, offsetof(Entry, words));

        if (((top & 3) == 0) && (top >= RamBegin) && (top < RamEnd)) {
            const auto words = std::min<size_t>(Words, (RamEnd - top) / sizeof(uint32_t));
            crash_flash_write(offset + offsetof(Entry, words), reinterpret_cast<const void*>(top),
                words * sizeof(uint32_t));
        }
    }
}

void dump(::espurna::Output& print) {
    print.print("tasks (state 0 running 1 ready 2 blocked 3 suspended), words at the saved sp:\n");
    for (size_t index = 0; index < Tasks; ++index) {
        Entry entry;
        ::espurna::beken::flash_read(CrashRecordOffset + TableOffset + index * sizeof(Entry),
            reinterpret_cast<uint8_t*>(&entry), sizeof(entry));
        if (static_cast<uint8_t>(entry.name[0]) == 0xff) {
            break;
        }

        print.printf_P(PSTR("%.8s #%u state %u prio %u/%u sp 0x%08x\n"),
            entry.name, entry.number, entry.state, entry.priority, entry.base, entry.top);
        if (entry.waits && (entry.waits != 0xffffffff)) {
            if (entry.mutex && (entry.mutex != 0xffffffff)) {
                print.printf_P(PSTR("  waits for mutex 0x%08x%s, held by #%u\n"), entry.mutex,
                    (entry.mutex == lwip_core_lock()) ? PSTR(" (lwIP core lock)") : PSTR(""), entry.holder);
            } else {
                print.printf_P(PSTR("  waits on list 0x%08x\n"), entry.waits);
            }
        }
        for (size_t word = 0; word < Words; word += 8) {
            for (size_t next = word; next < word + 8; ++next) {
                print.printf_P(PSTR("%08x "), entry.words[next]);
            }
            print.print('\n');
        }
    }
}

} // namespace stall
} // namespace

// the watchdog of beken_api.h: the stall watcher learns its period and its feeds, then the BDK (core_beken.cpp)
namespace espurna {
namespace beken {

void wdt_enable(uint32_t timeout) {
    espurna::system::stall::period(timeout);
    raw::wdt_enable(timeout);
}

void wdt_disable() {
    espurna::system::stall::period(0);
    raw::wdt_disable();
}

void wdt_feed() {
    espurna::system::stall::fed();
    raw::wdt_feed();
}

} // namespace beken
} // namespace espurna

// the common stall watcher, ref. system_chip.h
namespace espurna {
namespace system {
namespace stall {
namespace chip {

// the reset that follows is ours, not the SDK's watchdog: the register keeps the kind until then (a feed that still
// comes in time writes the whole register over, and that boot has no stall to report)
void record(const Report& report) {
    ::stall::write(report);
    retention() = rtcmemRetention() | static_cast<uint32_t>(RtcmemCrash::Stall);
}

// the SDK's next feed has written the register over already
void cancel() {
}

[[noreturn]] void reset(const Report& report) {
    ::stall::write(report);

    portDISABLE_INTERRUPTS();
    retention() = rtcmemRetention() | static_cast<uint32_t>(RtcmemCrash::Stall);
    ::espurna::beken::raw::wdt_disable();
    ::espurna::beken::raw::wdt_enable(5);
    for (;;) {
    }
}

} // namespace chip
} // namespace stall
} // namespace system
} // namespace espurna

// Also used before setup(), by the reset reason
bool rtcmemSysStatus() {
    return (::espurna::beken::reboot_reason() != ::espurna::beken::Reboot::Power)
        && ((_rtcmem_retention_at_boot & RetentionMarkerMask) == RetentionMarker);
}

RtcmemCrash rtcmemCrash() {
    return rtcmemSysStatus()
        ? static_cast<RtcmemCrash>(_rtcmem_retention_at_boot & RetentionCrashMask)
        : RtcmemCrash::None;
}

uint32_t rtcmemRetention() {
    return pack(Rtcmem->sys);
}

void rtcmemChipSetup() {
    CrashRecordOffset = ::espurna::beken::flashLayout().crash_offset;

    if (rtcmemSysStatus()) {
        Rtcmem->sys = unpack(_rtcmem_retention_at_boot);
    }
}

void rtcmemCrashSetup() {
    const auto build = buildTime();
    std::memcpy(_crash_build, build.data(), std::min(build.length(), sizeof(_crash_build) - 1));

    CrashRecord record;
    if (!crash_record_read(record) || (record.pending != CrashRecordPending)) {
        return;
    }

    const auto summary = crash_summary(record);
    DEBUG_MSG_P(PSTR("[MAIN] Last crash: %s (CRASH shows the registers)\n"), summary.c_str());

    const uint32_t reported { 0 };
    crash_flash_write(offsetof(CrashRecord, pending),
        reinterpret_cast<const uint8_t*>(&reported), sizeof(reported));

#if MQTT_SUPPORT
    // once, on the first connection
    _crash_mqtt = summary;
    mqttRegister([](unsigned int type, espurna::StringView, espurna::StringView) {
        if ((type == MQTT_CONNECT_EVENT) && _crash_mqtt.length()) {
            mqttSendRaw(mqttTopic("crash").c_str(), _crash_mqtt.c_str(), false);
            _crash_mqtt = espurna::string();
        }
    });
#endif
}

void rtcmemCrashDump(::espurna::Output& print) {
    CrashRecord record;
    if (!crash_record_read(record)) {
        print.print("No crash recorded\n");
        return;
    }

    print.printf_P(PSTR("Last crash: %s\n"), crash_summary(record).c_str());

    if (record.kind == static_cast<uint32_t>(RtcmemCrash::Stall)) {
        stall::dump(print);
        return;
    }

    const auto* regs = record.regs;
    print.printf_P(PSTR("cpsr=0x%08x spsr=0x%08x, sp lr of the interrupted mode\n"),
        regs[CrashRegisterCpsr], regs[CrashRegisterSpsr]);
    for (size_t index = 0; index < CrashRegisterSp; index += 4) {
        print.printf_P(PSTR("r%-2u 0x%08x"), index, regs[index]);
        for (size_t next = index + 1; (next < index + 4) && (next < CrashRegisterSp); ++next) {
            print.printf_P(PSTR("  r%-2u 0x%08x"), next, regs[next]);
        }
        print.print('\n');
    }

    print.printf_P(PSTR("stack at 0x%08x:\n"), regs[CrashRegisterSp]);
    for (size_t index = 0; index < CrashStackWords; index += 8) {
        for (size_t next = index; next < index + 8; ++next) {
            print.printf_P(PSTR("%08x "), record.stack[next]);
        }
        print.print('\n');
    }
}

// what it was and where (pc, lr, then the stack words that point into the flash: the page looks for a module there)
void rtcmemCrashCause(CrashCause& out) {
    std::unique_ptr<CrashRecord> record(new (std::nothrow) CrashRecord);
    if (!record || !crash_record_read(*record)) {
        return;
    }

    snprintf_P(out.text, sizeof(out.text), PSTR("%s in task %.16s, pc 0x%08x, %u ms after boot"),
        crash_name(record->kind), record->task, record->regs[CrashRegisterPc], record->uptime);
    // a stall: pc is the loop callback the loop task stood in
    out.addresses[out.count++] = record->regs[CrashRegisterPc];
    if (record->kind == static_cast<uint32_t>(RtcmemCrash::Stall)) {
        return;
    }

    out.addresses[out.count++] = record->regs[CrashRegisterLr];
    for (size_t index = 0; (index < CrashStackWords) && (out.count < std::size(out.addresses)); ++index) {
        const auto word = record->stack[index];
        if ((word >= 0x10000) && (word < 0x400000)) {
            out.addresses[out.count++] = word;
        }
    }
}
