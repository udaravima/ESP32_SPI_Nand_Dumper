#include <unity.h>
#include "nand_profile.h"

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

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_active_profile_is_110_bytes);
  RUN_TEST(test_decode_generic2);
  RUN_TEST(test_decode_micron3);
  RUN_TEST(test_decode_xtx4_reaches_index_15);
  RUN_TEST(test_decode_xtx_g0xa);
  return UNITY_END();
}
