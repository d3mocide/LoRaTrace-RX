#include "ui_task.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "board_pins.h"
#include "detection.h"
#include "gps_task.h"
#include "keyboard.h"
#include "logger_task.h"
#include "radio_task.h"
#include "spi_bus.h"
#include "ui_labels.h"
#include "ui_menu.h"
#include "version.h"
#include "wifi_task.h"

namespace {

Arduino_GFX *tft = nullptr;

// TCA8418 keyboard controller. Cardputer ADV replaced the base Cardputer's
// GPIO matrix with this I2C part — same SDA/SCL as the IO expander, a
// different address, which is ordinary shared-bus operation.
//
// CRITICAL: the TCA8418 boots in SLEEP and reports nothing until explicitly
// configured, even when I2C is perfectly healthy. Exactly the failure shape
// as the GPS power rail (a working bus proving nothing about a working
// device). begin() + matrix() is the wake sequence, taken from
// bmorcelli/Launcher's confirmed-working Cardputer-ADV interface.
Adafruit_TCA8418 keys;
bool keyboardReady = false;

UiPage page = UiPage::RADIO;
uint32_t lastPageChange = 0;

// Phase 6: the grouped menu (ui_menu.h) replaces Phase 5's flat, fixed-size
// list — see ui_menu.h's own header comment for why. "Carousel mode" is now
// simply !menu.isOpen(), so this file no longer tracks a separate UiMode
// enum the way Phase 5 did; menu.isOpen()/menu.level() are the single
// source of truth for whether the carousel or the menu is on screen.
//
// Root table: "Profile" is a DIRECT row (cycles the mission profile
// immediately, same action Phase 3/4 built); "System" is a GROUP holding
// the WiFi and Debug toggles Phase 3/Phase-5-bench-day added. Two root rows
// is not a hardcoded ceiling — DISCOVERY_SWEEP (Phase 7) and ENERGY_SWEEP
// (Phase 8) each get to add their own row (or their own group) here without
// touching MenuState itself, which is the whole point of this redesign
// (ROADMAP.md Phase 6, PROGRESS.md 2026-08-25 Decisions log).
constexpr MenuEntry SYSTEM_GROUP_ITEMS[] = {
    {"WiFi", MenuAction::WIFI_TOGGLE},
    {"Debug", MenuAction::DEBUG_TOGGLE},
};
constexpr RootEntry ROOT_ITEMS[] = {
    {"Profile", RootKind::DIRECT, MenuAction::PROFILE_SWITCH, nullptr, 0},
    {"System", RootKind::GROUP, MenuAction::NONE, SYSTEM_GROUP_ITEMS, 2},
};
constexpr uint8_t ROOT_COUNT = 2;
MenuState menu(ROOT_ITEMS, ROOT_COUNT);

// Toast layer (Phase 6): a brief overlay message for feedback that isn't
// tied to whichever menu row happens to be highlighted — e.g. confirming a
// toggle fired right before BACK leaves the menu, or (Phase 7+) a sweep
// hit landing while some other carousel page is on screen. No canvas/
// framebuffer involved (this project draws direct-to-panel, unlike
// M5PORKCHOP's M5Canvas-based compositing — DESIGN.md §1's no-framebuffer
// rule stays intact): the only cost is this static buffer, not a dynamic
// allocation, so there's no heap number to gate behind the way WiFi's AP
// needed one.
char toastMsg[40] = {0};
uint32_t toastShownAt = 0;
constexpr uint32_t TOAST_DURATION_MS = 1400;

void showToast(const char *msg) {
    strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
    toastMsg[sizeof(toastMsg) - 1] = '\0';
    toastShownAt = millis();
}

bool toastActive() {
    return toastMsg[0] != '\0' && (millis() - toastShownAt) < TOAST_DURATION_MS;
}

// Without a keyboard the pages rotate on their own — a device stuck on one
// page during a multi-hour field test is worse than one that cycles.
constexpr uint32_t AUTO_ADVANCE_MS = 8000;
constexpr uint32_t REDRAW_MS = 1000;

// 240x135 at rotation 1. Text size 1 is 6x8px; size 2 is 12x16px.
constexpr int16_t HEADER_H = 12;
constexpr uint16_t COL_BG = 0x0000;     // black
constexpr uint16_t COL_FG = 0xFFFF;     // white
constexpr uint16_t COL_DIM = 0x8410;    // grey
constexpr uint16_t COL_GOOD = 0x07E0;   // green
constexpr uint16_t COL_WARN = 0xFFE0;   // yellow
constexpr uint16_t COL_BAD = 0xF800;    // red

const char *pageName(UiPage p) {
    switch (p) {
        case UiPage::RADIO: return "RADIO";
        case UiPage::CHANNEL: return "CHANNEL";
        case UiPage::GPS: return "GPS";
        case UiPage::SYSTEM: return "SYSTEM";
        case UiPage::WIFI: return "WIFI";
        default: return "?";
    }
}

// Battery glyph + percentage, top right. Drawn on every page so the number
// an operator most wants mid-field is never more than a glance away.
void drawBattery() {
    const uint32_t mv = batteryMilliVolts();
    const int16_t w = 22, h = 9;
    const int16_t x = tft->width() - w - 4;
    const int16_t y = 2;

    tft->fillRect(x - 30, y - 1, w + 34, h + 2, COL_BG);

    if (mv == 0) {
        // Unknown, not empty. Drawing 0% would imply a dying battery when
        // the truth is the ADC gave an implausible reading (USB-only, no
        // cell fitted, etc.).
        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(x - 28, y);
        tft->print("bat ?");
        return;
    }

    const uint8_t pct = batteryPercentFromMv(mv);
    const uint16_t colour = (pct >= 50) ? COL_GOOD : (pct >= 20 ? COL_WARN : COL_BAD);

    tft->setTextSize(1);
    tft->setTextColor(colour, COL_BG);
    tft->setCursor(x - 30, y);
    tft->print(pct);
    tft->print('%');

    tft->drawRect(x, y, w, h, colour);
    tft->fillRect(x + w, y + 2, 2, h - 4, colour); // terminal nub
    const int16_t fill = (int16_t)((w - 2) * pct / 100);
    if (fill > 0) tft->fillRect(x + 1, y + 1, fill, h - 2, colour);
}

// Blinking dot in the header. Carried over from the Phase 1 splash for the
// same reason it existed there: on a quiet channel every number on screen
// can sit unchanged for minutes, so a healthy idle device and a wedged one
// look identical without it. This ticks only from the UI task's own loop,
// so it freezes exactly when something is actually wrong.
void drawHeartbeat() {
    static bool on = false;
    on = !on;
    tft->fillCircle(tft->width() - 60, 6, 2, on ? COL_DIM : COL_BG);
}

void drawHeader() {
    tft->fillRect(0, 0, tft->width(), HEADER_H, COL_BG);
    tft->setTextSize(1);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, 2);

