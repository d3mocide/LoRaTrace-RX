#include <unity.h>

#include <string.h>

#include "../../src/low_profile_protocol.h"

void test_valid_frame_round_trips() {
    char line[LOW_PROFILE_FRAME_MAX];
    TEST_ASSERT_TRUE(lowProfileFormatFrame(line, sizeof(line), 17,
                                           LowProfileOpcode::PROBE_START, "-") > 0);
    LowProfileFrame frame;
    TEST_ASSERT_TRUE(lowProfileParseFrame(line, frame));
    TEST_ASSERT_EQUAL_UINT16(17, frame.sequence);
    TEST_ASSERT_EQUAL_INT((int)LowProfileOpcode::PROBE_START, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("-", frame.argument);
}

void test_bad_crc_and_extra_tokens_are_rejected() {
    char badCrc[] = "@LTRX/1 1 STATUS - 0000";
    LowProfileFrame frame;
    TEST_ASSERT_FALSE(lowProfileParseFrame(badCrc, frame));

    char extraBody[] = "@LTRX/1 1 STATUS - extra";
    char extra[LOW_PROFILE_FRAME_MAX];
    snprintf(extra, sizeof(extra), "%s %04X", extraBody,
             (unsigned)lowProfileCrc16(extraBody, strlen(extraBody)));
    TEST_ASSERT_FALSE(lowProfileParseFrame(extra, frame));
}

void test_parser_rejects_unsupported_version_and_sequence_overflow() {
    char wrongVersion[] = "@LTRX/2 1 STATUS - 3C3A";
    LowProfileFrame frame;
    TEST_ASSERT_FALSE(lowProfileParseFrame(wrongVersion, frame));

    char overflow[] = "@LTRX/1 65536 STATUS - 0000";
    TEST_ASSERT_FALSE(lowProfileParseFrame(overflow, frame));
}

void test_formatter_refuses_space_containing_argument() {
    char line[LOW_PROFILE_FRAME_MAX];
    TEST_ASSERT_EQUAL_UINT(0, lowProfileFormatFrame(line, sizeof(line), 1,
                                                     LowProfileOpcode::ACK, "NOT SAFE"));
}

void test_bench_cad_opcode_round_trips() {
    char line[LOW_PROFILE_FRAME_MAX];
    TEST_ASSERT_TRUE(lowProfileFormatFrame(line, sizeof(line), 18,
                                           LowProfileOpcode::BENCH_CAD, "16") > 0);
    LowProfileFrame frame;
    TEST_ASSERT_TRUE(lowProfileParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)LowProfileOpcode::BENCH_CAD, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("16", frame.argument);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_frame_round_trips);
    RUN_TEST(test_bad_crc_and_extra_tokens_are_rejected);
    RUN_TEST(test_parser_rejects_unsupported_version_and_sequence_overflow);
    RUN_TEST(test_formatter_refuses_space_containing_argument);
    RUN_TEST(test_bench_cad_opcode_round_trips);
    return UNITY_END();
}
