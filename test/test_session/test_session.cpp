// Vendor-profile stage 3: the host -> device push session (design § 6).
// A byte-buffer link stands in for TCP, so the whole I/P/A/G exchange,
// including every fail-closed path, runs without hardware.
#include <unity.h>
#include <string.h>
#include "nand_session.h"
#include "nand_profile.h"
#include "dump_header.h"
#include "../test_profile/golden_blobs.h"

#define MAX_PAGE 8192

// ---- in-memory link -----------------------------------------------------------
typedef struct {
  uint8_t in[512]; size_t in_len, in_pos;
  uint8_t out[1024]; size_t out_len, out_pos;
} mem_link_t;

static size_t mem_read(void *ctx, uint8_t *buf, size_t n, uint32_t) {
  mem_link_t *m = (mem_link_t *)ctx;
  size_t k = m->in_len - m->in_pos < n ? m->in_len - m->in_pos : n;
  memcpy(buf, m->in + m->in_pos, k);
  m->in_pos += k;
  return k;
}
static size_t mem_write(void *ctx, const uint8_t *buf, size_t n) {
  mem_link_t *m = (mem_link_t *)ctx;
  memcpy(m->out + m->out_len, buf, n);
  m->out_len += n;
  return n;
}

static mem_link_t M;
static nand_link_t L = {mem_read, mem_write, &M};
static nand_session_t S;
static active_profile_t resident_active;

void setUp(void) {
  memset(&M, 0, sizeof(M));
  memset(&S, 0, sizeof(S));
  nand_profile_manual(&resident_active, 2112, 64, 64, 1024, 1);
  S.id[0] = 0xE5; S.id[1] = 0x71; S.id[2] = 0x00;     // a DS35 in the socket
  S.nor_id[0] = 0xFF; S.nor_id[1] = 0xE5; S.nor_id[2] = 0x71;   // its plain-9Fh view
  S.chip_state = NAND_CHIP_UNKNOWN;
  S.active = &resident_active;
  S.max_page_size = MAX_PAGE;
  S.timeout_ms = 100;
  nand_session_begin(&S);
}
void tearDown(void) {}

static void feed(const void *p, size_t n) { memcpy(M.in + M.in_len, p, n); M.in_len += n; }
static void feed_u32(uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  feed(b, 4);
}
static uint32_t blob_crc(const uint8_t *blob) {
  const uint8_t *c = blob + 6 + NAND_PROFILE_SIZE;
  return c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);
}

// Run commands until the input is drained or the session ends.
static nand_sess_action_t run(void) {
  nand_sess_action_t a = NAND_SESS_MORE;
  while (M.in_pos < M.in_len) {
    a = nand_session_handle(&S, &L, M.in[M.in_pos++]);
    if (a == NAND_SESS_GO || a == NAND_SESS_CLOSE) break;
  }
  return a;
}

// Pop the next response frame, checking magic and CRC. Returns its payload.
typedef struct { uint8_t cmd, status; uint16_t len; const uint8_t *payload; } resp_t;
static resp_t next_resp(void) {
  resp_t r;
  const uint8_t *f = M.out + M.out_pos;
  TEST_ASSERT_TRUE_MESSAGE(M.out_len - M.out_pos >= 12, "no response frame");
  TEST_ASSERT_EQUAL_MEMORY("NRSP", f, 4);
  r.cmd = f[4]; r.status = f[5]; r.len = f[6] | (f[7] << 8);
  r.payload = f + 8;
  const uint8_t *c = f + 8 + r.len;
  uint32_t crc = c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);
  TEST_ASSERT_EQUAL_HEX32(dump_crc32(f, 8 + r.len), crc);
  M.out_pos += 12 + r.len;
  return r;
}

static void push(const uint8_t *blob) { feed("P", 1); feed(blob, NAND_PROFILE_BLOB_SIZE); }

// ---- tests ----------------------------------------------------------------------
void test_bare_go_still_dumps(void) {
  feed("G", 1);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_GO, run());
  TEST_ASSERT_EQUAL_UINT(0, M.out_len);       // the dump header is the only reply
}

