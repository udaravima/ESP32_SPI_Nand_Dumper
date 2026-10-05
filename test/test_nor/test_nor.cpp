// SPI NOR read path against a simulated chip (no hardware): the sequencer,
// 4-byte addressing, quad fallback, SFDP-built profiles and family detection.
#include <unity.h>
#include <string.h>
#include "nor_seq.h"
#include "sfdp.h"
#include "chip_detect.h"
#include "nand_profile.h"
#include "sim_nor.h"

#define MIB (1024u * 1024u)
#define UNIT 4096

static sim_nor_t chip;
static nor_seq_t seq;

void setUp(void) {}
void tearDown(void) {}

static const active_profile_t *resident(const char *name) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  for (unsigned i = 0; i < n; i++)
    if (strcmp(t[i].name, name) == 0) return &t[i];
  return NULL;
}

static void start(uint32_t size, uint8_t mfr, uint8_t type, uint8_t cap,
                  const active_profile_t *p) {
  sim_nor_init(&chip, size, mfr, type, cap);
  nor_seq_init(&seq, sim_nor_bus(&chip));
  if (p) nor_seq_apply(&seq, p);
}

// Read units the way cmd_dump does (unit index * page_size); count bad units.
static int dump_units(const uint32_t *units, int n, bool verified) {
  static uint8_t got[UNIT], scratch[UNIT];
  int bad = 0;
  nor_seq_begin(&seq);
  for (int k = 0; k < n; k++) {
    uint32_t addr = units[k] * UNIT;
    if (verified) nor_seq_read_verified(&seq, addr, got, scratch, UNIT, 3, NULL);
    else nor_seq_read(&seq, addr, got, UNIT, seq.quad);
    for (int i = 0; i < UNIT; i++)
      if (got[i] != sim_nor_byte(addr + i)) { bad++; break; }
  }
  nor_seq_end(&seq);
  return bad;
}

// First, last and both sides of the 16 MiB line, for a part of `size` bytes.
static int sample_units(uint32_t size, uint32_t *u) {
  int n = 0;
  uint32_t last = size / UNIT - 1;
  u[n++] = 0; u[n++] = 1; u[n++] = last / 2; u[n++] = last;
  if (size > 16 * MIB) { u[n++] = 16 * MIB / UNIT - 1; u[n++] = 16 * MIB / UNIT; }
  return n;
}

static void sfdp_rd(void *ctx, uint32_t addr, uint8_t *buf, int len) {
  nor_seq_read_sfdp((nor_seq_t *)ctx, addr, buf, len);
}

// ---- identification -----------------------------------------------------------
void test_read_id_and_wake_from_power_down(void) {
  start(16 * MIB, 0xEF, 0x40, 0x18, NULL);
  chip.powered_down = true;
  uint8_t id[3];
  nor_seq_read_id(&seq, id);
  TEST_ASSERT_EQUAL_HEX8(0xFF, id[0]);            // asleep: 9Fh ignored
  nor_seq_release_power_down(&seq);
  nor_seq_read_id(&seq, id);
  const uint8_t want[3] = {0xEF, 0x40, 0x18};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, id, 3);
}

// ---- resident profiles from the flashrom-seeded DB ------------------------------
void test_resident_w25q128_dumps_exactly_with_3_byte_reads(void) {
  const active_profile_t *p = resident("W25Q128.V");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, p->family);
  TEST_ASSERT_EQUAL_UINT32(16 * MIB, nand_profile_bytes(p));
  start(16 * MIB, 0xEF, 0x40, 0x18, p);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(16 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
  TEST_ASSERT_FALSE(chip.four_byte);
}

void test_small_part_full_dump(void) {
  const active_profile_t *p = resident("AT25DF021A");       // 256 KiB
  TEST_ASSERT_NOT_NULL(p);
  start(256 * 1024, p->id_mfr, p->id_dev, p->id_dev2, p);
  uint32_t n = nand_profile_bytes(p) / UNIT, u[64];
  for (uint32_t i = 0; i < n; i++) u[i] = i;
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, (int)n, true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
}

void test_32mib_native_4_byte_opcodes_reach_the_upper_half(void) {
  const active_profile_t *p = resident("MX25L25635F");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_NATIVE, p->addr4_mode);
  TEST_ASSERT_EQUAL_HEX8(0x13, p->op_read_cache);
  start(32 * MIB, 0xC2, 0x20, 0x19, p);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(32 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
  TEST_ASSERT_FALSE(chip.four_byte);                         // no mode change needed
}

void test_32mib_enter_mode_sends_b7_and_leaves_it_after(void) {
  active_profile_t p;
  nand_profile_manual_nor(&p, 32 * MIB);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&p, 8192));
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_ENTER, p.addr4_mode);
  start(32 * MIB, 0xEF, 0x40, 0x19, &p);
  nor_seq_begin(&seq);
  TEST_ASSERT_TRUE(chip.four_byte);
  nor_seq_end(&seq);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(32 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
  TEST_ASSERT_FALSE(chip.four_byte);                          // back to power-on mode
}

