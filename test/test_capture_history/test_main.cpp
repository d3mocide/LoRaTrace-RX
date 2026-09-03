#include <unity.h>

#include "../../src/capture_history.h"

void test_budget_ceiling_matches_design_doc() {
    TEST_ASSERT_EQUAL_UINT8(8, CAPTURE_HISTORY_MAX_ENTRIES);
}

void test_summary_from_detection_drops_raw_packet() {
    Detection det;
    det.rx_millis = 12345;
    det.node_id = 0xAABBCCDD;
    det.freq_mhz = 906.875f;
    det.rssi_dbm = -72.5f;
    det.snr_db = 8.25f;
    det.bw_khz_x10 = 2500;
    det.sf = 11;
    det.cr_denom = 5;
    det.profile = (uint8_t)MissionProfile::MESHTASTIC;
    det.off_grid = true;
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_TRUE(detectionSetRawPacket(det, payload, sizeof(payload)));

    const CaptureSummary summary = captureSummaryFromDetection(det);
    TEST_ASSERT_EQUAL_UINT32(12345, summary.rx_millis);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCDD, summary.node_id);
    TEST_ASSERT_EQUAL_FLOAT(906.875f, summary.freq_mhz);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), summary.raw_len); // set by detectionSetRawPacket
    TEST_ASSERT_EQUAL_UINT8(11, summary.sf);
    TEST_ASSERT_TRUE(summary.off_grid);
    // CaptureSummary has no raw_packet field at all — this is a compile-time
    // guarantee (sizeof would need Detection's 255B buffer if it did), not
    // just a runtime check.
    TEST_ASSERT_TRUE(sizeof(CaptureSummary) < sizeof(Detection));
}

void test_push_and_recency_order() {
    CaptureHistory history;
    Detection a, b;
    a.rx_millis = 1000;
    b.rx_millis = 2000;
    captureHistoryPush(history, captureSummaryFromDetection(a));
    captureHistoryPush(history, captureSummaryFromDetection(b));

    CaptureSummary out;
    TEST_ASSERT_TRUE(captureHistoryEntryAt(history, 0, out));
    TEST_ASSERT_EQUAL_UINT32(2000, out.rx_millis);
    TEST_ASSERT_TRUE(captureHistoryEntryAt(history, 1, out));
    TEST_ASSERT_EQUAL_UINT32(1000, out.rx_millis);
    TEST_ASSERT_FALSE(captureHistoryEntryAt(history, 2, out));
}

void test_ring_wraps_and_evicts_oldest() {
    CaptureHistory history;
    for (uint32_t i = 0; i < CAPTURE_HISTORY_MAX_ENTRIES + 3; i++) {
        Detection det;
        det.rx_millis = 1000 + i;
        captureHistoryPush(history, captureSummaryFromDetection(det));
    }
    TEST_ASSERT_EQUAL_UINT8(CAPTURE_HISTORY_MAX_ENTRIES, history.count);
    CaptureSummary newest, oldest;
    TEST_ASSERT_TRUE(captureHistoryEntryAt(history, 0, newest));
    TEST_ASSERT_TRUE(captureHistoryEntryAt(history, CAPTURE_HISTORY_MAX_ENTRIES - 1, oldest));
    TEST_ASSERT_EQUAL_UINT32(1000 + CAPTURE_HISTORY_MAX_ENTRIES + 2, newest.rx_millis);
    TEST_ASSERT_EQUAL_UINT32(1000 + 3, oldest.rx_millis);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_ceiling_matches_design_doc);
    RUN_TEST(test_summary_from_detection_drops_raw_packet);
    RUN_TEST(test_push_and_recency_order);
    RUN_TEST(test_ring_wraps_and_evicts_oldest);
    return UNITY_END();
}
