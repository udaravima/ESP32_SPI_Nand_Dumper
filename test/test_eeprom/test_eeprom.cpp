// Native tests for the serial EEPROM read path (src/eeprom_seq.*) against the
// simulated 24xx and 25xx parts in sim_eeprom.h, using the resident profiles
// generated from db/chips/eeprom/.
#include <unity.h>
#include <stdlib.h>
#include "eeprom_seq.h"
#include "nand_profile.h"
#include "sim_eeprom.h"

static const active_profile_t *prof(uint8_t family, const char *name) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  const active_profile_t *p = nand_profile_by_name(t, n, family, name);
  TEST_ASSERT_NOT_NULL_MESSAGE(p, name);
  TEST_ASSERT_EQUAL(NAND_PRF_OK, nand_profile_check(p, 8192));
  return p;
}
static const active_profile_t *i2c_prof(const char *n) { return prof(CHIP_FAMILY_I2C_EEPROM, n); }
static const active_profile_t *spi_prof(const char *n) { return prof(CHIP_FAMILY_SPI_EEPROM, n); }

static eeprom_seq_t seq_i2c(sim_i2c_ee_t *c, const active_profile_t *p) {
  eeprom_bus_t bus = { sim_i2c_ee_read, sim_i2c_ee_probe, NULL, c };
  eeprom_seq_t s;
  eeprom_seq_init(&s, bus);
  eeprom_seq_apply(&s, p);
  return s;
}
static eeprom_seq_t seq_spi(sim_spi_ee_t *c, const active_profile_t *p) {
  eeprom_bus_t bus = { NULL, NULL, sim_spi_ee_xfer, c };
  eeprom_seq_t s;
  eeprom_seq_init(&s, bus);
  eeprom_seq_apply(&s, p);
  return s;
}

// Dump the whole part the way cmd_dump does (read units, verified) and check
// every byte against the simulated array.
static void dump_and_compare(eeprom_seq_t *s, const active_profile_t *p, const uint8_t *mem) {
  uint32_t size = nand_profile_bytes(p), unit = p->page_size;
  uint8_t *buf = (uint8_t *)malloc(unit), *scratch = (uint8_t *)malloc(unit);
  uint32_t retries = 0;
  for (uint32_t a = 0; a < size; a += unit) {
    TEST_ASSERT_EQUAL(NAND_PAGE_OK,
                      eeprom_seq_read_verified(s, a, buf, scratch, (int)unit, 3, &retries));
    TEST_ASSERT_EQUAL_MEMORY(mem + a, buf, unit);
  }
  TEST_ASSERT_EQUAL_UINT32(0, retries);
  free(buf); free(scratch);
}

// ---- I2C ----

static void test_scan_reports_every_address_the_part_answers(void) {
  sim_i2c_ee_t c;
  sim_i2c_ee_init(&c, 256, 1, 0, 0, 0x0);           // 24C02, A2..A0 = 000
  eeprom_seq_t s = seq_i2c(&c, i2c_prof("24C02"));
  TEST_ASSERT_EQUAL_HEX8(0x01, eeprom_seq_i2c_scan(&s));
  c.pins = 0x5;                                     // straps 101 -> 0x55
  TEST_ASSERT_EQUAL_HEX8(0x20, eeprom_seq_i2c_scan(&s));
  sim_i2c_ee_free(&c);

  sim_i2c_ee_init(&c, 2048, 1, 3, 0, 0x0);          // 24C16 takes all eight
  s = seq_i2c(&c, i2c_prof("24C16"));
  TEST_ASSERT_EQUAL_HEX8(0xFF, eeprom_seq_i2c_scan(&s));
  sim_i2c_ee_free(&c);

  sim_i2c_ee_init(&c, 512, 1, 1, 0, 0x6);           // 24C04, A2 A1 = 11
  s = seq_i2c(&c, i2c_prof("24C04"));
  TEST_ASSERT_EQUAL_HEX8(0xC0, eeprom_seq_i2c_scan(&s));
  c.present = false;
  TEST_ASSERT_EQUAL_HEX8(0x00, eeprom_seq_i2c_scan(&s));
  sim_i2c_ee_free(&c);
}

