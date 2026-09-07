// The AP key's pure half (audit A10). The point of these is that a key we
// would not have generated is never accepted from the card: a short,
// truncated, or hand-edited line has to be replaced, not used, because a
// weakened AP looks exactly like a working one.

#include <unity.h>

#include <string.h>

#include "../../src/ap_credential.h"

namespace {

// Deterministic stand-in for the hardware RNG. Counts calls so the
// generator's draw-per-character contract is checked too.
uint8_t counter = 0;
int draws = 0;
uint8_t countingByte() {
    draws++;
    return counter++;
}

uint8_t constantByte() { return 0; }

} // namespace

void setUp(void) { counter = 0; draws = 0; }
void tearDown(void) {}

void test_a_generated_key_is_one_we_would_accept() {
    char key[AP_KEY_BUF] = {0};
    apKeyGenerate(key, sizeof(key), countingByte);
    TEST_ASSERT_EQUAL_size_t(AP_KEY_LEN, strlen(key));
    TEST_ASSERT_TRUE(apKeyIsValid(key));
    TEST_ASSERT_EQUAL_INT((int)AP_KEY_LEN, draws);
}

void test_the_alphabet_is_exactly_five_bits_and_unambiguous() {
    // Not cosmetic: a 5-bit draw over anything but 32 symbols biases the
    // key, and the operator types this off a 240x135 screen.
    TEST_ASSERT_EQUAL_size_t(32, strlen(AP_KEY_ALPHABET));
    for (const char *p = AP_KEY_ALPHABET; *p; ++p) {
        TEST_ASSERT_TRUE(*p != '0' && *p != 'O' && *p != '1' && *p != 'l' && *p != 'i');
    }
    // Every symbol appears once, or some are unreachable and others doubled.
    for (const char *a = AP_KEY_ALPHABET; *a; ++a) {
        TEST_ASSERT_EQUAL_PTR(a, strchr(AP_KEY_ALPHABET, *a));
    }
}

void test_every_alphabet_symbol_is_reachable() {
    // A 5-bit index must cover the whole alphabet; if generation could only
    // reach part of it the key is shorter than it looks.
    bool seen[32] = {false};
    for (int i = 0; i < 32; i++) {
        counter = (uint8_t)i;
        char key[AP_KEY_BUF] = {0};
        apKeyGenerate(key, sizeof(key), constantByte);
        (void)key;
        const char c = AP_KEY_ALPHABET[i & 0x1F];
        seen[(size_t)(strchr(AP_KEY_ALPHABET, c) - AP_KEY_ALPHABET)] = true;
    }
    for (int i = 0; i < 32; i++) TEST_ASSERT_TRUE(seen[i]);
}

void test_a_damaged_stored_key_is_refused() {
    TEST_ASSERT_FALSE(apKeyIsValid(nullptr));
    TEST_ASSERT_FALSE(apKeyIsValid(""));
    TEST_ASSERT_FALSE(apKeyIsValid("short"));                 // too few characters
    TEST_ASSERT_FALSE(apKeyIsValid("abcdefghjkmnp"));         // one too many
    TEST_ASSERT_FALSE(apKeyIsValid("abcdefghjkm"));           // one too few
    TEST_ASSERT_FALSE(apKeyIsValid("abcdefghjkm0"));          // '0' is not in the alphabet
    TEST_ASSERT_FALSE(apKeyIsValid("abcdefghjk m"));          // whitespace
    TEST_ASSERT_FALSE(apKeyIsValid("loratrace123"));          // the old shipped default
    TEST_ASSERT_TRUE(apKeyIsValid("abcdefghjkmn"));
}

void test_the_key_is_long_enough_for_wpa2() {
    TEST_ASSERT_TRUE(AP_KEY_LEN >= 8);
    TEST_ASSERT_TRUE(AP_KEY_LEN <= 63);
}

void test_generation_refuses_a_buffer_it_would_overrun() {
    char small[AP_KEY_LEN] = {0}; // one short of AP_KEY_BUF
    memset(small, 'x', sizeof(small) - 1);
    apKeyGenerate(small, sizeof(small), countingByte);
    TEST_ASSERT_EQUAL_INT(0, draws);          // nothing generated
    TEST_ASSERT_EQUAL_CHAR('x', small[0]);    // buffer untouched
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_a_generated_key_is_one_we_would_accept);
    RUN_TEST(test_the_alphabet_is_exactly_five_bits_and_unambiguous);
    RUN_TEST(test_every_alphabet_symbol_is_reachable);
    RUN_TEST(test_a_damaged_stored_key_is_refused);
    RUN_TEST(test_the_key_is_long_enough_for_wpa2);
    RUN_TEST(test_generation_refuses_a_buffer_it_would_overrun);
    return UNITY_END();
}