    if (menu.isOpen()) {
        tft->print("MENU");
        if (menu.level() == MenuLevel::GROUP) {
            // Breadcrumb, e.g. "MENU > System" — tells an operator which
            // group they're inside without needing to back out to check.
            tft->setTextColor(COL_DIM, COL_BG);
            tft->print(" > ");
            tft->print(menu.currentRoot().label);
        }
        // Profile name deliberately omitted here (unlike the carousel
        // branch below): BRAND.md's longer labels ("Spectrum Trace" vs.
        // the old "general") made the combined header text run close to
        // the battery indicator's left edge, and the active profile is
        // already visible on the root menu's own "Profile" row.
    } else {
        tft->print(pageName(page));
        // Page position, so it's obvious more pages exist.
        tft->setTextColor(COL_DIM, COL_BG);
        tft->print(" ");
        tft->print((uint8_t)page + 1);
        tft->print('/');
        tft->print((uint8_t)UiPage::COUNT);

        // Active mission profile, using BRAND.md's on-device label
        // (ui_labels.h) rather than the raw internal/logged identifier —
        // whatever's on screen, an operator needs this to interpret it.
        tft->print(' ');
        tft->print(uiProfileLabel(radioActiveProfile()));
    }

    drawBattery();
    drawHeartbeat();
    tft->drawFastHLine(0, HEADER_H, tft->width(), COL_DIM);
}

