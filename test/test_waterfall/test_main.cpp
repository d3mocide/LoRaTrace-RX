#include <unity.h>

#include "../../src/waterfall.h"

void test_budget_ceiling_matches_design_doc() {
    TEST_ASSERT_EQUAL_UINT16(224, WATERFALL_MAX_BINS);
    TEST_ASSERT_EQUAL_UINT8(24, WATERFALL_MAX_ROWS);
}

void test_rssi_byte_round_trip_and_clamp() {
    TEST_ASSERT_EQUAL_UINT8(0, waterfallEncodeRssiByte(-2000));   // below floor clamps
    TEST_ASSERT_EQUAL_UINT8(254, waterfallEncodeRssiByte(500));   // above ceiling clamps
    TEST_ASSERT_EQUAL_UINT8(0, waterfallEncodeRssiByte(WATERFALL_RSSI_FLOOR_DBM_X10));
    TEST_ASSERT_EQUAL_UINT8(254, waterfallEncodeRssiByte(WATERFALL_RSSI_CEIL_DBM_X10));
    TEST_ASSERT_NOT_EQUAL(WATERFALL_NO_DATA, waterfallEncodeRssiByte(-800));

    const uint8_t mid = waterfallEncodeRssiByte(-800);
    const int16_t decoded = waterfallDecodeRssiByte(mid);
    // Quantization loses precision; within one bucket step is a round trip.
    TEST_ASSERT_INT16_WITHIN(6, -800, decoded);
}

void test_column_for_bin_endpoints_downsampling() {
    // 221 real bins onto a narrower 100-column display, both endpoints exact.
    TEST_ASSERT_EQUAL_UINT16(0, waterfallColumnForBin(0, 221, 100));
    TEST_ASSERT_EQUAL_UINT16(99, waterfallColumnForBin(220, 221, 100));
}

void test_column_for_bin_endpoints_upsampling() {
    // Fewer bins than columns (a narrow-band US sweep on a wide panel).
    TEST_ASSERT_EQUAL_UINT16(0, waterfallColumnForBin(0, 10, 224));
    TEST_ASSERT_EQUAL_UINT16(223, waterfallColumnForBin(9, 10, 224));
}

void test_column_for_bin_identity_when_equal() {
    for (uint16_t b = 0; b < 221; b++) {
        TEST_ASSERT_EQUAL_UINT16(b, waterfallColumnForBin(b, 221, 221));
    }
}

void test_aggregate_row_max_and_endpoints() {
    int16_t bins[4] = {-900, -400, -850, -700}; // tenths-of-dBm
    uint8_t columns[2];
    waterfallAggregateRow(bins, 4, columns, 2);
    // bins 0,1 -> col 0 (endpoint-anchored: (4-1)=3 bins span, col count 2);
    // verify no fabricated data and the stronger (less negative) bin wins.
    TEST_ASSERT_NOT_EQUAL(WATERFALL_NO_DATA, columns[0]);
    TEST_ASSERT_NOT_EQUAL(WATERFALL_NO_DATA, columns[1]);
    TEST_ASSERT_EQUAL_UINT8(waterfallEncodeRssiByte(-400), columns[0]);
}

void test_aggregate_row_single_bin_only_populates_its_own_column() {
    // A single real bin has no second point to anchor a "last column" slope
    // against, so it must not stretch to fabricate the rest of the row —
    // §8.6 "no fabricated vertical texture" wins over filling every column.
    int16_t bins[1] = {-900};
    uint8_t columns[3];
    waterfallAggregateRow(bins, 1, columns, 3);
    TEST_ASSERT_EQUAL_UINT8(waterfallEncodeRssiByte(-900), columns[0]);
    TEST_ASSERT_EQUAL_UINT8(WATERFALL_NO_DATA, columns[1]);
    TEST_ASSERT_EQUAL_UINT8(WATERFALL_NO_DATA, columns[2]);
}

void test_history_push_and_recency_order() {
    WaterfallHistory history;
    int16_t rowA[2] = {-900, -900};
    int16_t rowB[2] = {-500, -500};
    waterfallHistoryPushRow(history, rowA, 2, 1000);
    waterfallHistoryPushRow(history, rowB, 2, 2000);

    TEST_ASSERT_EQUAL_UINT8(2, history.count);
    const WaterfallRow *newest = waterfallHistoryRowAt(history, 0);
    const WaterfallRow *oldest = waterfallHistoryRowAt(history, 1);
    TEST_ASSERT_NOT_NULL(newest);
    TEST_ASSERT_NOT_NULL(oldest);
    TEST_ASSERT_EQUAL_UINT32(2000, newest->rx_millis);
    TEST_ASSERT_EQUAL_UINT32(1000, oldest->rx_millis);
    TEST_ASSERT_NULL(waterfallHistoryRowAt(history, 2));
}

