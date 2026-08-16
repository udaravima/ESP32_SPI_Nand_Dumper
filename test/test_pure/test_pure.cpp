#include <unity.h>
#include <string.h>
#include "nand_chips.h"
#include "nand_addr.h"
#include "dump_header.h"

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

void test_header_pack_layout_and_crc(void) {
  dump_geometry_t g = {0};
  g.page_size = 2176; g.spare_size = 128; g.pages_per_block = 64;
  g.total_blocks = 2048; g.total_pages = 2048u * 64u;
  g.total_bytes = 2048u * 64u * 2176u;
  g.mfr_id = 0x2C; g.dev_id = 0x24; g.page_addr_bits = 6;
  g.flags = 0x00; // ecc off, single, no verify

  uint8_t buf[32];
  dump_header_pack(buf, &g);

  TEST_ASSERT_EQUAL_UINT8_ARRAY("NANDMP", buf, 6);
  TEST_ASSERT_EQUAL_UINT8(2, buf[6]);                       // proto version
  TEST_ASSERT_EQUAL_UINT16(2176, buf[8] | (buf[9] << 8));   // page_size LE
  // CRC over bytes 0..27 lands in bytes 28..31
  uint32_t crc = dump_crc32(buf, 28);
  uint32_t stored = buf[28] | (buf[29] << 8) | (buf[30] << 16) | ((uint32_t)buf[31] << 24);
  TEST_ASSERT_EQUAL_UINT32(crc, stored);
}

void test_crc32_canonical_check_value(void) {
  // The per-page seal must be standard CRC-32 (poly 0xEDB88320, init/final 0xFFFFFFFF)
  // so Python's zlib.crc32 agrees byte-for-byte. Canonical check over "123456789".
  const uint8_t msg[] = {'1','2','3','4','5','6','7','8','9'};
  TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, dump_crc32(msg, 9));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_finds_micron);
  RUN_TEST(test_lookup_returns_null_for_unknown);
  RUN_TEST(test_row_addr_block1024_does_not_alias_zero);
  RUN_TEST(test_row_addr_last_page_of_2gbit);
  RUN_TEST(test_row_addr_masks_page_field);
  RUN_TEST(test_header_pack_layout_and_crc);
  RUN_TEST(test_crc32_canonical_check_value);
  return UNITY_END();
}
