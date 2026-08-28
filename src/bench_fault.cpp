#include "bench_fault.h"

#include <string.h>

namespace {

volatile bool armed = false;
volatile BenchFaultPoint armedPoint = BenchFaultPoint::CAD_WAIT;
volatile BenchFaultAction armedAction = BenchFaultAction::FAIL;
volatile unsigned char cadSymbols = 2;

bool parsePoint(const char *text, BenchFaultPoint &point) {
    if (strcmp(text, "BEFORE_RETUNE") == 0) point = BenchFaultPoint::BEFORE_RETUNE;
    else if (strcmp(text, "AFTER_RETUNE") == 0) point = BenchFaultPoint::AFTER_RETUNE;
    else if (strcmp(text, "CAD_WAIT") == 0) point = BenchFaultPoint::CAD_WAIT;
    else if (strcmp(text, "RX_WAIT") == 0) point = BenchFaultPoint::RX_WAIT;
    else if (strcmp(text, "HOME_RESTORE_BEFORE") == 0) point = BenchFaultPoint::HOME_RESTORE_BEFORE;
    else if (strcmp(text, "HOME_RESTORE_AFTER") == 0) point = BenchFaultPoint::HOME_RESTORE_AFTER;
    else return false;
    return true;
}

bool parseCadSymbols(const char *text, unsigned char &value) {
    if (text == nullptr || text[0] == '\0') return false;
    unsigned int parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        parsed = parsed * 10U + (unsigned int)(*p - '0');
        if (parsed > 16U) return false;
    }
    if (parsed != 1U && parsed != 2U && parsed != 4U && parsed != 8U && parsed != 16U) return false;
    value = (unsigned char)parsed;
    return true;
}

} // namespace

bool benchFaultConfigure(const char *argument) {
#if !defined(LORATRACE_BENCH_FAULTS)
    (void)argument;
    return false;
#else
    if (argument == nullptr) return false;
    if (strcmp(argument, "CLEAR") == 0) {
        armed = false;
        return true;
    }

    char copy[48] = {};
    const size_t length = strlen(argument);
    if (length == 0 || length >= sizeof(copy)) return false;
    memcpy(copy, argument, length + 1);
    char *separator = strchr(copy, ':');
    if (separator == nullptr || separator == copy || separator[1] == '\0') return false;
    *separator = '\0';

    BenchFaultPoint point;
    BenchFaultAction action;
    if (!parsePoint(copy, point)) return false;
    if (strcmp(separator + 1, "CANCEL") == 0) action = BenchFaultAction::CANCEL;
    else if (strcmp(separator + 1, "FAIL") == 0) action = BenchFaultAction::FAIL;
    else return false;

    armedPoint = point;
    armedAction = action;
    armed = true;
    return true;
#endif
}

bool benchFaultTake(BenchFaultPoint point, BenchFaultAction &action) {
#if !defined(LORATRACE_BENCH_FAULTS)
    (void)point;
    (void)action;
    return false;
#else
    if (!armed || armedPoint != point) return false;
    action = armedAction;
    armed = false;
    return true;
#endif
}

bool benchCadSymbolsConfigure(const char *argument) {
#if !defined(LORATRACE_BENCH_FAULTS)
    (void)argument;
    return false;
#else
    unsigned char parsed = 0;
    if (!parseCadSymbols(argument, parsed)) return false;
    cadSymbols = parsed;
    return true;
#endif
}

unsigned char benchCadSymbols() {
#if !defined(LORATRACE_BENCH_FAULTS)
    return 2;
#else
    return cadSymbols;
#endif
}
