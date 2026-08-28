// LoRaTrace RX — ui_task's menu-action business logic.
//
// Split out of ui_task.cpp (2026-08-25 cleanup pass, PROGRESS.md/CLAUDE.md)
// — this file owns exactly one thing: what actually happens when a menu row
// fires (radio/WiFi/logger/backlight/SD calls), separate from drawing
// (ui_pages.cpp) and task lifecycle/input/loop (ui_task.cpp). See
// ui_task_shared.h for the state this file reads/mutates directly
// (activeBrightnessPercent, displayDimmed, idleTimeoutIndex) and ui_task.h
// for the subsystem's overall design.

#include "ui_task_shared.h"

#include <stdio.h>

#include "backlight.h"
#include "display_settings.h"
#include "logger_task.h"
#include "serial_control.h"
#include "profile_state.h"
#include "radio_task.h"
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
            // Same non-blocking radio-task-owned shape as Probe above. No
            // dedicated result page yet (Phase 9 slice 2) — the toast plus
            // ui_task.cpp's async-completion toast are the only feedback.
            const bool cancelling = radioEnergySweepIsActive();
            if (!cancelling && !loggerSdReady()) {
                showToast("Sweep: SD REQUIRED");
            } else if (radioRequestEnergySweep()) {
                showToast(cancelling ? "Sweep: CANCEL" : "Sweep: START");
            } else {
                showToast("Sweep: UNAVAILABLE");
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
        case MenuAction::NONE:
        default:
            break;
    }
}