// A small "label above value" block, used for the secondary/context column
// every redesigned page below carries alongside its primary left-column
// numbers — Phase 6's answer to the old single-column layout's unused right
// half (PROGRESS.md 2026-08-25 Decisions log). One shared helper instead of
// each page inventing its own right-column formatting, since three-plus
// pages now do this.
void statBlock(int16_t x, int16_t y, const char *label, const char *value, uint16_t valueColour = COL_FG) {
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(x, y);
    tft->print(label);
    tft->setTextColor(valueColour, COL_BG);
    tft->setCursor(x, y + 9);
    tft->print(value);
}

void drawRadioPage() {
    const uint32_t drops = radioQueueDropCount() + loggerRowsDropped();
    char buf[16];

    // Left column — the three numbers an operator checks first.
    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print("rx ");
    tft->print(radioPacketCount());

    tft->setCursor(2, HEADER_H + 26);
    tft->print("log ");
    tft->print(loggerRowsWritten());

    // Drops are the number that decides whether the architecture is holding
    // up under real traffic, so they get colour rather than being buried.
    tft->setTextColor(drops == 0 ? COL_GOOD : COL_BAD, COL_BG);
    tft->setCursor(2, HEADER_H + 46);
    tft->print("drop ");
    tft->print(drops);

    // Right column — health/context detail, using the width the old
    // single-column layout left blank (PROGRESS.md 2026-08-25).
    constexpr int16_t RIGHT_X = 132;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)radioCrcErrorCount());
    statBlock(RIGHT_X, HEADER_H + 4, "crc", buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)radioBusMissCount());
    statBlock(RIGHT_X, HEADER_H + 26, "miss", buf);
    statBlock(RIGHT_X, HEADER_H + 48, "sd", loggerSdReady() ? "ok" : "DOWN",
              loggerSdReady() ? COL_FG : COL_BAD);
    snprintf(buf, sizeof(buf), "r%u", (unsigned)loggerRunIndex());
    statBlock(RIGHT_X, HEADER_H + 70, "run", buf);

    // Bottom band, full width — flush stats matter for judging whether
    // BATCH_BUF_SIZE needs retuning (DESIGN.md §8.2), not for a quick
    // glance, so they sit below both columns rather than competing with
    // either for attention.
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 94);
    tft->print("flush ");
    tft->print(loggerFlushCount());
    tft->print("  max ");
    tft->print(loggerMaxFlushMs());
    tft->print("ms");
}

// Read-only RF detail behind RADIO's counters — added Phase 5 alongside the
// menu's live profile switch, so an operator has an on-device way to
// confirm a switch actually retuned the radio (radioActiveChannel() already
// reflects it correctly post-switch, Phase 4) rather than trusting the
// header text alone. Previously this was visible only over Serial or the
// web UI's /api/config.
void drawChannelPage() {
    const ChannelParams ch = radioActiveChannel();

    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print(ch.freq_mhz, 3);
    tft->print(" MHz");

    tft->setCursor(2, HEADER_H + 28);
    tft->print("SF");
    tft->print(ch.sf);
    tft->print(" BW");
    tft->print(ch.bw_khz, 1);

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 52);
    tft->print("CR4/");
    tft->print(ch.cr_denom);
    tft->print("  sync 0x");
    tft->print(ch.sync_word, HEX);

    // Right column — Phase 6: the radio-mode label (BRAND.md's "Watch" for
    // HOME_LISTEN), the only one of the three mode labels with anything to
    // name until Phase 7 (Probe) / Phase 8 (Sweep) add the other two radio
    // states this same slot will grow into.
    statBlock(150, HEADER_H + 6, "mode", uiModeLabelWatch());
}

