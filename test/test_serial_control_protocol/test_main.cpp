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

void test_bench_sweep_margin_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 21,
                                              SerialControlOpcode::BENCH_SWEEP_MARGIN, "150") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_SWEEP_MARGIN, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("150", frame.argument);
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

void test_sd_retry_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 22,
                                           SerialControlOpcode::SD_RETRY, "-") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::SD_RETRY, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("-", frame.argument);
}

void test_bench_pass_b_cad_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 23,
                                           SerialControlOpcode::BENCH_PASS_B_CAD, "7") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_PASS_B_CAD, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("7", frame.argument);
}

void test_bench_focus_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 24,
                                           SerialControlOpcode::BENCH_FOCUS,
                                           "43:500:8") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_FOCUS, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("43:500:8", frame.argument);
}

void test_bench_focus_result_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 25,
                                           SerialControlOpcode::BENCH_FOCUS_RESULT,
                                           "-") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_FOCUS_RESULT, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("-", frame.argument);
}

void test_bench_focus_cancel_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 26,
                                           SerialControlOpcode::BENCH_FOCUS_CANCEL,
                                           "-") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_FOCUS_CANCEL, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("-", frame.argument);
}

void test_bench_action_opcode_round_trips() {
    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 27,
                                           SerialControlOpcode::BENCH_ACTION,
                                           "CELL:START") > 0);
    SerialControlFrame frame;
    TEST_ASSERT_TRUE(serialControlParseFrame(line, frame));
    TEST_ASSERT_EQUAL_INT((int)SerialControlOpcode::BENCH_ACTION, (int)frame.opcode);
    TEST_ASSERT_EQUAL_STRING("CELL:START", frame.argument);
}

void test_argument_budget_covers_the_longest_opcode_name() {
    // SERIAL_CONTROL_ARGUMENT_MAX is derived from this; a longer opcode name
    // added later would quietly overrun every buffer sized from it.
    for (int value = (int)SerialControlOpcode::HELLO; value <= (int)SerialControlOpcode::ERROR;
         ++value) {
        const char *name = serialControlOpcodeName((SerialControlOpcode)value);
        TEST_ASSERT_TRUE(strlen(name) <= SERIAL_CONTROL_OPCODE_NAME_MAX);
    }
}

void test_formatter_frames_a_saturated_status_argument() {
    // STATUS is the longest frame the device emits, and an over-long one is
    // dropped silently rather than truncated -- so the budget has to hold at
    // the maximum, not at today's typical field widths.
    char argument[SERIAL_CONTROL_ARGUMENT_MAX + 2];
    memset(argument, 'x', sizeof(argument) - 1);
    argument[sizeof(argument) - 1] = '\0';

    char line[SERIAL_CONTROL_FRAME_MAX];
    TEST_ASSERT_EQUAL_UINT(0, serialControlFormatFrame(line, sizeof(line), 65535,
                                                       SerialControlOpcode::BENCH_PASS_B_CAD_RESULT,
                                                       argument));
    argument[SERIAL_CONTROL_ARGUMENT_MAX] = '\0';
    TEST_ASSERT_TRUE(serialControlFormatFrame(line, sizeof(line), 65535,
                                              SerialControlOpcode::BENCH_PASS_B_CAD_RESULT,
                                              argument) > 0);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_frame_round_trips);
    RUN_TEST(test_bad_crc_and_extra_tokens_are_rejected);
    RUN_TEST(test_parser_rejects_unsupported_version_and_sequence_overflow);
    RUN_TEST(test_formatter_refuses_space_containing_argument);
    RUN_TEST(test_bench_cad_opcode_round_trips);
    RUN_TEST(test_bench_sweep_margin_opcode_round_trips);
    RUN_TEST(test_sweep_opcodes_round_trip);
    RUN_TEST(test_sd_retry_opcode_round_trips);
    RUN_TEST(test_bench_pass_b_cad_opcode_round_trips);
    RUN_TEST(test_bench_focus_opcode_round_trips);
    RUN_TEST(test_bench_focus_result_opcode_round_trips);
    RUN_TEST(test_bench_focus_cancel_opcode_round_trips);
    RUN_TEST(test_bench_action_opcode_round_trips);
    RUN_TEST(test_argument_budget_covers_the_longest_opcode_name);
    RUN_TEST(test_formatter_frames_a_saturated_status_argument);
    return UNITY_END();
}
