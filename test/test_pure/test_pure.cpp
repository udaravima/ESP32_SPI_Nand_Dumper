#include <unity.h>
#include "nand_chips.h"

void test_lookup_finds_micron(void) {
  const nand_chip_t *c = nand_chip_lookup(0x2C, 0x24);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_UINT16(2176, c->page_size);
  TEST_ASSERT_EQUAL_UINT16(2048, c->total_blocks);
  TEST_ASSERT_EQUAL_UINT8(6, c->page_addr_bits);
  TEST_ASSERT_FALSE(c->ecc_default_on);
}

void test_lookup_returns_null_for_unknown(void) {
  TEST_ASSERT_NULL(nand_chip_lookup(0x00, 0x00));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_finds_micron);
  RUN_TEST(test_lookup_returns_null_for_unknown);
  return UNITY_END();
}
