// Guards the one class of mistake DESIGN.md/CLAUDE.md worry about most for
// this file: a typo'd RF constant that's wrong but not obviously wrong
// (e.g. 906.875 -> 960.875, SF11 -> SF1). Runs on the host (`pio test -e
// native`), no hardware needed — see platformio.ini [env:native].

#include <unity.h>

#include "../../src/channel_plans.h"

void test_meshtastic_longfast_in_tuned_band() {
    // Module front end is tuned 868-923MHz (DESIGN.md §1).
    TEST_ASSERT_TRUE(CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz >= 868.0f);
    TEST_ASSERT_TRUE(CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz <= 923.0f);
    TEST_ASSERT_EQUAL_UINT8(11, CHANNEL_MESHTASTIC_LONGFAST_US.sf);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, CHANNEL_MESHTASTIC_LONGFAST_US.bw_khz);
    TEST_ASSERT_EQUAL_UINT8(8, CHANNEL_MESHTASTIC_LONGFAST_US.cr_denom);
}

// The sync word is a hardware RX filter — a wrong value means hearing
// nothing from Meshtastic while still hearing unrelated traffic, which is
// precisely the failure this firmware shipped with until 2026-08-23 (it was
// silently inheriting RadioLib's 0x12 default). Pinned against the upstream
// firmware source cited in channel_plans.h.
void test_meshtastic_sync_word_matches_upstream() {
    // meshtastic/firmware src/mesh/RadioLibInterface.h: `const uint8_t syncWord = 0x2b;`
    TEST_ASSERT_EQUAL_UINT8(0x2B, CHANNEL_MESHTASTIC_LONGFAST_US.sync_word);
    // Guard the two specific wrong values that are easy to land on: RadioLib's
    // default / pre-1.2 Meshtastic, and LoRaWAN's reserved word.
    TEST_ASSERT_NOT_EQUAL_UINT8(0x12, CHANNEL_MESHTASTIC_LONGFAST_US.sync_word);
    TEST_ASSERT_NOT_EQUAL_UINT8(0x34, CHANNEL_MESHTASTIC_LONGFAST_US.sync_word);
}

// Deliberately asserts the *unverified* state rather than a real value:
// CLAUDE.md forbids assuming MeshCore mirrors Meshtastic, so until its sync
// word is confirmed from upstream source this must stay on RadioLib's
// default. If someone resolves it in Phase 3, this test should fail and be
// updated alongside the citation in channel_plans.h — that's the point.
void test_meshcore_sync_word_still_unverified() {
    TEST_ASSERT_EQUAL_UINT8(SYNC_WORD_RADIOLIB_DEFAULT, CHANNEL_MESHCORE_US_NARROW.sync_word);
    TEST_ASSERT_NOT_EQUAL_UINT8(SYNC_WORD_MESHTASTIC, CHANNEL_MESHCORE_US_NARROW.sync_word);
}

void test_meshcore_narrow_in_tuned_band() {
    TEST_ASSERT_TRUE(CHANNEL_MESHCORE_US_NARROW.freq_mhz >= 868.0f);
    TEST_ASSERT_TRUE(CHANNEL_MESHCORE_US_NARROW.freq_mhz <= 923.0f);
    TEST_ASSERT_EQUAL_UINT8(7, CHANNEL_MESHCORE_US_NARROW.sf);
    TEST_ASSERT_EQUAL_FLOAT(62.5f, CHANNEL_MESHCORE_US_NARROW.bw_khz);
    TEST_ASSERT_EQUAL_UINT8(5, CHANNEL_MESHCORE_US_NARROW.cr_denom);
}

void test_meshtastic_and_meshcore_dont_collide() {
    // Different enough that a HOME_LISTEN lock on one won't pick up the
    // other by accident.
    float delta = CHANNEL_MESHTASTIC_LONGFAST_US.freq_mhz - CHANNEL_MESHCORE_US_NARROW.freq_mhz;
    if (delta < 0) delta = -delta;
    TEST_ASSERT_TRUE(delta > 1.0f);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_meshtastic_longfast_in_tuned_band);
    RUN_TEST(test_meshcore_narrow_in_tuned_band);
    RUN_TEST(test_meshtastic_and_meshcore_dont_collide);
    RUN_TEST(test_meshtastic_sync_word_matches_upstream);
    RUN_TEST(test_meshcore_sync_word_still_unverified);
    return UNITY_END();
}
