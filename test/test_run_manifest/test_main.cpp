// Audit A18/A25. A run folder copied off the card is the whole evidence
// package, and until this existed nothing inside it recorded which firmware,
// which region, which modem parameters, or how many separate boots wrote it.

#include <unity.h>

#include <string.h>

#include "../../src/run_manifest.h"

namespace {

uint8_t seq = 0;
uint8_t countingByte() { return seq++; }

RunManifestSession sample() {
    RunManifestSession s;
    s.session_id = "0123456789abcdef0123456789abcdef";
    s.firmware_version = "1.1.0";
    s.build_rev = "259e6e9-dirty";
    s.board = "cardputer-adv";
    s.radio = "sx1262";
    s.profile = "meshtastic";
    s.region = "us";
    s.timestamp_utc = "2026-09-07T18:03:00Z";
    s.run = 89;
    s.uptime_ms = 4200;
    s.freq_mhz = 906.875f;
    s.bw_khz = 250.0f;
    s.sf = 7;
    s.cr_denom = 5;
    s.sync_word = 0x2B;
    s.capture_window_ms = 2000;
    s.sweep_margin_dbm_x10 = 35;
    return s;
}

bool hasLine(const char *text, const char *line) {
    const char *found = strstr(text, line);
    if (found == nullptr) return false;
    // Must be a whole line, not a prefix of a longer key's value.
    const bool atLineStart = found == text || found[-1] == '\n';
    return atLineStart && found[strlen(line)] == '\n';
}

} // namespace

void setUp(void) { seq = 0; }
void tearDown(void) {}

void test_the_session_block_carries_build_and_radio_provenance() {
    char out[1024];
    const size_t n = runManifestFormatSession(sample(), out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_size_t(strlen(out), n);
    TEST_ASSERT_TRUE(hasLine(out, "schema=1"));
    TEST_ASSERT_TRUE(hasLine(out, "session_id=0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_TRUE(hasLine(out, "run=89"));
    TEST_ASSERT_TRUE(hasLine(out, "firmware_version=1.1.0"));
    // The dirty marker has to survive: a hardware bug report naming a build
    // that was never committed is exactly what build_rev exists for.
    TEST_ASSERT_TRUE(hasLine(out, "build_rev=259e6e9-dirty"));
    TEST_ASSERT_TRUE(hasLine(out, "radio=sx1262"));
}

void test_the_modem_parameters_are_the_resolved_ones() {
    // config.txt at the SD root can be edited after the run; these cannot.
    char out[1024];
    TEST_ASSERT_TRUE(runManifestFormatSession(sample(), out, sizeof(out)) > 0);
    TEST_ASSERT_TRUE(hasLine(out, "freq_mhz=906.875000"));
    TEST_ASSERT_TRUE(hasLine(out, "sf=7"));
    TEST_ASSERT_TRUE(hasLine(out, "bw_khz=250.00"));
    TEST_ASSERT_TRUE(hasLine(out, "cr_denom=5"));
    TEST_ASSERT_TRUE(hasLine(out, "sync_word=0x2B"));
    TEST_ASSERT_TRUE(hasLine(out, "region=us"));
    TEST_ASSERT_TRUE(hasLine(out, "capture_window_ms=2000"));
    TEST_ASSERT_TRUE(hasLine(out, "sweep_margin_dbm_x10=35"));
}

void test_timestamp_and_position_semantics_are_stated_not_assumed() {
    // The two things a host analyst most reliably gets wrong about this data.
    char out[1024];
    TEST_ASSERT_TRUE(runManifestFormatSession(sample(), out, sizeof(out)) > 0);
    TEST_ASSERT_TRUE(strstr(out, "rx_millis is device uptime") != nullptr);
    TEST_ASSERT_TRUE(strstr(out, "never the transmitter") != nullptr);
}

void test_a_rejoined_run_says_so() {
    // Two boots sharing one folder are two measurement sessions; the folder
    // name cannot distinguish them, so the block must.
    RunManifestSession s = sample();
    s.sd_recovered = true;
    char out[1024];
    TEST_ASSERT_TRUE(runManifestFormatSession(s, out, sizeof(out)) > 0);
    TEST_ASSERT_TRUE(hasLine(out, "rejoined_existing_run=1"));
}

void test_a_missing_field_renders_empty_not_garbage() {
    RunManifestSession s; // every string defaulted
    s.session_id = nullptr;
    s.timestamp_utc = nullptr;
    char out[1024];
    TEST_ASSERT_TRUE(runManifestFormatSession(s, out, sizeof(out)) > 0);
    TEST_ASSERT_TRUE(hasLine(out, "session_id="));
    TEST_ASSERT_TRUE(hasLine(out, "started_utc="));
}

void test_truncation_is_refused_rather_than_written_short() {
    char out[64];
    TEST_ASSERT_EQUAL_size_t(0, runManifestFormatSession(sample(), out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, runManifestFormatSession(sample(), out, 0));
}

void test_a_session_id_is_128_bits_of_hex() {
    char id[RUN_SESSION_ID_BUF] = {0};
    runSessionIdGenerate(id, sizeof(id), countingByte);
    TEST_ASSERT_EQUAL_size_t(RUN_SESSION_ID_LEN, strlen(id));
    TEST_ASSERT_TRUE(runSessionIdIsValid(id));
    TEST_ASSERT_EQUAL_STRING("000102030405060708090a0b0c0d0e0f", id);

    TEST_ASSERT_FALSE(runSessionIdIsValid(nullptr));
    TEST_ASSERT_FALSE(runSessionIdIsValid(""));
    TEST_ASSERT_FALSE(runSessionIdIsValid("0123456789abcdef0123456789abcde"));  // short
    TEST_ASSERT_FALSE(runSessionIdIsValid("0123456789abcdef0123456789abcdef0")); // long
    TEST_ASSERT_FALSE(runSessionIdIsValid("0123456789ABCDEF0123456789abcdef")); // uppercase
    TEST_ASSERT_FALSE(runSessionIdIsValid("0123456789abcdef0123456789abcdeg")); // not hex
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_the_session_block_carries_build_and_radio_provenance);
    RUN_TEST(test_the_modem_parameters_are_the_resolved_ones);
    RUN_TEST(test_timestamp_and_position_semantics_are_stated_not_assumed);
    RUN_TEST(test_a_rejoined_run_says_so);
    RUN_TEST(test_a_missing_field_renders_empty_not_garbage);
    RUN_TEST(test_truncation_is_refused_rather_than_written_short);
    RUN_TEST(test_a_session_id_is_128_bits_of_hex);
    return UNITY_END();
}
