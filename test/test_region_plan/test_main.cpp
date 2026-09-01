#include <unity.h>

#include "../../src/region_plan.h"

void test_region_label_us() {
    TEST_ASSERT_EQUAL_STRING("US", regionLabel(Region::US));
}

void test_region_label_global() {
    TEST_ASSERT_EQUAL_STRING("Global", regionLabel(Region::GLOBAL));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_region_label_us);
    RUN_TEST(test_region_label_global);
    return UNITY_END();
}
