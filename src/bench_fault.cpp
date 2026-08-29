#include "bench_fault.h"

#include <string.h>

#include "energy_observation.h"

namespace {

volatile bool armed = false;
volatile BenchFaultPoint armedPoint = BenchFaultPoint::CAD_WAIT;
volatile BenchFaultAction armedAction = BenchFaultAction::FAIL;
volatile unsigned char cadSymbols = 2;
// References ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10 rather than a copied
// literal so a bench image that never receives BENCH_SWEEP_MARGIN can
// never silently drift from production's real default the moment that
// constant is recalibrated (it did exactly that once, 2026-08-29: this
// was hardcoded to the pre-calibration 100 and kept returning it in
// benchSweepMarginDbmX10() below even after the real constant moved to
// 350, caught only because a live production-firmware sweep was checked
// against the calibration bench's own expected result instead of trusting
// the constant edit alone).
volatile int16_t sweepMarginDbmX10 = ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10;

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

// Bounded to [0, 500] tenths of dB (0-50dB) — wide enough to bracket any
// plausible calibration value, narrow enough that a typo can't silently
// arm something nonsensical (e.g. a negative margin, which would make
// every bin a "peak").
bool parseSweepMargin(const char *text, int16_t &value) {
    if (text == nullptr || text[0] == '\0') return false;
    int32_t parsed = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        parsed = parsed * 10 + (int32_t)(*p - '0');
        if (parsed > 500) return false;
    }
    value = (int16_t)parsed;
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

bool benchSweepMarginConfigure(const char *argument) {
#if !defined(LORATRACE_BENCH_FAULTS)
    (void)argument;
    return false;
#else
    int16_t parsed = 0;
    if (!parseSweepMargin(argument, parsed)) return false;
    sweepMarginDbmX10 = parsed;
    return true;
#endif
}

int16_t benchSweepMarginDbmX10() {
#if !defined(LORATRACE_BENCH_FAULTS)
    return ENERGY_DEFAULT_THRESHOLD_MARGIN_DBM_X10;
#else
    return sweepMarginDbmX10;
#endif
}
