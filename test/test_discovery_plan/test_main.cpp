#include <unity.h>

#include "../../src/discovery_plan.h"

void test_meshtastic_slot_formula_matches_upstream_longfast() {
    TEST_ASSERT_EQUAL_FLOAT(906.875f, meshtasticUsFrequencyForSlot(19, 250.0f));
    TEST_ASSERT_EQUAL_FLOAT(912.8125f, meshtasticUsFrequencyForSlot(86, 125.0f));
    TEST_ASSERT_EQUAL_FLOAT(908.75f, meshtasticUsFrequencyForSlot(13, 500.0f));
}

void test_meshtastic_standard_plan_is_fixed_and_in_band() {
    const DiscoveryPlan plan = discoveryPlanForProfile(MissionProfile::MESHTASTIC);
    TEST_ASSERT_EQUAL_UINT8(DISCOVERY_PLAN_VERSION, plan.version);
    // ShortTurbo's US slot-50 centre is 926.75 MHz, outside this module's
    // supported 868-923 MHz front-end range, so it is intentionally omitted.
    TEST_ASSERT_EQUAL_UINT8(8, plan.count);
    TEST_ASSERT_NOT_NULL(plan.candidates);

    for (uint8_t i = 0; i < plan.count; i++) {
        const ChannelParams &c = plan.candidates[i].channel;
        TEST_ASSERT_TRUE(c.freq_mhz >= 868.0f);
        TEST_ASSERT_TRUE(c.freq_mhz <= 923.0f);
        TEST_ASSERT_EQUAL_UINT8(SYNC_WORD_MESHTASTIC, c.sync_word);
        TEST_ASSERT_TRUE(plan.candidates[i].slot != DISCOVERY_NO_SLOT);
        for (uint8_t j = 0; j < i; j++) {
            TEST_ASSERT_FALSE(discoveryChannelEquals(c, plan.candidates[j].channel));
        }
    }
}

void test_meshcore_plan_keeps_only_source_backed_tuples() {
    const DiscoveryPlan plan = discoveryPlanForProfile(MissionProfile::MESHCORE);
    TEST_ASSERT_EQUAL_UINT8(DISCOVERY_PLAN_VERSION, plan.version);
    TEST_ASSERT_EQUAL_UINT8(2, plan.count);
    TEST_ASSERT_EQUAL_FLOAT(CHANNEL_MESHCORE_US_NARROW.freq_mhz,
                            plan.candidates[0].channel.freq_mhz);
    TEST_ASSERT_EQUAL_FLOAT(915.0f, plan.candidates[1].channel.freq_mhz);
    TEST_ASSERT_EQUAL_UINT8(SYNC_WORD_MESHCORE, plan.candidates[1].channel.sync_word);
    TEST_ASSERT_EQUAL_UINT8(DISCOVERY_NO_SLOT, plan.candidates[1].slot);
}

void test_non_fixed_profiles_have_no_phase8_candidates() {
    TEST_ASSERT_EQUAL_UINT8(0, discoveryPlanForProfile(MissionProfile::RETICULUM).count);
    TEST_ASSERT_EQUAL_UINT8(0, discoveryPlanForProfile(MissionProfile::GENERAL_EXPLORATION).count);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_meshtastic_slot_formula_matches_upstream_longfast);
    RUN_TEST(test_meshtastic_standard_plan_is_fixed_and_in_band);
    RUN_TEST(test_meshcore_plan_keeps_only_source_backed_tuples);
    RUN_TEST(test_non_fixed_profiles_have_no_phase8_candidates);
    return UNITY_END();
}
