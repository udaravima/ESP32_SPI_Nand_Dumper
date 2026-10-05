// Vendor-profile stage 2: the flat profile in C, checked against the bytes
// tools/chipdb.py packs (design § 9, golden blob), plus the pure-data ECC
// decoder, the fail-closed unpack path, ID resolution and the BBM check.
#include <unity.h>
#include <string.h>
#include "nand_profile.h"
#include "dump_header.h"
#include "golden_blobs.h"

#define MAX_PAGE 8192

void setUp(void) {}
void tearDown(void) {}

static const active_profile_t *resident(const char *name) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  for (unsigned i = 0; i < n; i++)
    if (strcmp(t[i].name, name) == 0) return &t[i];
  return NULL;
}

// Re-seal a blob after editing it, so a test reaches the tier it aims at.
static void reseal(uint8_t *blob) {
  uint32_t crc = dump_crc32(blob, 6 + NAND_PROFILE_SIZE);
  uint8_t *c = blob + 6 + NAND_PROFILE_SIZE;
  c[0] = crc; c[1] = crc >> 8; c[2] = crc >> 16; c[3] = crc >> 24;
}

// ---- golden blob ------------------------------------------------------------
void test_struct_size_matches_host_layout(void) {
  TEST_ASSERT_EQUAL_UINT(128, sizeof(active_profile_t));
  TEST_ASSERT_EQUAL_UINT(NAND_PROFILE_BLOB_SIZE, sizeof(GOLDEN_DS35Q1GA));
}

void test_golden_ds35_unpacks_every_field(void) {
  active_profile_t p;
  memset(&p, 0xAA, sizeof(p));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK,
      nand_profile_unpack(GOLDEN_DS35Q1GA, sizeof(GOLDEN_DS35Q1GA), MAX_PAGE, &p));
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p.name);
  TEST_ASSERT_EQUAL_HEX8(0xE5, p.id_mfr);
  TEST_ASSERT_EQUAL_HEX8(0x71, p.id_dev);
  TEST_ASSERT_EQUAL_HEX8(0x00, p.id_dev2);
  TEST_ASSERT_EQUAL_HEX8(0x00, p.id_flags);
  TEST_ASSERT_EQUAL_UINT32(2112, p.page_size);
  TEST_ASSERT_EQUAL_UINT32(64, p.spare_size);
  TEST_ASSERT_EQUAL_UINT32(64, p.pages_per_block);
  TEST_ASSERT_EQUAL_UINT32(1024, p.total_blocks);
  TEST_ASSERT_EQUAL_HEX8(0x13, p.op_page_read);
  TEST_ASSERT_EQUAL_HEX8(0x0B, p.op_read_cache);
  TEST_ASSERT_EQUAL_HEX8(0x6B, p.op_read_cache_x4);
  TEST_ASSERT_EQUAL_HEX8(0x0F, p.op_get_feat);
  TEST_ASSERT_EQUAL_HEX8(0x1F, p.op_set_feat);
  TEST_ASSERT_EQUAL_HEX8(0xC0, p.op_status_addr);
  TEST_ASSERT_EQUAL_HEX8(0xB0, p.op_cfg_addr);
  TEST_ASSERT_EQUAL_UINT8(4, p.ecc_en_bit);
  TEST_ASSERT_EQUAL_UINT8(4, p.ecc_shift);             // SR[5:4], not Micron's [6:4]
  TEST_ASSERT_EQUAL_HEX8(0x03, p.ecc_mask);
  const uint8_t map[16] = {0, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(map, p.ecc_map, 16);
  TEST_ASSERT_EQUAL_HEX8(0x00, p.status2_reg);
  TEST_ASSERT_EQUAL_UINT8(NAND_ID_METHOD_DUMMY, p.id_method);
  TEST_ASSERT_EQUAL_UINT8(2, p.id_n_bytes);
  TEST_ASSERT_EQUAL_HEX8(0xB0, p.qe_addr);             // QE at B0[0]
  TEST_ASSERT_EQUAL_HEX8(0x01, p.qe_bit);
  TEST_ASSERT_EQUAL_UINT8(NAND_READ_SINGLE, p.read_mode);
  TEST_ASSERT_EQUAL_UINT8(1, p.planes);
  TEST_ASSERT_EQUAL_UINT16(3300, p.vcc_mv);
  TEST_ASSERT_EQUAL_UINT8(0, p.bbm_off);
  TEST_ASSERT_EQUAL_UINT8(2, p.bbm_len);                // 2-byte BBM
  TEST_ASSERT_EQUAL_HEX8(0xFF, p.bbm_good);
  TEST_ASSERT_EQUAL_HEX8(NAND_BBM_PAGE_FIRST, p.bbm_pages);
  TEST_ASSERT_EQUAL_UINT8(4, p.oob_free_n);
  TEST_ASSERT_EQUAL_UINT8(4, p.oob_ecc_n);
  const uint16_t free_r[8] = {2, 6, 16, 8, 32, 8, 48, 8};
  const uint16_t ecc_r[8] = {8, 8, 24, 8, 40, 8, 56, 8};
  TEST_ASSERT_EQUAL_UINT16_ARRAY(free_r, p.oob_free, 8);
  TEST_ASSERT_EQUAL_UINT16_ARRAY(ecc_r, p.oob_ecc, 8);
}

