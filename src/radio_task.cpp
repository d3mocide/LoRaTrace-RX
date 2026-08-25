#include "radio_task.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/task.h>

#include "board_pins.h"
#include "spi_bus.h"

namespace {

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_IRQ, PIN_LORA_RST, PIN_LORA_BUSY, sharedSpi());

TaskHandle_t radioTaskHandle = nullptr;
QueueHandle_t detectionQueue = nullptr;
ChannelParams activeChannel;
MissionProfile activeProfile = MissionProfile::MESHTASTIC;
// Per-profile SD/web overrides, copied in once at radioTaskStart() and
// otherwise read-only — every radioRequestProfileSwitch() resolves against
// this same copy, which is what keeps a switch from reverting to the
// hardcoded default (channel_plans.h) the way the pre-2026-08-24 design did.
ProfileOverrides activeOverrides;

// One-slot mailbox for radioRequestProfileSwitch(): xQueueOverwrite always
// succeeds and always leaves only the most recent request in place, so a
// caller firing this twice in a row (a bouncy key) can't queue up a
// switch-then-switch-back — the radio task only ever sees the latest ask.
struct PendingSwitch {
    MissionProfile profile;
    ChannelParams channel;
};
QueueHandle_t profileSwitchQueue = nullptr;

// Same one-slot-mailbox pattern as profileSwitchQueue, for Trace pause/
// standby (radioRequestTracePause() below). tracePaused mirrors what the
// radio task's own loop has actually done (radio.sleep()/startReceive()),
// not just what was requested — same "reflects reality, not the ask"
// convention activeProfile already follows for profile switches.
QueueHandle_t pauseQueue = nullptr;
volatile bool tracePaused = false;

int lastError = RADIOLIB_ERR_NONE;

volatile uint32_t packetCount = 0;
volatile uint32_t crcErrorCount = 0;
volatile uint32_t queueDropCount = 0;
volatile uint32_t busMissCount = 0;

// How long the radio task will wait for the shared SPI bus. Generous enough
// to ride out a normal SD flush, short enough that a wedged logger can't
// take the receiver down with it. On timeout we drop this packet and keep
// listening — see the header for why that ordering is deliberate.
constexpr TickType_t BUS_WAIT = pdMS_TO_TICKS(250);