void test_enter_wren_mode(void) {
  active_profile_t p;
  nand_profile_manual_nor(&p, 64 * MIB);
  p.addr4_mode = NOR_ADDR4_ENTER_WREN;
  start(64 * MIB, 0x20, 0xBA, 0x20, &p);
  chip.needs_wren_for_4b = true;
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(64 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
}

// Why addr_bytes matters: a 3-byte read of a 32 MiB part only reaches A[23:0],
// so the upper half silently reads back as the lower half. The profile check
// refuses such a profile before it can do that.
void test_3_byte_profile_on_32mib_wraps_and_is_refused(void) {
  active_profile_t p;
  nand_profile_manual_nor(&p, 32 * MIB);
  p.addr_bytes = 3; p.addr4_mode = NOR_ADDR4_NONE;
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_GEOMETRY, nand_profile_check(&p, 8192));
  start(32 * MIB, 0xEF, 0x40, 0x19, &p);
  static uint8_t hi[UNIT];
  nor_seq_read(&seq, 16 * MIB, hi, UNIT, false);
  TEST_ASSERT_EQUAL_UINT32(0, chip.last_addr);               // wrapped to 0
  TEST_ASSERT_EQUAL_HEX8(sim_nor_byte(0), hi[0]);
}

// ---- quad --------------------------------------------------------------------------
void test_quad_with_qe_set_reads_exactly(void) {
  const active_profile_t *p = resident("W25Q128.V");
  start(16 * MIB, 0xEF, 0x40, 0x18, p);
  chip.qe = true;
  nor_seq_set_quad(&seq, true);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(16 * MIB, u), true));
  TEST_ASSERT_TRUE(chip.quad_reads > 0);
  TEST_ASSERT_TRUE(seq.quad);
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
}

void test_quad_with_qe_clear_falls_back_to_single(void) {
  const active_profile_t *p = resident("W25Q128.V");
  start(16 * MIB, 0xEF, 0x40, 0x18, p);
  chip.qe = false;                                           // IO2/IO3 not driven
  nor_seq_set_quad(&seq, true);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(16 * MIB, u), true));
  TEST_ASSERT_FALSE(seq.quad);                               // gave up on quad
  TEST_ASSERT_EQUAL_UINT32(NAND_QUAD_FALLBACK_LIMIT, seq.quad_fallbacks);
}

void test_qe_state_follows_the_profile_qer(void) {
  start(16 * MIB, 0xEF, 0x40, 0x18, resident("W25Q128.V"));     // nor-winbond, QER 5
  TEST_ASSERT_EQUAL_INT(0, nor_seq_qe_state(&seq));
  chip.qe = true;
  TEST_ASSERT_EQUAL_INT(1, nor_seq_qe_state(&seq));

  start(32 * MIB, 0xC2, 0x20, 0x19, resident("MX25L25635F"));   // nor-macronix, QER 2
  chip.qe_in_sr1 = true;
  TEST_ASSERT_EQUAL_INT(0, nor_seq_qe_state(&seq));
  chip.qe = true;
  TEST_ASSERT_EQUAL_INT(1, nor_seq_qe_state(&seq));

  active_profile_t g;
  nand_profile_manual_nor(&g, MIB);                             // QER 0: unknown
  start(MIB, 0x37, 0x30, 0x14, &g);
  TEST_ASSERT_EQUAL_INT(-1, nor_seq_qe_state(&seq));
}

// The dumper is read-only: a full detect -> quad probe -> dump never sends a
// status write, program or erase opcode.
void test_read_path_never_writes(void) {
  active_profile_t p;
  nand_profile_manual_nor(&p, 32 * MIB);
  start(32 * MIB, 0xEF, 0x40, 0x19, &p);
  sim_nor_set_sfdp(&chip, true, 5, 0x01, 1);
  uint8_t id[3];
  nor_seq_release_power_down(&seq);
  nor_seq_read_id(&seq, id);
  sfdp_info_t info;
  sfdp_probe(sfdp_rd, &seq, &info);
  nor_seq_qe_state(&seq);
  nor_seq_set_quad(&seq, true);
  uint32_t u[8];
  dump_units(u, sample_units(32 * MIB, u), true);
  TEST_ASSERT_EQUAL_INT(0, chip.writes);
}

