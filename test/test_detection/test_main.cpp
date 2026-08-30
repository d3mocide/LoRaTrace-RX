// Guards the detection record and its formatting — the two places a Phase 2
// bug would silently corrupt the log rather than crash.
//
// The Meshtastic fixtures below are REAL packets captured on hardware
// 2026-08-23 (PROGRESS.md), not synthetic bytes. That matters: they encode
// the original-vs-rebroadcast pairing that confused early bench testing, so
// a regression in header parsing shows up here as a concrete wrong answer
// about real traffic.

#include <unity.h>

#include <string.h>

#include "../../src/detection.h"

// Same broadcast packet, heard twice: once direct, once relayed. Identical
// `from`, `id` and ciphertext; hop_limit decremented and a different relay.
static const uint8_t PKT_ORIGINAL[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x5C, 0x06, 0xBF, 0x1B,
                                       0x2D, 0x8F, 0x61, 0x2C, 0xE7, 0xF7, 0x00, 0x5C,
                                       0x8B, 0x7F, 0x1C, 0xBE, 0x42, 0x51, 0x61, 0x50,
                                       0x3E, 0x02};
static const uint8_t PKT_RELAYED[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x5C, 0x06, 0xBF, 0x1B,
                                      0x2D, 0x8F, 0x61, 0x2C, 0xE6, 0xF7, 0x00, 0x6A,
                                      0x8B, 0x7F, 0x1C, 0xBE, 0x42, 0x51, 0x61, 0x50,
                                      0x3E, 0x02};
// A unicast (non-broadcast) frame: to=0x82D7776A, from=0x3B9292F1.
static const uint8_t PKT_UNICAST[] = {0x6A, 0x77, 0xD7, 0x82, 0xF1, 0x92, 0x92, 0x3B,
                                      0x6D, 0xBB, 0x65, 0x9B, 0xE7, 0xF7, 0x00, 0xF1,
                                      0x0B, 0x1F, 0xC6, 0x95};

void test_detection_fits_queue_budget() {
    TEST_ASSERT_TRUE(sizeof(Detection) <= 304);
}

void test_meshtastic_header_fields() {
    Detection det = {};
    TEST_ASSERT_TRUE(detectionApplyMeshtasticHeader(det, PKT_ORIGINAL, sizeof(PKT_ORIGINAL)));
    // Little-endian: bytes 5C 06 BF 1B -> 0x1BBF065C
    TEST_ASSERT_EQUAL_HEX32(0x1BBF065Cu, det.node_id);
    TEST_ASSERT_EQUAL_HEX32(0x2C618F2Du, det.packet_id);
    TEST_ASSERT_EQUAL_HEX8(0xF7, det.channel_hash);
    TEST_ASSERT_EQUAL_UINT8(7, det.hop_limit);  // flags 0xE7, bottom 3 bits
    TEST_ASSERT_EQUAL_UINT8(7, det.hop_start);  // flags 0xE7, bits 5-7
    TEST_ASSERT_EQUAL_HEX8(0x5C, det.relay_node);
}

void test_original_and_relay_share_dedupe_key() {
    Detection a = {}, b = {};
    detectionApplyMeshtasticHeader(a, PKT_ORIGINAL, sizeof(PKT_ORIGINAL));
    detectionApplyMeshtasticHeader(b, PKT_RELAYED, sizeof(PKT_RELAYED));

    // (node_id, packet_id) is the dedupe key: it must MATCH across a
    // rebroadcast, or "unique messages heard" would double-count the mesh.
    TEST_ASSERT_EQUAL_HEX32(a.node_id, b.node_id);
    TEST_ASSERT_EQUAL_HEX32(a.packet_id, b.packet_id);

    // ...while the routing metadata must DIFFER, or we'd be unable to tell a
    // relay from a direct reception at all.
    TEST_ASSERT_EQUAL_UINT8(6, b.hop_limit);
    TEST_ASSERT_EQUAL_UINT8(7, b.hop_start); // hop_start is preserved across hops
    TEST_ASSERT_EQUAL_HEX8(0x6A, b.relay_node);
    TEST_ASSERT_NOT_EQUAL_UINT8(a.hop_limit, b.hop_limit);
    TEST_ASSERT_NOT_EQUAL_UINT8(a.relay_node, b.relay_node);
}

void test_broadcast_vs_unicast() {
    TEST_ASSERT_TRUE(meshtasticIsBroadcast(PKT_ORIGINAL, sizeof(PKT_ORIGINAL)));
    TEST_ASSERT_FALSE(meshtasticIsBroadcast(PKT_UNICAST, sizeof(PKT_UNICAST)));

    Detection det = {};
    detectionApplyMeshtasticHeader(det, PKT_UNICAST, sizeof(PKT_UNICAST));
    TEST_ASSERT_EQUAL_HEX32(0x3B9292F1u, det.node_id);
}

