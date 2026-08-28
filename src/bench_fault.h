#pragma once

// Deterministic fault hooks used only by the dedicated cardputer-adv-bench
// build. Production firmware accepts no operation through this API.

enum class BenchFaultPoint : unsigned char {
    BEFORE_RETUNE,
    AFTER_RETUNE,
    CAD_WAIT,
    RX_WAIT,
    HOME_RESTORE_BEFORE,
    HOME_RESTORE_AFTER,
};

enum class BenchFaultAction : unsigned char {
    CANCEL,
    FAIL,
};

// Arm one one-shot hook with an argument such as "CAD_WAIT:FAIL". "CLEAR"
// disarms a pending hook. The command is deliberately bounded to named
// radio-task boundaries; it cannot issue arbitrary RadioLib calls.
bool benchFaultConfigure(const char *argument);

// Consumes the matching one-shot hook, if any. Called only at the boundaries
// named above by the radio task.
bool benchFaultTake(BenchFaultPoint point, BenchFaultAction &action);

// Bench-image-only CAD selector for the Phase 8 rate matrix. Production
// firmware always returns the source-backed two-symbol default and rejects
// changes, so serial control cannot retune its receiver behavior.
bool benchCadSymbolsConfigure(const char *argument);
unsigned char benchCadSymbols();
