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

// Every real Meshtastic standard-preset candidate must resolve to its
// actual upstream preset name, not the "?" fallback — a fallback here
// would mean the label switch has drifted from discovery_plan.h's slot
// table (e.g. a new candidate added without a matching label).
void test_meshtastic_candidate_labels_are_all_named() {
    const DiscoveryPlan plan = discoveryPlanForProfile(MissionProfile::MESHTASTIC);
    for (uint8_t i = 0; i < plan.count; i++) {
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", uiDiscoveryCandidateLabel(plan.candidates[i])));
    }
}

// Same collision reasoning as the profile labels above: two candidates in
// the same plan that read identically on-screen would make Probe's
// candidate-name readout ("hit: X, Y") ambiguous about which one hit.
void test_meshtastic_candidate_labels_dont_collide() {
    const DiscoveryPlan plan = discoveryPlanForProfile(MissionProfile::MESHTASTIC);
    for (uint8_t i = 0; i < plan.count; i++) {
        for (uint8_t j = i + 1; j < plan.count; j++) {
            TEST_ASSERT_NOT_EQUAL(
                0, strcmp(uiDiscoveryCandidateLabel(plan.candidates[i]),
                         uiDiscoveryCandidateLabel(plan.candidates[j])));
        }
    }
}

void test_meshcore_candidate_labels_dont_collide_or_fallback() {
    const DiscoveryPlan plan = discoveryPlanForProfile(MissionProfile::MESHCORE);
    TEST_ASSERT_EQUAL_UINT8(2, plan.count);
    TEST_ASSERT_NOT_EQUAL(0, strcmp("?", uiDiscoveryCandidateLabel(plan.candidates[0])));
    TEST_ASSERT_NOT_EQUAL(0, strcmp("?", uiDiscoveryCandidateLabel(plan.candidates[1])));
    TEST_ASSERT_NOT_EQUAL(
        0, strcmp(uiDiscoveryCandidateLabel(plan.candidates[0]),
                 uiDiscoveryCandidateLabel(plan.candidates[1])));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_profile_labels_match_brand_md);
    RUN_TEST(test_profile_labels_dont_collide);
    RUN_TEST(test_mode_label_watch_matches_brand_md);
    RUN_TEST(test_meshtastic_candidate_labels_are_all_named);
    RUN_TEST(test_meshtastic_candidate_labels_dont_collide);
    RUN_TEST(test_meshcore_candidate_labels_dont_collide_or_fallback);
    return UNITY_END();
}
