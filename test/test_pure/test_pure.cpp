#include <unity.h>
#include "nand_chips.h"
#include "nand_addr.h"

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

void test_row_addr_block1024_does_not_alias_zero(void) {
  // The headline bug: 1024<<6 = 65536 overflows uint16_t to 0.
  TEST_ASSERT_EQUAL_UINT32(65536u, nand_row_addr(1024, 0, 6));
}

void test_row_addr_last_page_of_2gbit(void) {
  // block 2047, page 63 -> 2047*64 + 63 = 131071 (17 bits)
  TEST_ASSERT_EQUAL_UINT32(131071u, nand_row_addr(2047, 63, 6));
}

void test_row_addr_masks_page_field(void) {
  TEST_ASSERT_EQUAL_UINT32((5u << 6) | 3u, nand_row_addr(5, 3, 6));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_finds_micron);
  RUN_TEST(test_lookup_returns_null_for_unknown);
  RUN_TEST(test_row_addr_block1024_does_not_alias_zero);
  RUN_TEST(test_row_addr_last_page_of_2gbit);
  RUN_TEST(test_row_addr_masks_page_field);
  return UNITY_END();
}
