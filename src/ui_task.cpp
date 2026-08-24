#include "ui_task.h"

#include <Adafruit_TCA8418.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "battery.h"
#include "board_pins.h"
#include "detection.h"
#include "gps_task.h"
#include "keyboard.h"
#include "logger_task.h"
#include "radio_task.h"
#include "spi_bus.h"
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

// Carousel = the read-only status pages, cycled by ','/'.'. Menu = the
// action list opened by Enter (Phase 5) — see keyboard.h for why only these
// four keys exist as input at all.
enum class UiMode : uint8_t { CAROUSEL, MENU };
UiMode mode = UiMode::CAROUSEL;

// The menu has exactly two items this phase (PROGRESS.md/DESIGN.md Phase 5
// scope: toggles only, no numeric settings editing) — profile switch and
// WiFi toggle, both already-built Phase 3/4 actions. Index into that fixed
// pair, not a general list.
constexpr int8_t MENU_ITEM_COUNT = 3;
int8_t menuSelection = 0;

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

    if (mode == UiMode::MENU) {
        tft->print("MENU");
    } else {
        tft->print(pageName(page));
        // Page position, so it's obvious more pages exist.
        tft->setTextColor(COL_DIM, COL_BG);
        tft->print(" ");
        tft->print((uint8_t)page + 1);
        tft->print('/');
        tft->print((uint8_t)UiPage::COUNT);
    }

    // Active mission profile, on every page/mode: whatever's on screen, an
    // operator needs this to interpret it — a GPS fix means something
    // different mid-MeshCore-run than mid-Meshtastic-run only insofar as
    // the operator remembers which one is live right now, and it's one of
    // the two things the menu itself can change.
    tft->setTextColor(COL_DIM, COL_BG);
    tft->print(' ');
    tft->print(missionProfileName((uint8_t)radioActiveProfile()));

    drawBattery();
    drawHeartbeat();
    tft->drawFastHLine(0, HEADER_H, tft->width(), COL_DIM);
}

void label(int16_t y, const char *text) {
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, y);
    tft->print(text);
}

void drawRadioPage() {
    const uint32_t drops = radioQueueDropCount() + loggerRowsDropped();

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

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 70);
    tft->print("crcerr ");
    tft->print(radioCrcErrorCount());
    tft->print("  busmiss ");
    tft->print(radioBusMissCount());

    tft->setCursor(2, HEADER_H + 82);
    tft->print("sd ");
    tft->print(loggerSdReady() ? "ok" : "DOWN");
    // Which run this drive is being recorded as. An operator about to set
    // off wants to know the folder their data is landing in, and it is the
    // one thing on this page they cannot infer from anything else.
    tft->print(" r");
    tft->print(loggerRunIndex());
    tft->print("  flush ");
    tft->print(loggerFlushCount());
    tft->print("/");
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

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, HEADER_H + 54);
    // Low-water heap next to the live number. The instantaneous value looks
    // healthy right up until it isn't; the trough is what a multi-hour run
    // is actually being judged on (ROADMAP.md Phase 2).
    tft->print("min ");
    tft->print(ESP.getMinFreeHeap() / 1024);
    tft->print("k  batt ");
    const uint32_t mv = batteryMilliVolts();
    if (mv == 0) {
        tft->print("unknown");
    } else {
        tft->print(mv / 1000.0, 2);
        tft->print("V");
    }

    tft->setCursor(2, HEADER_H + 66);
    tft->print("keys ");
    tft->print(keyboardReady ? "tca8418" : "none (auto)");

    tft->setCursor(2, HEADER_H + 78);
    tft->print("bus ");
    tft->print(spiBusContentionCount());
    // Health rows on the card. Shown so an operator can confirm the session
    // log is actually being written before driving off with the lid shut —
    // the whole point of it is that nobody is watching afterwards.
    tft->print("  health ");
    tft->print(loggerSessionRows());

    tft->setCursor(2, HEADER_H + 90);
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
    if (on) {
        char ssid[32];
        wifiApSsid(ssid, sizeof(ssid));
        tft->setCursor(2, HEADER_H + 30);
        tft->print(ssid);
        tft->setCursor(2, HEADER_H + 42);
        tft->print(WIFI_AP_IP);
        tft->setCursor(2, HEADER_H + 54);
        tft->print("clients ");
        tft->print(wifiClientCount());
    } else {
        char ssid[32];
        wifiApSsid(ssid, sizeof(ssid));
        tft->setTextColor(COL_DIM, COL_BG);
        tft->setCursor(2, HEADER_H + 30);
        tft->print("would be:");
        tft->setCursor(2, HEADER_H + 42);
        tft->print(ssid);
    }
}

