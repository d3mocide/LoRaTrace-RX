#pragma once
// LoRaTrace RX — how long a bounded action may own the radio.
//
// Every bounded action (Probe, Sweep, Cell, Scope, Focus) bounded itself by
// counting operations: so many bins, so many samples, each with its own
// per-operation timeout. That is not a bound on radio-away time, because the
// per-operation waits multiply. Scope's 240 samples at 20 ms read as ~5 s, but
// every sample could wait BUS_WAIT for the SPI bus and again for a display
// mutex — a worst case in minutes, on a receiver whose whole job is to be
// listening (audit A22).
//
// This is the missing contract: one wall-clock deadline for the whole action,
// checked between operations, plus a separate allowance for restoring home
// afterwards. It does not make anything faster and cannot preempt a driver
// call already in flight — it decides when to *stop starting* new work.
//
// Pure, so the arithmetic (including millis() rollover) is host-tested rather
// than inferred from a long soak.

#include <stdint.h>

// Deadlines are absolute millis() values compared with signed arithmetic, so
// they survive the 49.7-day rollover the way the rest of this codebase's
// timing does.
struct ActionBudget {
    uint32_t started_ms = 0;
    uint32_t acquisition_deadline_ms = 0; // stop starting new sampling work
    uint32_t total_deadline_ms = 0;       // acquisition plus the restore allowance
};

inline bool actionBudgetReached(uint32_t deadline_ms, uint32_t now_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

// `acquisition_ms` is what the action may spend measuring; `restore_ms` is the
// additional allowance for retuning home and confirming it. They are separate
// because a restore that runs late is still worth completing — abandoning it
// leaves the radio deaf, which is the outcome the budget exists to prevent.
inline ActionBudget actionBudgetBegin(uint32_t now_ms, uint32_t acquisition_ms,
                                      uint32_t restore_ms) {
    ActionBudget budget;
    budget.started_ms = now_ms;
    budget.acquisition_deadline_ms = now_ms + acquisition_ms;
    budget.total_deadline_ms = now_ms + acquisition_ms + restore_ms;
    return budget;
}

inline bool actionBudgetAcquisitionExpired(const ActionBudget &budget, uint32_t now_ms) {
    return actionBudgetReached(budget.acquisition_deadline_ms, now_ms);
}

inline bool actionBudgetTotalExpired(const ActionBudget &budget, uint32_t now_ms) {
    return actionBudgetReached(budget.total_deadline_ms, now_ms);
}

// Milliseconds left before the acquisition deadline, saturating at 0. Useful
// as a per-operation timeout so one wait cannot outlive the whole action.
inline uint32_t actionBudgetRemainingMs(const ActionBudget &budget, uint32_t now_ms) {
    const int32_t remaining = (int32_t)(budget.acquisition_deadline_ms - now_ms);
    return remaining > 0 ? (uint32_t)remaining : 0u;
}

inline uint32_t actionBudgetElapsedMs(const ActionBudget &budget, uint32_t now_ms) {
    return now_ms - budget.started_ms;
}

// A sample that should have been taken at `target_ms` but is being considered
// at `now_ms`. Late samples must be *skipped*, not taken immediately: firing
// them back to back to catch up with a schedule produces a burst whose spacing
// is nothing like the spacing the row claims, and a fixed requested spacing is
// not evidence of actual spacing under contention (audit A22).
//
// `tolerance_ms` is how late is still close enough to be the sample it claims
// to be.
inline bool actionSampleIsOnTime(uint32_t target_ms, uint32_t now_ms, uint32_t tolerance_ms) {
    const int32_t lateness = (int32_t)(now_ms - target_ms);
    return lateness <= (int32_t)tolerance_ms;
}
