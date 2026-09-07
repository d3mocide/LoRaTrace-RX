// Audit A12: a node name arriving over the air can be "=1+1" or
// "=cmd|'/c calc'!A1", and correct CSV quoting does not stop a spreadsheet
// executing it. These tests are as much about what must NOT change — every
// negative dBm reading in the file starts with '-'.

#include <unity.h>

#include <string.h>

#include "../../src/csv_safe_export.h"

void setUp(void) {}
void tearDown(void) {}

static const char *safe(const char *in) {
    static char out[512];
    size_t n = 0;
    if (!csvSafeLine(in, out, sizeof(out), n)) return "<REFUSED>";
    // The reported length must always match what actually landed.
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
    return out;
}

void test_negative_numbers_are_left_alone() {
    // The reason this is not simply "prefix anything starting with -".
    TEST_ASSERT_EQUAL_STRING("1,-95.5,-12,+3,0", safe("1,-95.5,-12,+3,0"));
    TEST_ASSERT_TRUE(csvFieldIsNumber("-95.5", 5));
    TEST_ASSERT_TRUE(csvFieldIsNumber("+3", 2));
    TEST_ASSERT_FALSE(csvFieldIsNumber("-95.5.1", 7));
    TEST_ASSERT_FALSE(csvFieldIsNumber("-", 1));
    TEST_ASSERT_FALSE(csvFieldIsNumber("", 0));
}

void test_a_formula_name_becomes_text() {
    TEST_ASSERT_EQUAL_STRING("a,\"'=1+1\",b", safe("a,=1+1,b"));
    TEST_ASSERT_EQUAL_STRING("a,\"'@SUM(1)\",b", safe("a,@SUM(1),b"));
    TEST_ASSERT_EQUAL_STRING("a,\"'-2+3+cmd\",b", safe("a,-2+3+cmd,b"));
    // '+1x' is not a number, so it is a formula as far as a spreadsheet is
    // concerned even though it starts like one.
    TEST_ASSERT_EQUAL_STRING("a,\"'+1x\",b", safe("a,+1x,b"));
}

void test_an_already_quoted_formula_is_still_neutralised() {
    // The realistic case: nodeIdentityFormatCsv() quotes names already.
    TEST_ASSERT_EQUAL_STRING("1,\"'=1+1\",x", safe("1,\"=1+1\",x"));
    // ...and its quoting, including embedded commas and doubled quotes,
    // survives intact.
    TEST_ASSERT_EQUAL_STRING("1,\"'=a,b\",x", safe("1,\"=a,b\",x"));
    TEST_ASSERT_EQUAL_STRING("1,\"say \"\"hi\"\"\",x", safe("1,\"say \"\"hi\"\"\",x"));
}

void test_whitespace_hidden_formulas_are_caught() {
    TEST_ASSERT_EQUAL_STRING("a,\"'\t=1+1\",b", safe("a,\t=1+1,b"));
    TEST_ASSERT_EQUAL_STRING("a,\"'\r=1+1\",b", safe("a,\r=1+1,b"));
}

void test_ordinary_rows_are_unchanged() {
    const char *row =
        "2026-08-23T04:15:00Z,!a1b2c3d4,meshtastic,Portland Node,PDX,-95.5,7.25,37.7,-122.4,1,7";
    TEST_ASSERT_EQUAL_STRING(row, safe(row));
    TEST_ASSERT_EQUAL_STRING("", safe(""));
    TEST_ASSERT_EQUAL_STRING(",,", safe(",,"));
}

void test_a_line_that_does_not_fit_is_refused() {
    char out[8];
    size_t n = 0;
    // Refused, never a truncated row.
    TEST_ASSERT_FALSE(csvSafeLine("a,=1+1,bbbbbbbbbb", out, sizeof(out), n));
    TEST_ASSERT_FALSE(csvSafeLine("abc", out, 0, n));
    TEST_ASSERT_FALSE(csvSafeLine(nullptr, out, sizeof(out), n));
    // A blank line succeeds and writes nothing, which is not the same thing.
    TEST_ASSERT_TRUE(csvSafeLine("", out, sizeof(out), n));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_negative_numbers_are_left_alone);
    RUN_TEST(test_a_formula_name_becomes_text);
    RUN_TEST(test_an_already_quoted_formula_is_still_neutralised);
    RUN_TEST(test_whitespace_hidden_formulas_are_caught);
    RUN_TEST(test_ordinary_rows_are_unchanged);
    RUN_TEST(test_a_line_that_does_not_fit_is_refused);
    return UNITY_END();
}
