// ui_labels.h maps MissionProfile to BRAND.md's on-device labels. Guards
// the two mistakes that matter here: a typo'd label, and two profiles
// colliding on the same string (an operator can't tell them apart on a
// 240px panel if they read identically) — same "guard against collision"
// pattern as test_channel_plans' sync-word tests. Runs on the host
// (`pio test -e native`), no hardware needed — see platformio.ini
// [env:native].

#include <string.h>

#include <unity.h>

#include "../../src/ui_labels.h"

void test_profile_labels_match_brand_md() {
    // BRAND.md "Interface Naming" table.
    TEST_ASSERT_EQUAL_STRING("Mesh Trace", uiProfileLabel(MissionProfile::MESHTASTIC));
    TEST_ASSERT_EQUAL_STRING("Core Trace", uiProfileLabel(MissionProfile::MESHCORE));
    TEST_ASSERT_EQUAL_STRING("Open Trace", uiProfileLabel(MissionProfile::RETICULUM));
    TEST_ASSERT_EQUAL_STRING("Spectrum Trace", uiProfileLabel(MissionProfile::GENERAL_EXPLORATION));
}

void test_profile_labels_dont_collide() {
    const char *labels[] = {
        uiProfileLabel(MissionProfile::MESHTASTIC),
        uiProfileLabel(MissionProfile::MESHCORE),
        uiProfileLabel(MissionProfile::RETICULUM),
        uiProfileLabel(MissionProfile::GENERAL_EXPLORATION),
    };
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = i + 1; j < 4; j++) {
            TEST_ASSERT_TRUE(strcmp(labels[i], labels[j]) != 0);
        }
    }
}

void test_mode_label_watch_matches_brand_md() {
    TEST_ASSERT_EQUAL_STRING("Watch", uiModeLabelWatch());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_profile_labels_match_brand_md);
    RUN_TEST(test_profile_labels_dont_collide);
    RUN_TEST(test_mode_label_watch_matches_brand_md);
    return UNITY_END();
}
