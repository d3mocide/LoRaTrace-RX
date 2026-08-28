#pragma once

// Low Profile is an on-device-authorized remote-control gate. Its USB parser
// is polled by ui_task on Core 0; it only queues existing safe subsystem
// requests and never owns the radio, SD, display, or WiFi.

#include <stdint.h>

void lowProfileSetEnabled(bool enabled);
bool lowProfileIsEnabled();
void lowProfilePoll();
