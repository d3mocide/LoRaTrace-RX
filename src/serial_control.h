#pragma once

// Serial Control is an on-device-authorized remote-control gate. Its USB parser
// is polled by ui_task on Core 0; a physical enable is persisted in NVS so
// native-USB reconnect resets do not clear it, and it only queues existing
// safe subsystem requests without owning the radio, SD, display, or WiFi.

#include <stdint.h>

void serialControlSetEnabled(bool enabled);
bool serialControlIsEnabled();
void serialControlPoll();
