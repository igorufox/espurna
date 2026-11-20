/*

PROTOTYPE: the Matter MODULE of the ESP32 head (docs/modules.md, "ESP32"): chip::ErrorStr and friends without
thread-local storage.

CHIP's own lib/core/ErrorStr.cpp keeps its result buffer `thread_local` (the only TLS of the stack). A module cannot have
TLS: every FreeRTOS task gets its TLS block from the template of the head image, and the head has none. Linked first in
the matter module, these definitions keep ErrorStr.cpp.obj out of it. The buffer is shared by the tasks: an error
string printed by two tasks at the same moment can be mixed up, the text of a log line, nothing else.
Same behaviour as the CHIP sources otherwise (CHIP_CONFIG_ERROR_SOURCE and CHIP_CONFIG_SHORT_ERROR_STR are 0 in the
Arduino-ESP32 build). The monolithic images keep the library's version (MODULE_BUILD only).

*/

#include "../../espurna.h"

#if MODULE_BUILD && MATTER_SUPPORT

// the library's options before its headers (ESP-IDF's sdkconfig.h has none of them)
#include "matter_config_chip.h"

#include <lib/core/ErrorStr.h>

#include <cinttypes>
#include <cstdio>

namespace chip {

static ErrorFormatter* sErrorFormatterList = nullptr;
static ErrorStrStorage sErrorStr;

const char* ErrorStr(CHIP_ERROR err, bool withSourceLocation, ErrorStrStorage& storage) {
    (void) withSourceLocation;
    char* formattedError = storage.buff;
    const uint16_t formattedSpace = storage.kBufferSize;

    if (err == CHIP_NO_ERROR) {
        (void) snprintf(formattedError, formattedSpace, CHIP_NO_ERROR_STRING);
        return storage.buff;
    }

    for (const ErrorFormatter* errFormatter = sErrorFormatterList; errFormatter != nullptr;
            errFormatter = errFormatter->Next)
    {
        if (errFormatter->FormatError(formattedError, formattedSpace, err)) {
            return storage.buff;
        }
    }

    FormatError(formattedError, formattedSpace, nullptr, err, nullptr);
    return storage.buff;
}

const char* ErrorStr(CHIP_ERROR err, bool withSourceLocation) {
    return ErrorStr(err, withSourceLocation, sErrorStr);
}

void RegisterErrorFormatter(ErrorFormatter* errFormatter) {
    for (ErrorFormatter* existing = sErrorFormatterList; existing != nullptr; existing = existing->Next) {
        if (existing->FormatError == errFormatter->FormatError) {
            return;
        }
    }

    errFormatter->Next = sErrorFormatterList;
    sErrorFormatterList = errFormatter;
}

void DeregisterErrorFormatter(ErrorFormatter* errFormatter) {
    for (ErrorFormatter** link = &sErrorFormatterList; *link != nullptr; link = &(*link)->Next) {
        if (*link == errFormatter) {
            *link = errFormatter->Next;
            return;
        }
    }
}

void FormatError(char* buf, uint16_t bufSize, const char* subsys, CHIP_ERROR err, const char* desc) {
    const char* subsysSep = " ";
    const char* descSep = ": ";

    if (subsys == nullptr) {
        subsys = "";
        subsysSep = "";
    }

    if (desc == nullptr) {
        desc = "";
        descSep = "";
    }

    (void) snprintf(buf, bufSize, "%s%s0x%08" PRIX32 "%s%s", subsys, subsysSep, err.AsInteger(), descSep, desc);
}

} // namespace chip

#endif
