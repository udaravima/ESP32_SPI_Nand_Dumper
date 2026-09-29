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

// ---- on-device quad -> single fallback ----------------------------------------

// Read every page the way cmd_dump does with verify on; count results.
static void verified_dump(int *ok, int *rescued, int *unstable, int *wrong) {
  uint8_t got[PAGE], scratch[PAGE], want[PAGE];
  *ok = *rescued = *unstable = *wrong = 0;
  uint32_t retries = 0;
  for (uint32_t b = 0; b < BLOCKS; b++)
    for (uint32_t p = 0; p < PPB; p++) {
      uint32_t row = nand_row_addr(b, p, PBITS);
      nand_page_result_t r = nand_seq_read_verified(&seq, row, got, scratch, PAGE, 2, &retries);
      if (r == NAND_PAGE_OK) (*ok)++;
      else if (r == NAND_PAGE_OK_SINGLE) (*rescued)++;
      else (*unstable)++;
      sim_page(row, want, PAGE);
      if (r != NAND_PAGE_UNSTABLE && memcmp(got, want, PAGE) != 0) (*wrong)++;
    }
}

void test_clean_quad_stays_quad(void) {
  int ok, rescued, unstable, wrong;
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  nand_seq_set_quad(&seq, true);
  verified_dump(&ok, &rescued, &unstable, &wrong);
  TEST_ASSERT_EQUAL_INT(BLOCKS * PPB, ok);
  TEST_ASSERT_EQUAL_INT(0, wrong);
  TEST_ASSERT_TRUE(seq.quad);
  TEST_ASSERT_EQUAL_INT(0, chip.single_reads);
}

// A marginal quad line: the first pages are rescued by single re-reads, then
// the session drops to single and the rest read clean. Every page is exact,
// on both planes.
void test_broken_quad_falls_back_to_single_and_dump_is_exact(void) {
  int ok, rescued, unstable, wrong;
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  nand_seq_set_quad(&seq, true);
  chip.quad_noisy = true;
  verified_dump(&ok, &rescued, &unstable, &wrong);
  TEST_ASSERT_EQUAL_INT(NAND_QUAD_FALLBACK_LIMIT, rescued);
  TEST_ASSERT_EQUAL_INT(BLOCKS * PPB - NAND_QUAD_FALLBACK_LIMIT, ok);
  TEST_ASSERT_EQUAL_INT(0, unstable);
  TEST_ASSERT_EQUAL_INT(0, wrong);
  TEST_ASSERT_FALSE(seq.quad);
  // Quad was only tried on the rescued pages; each page read the array once.
  TEST_ASSERT_EQUAL_INT(NAND_QUAD_FALLBACK_LIMIT * (1 + 3), chip.quad_reads);
  TEST_ASSERT_EQUAL_INT(BLOCKS * PPB, chip.page_reads);
}

// One flaky page is rescued without giving up quad for the rest.
void test_one_bad_quad_page_keeps_quad(void) {
  int ok, rescued, unstable, wrong;
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  nand_seq_set_quad(&seq, true);
  chip.noisy_row = (int32_t)nand_row_addr(3, 1, PBITS);    // an odd-plane page
  verified_dump(&ok, &rescued, &unstable, &wrong);
  TEST_ASSERT_EQUAL_INT(1, rescued);
  TEST_ASSERT_EQUAL_INT(0, wrong);
  TEST_ASSERT_TRUE(seq.quad);
  TEST_ASSERT_EQUAL_UINT32(1, seq.quad_fallbacks);
}

// Nothing settles: the page is reported unstable, never passed as good.
void test_noisy_single_reports_unstable(void) {
  int ok, rescued, unstable, wrong;
  nand_seq_set_planes(&seq, 2, PBITS, MAIN);
  nand_seq_set_quad(&seq, true);
  chip.quad_noisy = chip.single_noisy = true;
  verified_dump(&ok, &rescued, &unstable, &wrong);
  TEST_ASSERT_EQUAL_INT(BLOCKS * PPB, unstable);
  TEST_ASSERT_TRUE(seq.quad);    // no page was ever rescued by single
}

// Resetting the read mode starts a new session with a fresh fallback count.
void test_set_quad_resets_fallback_count(void) {
  seq.quad_fallbacks = 7;
  nand_seq_set_quad(&seq, true);
  TEST_ASSERT_EQUAL_UINT32(0, seq.quad_fallbacks);
}

// Opcodes come from the active profile, not constants.
static uint8_t seen_cmds[8];
static int n_seen;
static void record_xfer(void *, uint8_t cmd, uint32_t, uint8_t, uint8_t *, int, bool) {
  if (n_seen < 8) seen_cmds[n_seen++] = cmd;
}
void test_opcodes_come_from_profile(void) {
  nand_seq_t s;
  nand_bus_t bus = { record_xfer, NULL, NULL };
  nand_seq_init(&s, bus);
  nand_seq_set_opcodes(&s, 0x13, 0x03, 0xEB);
  n_seen = 0;
  nand_seq_page_read(&s, 0);
  nand_seq_read_cache(&s, NULL, 0, false);
  nand_seq_read_cache(&s, NULL, 0, true);
  TEST_ASSERT_EQUAL_INT(3, n_seen);
  TEST_ASSERT_EQUAL_HEX8(0x13, seen_cmds[0]);
  TEST_ASSERT_EQUAL_HEX8(0x03, seen_cmds[1]);
  TEST_ASSERT_EQUAL_HEX8(0xEB, seen_cmds[2]);
}

void test_read_id_returns_three_bytes(void) {
  uint8_t id[3] = {0};
  chip.id[2] = 0x7E;
  nand_seq_read_id(&seq, id, 3);
  TEST_ASSERT_EQUAL_HEX8(0x2C, id[0]);
  TEST_ASSERT_EQUAL_HEX8(0x24, id[1]);
  TEST_ASSERT_EQUAL_HEX8(0x7E, id[2]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_two_plane_dump_is_exact_with_plane_select);
  RUN_TEST(test_two_plane_quad_dump_is_exact);
  RUN_TEST(test_single_plane_config_corrupts_every_odd_block);
  RUN_TEST(test_missing_plane_select_mirrors_previous_blocks_last_page);
  RUN_TEST(test_single_plane_chip_reads_column_zero);
  RUN_TEST(test_clean_quad_stays_quad);
  RUN_TEST(test_broken_quad_falls_back_to_single_and_dump_is_exact);
  RUN_TEST(test_one_bad_quad_page_keeps_quad);
  RUN_TEST(test_noisy_single_reports_unstable);
  RUN_TEST(test_set_quad_resets_fallback_count);
  RUN_TEST(test_opcodes_come_from_profile);
  RUN_TEST(test_read_id_returns_three_bytes);
  return UNITY_END();
}
