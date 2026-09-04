// LoRaTrace RX — ui_task's menu-action business logic.
//
// Split out of ui_task.cpp (2026-08-25 cleanup pass, docs/history/CHANGELOG.md/CLAUDE.md)
// — this file owns exactly one thing: what actually happens when a menu row
// fires (radio/WiFi/logger/backlight/SD calls), separate from drawing
// (ui_pages.cpp) and task lifecycle/input/loop (ui_task.cpp). See
// ui_task_shared.h for the state this file reads/mutates directly
// (activeBrightnessPercent, displayDimmed, idleTimeoutIndex) and ui_task.h
// for the subsystem's overall design.

#include "ui_task_shared.h"

#include <stdio.h>

#include "backlight.h"
#include "capture_settings.h"
#include "display_settings.h"
#include "logger_task.h"
#include "serial_control.h"
#include "profile_state.h"
#include "radio_task.h"
#include "region_settings.h"
#include "ui_labels.h"
#include "wifi_task.h"

// Performs the actual toggle/switch behind a fired MenuAction and confirms
// it via the toast layer — the same radio_task.h/wifi_task.h/logger_task.h
// calls Phase 3/4/5 already made, just no longer inlined into the menu's
// key-handling switch (see uiTask() in ui_task.cpp, where MenuState.handle()'s
// return value is routed here instead).
void fireMenuAction(MenuAction action) {
    char msg[48];
    switch (action) {
        case MenuAction::SELECT_MESHTASTIC:
        case MenuAction::SELECT_MESHCORE: {
            // Same one-loop-iteration-of-lag caveat Phase 4/5 already
            // documented: this queues the switch, it doesn't apply it —
            // the header/menu still show the outgoing profile until
            // radio_task's own loop picks the request up, typically the
            // next redraw tick. Direct target switch, not a cycle —
            // picking Meshtastic vs. MeshCore is a distinct selection
            // inside Profile's group, not "whichever one isn't active."
            const MissionProfile target = (action == MenuAction::SELECT_MESHTASTIC)
                                               ? MissionProfile::MESHTASTIC
                                               : MissionProfile::MESHCORE;
            radioRequestProfileSwitch(target);
            // So the radio boots back into this profile next power-on
            // instead of always defaulting to Meshtastic (profile_state.h).
            writeLastProfileToSD(target);
            snprintf(msg, sizeof(msg), "Profile: %s", uiProfileLabel(target));
            showToast(msg);
            break;
        }
        case MenuAction::WIFI_TOGGLE: {
            // wifiToggle() only flips a *requested* flag — wifiTask (Core
            // 0) hasn't actually called softAP()/softAPdisconnect() yet by
            // the time this function returns, so wifiIsEnabled() read
            // right after wifiToggle() still reports the OLD state (same
            // one-loop-iteration-of-lag radioRequestProfileSwitch() already
            // has). Reading the pre-toggle state and negating it — instead
            // of re-querying post-toggle — is what SELECT_MESHTASTIC/
            // SELECT_MESHCORE above already do correctly by reporting the
            // requested target rather than the not-yet-applied live state;
            // this case just hadn't followed that pattern. Bug: toasted
            // "WiFi OFF" on the request that turned it on, and vice versa
            // (caught on hardware, 2026-08-25).
            const bool turningOn = !wifiIsEnabled();
            wifiToggle();
            if (turningOn) {
                char ssid[32];
                wifiApSsid(ssid, sizeof(ssid));
                snprintf(msg, sizeof(msg), "WiFi ON: %s", ssid);
            } else {
                snprintf(msg, sizeof(msg), "WiFi OFF");
            }
            showToast(msg);
            break;
        }
        case MenuAction::DEBUG_TOGGLE:
            loggerDebugToggle();
            showToast(loggerDebugIsEnabled() ? "Debug ON" : "Debug OFF");
            break;
        case MenuAction::IDENTITY_CAPTURE_TOGGLE: {
            const bool next = !radioIdentityCaptureIsEnabled();
            radioIdentityCaptureSetEnabled(next);
            showToast(next ? "Identity capture ON" : "Identity capture OFF");
            break;
        }
        case MenuAction::SD_RETRY:
            if (loggerSdReady()) {
                showToast("SD: READY");
            } else if (loggerRequestSdRetry()) {
                showToast("SD retry queued");
            } else {
                showToast("SD: UNAVAILABLE");
            }
            break;
        case MenuAction::TRACE_TOGGLE: {
            // Same pre-toggle-state-then-negate pattern as WIFI_TOGGLE above
            // (and the same reason): radioRequestTracePause() only queues
            // the request, radioIsTracePaused() doesn't reflect it until
            // the radio task's own loop picks it up — re-querying right
            // after the request would show the state being left, not
            // entered, same bug already fixed once this session for WiFi.
            const bool pausing = !radioIsTracePaused();
            radioRequestTracePause(pausing);
            showToast(pausing ? "Trace: STANDBY" : "Trace: ACTIVE");
            break;
        }
        case MenuAction::PROBE_TOGGLE: {
            // Probe is a radio-task-owned bounded sweep. The request is
            // non-blocking; selecting it again while active asks that task
            // to cancel and restore Watch before it returns to RX.
            const bool cancelling = radioDiscoverySweepIsActive();
            if (!cancelling && !loggerSdReady()) {
                showToast("Probe: SD REQUIRED");
            } else if (radioRequestDiscoverySweep()) {
                showToast(cancelling ? "Probe: CANCEL" : "Probe: START");
                showProbeResults();
            } else {
                showToast("Probe: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::SWEEP_TOGGLE: {
            // Same non-blocking radio-task-owned shape as Probe above.
            const bool cancelling = radioEnergySweepIsActive();
            if (!cancelling && !loggerSdReady()) {
                showToast("Sweep: SD REQUIRED");
            } else if (radioRequestEnergySweep()) {
                showToast(cancelling ? "Sweep: CANCEL" : "Sweep: START");
                showSweepResults();
            } else {
                showToast("Sweep: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::SWEEP_REPEAT_TOGGLE: {
            // R key (operator request, 2026-08-29; moved off a Ctrl+S
            // chord to its own dedicated key 2026-08-30 after real
            // hardware testing showed the TCA8418 can drop Ctrl's release
            // event — see keyboard.h) — "walk around and scan" field mode,
            // distinct from SWEEP_TOGGLE's bounded single-shot check. Same
            // non-blocking radio-task-owned shape.
            const bool stopping = radioEnergySweepRepeatIsActive();
            if (!stopping && !loggerSdReady()) {
                showToast("Sweep: SD REQUIRED");
            } else if (radioRequestEnergySweepRepeat()) {
                showToast(stopping ? "Sweep: REPEAT OFF" : "Sweep: REPEAT ON");
                showSweepResults();
            } else {
                showToast("Sweep: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::WATERFALL_SWEEP_REPEAT_TOGGLE: {
            // Same radioRequestEnergySweepRepeat() call as SWEEP_REPEAT_TOGGLE
            // above, deliberately without its showSweepResults() — see
            // ui_menu.h's own comment on this action for why staying on
            // Waterfall, not jumping to the Sweep card, is the whole point.
            const bool stopping = radioEnergySweepRepeatIsActive();
            if (!stopping && !loggerSdReady()) {
                showToast("Sweep: SD REQUIRED");
            } else if (radioRequestEnergySweepRepeat()) {
                showToast(stopping ? "Sweep: REPEAT OFF" : "Sweep: REPEAT ON");
            } else {
                showToast("Sweep: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::CELL_TOGGLE: {
            // Same non-blocking radio-task-owned shape as Probe/Sweep above,
            // including the dedicated results card (Phase 11, 2026-09-01).
            const bool cancelling = radioCellSweepIsActive();
            if (!cancelling && !loggerSdReady()) {
                showToast("Cell: SD REQUIRED");
            } else if (radioRequestCellSweep()) {
                showToast(cancelling ? "Cell: CANCEL" : "Cell: START");
                showCellResults();
            } else {
                showToast("Cell: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::CELL_REPEAT_TOGGLE: {
            // R key, page-gated to the Cell card by ui_task.cpp — same
            // non-blocking radio-task-owned shape as SWEEP_REPEAT_TOGGLE
            // above, mirrored exactly.
            const bool stopping = radioCellSweepRepeatIsActive();
            if (!stopping && !loggerSdReady()) {
                showToast("Cell: SD REQUIRED");
            } else if (radioRequestCellSweepRepeat()) {
                showToast(stopping ? "Cell: REPEAT OFF" : "Cell: REPEAT ON");
                showCellResults();
            } else {
                showToast("Cell: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::SERIAL_CONTROL_TOGGLE: {
            const bool next = !serialControlIsEnabled();
            serialControlSetEnabled(next);
            showToast(next ? "Serial Control: ON" : "Serial Control: OFF");
            break;
        }
        case MenuAction::BRIGHTNESS_UP:
        case MenuAction::BRIGHTNESS_DOWN: {
            // Live-applied every step, like scrubbing any real slider —
            // but deliberately NOT saved to SD here. Saving on every step
            // would hammer the card if someone holds the key down; the
            // save happens once, on BACK out of the slider (see uiTask()
            // in ui_task.cpp), the same debounce point a "confirm" button
            // would give a form.
            int16_t next = (int16_t)activeBrightnessPercent +
                            (action == MenuAction::BRIGHTNESS_UP ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP);
            if (next < BRIGHTNESS_MIN) next = BRIGHTNESS_MIN;
            if (next > BRIGHTNESS_MAX) next = BRIGHTNESS_MAX;
            activeBrightnessPercent = (uint8_t)next;
            // Adjusting the slider always shows the live result
            // immediately, undimming if the display happened to be idle-
            // dimmed — the operator just acted on the keyboard, so it
            // can't still be "idle" the instant after this fires.
            displayDimmed = false;
            backlightSetPercent(activeBrightnessPercent);
            break; // no toast — the slider screen's own live "NN%" readout is the feedback
        }
        case MenuAction::SWEEP_MARGIN_UP:
        case MenuAction::SWEEP_MARGIN_DOWN: {
            // Same live-applied-but-not-saved-per-step shape as Brightness
            // above (save happens once, on BACK/SELECT out of the slider —
            // see ui_task.cpp's leavingSliderKind). Unlike Brightness this
            // has no local shadow variable: radio_task.cpp is the value's
            // one owner (it's the task that actually reads it, at each
            // Sweep bin's peak decision), so this reads/writes straight
            // through radioEnergySweepMarginDbmX10()/
            // radioSetEnergySweepMarginDbmX10() every step instead of
            // mirroring it into ui_task_shared.h state.
            int32_t next = (int32_t)radioEnergySweepMarginDbmX10() +
                            (action == MenuAction::SWEEP_MARGIN_UP ? ENERGY_SWEEP_MARGIN_STEP_DBM_X10
                                                                    : -ENERGY_SWEEP_MARGIN_STEP_DBM_X10);
            if (next < ENERGY_SWEEP_MARGIN_MIN_DBM_X10) next = ENERGY_SWEEP_MARGIN_MIN_DBM_X10;
            if (next > ENERGY_SWEEP_MARGIN_MAX_DBM_X10) next = ENERGY_SWEEP_MARGIN_MAX_DBM_X10;
            radioSetEnergySweepMarginDbmX10((int16_t)next);
            break; // no toast — the slider screen's own live "NN.NdB" readout is the feedback
        }
        case MenuAction::IDLE_TIMEOUT_CYCLE: {
            // Plain local state this file owns directly (not an async
            // cross-task flag like WiFi's apActive), so there's no
            // pre/post-toggle staleness risk reading it right after
            // advancing it.
            idleTimeoutIndex = (uint8_t)((idleTimeoutIndex + 1) % IDLE_TIMEOUT_OPTION_COUNT);
            snprintf(msg, sizeof(msg), "Idle dim: %s", IDLE_TIMEOUT_OPTIONS[idleTimeoutIndex].label);
            showToast(msg);
            // One write per press — a discrete, deliberate tap, not a
            // continuous scrub, so this doesn't need the slider's
            // save-on-exit debounce.
            DisplaySettings settings;
            settings.brightness_pct = activeBrightnessPercent;
            settings.idle_timeout_index = idleTimeoutIndex;
            writeDisplaySettingsToSD(settings);
            break;
        }
        case MenuAction::OPEN_PROBE:
            showProbeResults();
            break;
        case MenuAction::OPEN_SWEEP:
            showSweepResults();
            break;
        case MenuAction::OPEN_CELL:
            showCellResults();
            break;
        case MenuAction::OPEN_METER:
            showMeterPage();
            break;
        case MenuAction::OPEN_WATERFALL:
            showWaterfallPage();
            break;
        case MenuAction::OPEN_SCOPE:
            showScopePage();
            break;
        case MenuAction::OPEN_CAPTURES:
            showCapturesPage();
            break;
        case MenuAction::OPEN_NODES:
            showNodesPage();
            break;
        case MenuAction::SCOPE_TOGGLE: {
            // Same non-blocking radio-task-owned shape as Probe/Sweep/Cell
            // above, minus their SD-required gate: a capture never writes
            // to SD, only the in-RAM ScopeTrace (scope_trace.h).
            const bool cancelling = radioScopeAcquireIsActive();
            const ChannelParams ch = radioActiveChannel();
            const uint32_t freqKhz = (uint32_t)(ch.freq_mhz * 1000.0f + 0.5f);
            if (radioRequestScopeAcquire(freqKhz)) {
                showToast(cancelling ? "Scope: CANCEL" : "Scope: START");
            } else {
                showToast("Scope: UNAVAILABLE");
            }
            break;
        }
        case MenuAction::CAPTURE_WINDOW_CYCLE: {
            // Lives in radio_task.cpp (mirrors Region/Margin above), not
            // local ui_task state — the radio task is the one that reads
            // it, once per repeat lap.
            uint8_t index = CAPTURE_WINDOW_DEFAULT_INDEX;
            for (uint8_t i = 0; i < CAPTURE_WINDOW_OPTION_COUNT; i++) {
                if (CAPTURE_WINDOW_OPTIONS_MS[i] == radioEnergySweepHomeListenMs()) {
                    index = i;
                    break;
                }
            }
            index = (uint8_t)((index + 1) % CAPTURE_WINDOW_OPTION_COUNT);
            radioSetEnergySweepHomeListenMs(captureWindowMsForIndex(index));
            snprintf(msg, sizeof(msg), "Capture: %s", captureWindowLabelForIndex(index));
            showToast(msg);
            // One write per press — a discrete tap, not a scrub, so no
            // save-on-exit debounce needed (same as Idle dim/Region).
            CaptureSettings captureSettings;
            captureSettings.window_index = index;
            writeCaptureSettingsToSD(captureSettings);
            break;
        }
        case MenuAction::REGION_CYCLE: {
            // Region lives in radio_task.cpp (mirrors activeChannel/
            // activeProfile), not local ui_task state — Sweep is the one
            // that reads it, at the start of each sweep.
            const Region next =
                radioEnergySweepRegion() == Region::US ? Region::GLOBAL : Region::US;
            radioSetEnergySweepRegion(next);
            snprintf(msg, sizeof(msg), "Region: %s", regionLabel(next));
            showToast(msg);
            RegionSettings regionSettings;
            regionSettings.region = next;
            writeRegionSettingsToSD(regionSettings);
            break;
        }
        case MenuAction::NONE:
        default:
            break;
    }
}
