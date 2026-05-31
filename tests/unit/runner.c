#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_placeholder(void) {
    TEST_ASSERT(1);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_placeholder);
    return UNITY_END();
}