void test_runt_frame_yields_no_ids() {
    // A frame too short to hold a header must not publish garbage node ids
    // into the log — zeros mean "unknown", which the CSV renders as empty.
    Detection det = {};
    det.node_id = 0xDEADBEEF;
    const uint8_t runt[] = {0xFF, 0xFF, 0xFF};
    TEST_ASSERT_FALSE(detectionApplyMeshtasticHeader(det, runt, sizeof(runt)));
    TEST_ASSERT_EQUAL_HEX32(0u, det.node_id);
    TEST_ASSERT_EQUAL_HEX32(0u, det.packet_id);
    TEST_ASSERT_FALSE(meshtasticIsBroadcast(runt, sizeof(runt)));
    TEST_ASSERT_FALSE(detectionApplyMeshtasticHeader(det, nullptr, 32));
}

void test_csv_row_with_fix() {
    Detection det = {};
    detectionApplyMeshtasticHeader(det, PKT_ORIGINAL, sizeof(PKT_ORIGINAL));
    det.freq_mhz = 918.5f;
    det.rssi_dbm = -60.0f;
    det.snr_db = 13.75f;
    TEST_ASSERT_TRUE(detectionSetRawPacket(det, PKT_ORIGINAL, sizeof(PKT_ORIGINAL)));
    det.bw_khz_x10 = 1250;
    det.sf = 8;
    det.cr_denom = 5;
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;
    det.rx_millis = 41250;

    char row[DETECTION_CSV_MAX_ROW];
    size_t n = detectionFormatCsv(det, row, sizeof(row), "2026-08-23T01:20:00Z", true, 45.5123456,
                                  -122.6789012, 1, 7);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(
        "2026-08-23T01:20:00Z,45.512346,-122.678901,1,7,41250,meshtastic,meshtastic,!1bbf065c,"
        "2c618f2d,7,7,5c,918.500,8,125.0,-60.0,13.75,26,"
        "ffffffff5c06bf1b2d8f612ce7f7005c8b7f1cbe425161503e02,",
        row);
}

void test_csv_exposes_relay_vs_original() {
    // The point of wiring these columns through at all: a same-node_id pair
    // heard seconds apart must be tellable apart, from the CSV alone, as
    // "direct + mesh relay" (same packet_id, different hop_limit/relay_node)
    // rather than a firmware bug re-logging one packet twice (which would
    // show identical values in every one of these fields too).
    Detection original = {}, relay = {};
    detectionApplyMeshtasticHeader(original, PKT_ORIGINAL, sizeof(PKT_ORIGINAL));
    detectionApplyMeshtasticHeader(relay, PKT_RELAYED, sizeof(PKT_RELAYED));
    original.profile = relay.profile = (uint8_t)MissionProfile::MESHTASTIC;

    char rowA[256], rowB[256];
    TEST_ASSERT_TRUE(detectionFormatCsv(original, rowA, sizeof(rowA), "t", false, 0, 0, 0, 1) > 0);
    TEST_ASSERT_TRUE(detectionFormatCsv(relay, rowB, sizeof(rowB), "t", false, 0, 0, 0, 1) > 0);

    TEST_ASSERT_NOT_NULL(strstr(rowA, ",2c618f2d,7,7,5c"));
    TEST_ASSERT_NOT_NULL(strstr(rowB, ",2c618f2d,6,7,6a")); // same packet_id, decremented hop, new relay
}

void test_uptime_anchors_a_detection_heard_before_the_first_fix() {
    // The case this column exists for. No GPS time and no position yet, so
    // timestamp and lat/lon are both empty — without rx_uptime_ms the row
    // could not be placed in time at all, nor joined to the session.csv row
    // covering the same minute.
    Detection det = {};
    detectionApplyMeshtasticHeader(det, PKT_ORIGINAL, sizeof(PKT_ORIGINAL));
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;
    det.rx_millis = 8123;

    char row[256];
    size_t n = detectionFormatCsv(det, row, sizeof(row), "", false, 0.0, 0.0, 0, 3);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT(0, strncmp(row, ",,,0,", 5)); // no time, no position
    TEST_ASSERT_NOT_NULL(strstr(row, ",3,8123,"));      // but always a run + uptime
}

// Phase 4: radio_task.cpp deliberately never calls
// detectionApplyMeshtasticHeader() for a MeshCore detection (its header
// layout isn't verified — DESIGN.md §7, detection.h's comment above). This
// pins the CSV-visible consequence of that: the profile/classification
// columns say "meshcore", and the id columns stay empty — not "!00000000",
// which would misread as a real, all-zero node id.
void test_meshcore_csv_row_has_no_header_fields() {
    Detection det = {};
    det.profile = (uint8_t)MissionProfile::MESHCORE;
    det.freq_mhz = 910.525f;
    det.rssi_dbm = -70.0f;
    det.snr_db = 9.5f;
    det.raw_len = 40;
    det.bw_khz_x10 = 625;
    det.sf = 7;
    det.cr_denom = 5;
    det.rx_millis = 12345;
    // node_id/packet_id/etc. left at their zero-init value on purpose — no
    // detectionApplyMeshtasticHeader() call, matching radio_task.cpp's gate.

    char row[256];
    size_t n = detectionFormatCsv(det, row, sizeof(row), "", false, 0.0, 0.0, 0, 1);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",meshcore,meshcore,"));
    TEST_ASSERT_NULL(strstr(row, "!00000000"));
}