// A resident chip and a pushed chip are the same bytes (design § 8).
void test_resident_entries_equal_pushed_blobs(void) {
  active_profile_t p;
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK,
      nand_profile_unpack(GOLDEN_DS35Q1GA, sizeof(GOLDEN_DS35Q1GA), MAX_PAGE, &p));
  TEST_ASSERT_EQUAL_MEMORY(resident("DS35Q1GA"), &p, sizeof(p));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_unpack(GOLDEN_MT29F2G01ABAGD,
      sizeof(GOLDEN_MT29F2G01ABAGD), MAX_PAGE, &p));
  TEST_ASSERT_EQUAL_MEMORY(resident("MT29F2G01ABAGD"), &p, sizeof(p));
  TEST_ASSERT_EQUAL_UINT8(2, p.planes);
}

void test_every_resident_entry_passes_boot_checks(void) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  TEST_ASSERT_TRUE(n >= 2);
  for (unsigned i = 0; i < n; i++)
    TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&t[i], MAX_PAGE));
}

// ---- fail-closed unpack -----------------------------------------------------
static nand_prf_err_t unpack_edited(void (*edit)(uint8_t *), bool reseal_it,
                                    active_profile_t *out) {
  uint8_t b[NAND_PROFILE_BLOB_SIZE];
  memcpy(b, GOLDEN_DS35Q1GA, sizeof(b));
  edit(b);
  if (reseal_it) reseal(b);
  return nand_profile_unpack(b, sizeof(b), MAX_PAGE, out);
}
static void flip_body(uint8_t *b) { b[6 + 30] ^= 0x01; }
static void bad_magic(uint8_t *b) { b[0] = 'X'; }
static void bad_ver(uint8_t *b) { b[3] = NAND_PROFILE_SCHEMA_VER + 1; }
static void bad_len(uint8_t *b) { b[4] = NAND_PROFILE_SIZE - 1; }
static void wide_mask(uint8_t *b) { b[6 + 53] = 0x1F; }                 // ecc_mask
static void bad_sev(uint8_t *b) { b[6 + 54 + 15] = 4; }                 // ecc_map[15]
static void long_name(uint8_t *b) { memset(b + 6, 'A', 24); }
static void huge_page(uint8_t *b) { b[6 + 28] = 0x00; b[6 + 29] = 0x40; } // 16384
static void odd_ppb(uint8_t *b) { b[6 + 36] = 48; }
static void bbm_past_spare(uint8_t *b) { b[6 + 80] = 63; }             // bbm_off 63, len 2
static void zero_opcode(uint8_t *b) { b[6 + 45] = 0; }                 // op_read_cache

