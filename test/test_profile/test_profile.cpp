#include <unity.h>
#include "nand_profile.h"

void test_active_profile_is_110_bytes(void) {
  // The wire contract: host packs 110 bytes, device reads 110 bytes.
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)sizeof(active_profile_t));
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)NAND_PROFILE_SIZE);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_active_profile_is_110_bytes);
  return UNITY_END();
}
