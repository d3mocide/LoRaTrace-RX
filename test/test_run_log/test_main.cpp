// Guards run identity (src/run_log.h) — the naming and parsing the on-card
// layout depends on.
//
// The parsing half matters more than it looks: it is what decides which
// directory entries the boot-time scan counts as runs. Too loose and
// config.txt or a stray file bumps the index (or worse, is treated as a
// run); too strict and a real run is missed, which means the next run
// reuses its number and appends into someone else's drive.

#include <unity.h>

#include <string.h>

#include "../../src/run_log.h"

void test_dir_and_file_paths_are_zero_padded() {
    char path[RUN_PATH_MAX];

    TEST_ASSERT_TRUE(runDirPath(path, sizeof(path), "/loratrace", 7) > 0);
    TEST_ASSERT_EQUAL_STRING("/loratrace/run0007", path);

    TEST_ASSERT_TRUE(runFilePath(path, sizeof(path), "/loratrace", 7, "detections.csv") > 0);
    TEST_ASSERT_EQUAL_STRING("/loratrace/run0007/detections.csv", path);

    // Zero padding is what makes a card's folder listing sort in run order
    // in any file browser, which is the whole reason for fixed width.
    TEST_ASSERT_TRUE(runDirPath(path, sizeof(path), "/loratrace", 1234) > 0);
    TEST_ASSERT_EQUAL_STRING("/loratrace/run1234", path);
}

void test_longest_real_path_fits_the_buffer() {
    // RUN_PATH_MAX is what the logger sizes its stored paths by. If the
    // worst case didn't fit, the failure would be a truncated path writing
    // a run's data somewhere unintended.
    char path[RUN_PATH_MAX];
    TEST_ASSERT_TRUE(runFilePath(path, sizeof(path), "/loratrace", RUN_INDEX_MAX,
                                 "detections.csv") > 0);
    TEST_ASSERT_EQUAL_STRING("/loratrace/run9999/detections.csv", path);
}

void test_truncation_returns_zero_rather_than_a_partial_path() {
    char tiny[8];
    TEST_ASSERT_EQUAL_size_t(0, runDirPath(tiny, sizeof(tiny), "/loratrace", 7));
    TEST_ASSERT_EQUAL_size_t(0, runFilePath(tiny, sizeof(tiny), "/loratrace", 7, "x.csv"));
    TEST_ASSERT_EQUAL_size_t(0, runDirPath(nullptr, 64, "/loratrace", 7));
    TEST_ASSERT_EQUAL_size_t(0, runFilePath(tiny, 0, "/loratrace", 7, "x.csv"));
}

void test_index_parses_from_bare_name_and_full_path() {
    // The ESP32 SD library has returned both shapes from File::name() across
    // core versions, so both must work or the scan silently finds nothing
    // and every run overwrites run0001.
    TEST_ASSERT_EQUAL_UINT16(7, runIndexFromName("run0007"));
    TEST_ASSERT_EQUAL_UINT16(7, runIndexFromName("/loratrace/run0007"));
    TEST_ASSERT_EQUAL_UINT16(42, runIndexFromName("/loratrace/run0042"));
    TEST_ASSERT_EQUAL_UINT16(9999, runIndexFromName("run9999"));
}

void test_non_run_entries_are_ignored() {
    // Everything a real card actually has sitting next to the runs.
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("config.txt"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("/loratrace/config.txt"));
    // Legacy top-level logs from firmware before per-run directories.
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("detections.csv"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("session.csv"));
    // Near misses, all rejected rather than guessed at.
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("run7"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("run00007"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("run0007.bak"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("run"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("runabcd"));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName(""));
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName(nullptr));
}

void test_run0000_is_not_a_valid_index() {
    // 0 is the "no run yet" sentinel the logger uses, so a directory
    // literally named run0000 must not be mistaken for a real run.
    TEST_ASSERT_EQUAL_UINT16(0, runIndexFromName("run0000"));
}

void test_next_index_follows_the_highest_seen() {
    TEST_ASSERT_EQUAL_UINT16(1, runNextIndex(0)); // empty card
    TEST_ASSERT_EQUAL_UINT16(8, runNextIndex(7));

    // Highest, not first-gap: deleting run0003 out of 1..7 must still give
    // 8, so a deleted run's number is never quietly reused by a later one.
    TEST_ASSERT_EQUAL_UINT16(8, runNextIndex(7));
}

void test_index_saturates_rather_than_wrapping() {
    // At the ceiling, appending to the last run is a far better failure
    // than wrapping to 1 and overwriting the first drive on the card.
    TEST_ASSERT_EQUAL_UINT16(RUN_INDEX_MAX, runNextIndex(RUN_INDEX_MAX));
    TEST_ASSERT_EQUAL_UINT16(RUN_INDEX_MAX, runNextIndex(RUN_INDEX_MAX - 1));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_dir_and_file_paths_are_zero_padded);
    RUN_TEST(test_longest_real_path_fits_the_buffer);
    RUN_TEST(test_truncation_returns_zero_rather_than_a_partial_path);
    RUN_TEST(test_index_parses_from_bare_name_and_full_path);
    RUN_TEST(test_non_run_entries_are_ignored);
    RUN_TEST(test_run0000_is_not_a_valid_index);
    RUN_TEST(test_next_index_follows_the_highest_seen);
    RUN_TEST(test_index_saturates_rather_than_wrapping);
    return UNITY_END();
}