static void test_i2c_base_needs_the_whole_address_set(void) {
  TEST_ASSERT_EQUAL(0x50, eeprom_i2c_base(i2c_prof("24C02"), 0x01));
  TEST_ASSERT_EQUAL(0x57, eeprom_i2c_base(i2c_prof("24C02"), 0x80));
  TEST_ASSERT_EQUAL(-1, eeprom_i2c_base(i2c_prof("24C02"), 0x00));
  TEST_ASSERT_EQUAL(0x50, eeprom_i2c_base(i2c_prof("24C16"), 0xFF));
  TEST_ASSERT_EQUAL(-1, eeprom_i2c_base(i2c_prof("24C16"), 0x7F));   // a 24C08 at most
  TEST_ASSERT_EQUAL(0x56, eeprom_i2c_base(i2c_prof("24C04"), 0xC0));
  TEST_ASSERT_EQUAL(-1, eeprom_i2c_base(i2c_prof("24C04"), 0x40));   // only half answers
  TEST_ASSERT_EQUAL(0x54, eeprom_i2c_base(i2c_prof("24C08"), 0xF0));
  // 24xx1025: block bit at A2, so the pair is 0x51 + 0x55.
  TEST_ASSERT_EQUAL(0x51, eeprom_i2c_base(i2c_prof("24xx1025"), 0x22));
  TEST_ASSERT_EQUAL(-1, eeprom_i2c_base(i2c_prof("24xx1025"), 0x03));
  TEST_ASSERT_EQUAL(-1, eeprom_i2c_base(spi_prof("25xx256"), 0xFF)); // not an I2C part
}

static void check_i2c_dump(const char *name, uint32_t size, uint8_t ab, uint8_t bits,
                           uint8_t shift, uint8_t pins, bool a2_high) {
  const active_profile_t *p = i2c_prof(name);
  TEST_ASSERT_EQUAL_UINT32(size, nand_profile_bytes(p));
  sim_i2c_ee_t c;
  sim_i2c_ee_init(&c, size, ab, bits, shift, pins);
  c.needs_a2_high = a2_high;
  eeprom_seq_t s = seq_i2c(&c, p);
  int base = eeprom_i2c_base(p, eeprom_seq_i2c_scan(&s));
  TEST_ASSERT_TRUE_MESSAGE(base >= 0, name);
  eeprom_seq_set_i2c_base(&s, (uint8_t)base);
  dump_and_compare(&s, p, c.mem);
  TEST_ASSERT_EQUAL_MESSAGE(0, c.protocol_errors, name);
  TEST_ASSERT_EQUAL_MESSAGE(0, c.page_bytes, name);   // never a data byte
  TEST_ASSERT_EQUAL_MESSAGE(0, c.writes, name);
  TEST_ASSERT_TRUE(c.max_rx <= EEPROM_I2C_CHUNK);
  TEST_ASSERT_EQUAL_UINT32(0, s.nacks);
  sim_i2c_ee_free(&c);
}

static void test_i2c_dumps_every_addressing_scheme(void) {
  check_i2c_dump("24C01", 128, 1, 0, 0, 0x0, false);
  check_i2c_dump("24C02", 256, 1, 0, 0, 0x3, false);
  check_i2c_dump("24C04", 512, 1, 1, 0, 0x4, false);
  check_i2c_dump("24C08", 1024, 1, 2, 0, 0x0, false);
  check_i2c_dump("24C16", 2048, 1, 3, 0, 0x0, false);
  check_i2c_dump("24C32", 4096, 2, 0, 0, 0x7, false);
  check_i2c_dump("24C256", 32768, 2, 0, 0, 0x0, false);
  check_i2c_dump("24C512", 65536, 2, 0, 0, 0x2, false);
  check_i2c_dump("24CM01", 131072, 2, 1, 0, 0x0, false);
  check_i2c_dump("24xx1025", 131072, 2, 1, 2, 0x4, true);
  check_i2c_dump("24CM02", 262144, 2, 2, 0, 0x4, false);
}

