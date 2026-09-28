#include <unity.h>
#include <string.h>
#include "nand_chips.h"
#include "nand_addr.h"
#include "dump_header.h"
#include "config_store.h"
#include "sys_info.h"
#include "nand_ecc.h"

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

void test_lookup_micron_is_two_plane(void) {
  TEST_ASSERT_EQUAL_UINT8(2, nand_chip_lookup(0x2C, 0x24)->planes);
}

void test_plane_bit_sits_above_main_area(void) {
  TEST_ASSERT_EQUAL_UINT8(12, nand_plane_bit(2048));  // M79A: CA[11:0] + plane select
  TEST_ASSERT_EQUAL_UINT8(13, nand_plane_bit(4096));
}

void test_cache_column_selects_odd_block_plane(void) {
  // MT29F2G01: 64 pages/block, plane = block & 1 (RA6), plane bit 12.
  TEST_ASSERT_EQUAL_UINT16(0x0000, nand_cache_column(nand_row_addr(0, 5, 6), 6, 2, 12));
  TEST_ASSERT_EQUAL_UINT16(0x1000, nand_cache_column(nand_row_addr(1, 0, 6), 6, 2, 12));
  TEST_ASSERT_EQUAL_UINT16(0x1000, nand_cache_column(nand_row_addr(2047, 63, 6), 6, 2, 12));
  TEST_ASSERT_EQUAL_UINT16(0x0000, nand_cache_column(nand_row_addr(2046, 63, 6), 6, 2, 12));
}

void test_cache_column_is_zero_on_single_plane(void) {
  TEST_ASSERT_EQUAL_UINT16(0, nand_cache_column(nand_row_addr(1, 0, 6), 6, 1, 12));
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

void test_config_defaults_have_empty_wifi(void) {
  nand_app_config_t c;
  config_defaults(&c);
  TEST_ASSERT_EQUAL_STRING("", c.ssid);     // no password shipped in source
  TEST_ASSERT_EQUAL_STRING("", c.pass);
  TEST_ASSERT_EQUAL_UINT16(3333, c.tcp_port);
  TEST_ASSERT_EQUAL_INT32(1000000, c.spi_clock_hz);
  TEST_ASSERT_TRUE(c.verify);
  TEST_ASSERT_FALSE(c.ecc_on);
  TEST_ASSERT_EQUAL_INT32(1, c.batch_pages);
}

void test_config_validate_clamps_garbage(void) {
  nand_app_config_t c;
  config_defaults(&c);
  c.tcp_port = 0; c.spi_clock_hz = 999999999; c.read_mode = 200; c.max_retries = 5000;
  c.batch_pages = 9999;
  config_validate(&c);
  TEST_ASSERT_EQUAL_UINT16(3333, c.tcp_port);
  TEST_ASSERT_EQUAL_INT32(CONFIG_CLOCK_MAX_HZ, c.spi_clock_hz);
  TEST_ASSERT_EQUAL_UINT8(0, c.read_mode);
  TEST_ASSERT_EQUAL_INT32(CONFIG_RETRIES_MAX, c.max_retries);
  TEST_ASSERT_EQUAL_INT32(CONFIG_BATCH_MAX, c.batch_pages);
}

void test_config_validate_raises_too_low_clock(void) {
  nand_app_config_t c;
  config_defaults(&c);
  c.spi_clock_hz = 50;   // below the floor
  config_validate(&c);
  TEST_ASSERT_EQUAL_INT32(CONFIG_CLOCK_MIN_HZ, c.spi_clock_hz);
}

void test_batch_pages_scales_with_memory(void) {
  // Plenty of RAM -> hits the hard cap; tight RAM -> a handful; near-empty -> 1.
  TEST_ASSERT_EQUAL_INT(64, sys_recommend_batch_pages(4 * 1024 * 1024, 2180, 64));
  TEST_ASSERT_EQUAL_INT(3,  sys_recommend_batch_pages(40000, 2180, 64));
  TEST_ASSERT_EQUAL_INT(1,  sys_recommend_batch_pages(1000, 2180, 64));
  TEST_ASSERT_EQUAL_INT(1,  sys_recommend_batch_pages(SYS_DMA_HEADROOM, 2180, 64));
}

void test_batch_pages_respects_cap(void) {
  TEST_ASSERT_EQUAL_INT(8, sys_recommend_batch_pages(4 * 1024 * 1024, 2180, 8));
}

void test_ecc_uncorrectable_only_for_010(void) {
  TEST_ASSERT_TRUE(nand_ecc_uncorrectable(0x02));   // the only uncorrectable code
  TEST_ASSERT_FALSE(nand_ecc_uncorrectable(0x00));
  TEST_ASSERT_FALSE(nand_ecc_uncorrectable(0x01));
  TEST_ASSERT_FALSE(nand_ecc_uncorrectable(0x03));
  TEST_ASSERT_FALSE(nand_ecc_uncorrectable(0x05));
  TEST_ASSERT_TRUE(nand_ecc_uncorrectable(0xF2));   // upper (CRBSY etc.) bits masked
}

void test_ecc_refresh_recommended(void) {
  TEST_ASSERT_TRUE(nand_ecc_refresh_recommended(0x03));
  TEST_ASSERT_TRUE(nand_ecc_refresh_recommended(0x05));
  TEST_ASSERT_FALSE(nand_ecc_refresh_recommended(0x00));
  TEST_ASSERT_FALSE(nand_ecc_refresh_recommended(0x01));
  TEST_ASSERT_FALSE(nand_ecc_refresh_recommended(0x02));   // uncorrectable is not "refresh"
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_lookup_finds_micron);
  RUN_TEST(test_lookup_returns_null_for_unknown);
  RUN_TEST(test_row_addr_block1024_does_not_alias_zero);
  RUN_TEST(test_row_addr_last_page_of_2gbit);
  RUN_TEST(test_row_addr_masks_page_field);
  RUN_TEST(test_lookup_micron_is_two_plane);
  RUN_TEST(test_plane_bit_sits_above_main_area);
  RUN_TEST(test_cache_column_selects_odd_block_plane);
  RUN_TEST(test_cache_column_is_zero_on_single_plane);
  RUN_TEST(test_header_pack_layout_and_crc);
  RUN_TEST(test_crc32_canonical_check_value);
  RUN_TEST(test_config_defaults_have_empty_wifi);
  RUN_TEST(test_config_validate_clamps_garbage);
  RUN_TEST(test_config_validate_raises_too_low_clock);
  RUN_TEST(test_batch_pages_scales_with_memory);
  RUN_TEST(test_batch_pages_respects_cap);
  RUN_TEST(test_ecc_uncorrectable_only_for_010);
  RUN_TEST(test_ecc_refresh_recommended);
  return UNITY_END();
}