void test_unpack_rejects_and_leaves_output_untouched(void) {
  struct { void (*edit)(uint8_t *); bool reseal; nand_prf_err_t want; } cases[] = {
    { flip_body, false, NAND_PRF_E_BAD_CRC },
    { bad_magic, true, NAND_PRF_E_BAD_MAGIC },
    { bad_ver, true, NAND_PRF_E_SCHEMA_VER },
    { bad_len, true, NAND_PRF_E_BAD_LEN },
    { wide_mask, true, NAND_PRF_E_STRUCTURE },
    { bad_sev, true, NAND_PRF_E_STRUCTURE },
    { long_name, true, NAND_PRF_E_STRUCTURE },
    { zero_opcode, true, NAND_PRF_E_STRUCTURE },
    { bbm_past_spare, true, NAND_PRF_E_STRUCTURE },
    { huge_page, true, NAND_PRF_E_PAGE_TOO_BIG },
    { odd_ppb, true, NAND_PRF_E_GEOMETRY },
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    active_profile_t out, before;
    memset(&out, 0x5A, sizeof(out));
    before = out;
    TEST_ASSERT_EQUAL_STRING(nand_prf_err_name(cases[i].want),
        nand_prf_err_name(unpack_edited(cases[i].edit, cases[i].reseal, &out)));
    TEST_ASSERT_EQUAL_MEMORY(&before, &out, sizeof(out));
  }
}

void test_unpack_rejects_truncated_blob(void) {
  active_profile_t out;
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_BAD_LEN,
      nand_profile_unpack(GOLDEN_DS35Q1GA, sizeof(GOLDEN_DS35Q1GA) - 1, MAX_PAGE, &out));
}

// ---- ECC decode: raw status register in, severity out ------------------------
void test_ds35_generic2_decode(void) {
  const active_profile_t *p = resident("DS35Q1GA");
  TEST_ASSERT_EQUAL_INT(NAND_ECC_OK, nand_profile_ecc_severity(p, 0x00));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED, nand_profile_ecc_severity(p, 0x10));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(p, 0x20));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(p, 0x30));  // reserved
  // Bit 6 is reserved on the DS35. Micron's 3-bit decode read 0x50 as
  // "7-8 corrected"; the 2-bit field sees 01 = corrected.
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED, nand_profile_ecc_severity(p, 0x50));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_OK, nand_profile_ecc_severity(p, 0x01));   // OIP ignored
}

void test_micron3_decode_matches_table_9(void) {
  const active_profile_t *p = resident("MT29F2G01ABAGD");
  TEST_ASSERT_EQUAL_INT(NAND_ECC_OK, nand_profile_ecc_severity(p, 0x00));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED, nand_profile_ecc_severity(p, 0x10));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(p, 0x20));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED_REFRESH, nand_profile_ecc_severity(p, 0x30));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED_REFRESH, nand_profile_ecc_severity(p, 0x50));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(p, 0x40));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(p, 0xA2)); // CRBSY etc. masked
}

void test_four_bit_field_reaches_index_15(void) {
  active_profile_t p = *resident("DS35Q1GA");
  p.ecc_shift = 4; p.ecc_mask = 0xF;          // XTX [7:4]
  memset(p.ecc_map, NAND_ECC_CORRECTED, 15);
  p.ecc_map[0] = NAND_ECC_OK; p.ecc_map[15] = NAND_ECC_UNCORRECTABLE;
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(&p, 0xF0));
  TEST_ASSERT_EQUAL_INT(NAND_ECC_CORRECTED, nand_profile_ecc_severity(&p, 0x80));
}

// ---- ID resolution (design § 5) -------------------------------------------------
void test_find_resolves_resident_ids(void) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  nand_prf_err_t e;
  const active_profile_t *p = nand_profile_find(t, n, 0xE5, 0x71, 0xE5, &e);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p->name);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, e);
  TEST_ASSERT_NULL(nand_profile_find(t, n, 0x00, 0x00, 0x00, &e));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_UNKNOWN_ID, e);
}

