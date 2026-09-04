#include <unity.h>

#include <string.h>

#include "../../src/config_line.h"

// config_line.h is the shared half of the settings-file parsers
// (docs/research/2026-09-04-project-audit.md, L1). These cover the line
// splitter and the strict integer parse; test_settings_parse/ covers what
// each module does with the results.

void setUp(void) {}
void tearDown(void) {}

// --- configLineSplit -----------------------------------------------------

void test_splits_key_and_value() {
    char k[32], v[32];
    TEST_ASSERT_TRUE(configLineSplit("region=US", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("region", k);
    TEST_ASSERT_EQUAL_STRING("US", v);
}

void test_trims_surrounding_whitespace() {
    char k[32], v[32];
    TEST_ASSERT_TRUE(configLineSplit("  region  =   US  \r\n", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("region", k);
    TEST_ASSERT_EQUAL_STRING("US", v);
}

void test_rejects_blank_and_comment_lines() {
    char k[32], v[32];
    TEST_ASSERT_FALSE(configLineSplit("", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_FALSE(configLineSplit("   \t\r\n", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_FALSE(configLineSplit("# region=US", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_FALSE(configLineSplit("   # indented comment", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_FALSE(configLineSplit(nullptr, k, sizeof(k), v, sizeof(v)));
}

void test_rejects_malformed_pairs() {
    char k[32], v[32];
    TEST_ASSERT_FALSE(configLineSplit("region", k, sizeof(k), v, sizeof(v)));       // no '='
    TEST_ASSERT_FALSE(configLineSplit("=US", k, sizeof(k), v, sizeof(v)));          // empty key
    TEST_ASSERT_FALSE(configLineSplit("region=", k, sizeof(k), v, sizeof(v)));      // empty value
    TEST_ASSERT_FALSE(configLineSplit("region=   ", k, sizeof(k), v, sizeof(v)));   // blank value
    TEST_ASSERT_FALSE(configLineSplit("   =   ", k, sizeof(k), v, sizeof(v)));
}

void test_splits_on_first_equals_so_values_may_contain_one() {
    char k[32], v[32];
    TEST_ASSERT_TRUE(configLineSplit("key=a=b", k, sizeof(k), v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("key", k);
    TEST_ASSERT_EQUAL_STRING("a=b", v);
}

// A key that doesn't fit must be refused outright: silently truncating it
// could make it collide with a different, real key.
void test_rejects_oversized_halves_rather_than_truncating() {
    char small[4], v[32];
    TEST_ASSERT_FALSE(configLineSplit("region=US", small, sizeof(small), v, sizeof(v)));
    char k[32], smallv[2];
    TEST_ASSERT_FALSE(configLineSplit("region=US", k, sizeof(k), smallv, sizeof(smallv)));
}

// --- configParseLong -----------------------------------------------------

void test_parses_plain_and_signed_integers() {
    long out = 0;
    TEST_ASSERT_TRUE(configParseLong("0", out));
    TEST_ASSERT_EQUAL_INT32(0, out);
    TEST_ASSERT_TRUE(configParseLong("350", out));
    TEST_ASSERT_EQUAL_INT32(350, out);
    TEST_ASSERT_TRUE(configParseLong("-42", out));
    TEST_ASSERT_EQUAL_INT32(-42, out);
    TEST_ASSERT_TRUE(configParseLong("+7", out));
    TEST_ASSERT_EQUAL_INT32(7, out);
}

// The bug this function exists to prevent: Arduino's String::toInt()
// returns 0 for unparseable input, so a corrupt line used to be honoured as
// a real "0" wherever 0 was in range.
void test_rejects_non_numeric_instead_of_yielding_zero() {
    long out = 12345;
    TEST_ASSERT_FALSE(configParseLong("garbage", out));
    TEST_ASSERT_FALSE(configParseLong("12abc", out));
    TEST_ASSERT_FALSE(configParseLong("abc12", out));
    TEST_ASSERT_FALSE(configParseLong("1.5", out));
    TEST_ASSERT_FALSE(configParseLong("", out));
    TEST_ASSERT_FALSE(configParseLong("-", out));
    TEST_ASSERT_FALSE(configParseLong(" 12", out)); // caller trims; this is raw
    TEST_ASSERT_FALSE(configParseLong(nullptr, out));
    TEST_ASSERT_EQUAL_INT32(12345, out); // untouched on every failure
}

void test_rejects_overflow_identically_on_host_and_target() {
    long out = 0;
    TEST_ASSERT_TRUE(configParseLong("2147483647", out));
    TEST_ASSERT_EQUAL_INT32(2147483647L, out);
    TEST_ASSERT_FALSE(configParseLong("2147483648", out));
    TEST_ASSERT_FALSE(configParseLong("99999999999999999999", out));
}

void test_range_check_rejects_outside_bounds() {
    long out = 0;
    TEST_ASSERT_TRUE(configParseLongInRange("5", 5, 100, out));
    TEST_ASSERT_EQUAL_INT32(5, out);
    TEST_ASSERT_TRUE(configParseLongInRange("100", 5, 100, out));
    TEST_ASSERT_FALSE(configParseLongInRange("4", 5, 100, out));
    TEST_ASSERT_FALSE(configParseLongInRange("101", 5, 100, out));
    TEST_ASSERT_FALSE(configParseLongInRange("garbage", 5, 100, out));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_splits_key_and_value);
    RUN_TEST(test_trims_surrounding_whitespace);
    RUN_TEST(test_rejects_blank_and_comment_lines);
    RUN_TEST(test_rejects_malformed_pairs);
    RUN_TEST(test_splits_on_first_equals_so_values_may_contain_one);
    RUN_TEST(test_rejects_oversized_halves_rather_than_truncating);
    RUN_TEST(test_parses_plain_and_signed_integers);
    RUN_TEST(test_rejects_non_numeric_instead_of_yielding_zero);
    RUN_TEST(test_rejects_overflow_identically_on_host_and_target);
    RUN_TEST(test_range_check_rejects_outside_bounds);
    return UNITY_END();
}