// DIO1 fires on RX-done. Keep this to a notification and nothing else: no
// SPI, no Serial, no allocation. RadioLib requires the ISR be IRAM-safe.
void IRAM_ATTR onDio1Action() {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (radioTaskHandle != nullptr) {
        vTaskNotifyGiveFromISR(radioTaskHandle, &higherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void radioTask(void *) {
    uint8_t buf[256];
    bool paused = false;

    for (;;) {
        // Block until DIO1 says a packet landed. The timeout is a liveness
        // safety net, not an expected path: if an interrupt is ever missed,
        // this re-checks and re-arms rather than deafening the receiver
        // permanently. Skipped entirely while paused — re-arming RX here
        // would silently undo radio.sleep() every 5 seconds, and a sleeping
        // SX1262 can't miss a DIO1 it's incapable of firing in the first
        // place.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            if (!paused) {
                SpiBusLock lock(BUS_WAIT);
                if (lock.held()) radio.startReceive();
            }
            continue;
        }

        // The same notification wakes this task for three different
        // reasons: a real DIO1 packet IRQ, a profile-switch request
        // (radioRequestProfileSwitch), or a Trace pause/resume request
        // (radioRequestTracePause). Check both mailboxes first —
        // non-blocking, so a genuine packet arriving at the same instant is
        // never held up behind either. Nothing queued in either means this
        // really was a packet, and falls through unchanged below.
        PendingSwitch swreq;
        if (profileSwitchQueue != nullptr && xQueueReceive(profileSwitchQueue, &swreq, 0) == pdTRUE) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) {
                // Reconfiguring the modem abandons anything mid-flight on
                // the old channel — the accepted cost of DESIGN.md §5's
                // "mutually exclusive": a requested switch means the
                // operator no longer wants the old profile, not "also don't
                // lose this one packet."
                lastError = radio.begin(swreq.channel.freq_mhz, swreq.channel.bw_khz,
                                        swreq.channel.sf, swreq.channel.cr_denom,
                                        swreq.channel.sync_word);
                if (lastError == RADIOLIB_ERR_NONE) {
                    activeChannel = swreq.channel;
                    activeProfile = swreq.profile;
                }
                // A profile switch always means "go back to listening" —
                // even if a pause was in effect, picking a different
                // protocol is an active operator choice that should be
                // heard, not silently swallowed by a stale pause. Matches
                // resolving the ambiguity in favor of what the operator
                // asked for last.
                paused = false;
                tracePaused = false;
                radio.startReceive();
            }
            continue;
        }

        bool pauseReq;
        if (pauseQueue != nullptr && xQueueReceive(pauseQueue, &pauseReq, 0) == pdTRUE) {
            SpiBusLock lock(BUS_WAIT);
            if (lock.held()) {
                if (pauseReq) {
                    radio.sleep(true); // warm sleep — retains config, cheap to resume
                } else {
                    radio.startReceive(); // wakes a warm-sleeping SX126x automatically
                }
                paused = pauseReq;
                tracePaused = pauseReq;
            }
            continue;
        }

        if (paused) {
            // Nothing else to do while asleep — DIO1 can't fire, and the
            // liveness branch above already skips re-arming. Avoid falling
            // through to a getPacketLength()/readData() pass against a
            // sleeping chip.
            continue;
        }

        Detection det = {};
        bool haveDetection = false;

        {
            // Everything touching the SX1262 happens inside this one short
            // critical section. Order matters and mirrors the Phase 1 fix:
            // read the stats, then re-arm RX *before* doing anything slow,
            // because the chip is deaf between DIO1 firing and startReceive().
            SpiBusLock lock(BUS_WAIT);
            if (!lock.held()) {
                busMissCount++;
                continue;
            }

            size_t len = radio.getPacketLength();
            int state = RADIOLIB_ERR_NONE;
            if (len > 0 && len <= sizeof(buf)) {
                state = radio.readData(buf, len);
            } else {
                state = RADIOLIB_ERR_UNKNOWN;
            }

            float rssi = 0.0f, snr = 0.0f;
            if (state == RADIOLIB_ERR_NONE) {
                // Must be read before re-arming: GetPacketStatus reports the
                // *last* packet, so a new arrival would overwrite these.
                rssi = radio.getRSSI();
                snr = radio.getSNR();
            }

            radio.startReceive();

            if (state == RADIOLIB_ERR_NONE) {
                det.rx_millis = millis();
                det.freq_mhz = activeChannel.freq_mhz;
                det.rssi_dbm = rssi;
                det.snr_db = snr;
                det.raw_len = (uint16_t)len;
                det.bw_khz_x10 = (uint16_t)(activeChannel.bw_khz * 10.0f + 0.5f);
                det.sf = activeChannel.sf;
                det.cr_denom = activeChannel.cr_denom;
                det.sync_word = activeChannel.sync_word;
                det.profile = (uint8_t)activeProfile;

                // Header fields are protocol-specific. HOME_LISTEN knows
                // which protocol it's locked to, so parse accordingly rather
                // than guessing from the bytes (that's §6 fingerprinting,
                // phase 5). Meshtastic's 16-byte to/from/id/flags layout is
                // verified (detection.h); MeshCore's is not — its
                // encryption/PSK model is still open (DESIGN.md §7) and
                // CLAUDE.md's house rule is not to assume it mirrors
                // Meshtastic's. So a MeshCore detection logs RSSI/SF/BW/
                // timing and the profile tag, exactly what ROADMAP.md Phase
                // 4 promises ("basic detection" without payload decode), but
                // leaves node_id/packet_id/etc. zeroed rather than parsing
                // Meshtastic's byte layout against bytes that aren't
                // Meshtastic's — that would produce numbers that look valid
                // but mean nothing. A runt Meshtastic frame also leaves
                // these zeroed instead of publishing garbage node ids.
                if (activeProfile == MissionProfile::MESHTASTIC) {
                    detectionApplyMeshtasticHeader(det, buf, len);
                }

                packetCount++;
                haveDetection = true;
            } else {
                crcErrorCount++;
            }
        } // bus released here, before any queue work

        if (haveDetection && detectionQueue != nullptr) {
            // Non-blocking by design: never stall the receiver for the log.
            if (xQueueSend(detectionQueue, &det, 0) != pdTRUE) {
                queueDropCount++;
            }
        }
    }
}

} // namespace

