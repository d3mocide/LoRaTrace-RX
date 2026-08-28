#include <unity.h>

#include <string.h>

#include "../../src/serial_control_protocol.h"

void test_valid_frame_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 17,
                                           SerialControlOpcode::PROBE_START, "-") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_UINT16(17, frame.sequence);
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::PROBE_START, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("-", frame.argument);
}

void test_bad_crc_and_extra_tokens_are_rejected() {
    char badCrc[] = "@LTRX/1 1 STATUS - 0000";
    SerialControlFrame frame;
    TEST_ASSERT_FALSE(serialControlParseFrame(badCrc, frame));

    char extraBody[] = "@LTRX/1 1 STATUS - extra";
    char extra[SERIAL_CONTROL_FRAME_MAX];
    snprintf(extra, sizeof(extra), "%s %04X", extraBody,
             (unsigned)serialControlCrc16(extraBody, strlen(extraBody)));
    TEST_ASSERT_FALSE(serialControlParseFrame(extra, frame));
}

void test_parser_rejects_unsupported_version_and_sequence_overflow() {
    char wrongVersion[] = "@LTRX/2 1 STATUS - 3C3A";
    SerialControlFrame frame;
    TEST_ASSERT_FALSE(serialControlParseFrame(wrongVersion, frame));

    char overflow[] = "@LTRX/1 65536 STATUS - 0000";
    TEST_ASSERT_FALSE(serialControlParseFrame(overflow, frame));
}

void test_formatter_refuses_space_containing_argument() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_EQUAL_UINT(0, serialControlFormatFrame(line, sizeof(line), 1,
                                                     SerialControlOpcode::ACK, "NOT SAFE"));
}

void test_bench_cad_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 18,
                                           SerialControlOpcode::BENCH_CAD, "16") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_CAD, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("16", frame.argument);
}

void test_sweep_opcodes_round_trip() {
    char startLine[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(startLine, sizeof(startLine), 19,
                                           SerialControlOpcode::SWEEP_START, "-") > 0);
    SerialControlFrame startFrame;
    TEST_ASSERT_TRUE(serialControlParseFrame(startLine, startFrame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::SWEEP_START, (int)startFrame.opcode);

    char cancelLine[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(cancelLine, sizeof(cancelLine), 20,
                                           SerialControlOpcode::SWEEP_CANCEL, "-") > 0);
    SerialControlFrame cancelFrame;
    TEST_ASSERT_TRUE(serialControlParseFrame(cancelLine, cancelFrame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::SWEEP_CANCEL, (int)cancelFrame.opcode);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_frame_round_trips);
    RUN_TEST(test_bad_crc_and_extra_tokens_are_rejected);
    RUN_TEST(test_parser_rejects_unsupported_version_and_sequence_overflow);
    RUN_TEST(test_formatter_refuses_space_containing_argument);
    RUN_TEST(test_bench_cad_opcode_round_trips);
    RUN_TEST(test_sweep_opcodes_round_trip);
    return UNITY_END();
}