static void test_i2c_never_straddles_a_device_address(void) {
  // A read across 24C04's A8 boundary is split: 0x50 for the low half, 0x51 above.
  sim_i2c_ee_t c;
  sim_i2c_ee_init(&c, 512, 1, 1, 0, 0x0);
  eeprom_seq_t s = seq_i2c(&c, i2c_prof("24C04"));
  TEST_ASSERT_EQUAL_HEX8(0x50, eeprom_i2c_dev(&s, 0x0FF));
  TEST_ASSERT_EQUAL_HEX8(0x51, eeprom_i2c_dev(&s, 0x100));
  TEST_ASSERT_EQUAL_UINT32(0x10, eeprom_word_span(&s, 0xF0));
  uint8_t buf[0x40];
  TEST_ASSERT_TRUE(eeprom_seq_read(&s, 0xF0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_MEMORY(c.mem + 0xF0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(2, c.transactions);
  sim_i2c_ee_free(&c);
}

static void test_i2c_noise_is_retried(void) {
  sim_i2c_ee_t c;
  sim_i2c_ee_init(&c, 4096, 2, 0, 0, 0x0);
  c.flip_every = 5;
  const active_profile_t *p = i2c_prof("24C32");
  eeprom_seq_t s = seq_i2c(&c, p);
  uint8_t buf[256], scratch[256];
  uint32_t retries = 0;
  for (uint32_t a = 0; a < 4096; a += 256) {
    TEST_ASSERT_EQUAL(NAND_PAGE_OK,
                      eeprom_seq_read_verified(&s, a, buf, scratch, 256, 5, &retries));
    TEST_ASSERT_EQUAL_MEMORY(c.mem + a, buf, 256);
  }
  TEST_ASSERT_TRUE(retries > 0);
  sim_i2c_ee_free(&c);
}

static void test_i2c_part_gone_is_unstable_and_reads_ff(void) {
  sim_i2c_ee_t c;
  sim_i2c_ee_init(&c, 256, 1, 0, 0, 0x0);
  eeprom_seq_t s = seq_i2c(&c, i2c_prof("24C02"));
  c.present = false;
  uint8_t buf[256], scratch[256];
  uint32_t retries = 0;
  TEST_ASSERT_EQUAL(NAND_PAGE_UNSTABLE,
                    eeprom_seq_read_verified(&s, 0, buf, scratch, 256, 2, &retries));
  for (int i = 0; i < 256; i++) TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
  TEST_ASSERT_TRUE(s.nacks > 0);
  sim_i2c_ee_free(&c);
}

// ---- SPI ----

static void check_spi_dump(const char *name, uint32_t size, uint8_t ab, bool a8) {
  const active_profile_t *p = spi_prof(name);
  TEST_ASSERT_EQUAL_UINT32(size, nand_profile_bytes(p));
  sim_spi_ee_t c;
  sim_spi_ee_init(&c, size, ab, a8);
  eeprom_seq_t s = seq_spi(&c, p);
  TEST_ASSERT_TRUE(eeprom_spi_status_plausible(eeprom_seq_spi_status(&s)));
  dump_and_compare(&s, p, c.mem);
  TEST_ASSERT_EQUAL_MESSAGE(0, c.protocol_errors, name);
  TEST_ASSERT_EQUAL_MESSAGE(0, c.writes, name);
  sim_spi_ee_free(&c);
}

static void test_spi_dumps_every_address_width(void) {
  check_spi_dump("25xx010", 128, 1, false);
  check_spi_dump("25xx020", 256, 1, false);
  check_spi_dump("25xx040", 512, 1, true);
  check_spi_dump("25xx080", 1024, 2, false);
  check_spi_dump("25xx256", 32768, 2, false);
  check_spi_dump("25xx1024", 131072, 3, false);
  check_spi_dump("M95M04", 524288, 3, false);
  check_spi_dump("MB85RS64", 8192, 2, false);
}

static void test_spi_a8_goes_in_opcode_bit3(void) {
  sim_spi_ee_t c;
  sim_spi_ee_init(&c, 512, 1, true);
  eeprom_seq_t s = seq_spi(&c, spi_prof("25xx040"));
  TEST_ASSERT_EQUAL_HEX8(0x03, eeprom_spi_opcode(&s, 0x0FF));
  TEST_ASSERT_EQUAL_HEX8(0x0B, eeprom_spi_opcode(&s, 0x100));
  uint8_t buf[0x20];
  TEST_ASSERT_TRUE(eeprom_seq_read(&s, 0xF0, buf, sizeof(buf)));   // split at A8
  TEST_ASSERT_EQUAL_MEMORY(c.mem + 0xF0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(2, c.reads);
  sim_spi_ee_free(&c);
}

static void test_spi_status_tells_an_empty_socket(void) {
  sim_spi_ee_t c;
  sim_spi_ee_init(&c, 32768, 2, false);
  c.sr = 0x8C;                                      // WPEN + BP1:0 set: still a 25xx
  eeprom_seq_t s = seq_spi(&c, spi_prof("25xx256"));
  TEST_ASSERT_TRUE(eeprom_spi_status_plausible(eeprom_seq_spi_status(&s)));
  c.present = false;
  TEST_ASSERT_EQUAL_HEX8(0xFF, eeprom_seq_spi_status(&s));
  TEST_ASSERT_FALSE(eeprom_spi_status_plausible(0xFF));
  TEST_ASSERT_EQUAL(0, c.writes);
  sim_spi_ee_free(&c);
}

static void test_spi_noise_is_retried(void) {
  sim_spi_ee_t c;
  sim_spi_ee_init(&c, 8192, 2, false);
  c.flip_every = 3;
  eeprom_seq_t s = seq_spi(&c, spi_prof("25xx640"));
  uint8_t buf[256], scratch[256];
  uint32_t retries = 0;
  for (uint32_t a = 0; a < 8192; a += 256) {
    TEST_ASSERT_EQUAL(NAND_PAGE_OK,
                      eeprom_seq_read_verified(&s, a, buf, scratch, 256, 5, &retries));
    TEST_ASSERT_EQUAL_MEMORY(c.mem + a, buf, 256);
  }
  TEST_ASSERT_TRUE(retries > 0);
  sim_spi_ee_free(&c);
}

// ---- Profiles ----

static void test_eeprom_profiles_never_match_an_id(void) {
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  nand_prf_err_t e;
  // An empty socket reads 00 00 00 or FF FF FF; neither may resolve to an EEPROM.
  const active_profile_t *hit = nand_profile_find(t, n, 0x00, 0x00, 0x00, &e);
  TEST_ASSERT_TRUE(hit == NULL || !chip_family_is_eeprom(hit->family));
  hit = nand_profile_find(t, n, 0xFF, 0xFF, 0xFF, &e);
  TEST_ASSERT_TRUE(hit == NULL || !chip_family_is_eeprom(hit->family));
  TEST_ASSERT_NULL(nand_profile_pick(t, n, 0x00, 0x00, "24C02"));
  TEST_ASSERT_NULL(nand_profile_by_name(t, n, CHIP_FAMILY_SPI_EEPROM, "24C02"));
}

static void test_eeprom_profile_checks(void) {
  active_profile_t p = *i2c_prof("24C16");
  p.dev_addr_bits = 2;                               // 2 KiB no longer reachable
  TEST_ASSERT_EQUAL(NAND_PRF_E_GEOMETRY, nand_profile_check(&p, 8192));
  p = *i2c_prof("24C16");
  p.dev_addr_shift = 1;                              // bits 1..3: past A2
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *i2c_prof("24C02");
  p.id_mfr = 0x1F;                                   // an EEPROM carries no ID
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *i2c_prof("24C02");
  p.op_read_cache = 0x03;                            // I2C has no opcodes
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *spi_prof("25xx256");
  p.op_set_feat = 0x01;                              // no status write, ever
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *spi_prof("25xx256");
  p.read_mode = NAND_READ_QUAD;
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *spi_prof("25xx256");
  p.dev_addr_bits = 1; p.dev_addr_shift = 3;         // A8 carry needs a 1-byte address
  TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
  p = *spi_prof("M95M04");
  p.total_blocks *= 2;                               // 1 MiB: above the EEPROM bound
  TEST_ASSERT_EQUAL(NAND_PRF_E_GEOMETRY, nand_profile_check(&p, 8192));
  // A NAND/NOR profile must keep the EEPROM bytes zero.
  unsigned n;
  const active_profile_t *t = nand_profile_resident(&n);
  for (unsigned i = 0; i < n; i++)
    if (!chip_family_is_eeprom(t[i].family)) {
      p = t[i];
      p.dev_addr_bits = 1;
      TEST_ASSERT_EQUAL(NAND_PRF_E_STRUCTURE, nand_profile_check(&p, 8192));
    }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_scan_reports_every_address_the_part_answers);
  RUN_TEST(test_i2c_base_needs_the_whole_address_set);
  RUN_TEST(test_i2c_dumps_every_addressing_scheme);
  RUN_TEST(test_i2c_never_straddles_a_device_address);
  RUN_TEST(test_i2c_noise_is_retried);
  RUN_TEST(test_i2c_part_gone_is_unstable_and_reads_ff);
  RUN_TEST(test_spi_dumps_every_address_width);
  RUN_TEST(test_spi_a8_goes_in_opcode_bit3);
  RUN_TEST(test_spi_status_tells_an_empty_socket);
  RUN_TEST(test_spi_noise_is_retried);
  RUN_TEST(test_eeprom_profiles_never_match_an_id);
  RUN_TEST(test_eeprom_profile_checks);
  return UNITY_END();
}
