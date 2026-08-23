#pragma once
// LoRaTrace RX — WiFi AP + on-device web UI (Core 0, lowest priority).
//
// ROADMAP.md Phase 3: pull detections.csv/session.csv, view live device
// status, and edit the LoRa channel config over a browser — without ejecting
// the SD card. Deliberately the least latency-sensitive task in the system:
// Core 0, priority 1 (same tier as gps_task/ui_task), strictly below
// logger_task (2) and radio_task (3), which must always win. See
// PROGRESS.md for the heap-budget spike this was gated behind before being
// built (ROADMAP.md previously called WiFi "lowest priority" for exactly
// this reason).
//
// **Off by default, on-demand only.** The task is created at boot but does
// nothing — no WiFi.mode(), no AP, no server — until toggled. Starting the
// AP is the only thing here with a real RAM/CPU/RF-noise cost, so it never
// runs during an actual drive unless the operator explicitly asks for it
// (ui_task's long-press gesture — see ui_task.cpp). Toggling off tears the
// AP fully down (WIFI_OFF, not just "stop accepting connections"), so the
// cost actually goes away, not just goes idle.
//
// Settings saved from the web UI write to the same /loratrace/config.txt
// loadChannelConfigFromSD() already reads (config.h) and apply on the next
// boot — no attempt to hot-reload the running SX1262 from a different task.
// See config.h's writeChannelConfigToSD() for why.

#include <stddef.h>
#include <stdint.h>

// Starts the task on Core 0. AP is OFF until wifiToggle() is called.
// Returns false if the task could not be created.
bool wifiTaskStart();

// Flips AP+server on/off. Safe to call from any task (ui_task calls this on
// its long-press gesture) — the task itself picks up the request on its
// next loop iteration and does the actual start/stop.
void wifiToggle();

// True once the AP is actually up (not just requested — there's one loop
// iteration of lag while wifi_task does the real WiFi.softAP() call).
bool wifiIsEnabled();

// Connected station count, 0 whenever the AP is off.
uint8_t wifiClientCount();

// Formats this device's AP SSID into `buf` (device-unique, derived from the
// chip's efuse MAC so multiple units nearby don't collide). Safe to call
// before the AP has ever been started — it's pure formatting, no radio
// involved — so main.cpp's boot splash can show it without turning WiFi on.
void wifiApSsid(char *buf, size_t bufLen);

// The AP's default IP once started — ESP32's softAP default when no custom
// AP network config is applied (this code never calls WiFi.softAPConfig()).
constexpr const char *WIFI_AP_IP = "192.168.4.1";
