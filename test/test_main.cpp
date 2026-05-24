#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void test_sample(void) {
    TEST_ASSERT_TRUE(1);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_sample);
    return UNITY_END();
}