void test_find_uses_dev2_and_never_guesses(void) {
  active_profile_t t[2];
  t[0] = *resident("DS35Q1GA"); t[1] = t[0];
  strcpy(t[1].name, "TWIN");
  nand_prf_err_t e;
  // Same (mfr, dev), no dev2 declared: ambiguous, fail closed.
  TEST_ASSERT_NULL(nand_profile_find(t, 2, 0xE5, 0x71, 0x00, &e));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_AMBIGUOUS_ID, e);
  // dev2 declared on both: the third ID byte picks one.
  t[0].id_flags = t[1].id_flags = NAND_PROFILE_ID_HAS_DEV2;
  t[0].id_dev2 = 0x11; t[1].id_dev2 = 0x22;
  TEST_ASSERT_EQUAL_PTR(&t[1], nand_profile_find(t, 2, 0xE5, 0x71, 0x22, &e));
  // A dev2 neither declares: neither is this chip.
  TEST_ASSERT_NULL(nand_profile_find(t, 2, 0xE5, 0x71, 0x33, &e));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_UNKNOWN_ID, e);
  // A lone entry that declares dev2 still needs it to match (a W25Q256 must
  // not resolve to the W25Q128 entry that shares its first two ID bytes).
  TEST_ASSERT_NULL(nand_profile_find(t, 1, 0xE5, 0x71, 0x22, &e));
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_UNKNOWN_ID, e);
}

void test_pick_honours_saved_choice_only_for_its_id(void) {
  active_profile_t t[3];
  t[0] = *resident("DS35Q1GA"); t[1] = t[0];
  strcpy(t[1].name, "TWIN");
  t[2] = *resident("MT29F2G01ABAGD");
  // The saved choice resolves the shared ID that nand_profile_find refuses.
  TEST_ASSERT_EQUAL_PTR(&t[1], nand_profile_pick(t, 3, 0xE5, 0x71, "TWIN"));
  TEST_ASSERT_EQUAL_PTR(&t[0], nand_profile_pick(t, 3, 0xE5, 0x71, "DS35Q1GA"));
  // A choice saved for another chip never applies to this one.
  TEST_ASSERT_NULL(nand_profile_pick(t, 3, 0xE5, 0x71, "MT29F2G01ABAGD"));
  TEST_ASSERT_NULL(nand_profile_pick(t, 3, 0x2C, 0x24, "TWIN"));
  // A name no longer in the table (reflashed firmware) or none at all.
  TEST_ASSERT_NULL(nand_profile_pick(t, 3, 0xE5, 0x71, "GONE"));
  TEST_ASSERT_NULL(nand_profile_pick(t, 3, 0xE5, 0x71, ""));
  TEST_ASSERT_NULL(nand_profile_pick(t, 3, 0xE5, 0x71, NULL));
}

void test_id_cross_check(void) {
  const active_profile_t *p = resident("MT29F2G01ABAGD");
  TEST_ASSERT_TRUE(nand_profile_id_matches(p, 0x2C, 0x24, 0x99));
  TEST_ASSERT_FALSE(nand_profile_id_matches(p, 0x2C, 0x25, 0x00));
}

// ---- bad-block marker ----------------------------------------------------------
void test_bbm_uses_profile_width_and_offset(void) {
  const active_profile_t *ds = resident("DS35Q1GA");          // 2 bytes at spare[0]
  uint8_t page[2112];
  memset(page, 0xFF, sizeof(page));
  TEST_ASSERT_FALSE(nand_profile_marker_bad(ds, page, 2112, 64));
  page[2048 + 1] = 0x00;                                     // second marker byte
  TEST_ASSERT_TRUE(nand_profile_marker_bad(ds, page, 2112, 64));

  const active_profile_t *mt = resident("MT29F2G01ABAGD");   // 1 byte at spare[0]
  uint8_t mp[2176];
  memset(mp, 0xFF, sizeof(mp));
  mp[2048 + 1] = 0x00;                                       // outside a 1-byte BBM
  TEST_ASSERT_FALSE(nand_profile_marker_bad(mt, mp, 2176, 128));
  mp[2048] = 0x00;
  TEST_ASSERT_TRUE(nand_profile_marker_bad(mt, mp, 2176, 128));
}