// One row of the menu, selected or not. Selection is carried by inverting
// FG/BG on that row's whole width — same colour-carries-state convention
// drawBattery()/drawRadioPage() already use, just applied to a full row
// instead of a single value.
void drawMenuRow(int16_t y, const char *rowLabel, const char *value, bool selected) {
    const uint16_t fg = selected ? COL_BG : COL_FG;
    const uint16_t bg = selected ? COL_FG : COL_BG;
    tft->fillRect(0, y - 3, tft->width(), 20, bg);
    tft->setTextSize(2);
    tft->setTextColor(fg, bg);
    tft->setCursor(4, y);
    tft->print(rowLabel);
    tft->print(value);
}

// The menu itself (Phase 5): the two toggles the old hold-gestures used to
// trigger (radioRequestProfileSwitch()/wifiToggle(), both unchanged, Phase
// 3/4), plus a third added after the Phase 5 bench pass — verbose serial
// debug mode (loggerDebugToggle(), logger_task.cpp) — reached the same way:
// moving a highlighted selection with ','/'.' and activating it with Enter,
// per DESIGN.md/PROGRESS.md's Phase 5 scope decision (toggles only — no
// numeric settings editing here).
void drawMenu() {
    drawMenuRow(HEADER_H + 10, "Profile ", missionProfileName((uint8_t)radioActiveProfile()),
                menuSelection == 0);
    drawMenuRow(HEADER_H + 34, "WiFi ", wifiIsEnabled() ? "ON" : "OFF", menuSelection == 1);
    drawMenuRow(HEADER_H + 58, "Debug ", loggerDebugIsEnabled() ? "ON" : "OFF", menuSelection == 2);

    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. move   Enter act   ` back");
}

void drawPage() {
    tft->fillRect(0, HEADER_H + 1, tft->width(), tft->height() - HEADER_H - 1, COL_BG);

    if (mode == UiMode::MENU) {
        drawMenu();
        return;
    }

    switch (page) {
        case UiPage::RADIO: drawRadioPage(); break;
        case UiPage::CHANNEL: drawChannelPage(); break;
        case UiPage::GPS: drawGpsPage(); break;
        case UiPage::SYSTEM: drawSystemPage(); break;
        case UiPage::WIFI: drawWifiPage(); break;
        default: break;
    }

    // Persistent, page-independent — replaces the two gesture-specific
    // hints (RADIO's "hold ~3s", WIFI's "hold ~1s") that used to live
    // inside individual page-draw functions, since both actions moved into
    // one menu that isn't specific to either page any more. "1-5 jump"
    // added alongside the digit-key shortcuts, still well under the
    // ~40-char/line budget at text size 1 on a 240px-wide panel. "` menu"
    // (not "Enter menu") since the 2026-08-24 bench pass moved menu-open
    // onto ESC/backtick — see the BACK branch in uiTask() above.
    tft->setTextSize(1);
    tft->setTextColor(COL_DIM, COL_BG);
    tft->setCursor(2, tft->height() - 9);
    tft->print(",/. page  1-5 jump  ` menu");
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

        if (mode == UiMode::CAROUSEL) {
            if (action == KeyAction::PREV) {
                prevPage();
                redraw = true;
            } else if (action == KeyAction::NEXT) {
                nextPage();
                redraw = true;
            } else if (action == KeyAction::BACK) {
                mode = UiMode::MENU;
                menuSelection = 0;
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
            // closes it also opens it. Digit jumps are only handled in this
            // branch; the menu ignores them rather than reusing them for
            // its own two-item selection.
        } else { // UiMode::MENU
            if (action == KeyAction::PREV) {
                menuSelection = (int8_t)((menuSelection + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT);
                redraw = true;
            } else if (action == KeyAction::NEXT) {
                menuSelection = (int8_t)((menuSelection + 1) % MENU_ITEM_COUNT);
                redraw = true;
            } else if (action == KeyAction::SELECT) {
                if (menuSelection == 0) {
                    // Same one-loop-iteration-of-lag caveat the old
                    // hold-gesture had: this queues the switch, it doesn't
                    // apply it — the header still shows the outgoing
                    // profile until radio_task's own loop picks the
                    // request up, typically the next redraw tick.
                    radioRequestProfileSwitch(nextHomeListenProfile(radioActiveProfile()));
                } else if (menuSelection == 1) {
                    wifiToggle();
                } else {
                    loggerDebugToggle();
                }
                redraw = true;
            } else if (action == KeyAction::BACK) {
                mode = UiMode::CAROUSEL;
                redraw = true;
            }
        }

        if (redraw) {
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (mode == UiMode::CAROUSEL && !keyboardReady &&
                   millis() - lastPageChange >= AUTO_ADVANCE_MS) {
            nextPage();
            drawHeader();
            drawPage();
            lastRedraw = millis();
        } else if (millis() - lastRedraw >= REDRAW_MS) {
            drawHeader(); // battery + page indicator
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