void drawGpsPage() {
    GpsFix fix;
    const bool have = gpsGetFix(fix, pdMS_TO_TICKS(100));

    tft->setTextSize(2);
    if (have && fix.has_position) {
        tft->setTextColor(COL_GOOD, COL_BG);
        tft->setCursor(2, HEADER_H + 6);
        tft->print(fix.fix_type >= 3 ? "3D FIX" : "2D FIX");

        tft->setTextColor(COL_FG, COL_BG);
        tft->setCursor(2, HEADER_H + 28);
        tft->print(fix.lat, 5);
        tft->setCursor(2, HEADER_H + 48);
        tft->print(fix.lon, 5);

        // Right column — satellites USED, a real gap the old layout had:
        // it showed sats-in-view before a fix (the leading indicator) but
        // never showed the used count once a fix actually landed.
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)fix.satellites);
        statBlock(170, HEADER_H + 6, "sats", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)fix.fix_quality);
        statBlock(170, HEADER_H + 28, "qual", buf);
    } else {
        // Before a fix, sats-IN-VIEW is the number that matters: it says
        // whether the antenna can see sky at all, minutes before a fix
        // lands. `satellites` (used) stays 0 until then and would look
        // identical whether the antenna were working or disconnected.
        tft->setTextColor(fix.sats_in_view > 0 ? COL_WARN : COL_BAD, COL_BG);
        tft->setCursor(2, HEADER_H + 6);
        tft->print("NO FIX");

        tft->setTextColor(COL_FG, COL_BG);
        tft->setCursor(2, HEADER_H + 30);
        tft->print("view ");
        tft->print(fix.sats_in_view);

        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(2, HEADER_H + 52);
        tft->print(fix.sats_in_view > 0 ? "acquiring, keep still" : "no sky - go outside");
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 68);
    for (uint8_t i = 0; i < fix.talker_count && i < 5; i++) {
        tft->print(fix.talkers[i].id);
        tft->print(':');
        tft->print(fix.talkers[i].in_view);
        tft->print(' ');
    }

    tft->setCursor(2, HEADER_H + 80);
    if (have && fix.has_time) {
        char ts[24];
        detectionFormatTimestamp(ts, sizeof(ts), true, fix.year, fix.month, fix.day, fix.hour,
                                 fix.minute, fix.second);
        tft->print(ts);
    } else {
        tft->print("nmea ");
        tft->print(gpsSentenceCount());
        tft->print(" crc ");
        tft->print(gpsChecksumErrorCount());
    }
}

void drawSystemPage() {
    char buf[16];

    tft->setTextSize(2);
    tft->setTextColor(COL_FG, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print(millis() / 60000);
    tft->print(" min");

    const uint32_t heap = ESP.getFreeHeap();
    tft->setTextColor(heap > 100000 ? COL_GOOD : COL_WARN, COL_BG);
    tft->setCursor(2, HEADER_H + 28);
    tft->print(heap / 1024);
    tft->print("k heap");

    // Right column — Phase 6: the low-water heap, battery voltage, and bus
    // contention count moved here from the old cramped bottom-of-page
    // stack of size-1 lines, using the width that stack never touched.
    snprintf(buf, sizeof(buf), "%luk", (unsigned long)(ESP.getMinFreeHeap() / 1024));
    statBlock(140, HEADER_H + 6, "min heap", buf);

    const uint32_t mv = batteryMilliVolts();
    if (mv == 0) {
        statBlock(140, HEADER_H + 28, "batt", "unknown");
    } else {
        snprintf(buf, sizeof(buf), "%.2fV", mv / 1000.0);
        statBlock(140, HEADER_H + 28, "batt", buf);
    }

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)spiBusContentionCount());
    statBlock(140, HEADER_H + 50, "bus", buf);

    // Bottom band, full width — lower-priority context: keyboard presence,
    // health-row count (confirms the session log is actually being written
    // before driving off with the lid shut), firmware version.
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 66);
    tft->print("keys ");
    tft->print(keyboardReady ? "tca8418" : "none (auto)");
    tft->print("  health ");
    tft->print(loggerSessionRows());

    tft->setCursor(2, HEADER_H + 78);
    tft->print(FIRMWARE_VERSION);
}