void test_info_reports_id_state_and_active_name(void) {
  feed("I", 1);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT8('I', r.cmd);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, r.status);
  TEST_ASSERT_EQUAL_UINT16(NAND_INFO_SIZE, r.len);
  TEST_ASSERT_EQUAL_UINT8(NAND_SESSION_VER, r.payload[0]);
  TEST_ASSERT_EQUAL_UINT8(NAND_PROFILE_SCHEMA_VER, r.payload[1]);
  TEST_ASSERT_EQUAL_UINT8(MAX_PAGE & 0xFF, r.payload[2]);
  TEST_ASSERT_EQUAL_UINT8(MAX_PAGE >> 8, r.payload[3]);
  TEST_ASSERT_EQUAL_HEX8(0xE5, r.payload[6]);
  TEST_ASSERT_EQUAL_HEX8(0x71, r.payload[7]);
  TEST_ASSERT_EQUAL_UINT8(NAND_CHIP_UNKNOWN, r.payload[9]);
  TEST_ASSERT_EQUAL_STRING("MANUAL", (const char *)r.payload + 10);
}

void test_push_echo_arm_go(void) {
  push(GOLDEN_DS35Q1GA);
  feed("A", 1); feed_u32(blob_crc(GOLDEN_DS35Q1GA));
  TEST_ASSERT_EQUAL_INT(NAND_SESS_ARMED, run());

  resp_t p = next_resp();
  TEST_ASSERT_EQUAL_UINT8('P', p.cmd);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, p.status);
  TEST_ASSERT_EQUAL_UINT16(NAND_ECHO_SIZE, p.len);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", (const char *)p.payload);
  TEST_ASSERT_EQUAL_UINT8(2112 & 0xFF, p.payload[24]);
  TEST_ASSERT_EQUAL_UINT8(2112 >> 8, p.payload[25]);
  TEST_ASSERT_EQUAL_UINT8(1, p.payload[40]);                  // planes
  TEST_ASSERT_EQUAL_HEX8(0xE5, p.payload[41]);                // expected mfr
  TEST_ASSERT_EQUAL_HEX8(0xE5, p.payload[45]);                // detected mfr
  TEST_ASSERT_EQUAL_MEMORY(GOLDEN_DS35Q1GA + 6 + NAND_PROFILE_SIZE, p.payload + 48, 4);   // blob CRC

  resp_t a = next_resp();
  TEST_ASSERT_EQUAL_UINT8('A', a.cmd);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, a.status);
  TEST_ASSERT_EQUAL_STRING("DS35Q1GA", S.armed.name);
  TEST_ASSERT_EQUAL_UINT32(2112, S.armed.page_size);
  TEST_ASSERT_FALSE(S.has_staged);

  feed("G", 1);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_GO, run());
}

void test_push_for_another_chip_is_refused(void) {
  push(GOLDEN_MT29F2G01ABAGD);                // Micron blob, Dosilicon silicon
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ID_MISMATCH, r.status);
  TEST_ASSERT_EQUAL_UINT16(NAND_ECHO_SIZE, r.len);            // echo says why
  TEST_ASSERT_EQUAL_HEX8(0x2C, r.payload[41]);
  TEST_ASSERT_EQUAL_HEX8(0xE5, r.payload[45]);
  TEST_ASSERT_FALSE(S.has_staged);
}

void test_dev2_mismatch_is_refused(void) {
  uint8_t blob[NAND_PROFILE_BLOB_SIZE];
  memcpy(blob, GOLDEN_DS35Q1GA, sizeof(blob));
  active_profile_t p;                         // blob + 6 is not 4-byte aligned
  memcpy(&p, blob + 6, sizeof(p));
  p.id_dev2 = 0x05; p.id_flags = NAND_PROFILE_ID_HAS_DEV2;
  memcpy(blob + 6, &p, sizeof(p));
  uint32_t crc = dump_crc32(blob, 6 + NAND_PROFILE_SIZE);
  memcpy(blob + 6 + NAND_PROFILE_SIZE, &crc, 4);
  push(blob);
  run();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ID_MISMATCH, next_resp().status);
}

