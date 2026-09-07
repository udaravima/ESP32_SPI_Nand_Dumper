#include <unity.h>
#include <string.h>
#include "nand_profile.h"
#include "golden_ds35.h"
#include "nand_profiles_generated.h"

void test_active_profile_is_110_bytes(void) {
  // The wire contract: host packs 110 bytes, device reads 110 bytes.
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)sizeof(active_profile_t));
  TEST_ASSERT_EQUAL_UINT32(110u, (uint32_t)NAND_PROFILE_SIZE);
}

static active_profile_t mk(uint8_t shift, uint8_t mask, const uint8_t map[16]) {
  active_profile_t p; // zeroed fields we don't touch don't matter here
  p.ecc_shift = shift; p.ecc_mask = mask;
  for (int i = 0; i < 16; i++) p.ecc_map[i] = map[i];
  return p;
}

void test_decode_generic2(void) {
  // [5:4], mask 0x3: 0=OK 1=CORR 2=UNCOR 3=UNCOR
  const uint8_t m[16] = {0,1,3,3, 3,3,3,3, 3,3,3,3, 3,3,3,3};
  active_profile_t p = mk(4, 0x3, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_OK,            nand_profile_severity(&p, 0x00));
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED,     nand_profile_severity(&p, 0x10)); // field 1
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x20)); // field 2
}

void test_decode_micron3(void) {
  // [6:4], mask 0x7: 0=OK 1=CORR 2=UNCOR 3=REFRESH 5=REFRESH
  const uint8_t m[16] = {0,1,3,2, 3,2,3,3, 3,3,3,3, 3,3,3,3};
  active_profile_t p = mk(4, 0x7, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE,        nand_profile_severity(&p, 0x20)); // field 2
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH,    nand_profile_severity(&p, 0x30)); // field 3
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH,    nand_profile_severity(&p, 0x50)); // field 5
}

void test_decode_micron3_reserved_values(void) {
  // Datasheet-reserved ECCS field values (4, 6, 7) are unnamed in the micron3
  // map and must fail closed to UNCORRECTABLE, not silently be ignored.
  const uint8_t m[16] = {0,1,3,2, 3,2,3,3, 3,3,3,3, 3,3,3,3};
  active_profile_t p = mk(4, 0x7, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x40)); // field 4 (reserved)
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x60)); // field 6 (reserved)
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x70)); // field 7 (reserved)
}

void test_decode_xtx4_reaches_index_15(void) {
  // [7:4], mask 0xF, uncorrectable at field 0xF — the Wall-1 out-of-bounds case
  uint8_t m[16]; m[0]=0; for (int i=1;i<15;i++) m[i]=1; m[15]=3;
  active_profile_t p = mk(4, 0xF, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0xF0)); // field 15
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED,     nand_profile_severity(&p, 0x50)); // field 5
}

void test_decode_xtx_g0xa(void) {
  // [5:2], mask 0xF, uncorrectable at field 8, refresh at 12 (mainline encoding)
  uint8_t m[16]; for (int i=0;i<16;i++) m[i]=1; m[0]=0; m[8]=3; m[12]=2;
  active_profile_t p = mk(2, 0xF, m);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE,     nand_profile_severity(&p, 8u<<2)); // field 8
  TEST_ASSERT_EQUAL_INT(NAND_SEV_CORRECTED_REFRESH, nand_profile_severity(&p, 12u<<2)); // field 12
}

void test_golden_blob_unpacks_to_ds35(void) {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(active_profile_t), GOLDEN_DS35Q1GA_LEN);
  active_profile_t p;
  memcpy(&p, GOLDEN_DS35Q1GA_BLOB, sizeof(p));
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p.name);
  TEST_ASSERT_EQUAL_UINT32(2112u, p.page_size);
  TEST_ASSERT_EQUAL_UINT32(64u, p.spare_size);
  TEST_ASSERT_EQUAL_UINT32(64u, p.pages_per_block);
  TEST_ASSERT_EQUAL_UINT32(1024u, p.total_blocks);
  TEST_ASSERT_EQUAL_UINT8(4, p.ecc_shift);
  TEST_ASSERT_EQUAL_UINT8(0x3, p.ecc_mask);
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, p.ecc_map[2]);
  TEST_ASSERT_EQUAL_UINT8(0xB0, p.qe_addr);
  TEST_ASSERT_EQUAL_UINT8(0x01, p.qe_bit);
  TEST_ASSERT_EQUAL_UINT8(0x00, p.bbm_off);
  TEST_ASSERT_EQUAL_UINT8(0xFF, p.bbm_good);
  TEST_ASSERT_EQUAL_UINT16(3300u, p.vcc_mv);
  // decoder runs against the unpacked profile
  TEST_ASSERT_EQUAL_INT(NAND_SEV_UNCORRECTABLE, nand_profile_severity(&p, 0x20));
}

void test_resident_array_has_two_chips(void) {
  TEST_ASSERT_EQUAL_UINT32(2u, PROFILES_COUNT);
}

void test_profile_lookup_finds_ds35(void) {
  const active_profile_t *p = nand_profile_lookup(0xE5, 0x71);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", p->name);
  TEST_ASSERT_EQUAL_UINT32(2112u, p->page_size);
  TEST_ASSERT_EQUAL_UINT8(0xB0, p->qe_addr);   // the profile that carries the DS35 QE fix
}

void test_profile_lookup_finds_micron(void) {
  const active_profile_t *p = nand_profile_lookup(0x2C, 0x24);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_STRING("MT29F2G01ABAGD", p->name);
}

void test_profile_lookup_unknown_is_null(void) {
  TEST_ASSERT_NULL(nand_profile_lookup(0x00, 0x00));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_active_profile_is_110_bytes);
  RUN_TEST(test_decode_generic2);
  RUN_TEST(test_decode_micron3);
  RUN_TEST(test_decode_micron3_reserved_values);
  RUN_TEST(test_decode_xtx4_reaches_index_15);
  RUN_TEST(test_decode_xtx_g0xa);
  RUN_TEST(test_golden_blob_unpacks_to_ds35);
  RUN_TEST(test_resident_array_has_two_chips);
  RUN_TEST(test_profile_lookup_finds_ds35);
  RUN_TEST(test_profile_lookup_finds_micron);
  RUN_TEST(test_profile_lookup_unknown_is_null);
  return UNITY_END();
}