void drawWifiPage() {
    const bool on = wifiIsEnabled();

    tft->setTextSize(2);
    tft->setTextColor(on ? COL_GOOD : COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 6);
    tft->print(on ? "AP ON" : "AP OFF");

    tft->setTextSize(1);
    tft->setTextColor(COL_FG, COL_BG);
    char ssid[32];
    wifiApSsid(ssid, sizeof(ssid));
    if (on) {
        tft->setCursor(2, HEADER_H + 30);
        tft->print(ssid);

        // Right column — Phase 6: IP and client count moved out of the old
        // single left column into their own block, next to the SSID rather
        // than stacked under it.
        statBlock(150, HEADER_H + 6, "ip", WIFI_AP_IP);
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)wifiClientCount());
        statBlock(150, HEADER_H + 28, "clients", buf);
    } else {
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(2, HEADER_H + 30);
        tft->print("would be:");
        tft->setCursor(2, HEADER_H + 42);
        tft->print(ssid);
    }
}

// One row of a menu list (root or group), selected or not. Selection is
// carried by inverting FG/BG on that row's whole width — same
// colour-carries-state convention drawBattery()/drawRadioPage() already
// use, just applied to a full row instead of a single value. The label/
// value separator lives here, not baked into each label string as a
// trailing space (Phase 5's convention) — a table entry forgetting the
// space is an easy, silent mistake this removes entirely.
void drawMenuRow(int16_t y, const char *rowLabel, const char *value, bool selected) {
    const uint16_t fg = selected ? COL_BG : COL_FG;
    const uint16_t bg = selected ? COL_FG : COL_BG;
    tft->fillRect(0, y - 3, tft->width(), 20, bg);
    tft->setTextSize(2);
    tft->setTextColor(fg, bg);
    tft->setCursor(4, y);
    tft->print(rowLabel);
    if (value != nullptr && value[0] != '\0') {
        tft->print(' ');
        tft->print(value);
    }
}

const char *menuEntryValue(MenuAction action) {
    switch (action) {
        case MenuAction::WIFI_TOGGLE: return wifiIsEnabled() ? "ON" : "OFF";
        case MenuAction::DEBUG_TOGGLE: return loggerDebugIsEnabled() ? "ON" : "OFF";
        default: return "";
    }
}