void test_arm_needs_the_staged_crc(void) {
  push(GOLDEN_DS35Q1GA);
  feed("A", 1); feed_u32(blob_crc(GOLDEN_DS35Q1GA) ^ 1);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  next_resp();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ARM_CRC, next_resp().status);
  TEST_ASSERT_FALSE(S.has_staged);            // a wrong ARM disarms entirely

  feed("A", 1); feed_u32(blob_crc(GOLDEN_DS35Q1GA));       // too late now
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_NOT_STAGED, next_resp().status);
}

void test_go_with_unarmed_push_is_refused(void) {
  push(GOLDEN_DS35Q1GA);
  feed("G", 1);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_CLOSE, run());
  next_resp();
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT8('G', r.cmd);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_NOT_ARMED, r.status);
}

void test_bad_crc_keeps_session_but_stages_nothing(void) {
  uint8_t blob[NAND_PROFILE_BLOB_SIZE];
  memcpy(blob, GOLDEN_DS35Q1GA, sizeof(blob));
  blob[40] ^= 0xFF;
  push(blob);
  feed("I", 1);
  run();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_BAD_CRC, next_resp().status);
  TEST_ASSERT_EQUAL_UINT8('I', next_resp().cmd);            // still in sync
  TEST_ASSERT_FALSE(S.has_staged);
}

void test_failed_push_clears_an_earlier_stage(void) {
  push(GOLDEN_DS35Q1GA);
  push(GOLDEN_MT29F2G01ABAGD);
  feed("A", 1); feed_u32(blob_crc(GOLDEN_DS35Q1GA));
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, next_resp().status);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ID_MISMATCH, next_resp().status);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_NOT_STAGED, next_resp().status);
}

void test_insane_geometry_is_refused(void) {
  uint8_t blob[NAND_PROFILE_BLOB_SIZE];
  memcpy(blob, GOLDEN_DS35Q1GA, sizeof(blob));
  uint32_t big = 16384;                                     // > the page buffer
  memcpy(blob + 6 + offsetof(active_profile_t, page_size), &big, 4);
  uint32_t crc = dump_crc32(blob, 6 + NAND_PROFILE_SIZE);
  memcpy(blob + 6 + NAND_PROFILE_SIZE, &crc, 4);
  push(blob);
  run();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_PAGE_TOO_BIG, next_resp().status);
  TEST_ASSERT_FALSE(S.has_staged);
}

void test_unknown_framing_closes_the_connection(void) {
  uint8_t blob[NAND_PROFILE_BLOB_SIZE];
  memcpy(blob, GOLDEN_DS35Q1GA, sizeof(blob));
  blob[3] = NAND_PROFILE_SCHEMA_VER + 1;     // a newer schema: never guess its length
  push(blob);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_CLOSE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_SCHEMA_VER, next_resp().status);

  setUp();
  memcpy(blob, GOLDEN_DS35Q1GA, sizeof(blob));
  blob[4] = 0x79;                             // len 121
  push(blob);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_CLOSE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_BAD_LEN, next_resp().status);

  setUp();
  feed("PXYZ", 4); feed("\x01\x78\x00", 3);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_CLOSE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_BAD_MAGIC, next_resp().status);
}

void test_truncated_push_times_out_and_closes(void) {
  feed("P", 1);
  feed(GOLDEN_DS35Q1GA, 60);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_CLOSE, run());
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_TIMEOUT, next_resp().status);
  TEST_ASSERT_FALSE(S.has_staged);
}

void test_unknown_command_is_answered(void) {
  feed("Z\n", 2);
  TEST_ASSERT_EQUAL_INT(NAND_SESS_MORE, run());
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT8('Z', r.cmd);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_BAD_CMD, r.status);
  TEST_ASSERT_EQUAL_UINT(M.out_pos, M.out_len);              // newline ignored
}