// Phase 9 Pass B: a CAD hit at an arbitrary Sweep peak bin must never read
// as the active mission profile's name in the CSV (DESIGN.md §7.2 — "not
// Reticulum... protocol attribution requires evidence the radio layer
// cannot provide"). Sweep only ever runs under RETICULUM/
// GENERAL_EXPLORATION, so this is the exact case that would otherwise
// silently mislabel an unknown signal as one of those two profiles.
void test_off_grid_hit_is_not_labeled_by_profile() {
    Detection det = {};
    det.profile = (uint8_t)MissionProfile::RETICULUM;
    det.off_grid = true;
    det.freq_mhz = 900.125f;
    det.rssi_dbm = -60.0f;
    det.sf = 9;
    det.bw_khz_x10 = 2500;

    TEST_ASSERT_EQUAL_STRING("unknown_lora_candidate", detectionClassification(det));

    char row[256];
    size_t n = detectionFormatCsv(det, row, sizeof(row), "", false, 0.0, 0.0, 0, 1);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",reticulum,unknown_lora_candidate,"));
}

void test_csv_row_without_fix_leaves_coords_empty() {
    // The important one: no fix must NOT render as 0,0. Null Island is a
    // real coordinate and would quietly poison a track.
    Detection det = {};
    det.freq_mhz = 918.5f;
    det.bw_khz_x10 = 1250;
    det.sf = 8;
    det.raw_len = 10;
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;

    char row[256];
    size_t n = detectionFormatCsv(det, row, sizeof(row), "", false, 0.0, 0.0, 0, 3);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",,,0,3,0,meshtastic,"));
    TEST_ASSERT_NULL(strstr(row, "0.000000"));
    // node id unknown -> empty column, not "!00000000"
    TEST_ASSERT_NULL(strstr(row, "!00000000"));
}

void test_csv_truncation_is_reported() {
    Detection det = {};
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;
    char tiny[8];
    // Returning 0 tells the caller to drop the row rather than write a
    // half-formed one that would corrupt the CSV.
    TEST_ASSERT_EQUAL_UINT32(0, detectionFormatCsv(det, tiny, sizeof(tiny), "x", false, 0, 0, 0, 1));
}

void test_raw_packet_length_is_bounded_and_csv_safe() {
    Detection det = {};
    det.profile = (uint8_t)MissionProfile::MESHCORE;
    uint8_t full[DETECTION_RAW_MAX_LEN];
    memset(full, 0xA5, sizeof(full));
    TEST_ASSERT_TRUE(detectionSetRawPacket(det, full, sizeof(full)));

    char row[DETECTION_CSV_MAX_ROW];
    TEST_ASSERT_TRUE(detectionFormatCsv(det, row, sizeof(row), "", false, 0, 0, 0, 1) > 0);
    TEST_ASSERT_NOT_NULL(strstr(row, ",a5a5a5a5"));
    TEST_ASSERT_EQUAL_UINT16(DETECTION_RAW_MAX_LEN, det.raw_len);
    TEST_ASSERT_FALSE(detectionSetRawPacket(det, full, sizeof(full) + 1));
}

void test_header_column_count_matches_row() {
    Detection det = {};
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;
    char row[256];
    TEST_ASSERT_TRUE(detectionFormatCsv(det, row, sizeof(row), "t", false, 0, 0, 0, 1) > 0);

    auto commas = [](const char *s) {
        int c = 0;
        for (; *s; s++)
            if (*s == ',') c++;
        return c;
    };
    // A row that doesn't match the header is the classic silent CSV bug.
    TEST_ASSERT_EQUAL_INT(commas(LOG_CSV_HEADER), commas(row));
}

void test_timestamp_formatting() {
    char ts[24];
    detectionFormatTimestamp(ts, sizeof(ts), true, 2026, 8, 23, 1, 5, 9);
    TEST_ASSERT_EQUAL_STRING("2026-08-23T01:05:09Z", ts);

    detectionFormatTimestamp(ts, sizeof(ts), false, 2026, 8, 23, 1, 5, 9);
    TEST_ASSERT_EQUAL_STRING("", ts);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_detection_fits_queue_budget);
    RUN_TEST(test_meshtastic_header_fields);
    RUN_TEST(test_original_and_relay_share_dedupe_key);
    RUN_TEST(test_broadcast_vs_unicast);
    RUN_TEST(test_runt_frame_yields_no_ids);
    RUN_TEST(test_csv_row_with_fix);
    RUN_TEST(test_meshcore_csv_row_has_no_header_fields);
    RUN_TEST(test_off_grid_hit_is_not_labeled_by_profile);
    RUN_TEST(test_csv_exposes_relay_vs_original);
    RUN_TEST(test_csv_row_without_fix_leaves_coords_empty);
    RUN_TEST(test_uptime_anchors_a_detection_heard_before_the_first_fix);
    RUN_TEST(test_csv_truncation_is_reported);
    RUN_TEST(test_raw_packet_length_is_bounded_and_csv_safe);
    RUN_TEST(test_header_column_count_matches_row);
    RUN_TEST(test_timestamp_formatting);
    return UNITY_END();
}