void test_history_ring_wraps_and_evicts_oldest() {
    WaterfallHistory history;
    int16_t row[1] = {-900};
    for (uint32_t i = 0; i < WATERFALL_MAX_ROWS + 3; i++) {
        row[0] = (int16_t)(-1000 + (int16_t)i);
        waterfallHistoryPushRow(history, row, 1, 1000 + i);
    }
    TEST_ASSERT_EQUAL_UINT8(WATERFALL_MAX_ROWS, history.count);
    const WaterfallRow *newest = waterfallHistoryRowAt(history, 0);
    const WaterfallRow *last = waterfallHistoryRowAt(history, WATERFALL_MAX_ROWS - 1);
    TEST_ASSERT_NOT_NULL(newest);
    TEST_ASSERT_NOT_NULL(last);
    TEST_ASSERT_EQUAL_UINT32(1000 + WATERFALL_MAX_ROWS + 2, newest->rx_millis);
    TEST_ASSERT_EQUAL_UINT32(1000 + 3, last->rx_millis); // 3 oldest rows evicted
}

void test_history_row_bin_count_narrower_than_max_leaves_no_data_tail() {
    WaterfallHistory history;
    int16_t row[3] = {-900, -800, -700};
    waterfallHistoryPushRow(history, row, 3, 500);
    const WaterfallRow *r = waterfallHistoryRowAt(history, 0);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT16(3, r->bin_count);
    TEST_ASSERT_EQUAL_UINT8(WATERFALL_NO_DATA, r->bins[3]);
    TEST_ASSERT_EQUAL_UINT8(WATERFALL_NO_DATA, r->bins[WATERFALL_MAX_BINS - 1]);
}

// --- capture marks (v1.0.3/v1.0.4) -------------------------------------
// The green "a packet was decoded here" channel is deliberately separate
// storage from bins[], so these assert it stays independent of the energy
// data and refuses to plot a capture it can't honestly place.

void test_capture_defaults_to_none() {
    WaterfallHistory history;
    int16_t bins[4] = {0, 0, 0, 0};
    waterfallHistoryPushRow(history, bins, 4, 100);
    const WaterfallRow *row = waterfallHistoryRowAt(history, 0);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_UINT16(WATERFALL_NO_CAPTURE_BIN, row->capture_bin);
    TEST_ASSERT_EQUAL_UINT8(0, row->capture_count);
}

void test_capture_recorded_independently_of_energy() {
    WaterfallHistory history;
    // Every bin quiet: a capture must still record, since Pass A missing
    // the traffic is the normal case this exists for.
    int16_t bins[8];
    for (int i = 0; i < 8; i++) bins[i] = WATERFALL_RSSI_FLOOR_DBM_X10;
    waterfallHistoryPushRow(history, bins, 8, 100, /*capture_bin=*/3, /*capture_count=*/5);
    const WaterfallRow *row = waterfallHistoryRowAt(history, 0);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_UINT16(3, row->capture_bin);
    TEST_ASSERT_EQUAL_UINT8(5, row->capture_count);
    // ...and the energy channel is untouched by it.
    TEST_ASSERT_EQUAL_UINT8(0, row->bins[3]);
}

void test_capture_outside_swept_range_is_refused_not_clamped() {
    WaterfallHistory history;
    int16_t bins[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // bin 9 does not exist in an 8-bin row — clamping it onto bin 7 would
    // draw a packet mark at a frequency it never happened at.
    waterfallHistoryPushRow(history, bins, 8, 100, /*capture_bin=*/9, /*capture_count=*/2);
    const WaterfallRow *row = waterfallHistoryRowAt(history, 0);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_UINT16(WATERFALL_NO_CAPTURE_BIN, row->capture_bin);
    TEST_ASSERT_EQUAL_UINT8(0, row->capture_count);
}

void test_zero_count_records_no_capture_bin() {
    WaterfallHistory history;
    int16_t bins[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    waterfallHistoryPushRow(history, bins, 8, 100, /*capture_bin=*/3, /*capture_count=*/0);
    const WaterfallRow *row = waterfallHistoryRowAt(history, 0);
    TEST_ASSERT_NOT_NULL(row);
    TEST_ASSERT_EQUAL_UINT16(WATERFALL_NO_CAPTURE_BIN, row->capture_bin);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_budget_ceiling_matches_design_doc);
    RUN_TEST(test_rssi_byte_round_trip_and_clamp);
    RUN_TEST(test_column_for_bin_endpoints_downsampling);
    RUN_TEST(test_column_for_bin_endpoints_upsampling);
    RUN_TEST(test_column_for_bin_identity_when_equal);
    RUN_TEST(test_aggregate_row_max_and_endpoints);
    RUN_TEST(test_aggregate_row_single_bin_only_populates_its_own_column);
    RUN_TEST(test_history_push_and_recency_order);
    RUN_TEST(test_history_ring_wraps_and_evicts_oldest);
    RUN_TEST(test_history_row_bin_count_narrower_than_max_leaves_no_data_tail);
    RUN_TEST(test_capture_defaults_to_none);
    RUN_TEST(test_capture_recorded_independently_of_energy);
    RUN_TEST(test_capture_outside_swept_range_is_refused_not_clamped);
    RUN_TEST(test_zero_count_records_no_capture_bin);
    return UNITY_END();
}
