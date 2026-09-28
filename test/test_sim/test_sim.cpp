// Read-path tests against a simulated multi-plane NAND (no hardware).
#include <unity.h>
#include "nand_seq.h"
#include "nand_addr.h"
#include "sim_nand.h"

// A shrunken 2-plane part: 64-byte main + 8 spare, 4 pages/block, 8 blocks.
// Main size 64 puts the plane-select bit at column bit 7 (fls(64)), the same
// rule as bit 12 for the real 2048-byte page.
#define MAIN   64
#define SPARE  8
#define PAGE   (MAIN + SPARE)
#define PPB    4
#define PBITS  2
#define BLOCKS 8

static sim_nand_t chip;
static nand_seq_t seq;

void setUp(void) {
  sim_init(&chip, 2, PAGE, PPB, PBITS, nand_plane_bit(MAIN));
  nand_seq_init(&seq, sim_bus(&chip));
}
void tearDown(void) {}

// Read every page the way cmd_dump does; count pages that differ from the array.
static int dump_and_count_bad(bool quad) {
  uint8_t got[PAGE], want[PAGE];
  int bad = 0;
  for (uint32_t b = 0; b < BLOCKS; b++)
    for (uint32_t p = 0; p < PPB; p++) {
      uint32_t row = nand_row_addr(b, p, PBITS);
      nand_seq_page_read(&seq, row);
      nand_seq_read_cache(&seq, got, PAGE, quad);
      sim_page(row, want, PAGE);
      if (memcmp(got, want, PAGE) != 0) bad++;
    }
  return bad;
}

void test_two_plane_dump_is_exact_with_plane_select(void) {
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  TEST_ASSERT_EQUAL_INT(0, dump_and_count_bad(false));
  TEST_ASSERT_EQUAL_INT(BLOCKS * PPB, chip.page_reads);
}

void test_two_plane_quad_dump_is_exact(void) {
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  TEST_ASSERT_EQUAL_INT(0, dump_and_count_bad(true));
}

// Regression guard for the pre-PR#1 behavior: treating the 2-plane chip as
// single-plane corrupts exactly the odd blocks.
void test_single_plane_config_corrupts_every_odd_block(void) {
  nand_seq_set_planes(&seq, 1, PBITS, MAIN);
  TEST_ASSERT_EQUAL_INT((BLOCKS / 2) * PPB, dump_and_count_bad(false));
}

// The corruption has the exact signature tools/check_planes.py looks for:
// each page of odd block 2k+1 equals the last page of block 2k.
void test_missing_plane_select_mirrors_previous_blocks_last_page(void) {
  nand_seq_set_planes(&seq, 1, PBITS, MAIN);
  uint8_t got[PAGE], ref[PAGE];
  for (uint32_t b = 1; b < BLOCKS; b += 2) {
    sim_page(nand_row_addr(b - 1, PPB - 1, PBITS), ref, PAGE);
    for (uint32_t p = 0; p < PPB; p++) {
      if (p == 0) {   // the dump loop has just read block b-1's last page
        nand_seq_page_read(&seq, nand_row_addr(b - 1, PPB - 1, PBITS));
        nand_seq_read_cache(&seq, got, PAGE, false);
      }
      nand_seq_page_read(&seq, nand_row_addr(b, p, PBITS));
      nand_seq_read_cache(&seq, got, PAGE, false);
      TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, got, PAGE);
    }
  }
}

void test_single_plane_chip_reads_column_zero(void) {
  sim_init(&chip, 1, PAGE, PPB, PBITS, nand_plane_bit(MAIN));
  nand_seq_set_planes(&seq, 1, PBITS, MAIN);
  TEST_ASSERT_EQUAL_INT(0, dump_and_count_bad(false));
  TEST_ASSERT_EQUAL_UINT16(0, seq.cache_col);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_two_plane_dump_is_exact_with_plane_select);
  RUN_TEST(test_two_plane_quad_dump_is_exact);
  RUN_TEST(test_single_plane_config_corrupts_every_odd_block);
  RUN_TEST(test_missing_plane_select_mirrors_previous_blocks_last_page);
  RUN_TEST(test_single_plane_chip_reads_column_zero);
  return UNITY_END();
}