void drawMenuRoot() {
    for (uint8_t i = 0; i < ROOT_COUNT; i++) {
        const RootEntry &r = ROOT_ITEMS[i];
        const char *value = "";
        if (r.kind == RootKind::DIRECT && r.directAction == MenuAction::PROFILE_SWITCH) {
            value = uiProfileLabel(radioActiveProfile());
        } else if (r.kind == RootKind::GROUP) {
            value = ">"; // affordance: this row opens a sub-list
        }
        drawMenuRow(HEADER_H + 10 + i * 24, r.label, value, menu.rootIndex() == i);
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter select   ` back");
}

void drawMenuGroup() {
    const RootEntry &r = menu.currentRoot();
    for (uint8_t i = 0; i < r.groupCount; i++) {
        const MenuEntry &e = r.groupItems[i];
        drawMenuRow(HEADER_H + 10 + i * 24, e.label, menuEntryValue(e.action), menu.groupIndex() == i);
    }

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter act   ` back");
}

// Toast overlay — drawn last, on top of whatever page/menu is showing, and
// only while active(). Placed just above the persistent footer hint rather
// than centred, so it never covers a page's primary values.
void drawToast() {
    if (!toastActive()) return;
    const int16_t h = 16;
    const int16_t y = tft->height() - HEADER_H - h - 2;
    tft->fillRect(0, y, tft->width(), h, COL_FG);
    tft->setTextSize(1);
    tft->setTextColor(COL_BG, COL_FG);
    tft->setCursor(4, y + 4);
    tft->print(toastMsg);
}

void drawPage() {
    tft->fillRect(0, HEADER_H + 1, tft->width(), tft->height() - HEADER_H - 1, COL_BG);

    if (menu.level() == MenuLevel::ROOT) {
        drawMenuRoot();
    } else if (menu.level() == MenuLevel::GROUP) {
        drawMenuGroup();
    } else {
        switch (page) {
            case UiPage::RADIO: drawRadioPage(); break;
            case UiPage::CHANNEL: drawChannelPage(); break;
            case UiPage::GPS: drawGpsPage(); break;
            case UiPage::SYSTEM: drawSystemPage(); break;
            case UiPage::WIFI: drawWifiPage(); break;
            default: break;
        }

        // Persistent, page-independent hint line. "1-5 jump" matches the
        // digit-key shortcuts, still well under the ~40-char/line budget at
        // text size 1 on a 240px-wide panel. "` menu" (not "Enter menu")
        // since the 2026-08-24 bench pass moved menu-open onto ESC/backtick
        // — see the carousel branch in uiTask() below.
        tft->setTextSize(1);
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(2, tft->height() - 9);
        tft->print(",/. page  1-5 jump  ` menu");
    }

    // Drawn last regardless of branch, so a toast (e.g. a toggle just fired
    // from inside the menu) is visible immediately rather than only after
    // backing out to the carousel.
    drawToast();
}

void nextPage() {
    page = (UiPage)(((uint8_t)page + 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
}

void prevPage() {
    page = (UiPage)(((uint8_t)page + (uint8_t)UiPage::COUNT - 1) % (uint8_t)UiPage::COUNT);
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
}

void jumpToPage(UiPage p) {
    page = p;
    lastPageChange = millis();
    tft->fillScreen(COL_BG);
}

// Performs the actual toggle/switch behind a fired MenuAction and confirms
// it via the toast layer — the same radio_task.h/wifi_task.h/logger_task.h
// calls Phase 3/4/5 already made, just no longer inlined into the menu's
// key-handling switch (see uiTask() below, where MenuState.handle()'s
// return value is routed here instead).
void fireMenuAction(MenuAction action) {
    char msg[40];
    switch (action) {
        case MenuAction::PROFILE_SWITCH: {
            // Same one-loop-iteration-of-lag caveat Phase 4/5 already
            // documented: this queues the switch, it doesn't apply it — the
            // header/menu still show the outgoing profile until
            // radio_task's own loop picks the request up, typically the
            // next redraw tick.
            const MissionProfile next = nextHomeListenProfile(radioActiveProfile());
            radioRequestProfileSwitch(next);
            snprintf(msg, sizeof(msg), "Profile: %s", uiProfileLabel(next));
            showToast(msg);
            break;
        }
        case MenuAction::WIFI_TOGGLE:
            wifiToggle();
            showToast(wifiIsEnabled() ? "WiFi ON" : "WiFi OFF");
            break;
        case MenuAction::DEBUG_TOGGLE:
            loggerDebugToggle();
            showToast(loggerDebugIsEnabled() ? "Debug ON" : "Debug OFF");
            break;
        case MenuAction::NONE:
        default:
            break;
    }
}

// Drains the TCA8418 event FIFO and returns the most recently recognized
// KeyAction this poll (keyboard.h), or NONE. Several actions queued between
// polls collapse to the last one — an accepted imprecision at a 30ms poll
// interval for sparse, deliberate keypresses, the same tolerance the old
// gesture code documented for simultaneous keys.
KeyAction pollKeyAction() {
    if (!keyboardReady) return KeyAction::NONE;
    KeyAction result = KeyAction::NONE;
    while (keys.available() > 0) {
        const KeyAction a = keyboardDecodeEvent((uint8_t)keys.getEvent());
        if (a != KeyAction::NONE) result = a;
    }
    return result;
}

void uiTask(void *) {
    tft->fillScreen(COL_BG);
    drawHeader();
    drawPage();

    uint32_t lastRedraw = millis();
    lastPageChange = lastRedraw;

    for (;;) {
        const KeyAction action = pollKeyAction();
        bool redraw = false;

        if (!menu.isOpen()) {
            // Carousel: page navigation is this file's own concern, not
            // MenuState's (ui_menu.h stays free of any UiPage dependency).
            if (action == KeyAction::PREV) {
                prevPage();
                redraw = true;
            } else if (action == KeyAction::NEXT) {
                nextPage();
                redraw = true;
            } else if (action == KeyAction::BACK) {
                menu.open();
                redraw = true;
            } else if (action == KeyAction::JUMP_1) {
                jumpToPage(UiPage::RADIO);
                redraw = true;
            } else if (action == KeyAction::JUMP_2) {
                jumpToPage(UiPage::CHANNEL);
                redraw = true;
            } else if (action == KeyAction::JUMP_3) {
                jumpToPage(UiPage::GPS);
                redraw = true;
            } else if (action == KeyAction::JUMP_4) {
                jumpToPage(UiPage::SYSTEM);
                redraw = true;
            } else if (action == KeyAction::JUMP_5) {
                jumpToPage(UiPage::WIFI);
                redraw = true;
            }
            // SELECT (Enter) is a no-op here — bench feedback 2026-08-24
            // found opening the menu with Enter felt wrong, since Enter's
            // role inside the menu is committing the highlighted change;
            // ESC (BACK) opens the menu instead, so the same key that
            // closes it also opens it.
        } else if (action != KeyAction::NONE) {
            // Menu open (root or group level) — MenuState owns navigation
            // and level transitions; this file only reacts to what fired.
            const MenuAction fired = menu.handle(action);
            if (fired != MenuAction::NONE) fireMenuAction(fired);
            redraw = true;
        }

        if (redraw) {
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (!menu.isOpen() && !keyboardReady && millis() - lastPageChange >= AUTO_ADVANCE_MS) {
            nextPage();
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (millis() - lastRedraw >= REDRAW_MS) {
            drawHeader(); // battery + page indicator
            drawPage();
            lastRedraw = millis();
        } else if (toastMsg[0] != '\0' && !toastActive()) {
            // Toast just expired since the last redraw — force one more
            // pass so its overlay actually clears rather than lingering
            // until the next periodic REDRAW_MS tick (up to ~1s stale).
            toastMsg[0] = '\0';
            drawHeader();
            drawPage();
            lastRedraw = millis();
        }

        // Poll rather than use the INT pin on GPIO11. At 30ms a keypress
        // feels immediate, and polling keeps this task free of an ISR that
        // would need its own I2C access — I2C is not interrupt-safe.
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

} // namespace

bool uiTaskStart(Arduino_GFX *gfx) {
    if (gfx == nullptr) return false;
    tft = gfx;

    // Wire is already up from ioExpanderInit(); begin() again is harmless
    // and keeps this call self-contained if the boot order ever changes.
    Wire.begin(PIN_IOEXP_SDA, PIN_IOEXP_SCL);
    keyboardReady = keys.begin(KEYBOARD_I2C_ADDR, &Wire);
    if (keyboardReady) {
        keys.matrix(KEYBOARD_MATRIX_ROWS, KEYBOARD_MATRIX_COLS);
        keys.flush(); // discard boot-time noise
    }

    BaseType_t ok = xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 0);
    return ok == pdPASS;
}

bool uiKeyboardReady() {
    return keyboardReady;
}
