#pragma once
// Phase 12 radio-task lifecycle bookkeeping. Hardware calls remain outside
// this header; it makes the terminal row impossible to publish before home
// restore has been recorded.

#include <stdint.h>

#include "focus_observation.h" // FocusRequestStatus

enum class FocusRuntimeState : uint8_t {
    IDLE,
    SURVEYING,
    RESTORING,
    COMPLETE,
    CANCELLED,
    TIMEOUT,
    FAILED,
};

struct FocusRuntime {
    FocusRequest request;
    FocusRuntimeState state = FocusRuntimeState::IDLE;
    uint16_t valid_passes = 0;
    uint32_t observation_ms = 0;
    bool cancelled = false;
    bool timed_out = false;
    bool failed = false;
};

inline bool focusRuntimeBegin(FocusRuntime &runtime, const FocusRequest &request) {
    if (runtime.state == FocusRuntimeState::SURVEYING ||
        runtime.state == FocusRuntimeState::RESTORING || !focusRequestIsValid(request)) {
        return false;
    }
    runtime = FocusRuntime{};
    runtime.request = request;
    runtime.state = FocusRuntimeState::SURVEYING;
    return true;
}

inline void focusRuntimeNoteValidPass(FocusRuntime &runtime, uint32_t observation_ms) {
    if (runtime.state != FocusRuntimeState::SURVEYING) return;
    runtime.valid_passes++;
    runtime.observation_ms += observation_ms;
}

inline void focusRuntimeCancel(FocusRuntime &runtime) {
    if (runtime.state == FocusRuntimeState::SURVEYING) runtime.cancelled = true;
}

inline void focusRuntimeTimeout(FocusRuntime &runtime) {
    if (runtime.state == FocusRuntimeState::SURVEYING) runtime.timed_out = true;
}

inline void focusRuntimeFail(FocusRuntime &runtime) {
    if (runtime.state == FocusRuntimeState::SURVEYING) runtime.failed = true;
}

inline void focusRuntimeBeginRestore(FocusRuntime &runtime) {
    if (runtime.state == FocusRuntimeState::SURVEYING) runtime.state = FocusRuntimeState::RESTORING;
}

inline FocusRequestStatus focusRuntimeFinishRestore(FocusRuntime &runtime, bool restored) {
    if (!restored) runtime.failed = true;
    if (runtime.failed) runtime.state = FocusRuntimeState::FAILED;
    else if (runtime.cancelled) runtime.state = FocusRuntimeState::CANCELLED;
    else if (runtime.timed_out) runtime.state = FocusRuntimeState::TIMEOUT;
    else runtime.state = FocusRuntimeState::COMPLETE;

    switch (runtime.state) {
        case FocusRuntimeState::COMPLETE: return FocusRequestStatus::COMPLETE;
        case FocusRuntimeState::CANCELLED: return FocusRequestStatus::CANCELLED;
        case FocusRuntimeState::TIMEOUT: return FocusRequestStatus::TIMEOUT;
        default: return FocusRequestStatus::FAILED;
    }
}