bool radioTaskStart(const ChannelParams &channel, MissionProfile profile,
                    const ProfileOverrides &overrides, QueueHandle_t queue) {
    activeChannel = channel;
    activeProfile = profile;
    activeOverrides = overrides;
    detectionQueue = queue;

    // Depth-1 mailbox for radioRequestProfileSwitch(). Created here (not
    // lazily) so a switch request arriving right after boot can never race
    // an as-yet-nonexistent queue.
    profileSwitchQueue = xQueueCreate(1, sizeof(PendingSwitch));
    if (profileSwitchQueue == nullptr) return false;

    pauseQueue = xQueueCreate(1, sizeof(bool));
    if (pauseQueue == nullptr) return false;

    {
        SpiBusLock lock(portMAX_DELAY);
        if (!lock.held()) return false;

        lastError = radio.begin(channel.freq_mhz, channel.bw_khz, channel.sf, channel.cr_denom,
                                channel.sync_word);
        if (lastError != RADIOLIB_ERR_NONE) return false;
    }

    // Create the task before wiring the ISR: onDio1Action dereferences
    // radioTaskHandle, and an interrupt arriving between the two would
    // otherwise find it null (harmless here, but the ordering makes the
    // dependency explicit rather than accidental).
    BaseType_t ok =
        xTaskCreatePinnedToCore(radioTask, "radio", 4096, nullptr, 3, &radioTaskHandle, 1);
    if (ok != pdPASS) return false;

    SpiBusLock lock(portMAX_DELAY);
    if (!lock.held()) return false;
    radio.setDio1Action(onDio1Action);
    lastError = radio.startReceive();
    return lastError == RADIOLIB_ERR_NONE;
}

int radioLastError() {
    return lastError;
}

uint32_t radioPacketCount() {
    return packetCount;
}
uint32_t radioCrcErrorCount() {
    return crcErrorCount;
}
uint32_t radioQueueDropCount() {
    return queueDropCount;
}
uint32_t radioBusMissCount() {
    return busMissCount;
}

ChannelParams radioActiveChannel() {
    return activeChannel; // small POD struct, cheap to return by value
}

MissionProfile radioActiveProfile() {
    return activeProfile; // same small-POD, no-lock convention as above
}

ProfileOverrides radioActiveOverrides() {
    return activeOverrides; // same small-POD, no-lock convention as above
}

bool radioRequestProfileSwitch(MissionProfile profile) {
    if (profileSwitchQueue == nullptr || radioTaskHandle == nullptr) return false;
    // resolvedChannelForProfile(), not channelParamsForProfile() directly —
    // this is the fix for the pre-2026-08-24 bug where switching to a
    // profile always used its hardcoded table, silently dropping whatever
    // SD/web override had been loaded for it at boot.
    PendingSwitch req{profile, resolvedChannelForProfile(activeOverrides, profile)};
    xQueueOverwrite(profileSwitchQueue, &req);
    // Wakes the radio task immediately even if it's parked in the 5s
    // liveness wait in radioTask()'s ulTaskNotifyTake — without this the
    // switch would sit in the mailbox for up to 5 seconds before being
    // noticed.
    xTaskNotifyGive(radioTaskHandle);
    return true;
}

bool radioRequestTracePause(bool paused) {
    if (pauseQueue == nullptr || radioTaskHandle == nullptr) return false;
    xQueueOverwrite(pauseQueue, &paused);
    xTaskNotifyGive(radioTaskHandle); // same immediate-wake reason as above
    return true;
}

bool radioIsTracePaused() {
    return tracePaused;
}
