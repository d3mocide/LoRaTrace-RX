// ui_labels.h maps MissionProfile to BRAND.md's on-device labels. Guards
// the mistakes that matter here: a typo'd label, two *distinct* profiles
// colliding on the same string (an operator can't tell them apart on a
// 240px panel if they read identically) — same "guard against collision"
// pattern as test_channel_plans' sync-word tests — and the 2026-08-25
// family/sub-profile split (BRAND.md "Revised 2026-08-25"): Meshtastic and
// MeshCore are now expected to share one family name, not collide-checked
// against each other, since sharing "Mesh Trace" is the whole point.
// Runs on the host (`pio test -e native`), no hardware needed — see
// platformio.ini [env:native].

#include <string.h>

#include <unity.h>

#include "../../src/ui_labels.h"

void test_trace_mode_labels_match_brand_md() {
    // BRAND.md "Interface Naming": Meshtastic and MeshCore share one
    // family name on purpose.
    TEST_ASSERT_EQUAL_STRING("Mesh Trace", uiTraceModeLabel(MissionProfile::MESHTASTIC));
    TEST_ASSERT_EQUAL_STRING("Mesh Trace", uiTraceModeLabel(MissionProfile::MESHCORE));
    TEST_ASSERT_EQUAL_STRING("Open Trace", uiTraceModeLabel(MissionProfile::RETICULUM));
    TEST_ASSERT_EQUAL_STRING("Spectrum Trace", uiTraceModeLabel(MissionProfile::GENERAL_EXPLORATION));
}

// The real "can an operator tell these apart" guard now lives one level up
// from Meshtastic/MeshCore: the three family names themselves.
void test_trace_mode_labels_distinct_at_family_level() {
    const char *families[] = {
        uiTraceModeLabel(MissionProfile::MESHTASTIC), // == MeshCore's, see above
        uiTraceModeLabel(MissionProfile::RETICULUM),
        uiTraceModeLabel(MissionProfile::GENERAL_EXPLORATION),
    };
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = i + 1; j < 3; j++) {
            TEST_ASSERT_TRUE(strcmp(families[i], families[j]) != 0);
        }
    }
}

void test_sub_profile_labels_match_brand_md() {
    TEST_ASSERT_EQUAL_STRING("Meshtastic", uiSubProfileLabel(MissionProfile::MESHTASTIC));
    TEST_ASSERT_EQUAL_STRING("MeshCore", uiSubProfileLabel(MissionProfile::MESHCORE));
    // No sub-profile for either — single-profile families.
    TEST_ASSERT_EQUAL_STRING("", uiSubProfileLabel(MissionProfile::RETICULUM));
    TEST_ASSERT_EQUAL_STRING("", uiSubProfileLabel(MissionProfile::GENERAL_EXPLORATION));
}

void test_sub_profile_labels_dont_collide() {
    TEST_ASSERT_TRUE(strcmp(uiSubProfileLabel(MissionProfile::MESHTASTIC),
                            uiSubProfileLabel(MissionProfile::MESHCORE)) != 0);
}

void test_active_profile_label_composes_family_and_sub_profile() {
    char buf[32];
    uiActiveProfileLabel(MissionProfile::MESHTASTIC, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Mesh Trace: Meshtastic", buf);
    uiActiveProfileLabel(MissionProfile::MESHCORE, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Mesh Trace: MeshCore", buf);
}

void test_active_profile_label_is_bare_family_name_with_no_sub_profile() {
    char buf[32];
    uiActiveProfileLabel(MissionProfile::RETICULUM, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Open Trace", buf);
    uiActiveProfileLabel(MissionProfile::GENERAL_EXPLORATION, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("Spectrum Trace", buf);
}

void test_mode_label_watch_matches_brand_md() {
    TEST_ASSERT_EQUAL_STRING("Watch", uiModeLabelWatch());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_trace_mode_labels_match_brand_md);
    RUN_TEST(test_trace_mode_labels_distinct_at_family_level);
    RUN_TEST(test_sub_profile_labels_match_brand_md);
    RUN_TEST(test_sub_profile_labels_dont_collide);
    RUN_TEST(test_active_profile_label_composes_family_and_sub_profile);
    RUN_TEST(test_active_profile_label_is_bare_family_name_with_no_sub_profile);
    RUN_TEST(test_mode_label_watch_matches_brand_md);
    return UNITY_END();
}