void test_bbm_pages(void) {
  active_profile_t p = *resident("DS35Q1GA");
  TEST_ASSERT_TRUE(nand_profile_is_bbm_page(&p, 0, 64));
  TEST_ASSERT_FALSE(nand_profile_is_bbm_page(&p, 1, 64));
  p.bbm_pages = NAND_BBM_PAGE_SECOND | NAND_BBM_PAGE_LAST;
  TEST_ASSERT_FALSE(nand_profile_is_bbm_page(&p, 0, 64));
  TEST_ASSERT_TRUE(nand_profile_is_bbm_page(&p, 1, 64));
  TEST_ASSERT_TRUE(nand_profile_is_bbm_page(&p, 63, 64));
}

void test_manual_profile_is_valid_and_generic(void) {
  active_profile_t p;
  nand_profile_manual(&p, 4320, 224, 64, 2048, 1);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&p, MAX_PAGE));
  TEST_ASSERT_EQUAL_HEX8(0x0B, p.op_read_cache);
  TEST_ASSERT_EQUAL_HEX8(0x00, p.qe_addr);
  TEST_ASSERT_EQUAL_INT(NAND_ECC_UNCORRECTABLE, nand_profile_ecc_severity(&p, 0x20));
}

// ---- SPI NOR (schema v2) ---------------------------------------------------------
void test_golden_nor_blobs_unpack_and_equal_resident(void) {
  active_profile_t p;
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK,
      nand_profile_unpack(GOLDEN_W25Q128_V, sizeof(GOLDEN_W25Q128_V), MAX_PAGE, &p));
  TEST_ASSERT_EQUAL_MEMORY(resident("W25Q128.V"), &p, sizeof(p));
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, p.family);
  TEST_ASSERT_EQUAL_HEX8(0xEF, p.id_mfr);
  TEST_ASSERT_EQUAL_HEX8(0x40, p.id_dev);
  TEST_ASSERT_EQUAL_HEX8(0x18, p.id_dev2);
  TEST_ASSERT_EQUAL_HEX8(NAND_PROFILE_ID_HAS_DEV2, p.id_flags);
  TEST_ASSERT_EQUAL_UINT32(4096, p.page_size);          // one read unit
  TEST_ASSERT_EQUAL_UINT32(0, p.spare_size);
  TEST_ASSERT_EQUAL_UINT32(16u << 20, nand_profile_bytes(&p));
  TEST_ASSERT_EQUAL_HEX8(0x03, p.op_read_cache);
  TEST_ASSERT_EQUAL_HEX8(0x6B, p.op_read_cache_x4);
  TEST_ASSERT_EQUAL_HEX8(0x05, p.op_get_feat);
  TEST_ASSERT_EQUAL_HEX8(0x00, p.op_page_read);         // no array -> cache step
  TEST_ASSERT_EQUAL_UINT8(NAND_ID_METHOD_NONE, p.id_method);
  TEST_ASSERT_EQUAL_UINT8(3, p.id_n_bytes);
  TEST_ASSERT_EQUAL_UINT8(3, p.addr_bytes);
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_NONE, p.addr4_mode);
  TEST_ASSERT_EQUAL_UINT8(0, p.dummy_x1);
  TEST_ASSERT_EQUAL_UINT8(8, p.dummy_x4);
  TEST_ASSERT_EQUAL_UINT8(5, p.qer);                    // nor-winbond: SR2 bit 1 via 35h

  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK,
      nand_profile_unpack(GOLDEN_MX25L25635F, sizeof(GOLDEN_MX25L25635F), MAX_PAGE, &p));
  TEST_ASSERT_EQUAL_MEMORY(resident("MX25L25635F"), &p, sizeof(p));
  TEST_ASSERT_EQUAL_UINT8(4, p.addr_bytes);
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_NATIVE, p.addr4_mode);
  TEST_ASSERT_EQUAL_HEX8(0x13, p.op_read_cache);
  TEST_ASSERT_EQUAL_HEX8(0x6C, p.op_read_cache_x4);
  TEST_ASSERT_EQUAL_UINT8(2, p.qer);                    // nor-macronix: SR1 bit 6
}

