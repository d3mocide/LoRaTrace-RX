#include <unity.h>

#include "../../src/channel_plans.h"  // MissionProfile
#include "../../src/node_roster.h"

void test_budget_ceiling_matches_design_doc() {
    TEST_ASSERT_EQUAL_UINT8(24, NODE_ROSTER_MAX_ENTRIES);
}

void test_unknown_node_id_refused() {
    NodeRoster roster;
    TEST_ASSERT_FALSE(nodeRosterUpdate(roster, NODE_ROSTER_EMPTY_ID, 100, -80.0f, 5.0f,
                                        (uint8_t)MissionProfile::MESHTASTIC, false, 0, 0));
}

void test_new_node_fills_empty_slot() {
    NodeRoster roster;
    TEST_ASSERT_TRUE(nodeRosterUpdate(roster, 0x1001, 100, -80.0f, 5.0f,
                                       (uint8_t)MissionProfile::MESHTASTIC, false, 0, 0));
    const uint8_t slot = nodeRosterFindSlot(roster, 0x1001);
    TEST_ASSERT_EQUAL_UINT32(0x1001, roster.entries[slot].node_id);
    TEST_ASSERT_EQUAL_UINT32(1, roster.entries[slot].packet_count);
    TEST_ASSERT_EQUAL_FLOAT(-80.0f, roster.entries[slot].best_rssi_dbm);
    TEST_ASSERT_FALSE(roster.entries[slot].has_hop_metadata);
}

void test_repeat_sighting_updates_in_place_and_tracks_best_rssi() {
    NodeRoster roster;
    nodeRosterUpdate(roster, 0x1001, 100, -90.0f, 5.0f, (uint8_t)MissionProfile::MESHTASTIC, false, 0, 0);
    nodeRosterUpdate(roster, 0x1001, 200, -70.0f, 6.0f, (uint8_t)MissionProfile::MESHTASTIC, true, 3, 7);
    nodeRosterUpdate(roster, 0x1001, 300, -95.0f, 4.0f, (uint8_t)MissionProfile::MESHTASTIC, false, 0, 0);

    const uint8_t slot = nodeRosterFindSlot(roster, 0x1001);
    const NodeRosterEntry &e = roster.entries[slot];
    TEST_ASSERT_EQUAL_UINT32(3, e.packet_count);
    TEST_ASSERT_EQUAL_UINT32(300, e.last_seen_millis);
    TEST_ASSERT_EQUAL_FLOAT(-95.0f, e.latest_rssi_dbm); // latest, even though weaker
    TEST_ASSERT_EQUAL_FLOAT(-70.0f, e.best_rssi_dbm);   // strongest ever seen
    // hop metadata from the one sighting that had it survives a later
    // sighting that didn't supply any.
    TEST_ASSERT_TRUE(e.has_hop_metadata);
    TEST_ASSERT_EQUAL_UINT8(3, e.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(7, e.hop_start);
}

void test_eviction_is_deterministic_lru_lowest_index_on_tie() {
    NodeRoster roster;
    // Fill all 24 slots at the same last_seen_millis so eviction must fall
    // back to "lowest index" deterministically, not iteration order.
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        nodeRosterUpdate(roster, 0x2000 + i, 500, -80.0f, 5.0f,
                          (uint8_t)MissionProfile::MESHTASTIC, false, 0, 0);
    }
    // Make slot 5 the unambiguous oldest.
    roster.entries[5].last_seen_millis = 1;

    TEST_ASSERT_TRUE(nodeRosterUpdate(roster, 0x9999, 999, -60.0f, 9.0f,
                                       (uint8_t)MissionProfile::MESHCORE, false, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0x9999, roster.entries[5].node_id);

    // Every other original entry must still be present untouched.
    for (uint8_t i = 0; i < NODE_ROSTER_MAX_ENTRIES; i++) {
        if (i == 5) continue;
        TEST_ASSERT_EQUAL_UINT8(i, nodeRosterFindSlot(roster, 0x2000 + i));
    }
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_ceiling_matches_design_doc);
    RUN_TEST(test_unknown_node_id_refused);
    RUN_TEST(test_new_node_fills_empty_slot);
    RUN_TEST(test_repeat_sighting_updates_in_place_and_tracks_best_rssi);
    RUN_TEST(test_eviction_is_deterministic_lru_lowest_index_on_tie);
    return UNITY_END();
}