void test_new_connection_forgets_the_stage(void) {
  push(GOLDEN_DS35Q1GA);
  run();
  TEST_ASSERT_TRUE(S.has_staged);
  nand_session_begin(&S);
  TEST_ASSERT_FALSE(S.has_staged);
}

// ---- SPI NOR (session v2) ------------------------------------------------------------
static void socket_holds_w25q128(void) {
  S.id[0] = 0x40; S.id[1] = 0x18; S.id[2] = 0xEF;      // NAND-style read of a NOR part
  S.nor_id[0] = 0xEF; S.nor_id[1] = 0x40; S.nor_id[2] = 0x18;
}

void test_info_v2_carries_nor_id_and_family(void) {
  socket_holds_w25q128();
  feed("I", 1);
  run();
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT16(38, r.len);
  TEST_ASSERT_EQUAL_UINT8(2, r.payload[0]);                   // session v2
  TEST_ASSERT_EQUAL_HEX8(0xEF, r.payload[34]);
  TEST_ASSERT_EQUAL_HEX8(0x40, r.payload[35]);
  TEST_ASSERT_EQUAL_HEX8(0x18, r.payload[36]);
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NAND, r.payload[37]);   // manual NAND active
}

void test_nor_push_is_checked_against_the_nor_id(void) {
  socket_holds_w25q128();
  push(GOLDEN_W25Q128_V);
  feed("A", 1); feed_u32(blob_crc(GOLDEN_W25Q128_V));
  TEST_ASSERT_EQUAL_INT(NAND_SESS_ARMED, run());
  resp_t p = next_resp();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, p.status);
  TEST_ASSERT_EQUAL_UINT16(NAND_ECHO_SIZE, p.len);
  TEST_ASSERT_EQUAL_HEX8(0xEF, p.payload[45]);                // detected, NOR view
  TEST_ASSERT_EQUAL_HEX8(0x18, p.payload[47]);
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, p.payload[52]);
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_OK, next_resp().status);   // ARM
  TEST_ASSERT_EQUAL_UINT8(CHIP_FAMILY_SPI_NOR, S.armed.family);
}

void test_nor_profile_for_a_nand_socket_is_refused(void) {
  push(GOLDEN_W25Q128_V);                                     // DS35 in the socket
  run();
  resp_t r = next_resp();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ID_MISMATCH, r.status);
  TEST_ASSERT_EQUAL_HEX8(0xFF, r.payload[45]);                // the NOR view it compared
  TEST_ASSERT_FALSE(S.has_staged);
}

void test_nor_capacity_byte_must_match(void) {
  socket_holds_w25q128();
  S.nor_id[2] = 0x19;                                         // a W25Q256 in the socket
  push(GOLDEN_W25Q128_V);
  run();
  TEST_ASSERT_EQUAL_UINT8(NAND_PRF_E_ID_MISMATCH, next_resp().status);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_bare_go_still_dumps);
  RUN_TEST(test_info_reports_id_state_and_active_name);
  RUN_TEST(test_push_echo_arm_go);
  RUN_TEST(test_push_for_another_chip_is_refused);
  RUN_TEST(test_dev2_mismatch_is_refused);
  RUN_TEST(test_arm_needs_the_staged_crc);
  RUN_TEST(test_go_with_unarmed_push_is_refused);
  RUN_TEST(test_bad_crc_keeps_session_but_stages_nothing);
  RUN_TEST(test_failed_push_clears_an_earlier_stage);
  RUN_TEST(test_insane_geometry_is_refused);
  RUN_TEST(test_unknown_framing_closes_the_connection);
  RUN_TEST(test_truncated_push_times_out_and_closes);
  RUN_TEST(test_unknown_command_is_answered);
  RUN_TEST(test_new_connection_forgets_the_stage);
  RUN_TEST(test_info_v2_carries_nor_id_and_family);
  RUN_TEST(test_nor_push_is_checked_against_the_nor_id);
  RUN_TEST(test_nor_profile_for_a_nand_socket_is_refused);
  RUN_TEST(test_nor_capacity_byte_must_match);
  return UNITY_END();
}
