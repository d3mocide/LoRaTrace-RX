// ui_labels.h maps MissionProfile to BRAND.md's on-device labels. Guards
// the mistakes that matter here: a typo'd label, and two *distinct*
// profiles colliding on the same string (an operator can't tell them apart
// on a 240px panel if they read identically) — same "guard against
// collision" pattern as test_channel_plans' sync-word tests. Revised
// 2026-08-25 (BRAND.md "Revised again 2026-08-25"): the earlier
// family/sub-profile split (Mesh Trace + Meshtastic/MeshCore) was walked
// back the same day in favor of one flat label per profile, so this file
// is back to a single collision/lookup check, not a two-tier one.
// Runs on the host (`pio test -e native`), no hardware needed — see
// platformio.ini [env:native].

#include <string.h>

#include <unity.h>

#include "../../src/ui_labels.h"

void test_profile_labels_match_brand_md() {
    TEST_ASSERT_EQUAL_STRING("Meshtastic", uiProfileLabel(MissionProfile::MESHTASTIC));
    TEST_ASSERT_EQUAL_STRING("MeshCore", uiProfileLabel(MissionProfile::MESHCORE));
    TEST_ASSERT_EQUAL_STRING("Reticulum", uiProfileLabel(MissionProfile::RETICULUM));
    TEST_ASSERT_EQUAL_STRING("Spectrum", uiProfileLabel(MissionProfile::GENERAL_EXPLORATION));
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