// ---- SFDP ----------------------------------------------------------------------
void test_sfdp_parses_density_quad_and_qer(void) {
  start(8 * MIB, 0xAB, 0xCD, 0x17, NULL);
  sim_nor_set_sfdp(&chip, true, 5, 0, 0);
  sfdp_info_t info;
  TEST_ASSERT_TRUE(sfdp_probe(sfdp_rd, &seq, &info));
  TEST_ASSERT_EQUAL_UINT32(8 * MIB, info.size_bytes);
  TEST_ASSERT_TRUE(info.has_114);
  TEST_ASSERT_EQUAL_HEX8(0x6B, info.op_114);
  TEST_ASSERT_EQUAL_UINT8(8, info.dummy_114);
  TEST_ASSERT_EQUAL_UINT8(5, info.qer);
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
}

void test_sfdp_profile_dumps_an_unlisted_chip(void) {
  start(8 * MIB, 0xAB, 0xCD, 0x17, NULL);
  sim_nor_set_sfdp(&chip, true, 5, 0, 0);
  sfdp_info_t info;
  TEST_ASSERT_TRUE(sfdp_probe(sfdp_rd, &seq, &info));
  active_profile_t p;
  TEST_ASSERT_TRUE(sfdp_build_profile(&info, chip.id, &p));
  TEST_ASSERT_EQUAL_STRING("SFDP-ABCD17", p.name);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&p, 8192));
  TEST_ASSERT_TRUE(nand_profile_id_matches(&p, 0xAB, 0xCD, 0x17));
  TEST_ASSERT_EQUAL_UINT8(5, p.qer);
  nor_seq_apply(&seq, &p);
  chip.qe = true;
  nor_seq_set_quad(&seq, true);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(8 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);
}

void test_sfdp_4_byte_method_choice(void) {
  sfdp_info_t info;
  active_profile_t p;
  start(32 * MIB, 0xAB, 0xCD, 0x19, NULL);
  sim_nor_set_sfdp(&chip, true, 0, 0x01 | 0x20, 1);          // B7h and 4-byte opcodes
  TEST_ASSERT_TRUE(sfdp_probe(sfdp_rd, &seq, &info));
  TEST_ASSERT_TRUE(sfdp_build_profile(&info, chip.id, &p));
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_ENTER, p.addr4_mode);    // B7h preferred

  sim_nor_set_sfdp(&chip, true, 0, 0x20, 1);                 // 4-byte opcodes only
  TEST_ASSERT_TRUE(sfdp_probe(sfdp_rd, &seq, &info));
  TEST_ASSERT_TRUE(sfdp_build_profile(&info, chip.id, &p));
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_NATIVE, p.addr4_mode);
  TEST_ASSERT_EQUAL_HEX8(0x13, p.op_read_cache);
  nor_seq_apply(&seq, &p);
  uint32_t u[8];
  TEST_ASSERT_EQUAL_INT(0, dump_units(u, sample_units(32 * MIB, u), true));
  TEST_ASSERT_EQUAL_INT(0, chip.protocol_errors);

  sim_nor_set_sfdp(&chip, true, 0, 0x04, 1);                 // extended address register only
  TEST_ASSERT_TRUE(sfdp_probe(sfdp_rd, &seq, &info));
  memset(&p, 0x5A, sizeof(p));
  active_profile_t before = p;
  TEST_ASSERT_FALSE(sfdp_build_profile(&info, chip.id, &p));
  TEST_ASSERT_EQUAL_MEMORY(&before, &p, sizeof(p));          // untouched on refusal
}

void test_no_sfdp_means_no_profile(void) {
  start(MIB, 0x1F, 0x45, 0x01, NULL);                        // pre-SFDP part
  sfdp_info_t info;
  TEST_ASSERT_FALSE(sfdp_probe(sfdp_rd, &seq, &info));
  sim_nor_set_sfdp(&chip, false, 0, 0, 0);
  const uint8_t huge[4] = {40, 0, 0, 0x80};                  // density 2^40 bits: > 4 GiB
  memcpy(chip.sfdp + 0x30 + 4, huge, 4);
  TEST_ASSERT_FALSE(sfdp_probe(sfdp_rd, &seq, &info));
}

// ---- family detection --------------------------------------------------------------
static active_profile_t T[4];
static unsigned make_table(void) {
  nand_profile_manual(&T[0], 2112, 64, 64, 1024, 1);            // a SPI NAND
  strcpy(T[0].name, "NAND"); T[0].id_mfr = 0xE5; T[0].id_dev = 0x71;
  nand_profile_manual_nor(&T[1], 16 * MIB);                      // a SPI NOR
  strcpy(T[1].name, "NOR"); T[1].id_mfr = 0xEF; T[1].id_dev = 0x40;
  T[1].id_dev2 = 0x18; T[1].id_flags = NAND_PROFILE_ID_HAS_DEV2;
  T[2] = T[1]; strcpy(T[2].name, "TWIN_A"); T[2].id_dev = 0x8A; T[2].id_dev2 = 0x16;
  T[3] = T[2]; strcpy(T[3].name, "TWIN_B");                      // same full ID
  return 4;
}