void test_nor_checks_fail_closed(void) {
  const active_profile_t *w = resident("W25Q128.V");
  active_profile_t p;
  struct { void (*edit)(active_profile_t *); nand_prf_err_t want; } cases[] = {
    { [](active_profile_t *q) { q->spare_size = 64; }, NAND_PRF_E_GEOMETRY },
    { [](active_profile_t *q) { q->bbm_len = 1; }, NAND_PRF_E_STRUCTURE },      // NAND field
    { [](active_profile_t *q) { q->ecc_mask = 0x3; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->addr_bytes = 5; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->addr4_mode = NOR_ADDR4_ENTER; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->qer = 7; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->dummy_x4 = 40; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->id_method = NAND_ID_METHOD_DUMMY; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->op_read_cache = 0; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->family = 2; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->_pad1[4] = 1; }, NAND_PRF_E_STRUCTURE },
    { [](active_profile_t *q) { q->page_size = 3000; }, NAND_PRF_E_GEOMETRY },
    { [](active_profile_t *q) { q->total_blocks = 512; }, NAND_PRF_E_GEOMETRY },  // 32 MiB, 3-byte
    { [](active_profile_t *q) { q->page_size = 16384; q->pages_per_block = 4; },
      NAND_PRF_E_PAGE_TOO_BIG },
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    p = *w;
    cases[i].edit(&p);
    TEST_ASSERT_EQUAL_STRING(nand_prf_err_name(cases[i].want),
                             nand_prf_err_name(nand_profile_check(&p, MAX_PAGE)));
  }
  // NOR tail bytes on a NAND profile are refused too.
  p = *resident("DS35Q1GA");
  p.addr_bytes = 3;
  TEST_ASSERT_EQUAL_INT(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, MAX_PAGE));
}

void test_manual_nor_profile(void) {
  active_profile_t p;
  nand_profile_manual_nor(&p, 64 * 1024);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&p, MAX_PAGE));
  TEST_ASSERT_EQUAL_UINT32(64 * 1024, nand_profile_bytes(&p));
  TEST_ASSERT_EQUAL_UINT8(3, p.addr_bytes);
  nand_profile_manual_nor(&p, 64u << 20);
  TEST_ASSERT_EQUAL_INT(NAND_PRF_OK, nand_profile_check(&p, MAX_PAGE));
  TEST_ASSERT_EQUAL_UINT8(4, p.addr_bytes);
  TEST_ASSERT_EQUAL_UINT8(NOR_ADDR4_ENTER, p.addr4_mode);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_struct_size_matches_host_layout);
  RUN_TEST(test_golden_ds35_unpacks_every_field);
  RUN_TEST(test_resident_entries_equal_pushed_blobs);
  RUN_TEST(test_every_resident_entry_passes_boot_checks);
  RUN_TEST(test_unpack_rejects_and_leaves_output_untouched);
  RUN_TEST(test_unpack_rejects_truncated_blob);
  RUN_TEST(test_ds35_generic2_decode);
  RUN_TEST(test_micron3_decode_matches_table_9);
  RUN_TEST(test_four_bit_field_reaches_index_15);
  RUN_TEST(test_find_resolves_resident_ids);
  RUN_TEST(test_find_uses_dev2_and_never_guesses);
  RUN_TEST(test_pick_honours_saved_choice_only_for_its_id);
  RUN_TEST(test_id_cross_check);
  RUN_TEST(test_bbm_uses_profile_width_and_offset);
  RUN_TEST(test_bbm_pages);
  RUN_TEST(test_manual_profile_is_valid_and_generic);
  RUN_TEST(test_golden_nor_blobs_unpack_and_equal_resident);
  RUN_TEST(test_nor_checks_fail_closed);
  RUN_TEST(test_manual_nor_profile);
  return UNITY_END();
}