void test_detect_picks_the_family_whose_view_matches(void) {
  unsigned n = make_table();
  const uint8_t nand_view_of_nand[3] = {0xE5, 0x71, 0x00}, nor_view_of_nand[3] = {0xFF, 0xE5, 0x71};
  chip_detect_t r = chip_detect_resident(T, n, nand_view_of_nand, nor_view_of_nand);
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_RESIDENT, r.state);
  TEST_ASSERT_EQUAL_STRING("NAND", r.hit->name);

  // A W25Q128 seen through the NAND read: the first ID byte went by during
  // the address byte, so that view is (type, capacity, ...).
  const uint8_t nand_view_of_nor[3] = {0x40, 0x18, 0xEF}, nor_view_of_nor[3] = {0xEF, 0x40, 0x18};
  r = chip_detect_resident(T, n, nand_view_of_nor, nor_view_of_nor);
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_RESIDENT, r.state);
  TEST_ASSERT_EQUAL_STRING("NOR", r.hit->name);
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, r.family);
}

void test_detect_fails_closed(void) {
  unsigned n = make_table();
  // Both views match an entry: refuse to pick a family.
  const uint8_t a[3] = {0xE5, 0x71, 0x00}, b[3] = {0xEF, 0x40, 0x18};
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_AMBIGUOUS, chip_detect_resident(T, n, a, b).state);
  // Two NOR entries with one full ID.
  const uint8_t z[3] = {0, 0, 0}, twin[3] = {0xEF, 0x8A, 0x16};
  chip_detect_t r = chip_detect_resident(T, n, z, twin);
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_AMBIGUOUS, r.state);
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, r.family);
  // Same (mfr, type), other capacity: a different chip, not the 16 MiB one.
  const uint8_t w256[3] = {0xEF, 0x40, 0x19};
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_UNKNOWN, chip_detect_resident(T, n, z, w256).state);
}

void test_floating_bus_is_not_a_nor_id(void) {
  const uint8_t ff[3] = {0xFF, 0xFF, 0xFF}, zz[3] = {0, 0, 0}, ok[3] = {0xEF, 0x40, 0x18};
  TEST_ASSERT_FALSE(chip_detect_nor_id_plausible(ff));
  TEST_ASSERT_FALSE(chip_detect_nor_id_plausible(zz));
  TEST_ASSERT_TRUE(chip_detect_nor_id_plausible(ok));
}

void test_real_table_resolves_w25q128_and_w25q256(void) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  const uint8_t z[3] = {0, 0, 0};
  const uint8_t w128[3] = {0xEF, 0x40, 0x18}, w256[3] = {0xEF, 0x40, 0x19};
  chip_detect_t r = chip_detect_resident(t, n, z, w128);
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_RESIDENT, r.state);
  TEST_ASSERT_EQUAL_STRING("W25Q128.V", r.hit->name);
  r = chip_detect_resident(t, n, z, w256);
  TEST_ASSERT_EQUAL_INT(NAND_CHIP_RESIDENT, r.state);
  TEST_ASSERT_EQUAL_UINT32(32 * MIB, nand_profile_bytes(r.hit));
  // The two resident SPI NAND chips still resolve through their own view.
  const uint8_t ds35[3] = {0xE5, 0x71, 0x00}, ff[3] = {0xFF, 0xE5, 0x71};
  r = chip_detect_resident(t, n, ds35, ff);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", r.hit->name);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_read_id_and_wake_from_power_down);
  RUN_TEST(test_resident_w25q128_dumps_exactly_with_3_byte_reads);
  RUN_TEST(test_small_part_full_dump);
  RUN_TEST(test_32mib_native_4_byte_opcodes_reach_the_upper_half);
  RUN_TEST(test_32mib_enter_mode_sends_b7_and_leaves_it_after);
  RUN_TEST(test_enter_wren_mode);
  RUN_TEST(test_3_byte_profile_on_32mib_wraps_and_is_refused);
  RUN_TEST(test_quad_with_qe_set_reads_exactly);
  RUN_TEST(test_quad_with_qe_clear_falls_back_to_single);
  RUN_TEST(test_qe_state_follows_the_profile_qer);
  RUN_TEST(test_read_path_never_writes);
  RUN_TEST(test_sfdp_parses_density_quad_and_qer);
  RUN_TEST(test_sfdp_profile_dumps_an_unlisted_chip);
  RUN_TEST(test_sfdp_4_byte_method_choice);
  RUN_TEST(test_no_sfdp_means_no_profile);
  RUN_TEST(test_detect_picks_the_family_whose_view_matches);
  RUN_TEST(test_detect_fails_closed);
  RUN_TEST(test_floating_bus_is_not_a_nor_id);
  RUN_TEST(test_real_table_resolves_w25q128_and_w25q256);
  return UNITY_END();
}
