#include "nand_session.h"
#include "dump_header.h"   // dump_crc32
#include "eeprom_seq.h"    // eeprom_i2c_base, eeprom_spi_status_plausible
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

size_t nand_session_frame(uint8_t *out, uint8_t cmd, uint8_t status,
                          const uint8_t *payload, uint16_t len) {
  memcpy(out, NAND_RESP_MAGIC, 4);
  out[4] = cmd;
  out[5] = status;
  put_u16(out + 6, len);
  if (len) memcpy(out + NAND_RESP_HDR_SIZE, payload, len);
  size_t n = NAND_RESP_HDR_SIZE + len;
  put_u32(out + n, dump_crc32(out, (unsigned)n));
  return n + 4;
}

static void reply(const nand_link_t *l, uint8_t cmd, nand_prf_err_t st,
                  const uint8_t *payload, uint16_t len) {
  uint8_t buf[NAND_RESP_HDR_SIZE + NAND_RESP_MAX_PAYLOAD + 4];
  l->write(l->ctx, buf, nand_session_frame(buf, cmd, (uint8_t)st, payload, len));
}

void nand_session_begin(nand_session_t *s) {
  s->has_staged = false;
  s->staged_crc = 0;
}

// The detected ID in the view a profile of this family is checked against.
// An EEPROM has none: its view is the presence byte (ACK mask or RDSR).
static void detected_id(const nand_session_t *s, uint8_t family, uint8_t out[3]) {
  if (family == CHIP_FAMILY_I2C_EEPROM || family == CHIP_FAMILY_SPI_EEPROM) {
    out[0] = family == CHIP_FAMILY_I2C_EEPROM ? s->i2c_ack_mask : s->spi_ee_status;
    out[1] = out[2] = 0;
    return;
  }
  memcpy(out, family == CHIP_FAMILY_SPI_NOR ? s->nor_id : s->id, 3);
}

// The key interlock: is this profile's chip the one in the socket? By ID for
// SPI NAND/NOR; by presence for an EEPROM (an I2C part must acknowledge at
// every address it occupies, a SPI part must return a plausible status).
static bool bound_to_socket(const nand_session_t *s, const active_profile_t *p) {
  if (p->family == CHIP_FAMILY_I2C_EEPROM) return eeprom_i2c_base(p, s->i2c_ack_mask) >= 0;
  if (p->family == CHIP_FAMILY_SPI_EEPROM) return eeprom_spi_status_plausible(s->spi_ee_status);
  uint8_t det[3];
  detected_id(s, p->family, det);
  return nand_profile_id_matches(p, det[0], det[1], det[2]);
}

// 'I': session version, schema version, page bound, detected ID, state, name,
// then (v2) the SPI NOR ID view and the active profile's family, then (v3) the
// EEPROM presence bytes.
static void do_info(nand_session_t *s, const nand_link_t *l) {
  uint8_t p[NAND_INFO_SIZE] = {0};
  p[0] = NAND_SESSION_VER;
  p[1] = NAND_PROFILE_SCHEMA_VER;
  put_u32(p + 2, s->max_page_size);
  memcpy(p + 6, s->id, 3);
  p[9] = (uint8_t)s->chip_state;
  if (s->active) memcpy(p + 10, s->active->name, sizeof(s->active->name));
  p[10 + 23] = '\0';
  memcpy(p + 34, s->nor_id, 3);
  p[37] = s->active ? s->active->family : (uint8_t)CHIP_FAMILY_SPI_NAND;
  p[38] = s->i2c_ack_mask;
  p[39] = s->spi_ee_status;
  reply(l, NAND_CMD_INFO, NAND_PRF_OK, p, sizeof(p));
}

// Echo of a pushed profile: what the device will run, and against which chip.
static void echo(const nand_session_t *s, const active_profile_t *p, uint32_t crc,
                 uint8_t *out) {
  memcpy(out, p->name, sizeof(p->name));
  put_u32(out + 24, p->page_size);
  put_u32(out + 28, p->spare_size);
  put_u32(out + 32, p->pages_per_block);
  put_u32(out + 36, p->total_blocks);
  out[40] = p->planes;
  out[41] = p->id_mfr; out[42] = p->id_dev; out[43] = p->id_dev2; out[44] = p->id_flags;
  detected_id(s, p->family, out + 45);
  put_u32(out + 48, crc);
  out[52] = p->family;
}

static nand_sess_action_t do_push(nand_session_t *s, const nand_link_t *l) {
  // Any push, good or bad, replaces what was staged: never arm a stale profile.
  s->has_staged = false;

  uint8_t blob[NAND_PROFILE_BLOB_SIZE];
  if (l->read(l->ctx, blob, 6, s->timeout_ms) != 6) {
    reply(l, NAND_CMD_PUSH, NAND_PRF_E_TIMEOUT, NULL, 0);
    return NAND_SESS_CLOSE;
  }
  // The framing header decides how many bytes follow. If it is not exactly
  // what this firmware understands, the rest of the stream can't be parsed
  // safely: refuse and drop the connection rather than guess a length.
  nand_prf_err_t e = NAND_PRF_OK;
  uint16_t body = (uint16_t)(blob[4] | (blob[5] << 8));
  if (memcmp(blob, "PRF", 3) != 0)              e = NAND_PRF_E_BAD_MAGIC;
  else if (blob[3] != NAND_PROFILE_SCHEMA_VER)  e = NAND_PRF_E_SCHEMA_VER;
  else if (body != NAND_PROFILE_SIZE)           e = NAND_PRF_E_BAD_LEN;
  if (e != NAND_PRF_OK) {
    reply(l, NAND_CMD_PUSH, e, NULL, 0);
    return NAND_SESS_CLOSE;
  }
  size_t rest = NAND_PROFILE_BLOB_SIZE - 6;
  if (l->read(l->ctx, blob + 6, rest, s->timeout_ms) != rest) {
    reply(l, NAND_CMD_PUSH, NAND_PRF_E_TIMEOUT, NULL, 0);
    return NAND_SESS_CLOSE;
  }

  // Tiers 1-3. From here the stream is in sync, so a rejection keeps the session.
  active_profile_t p;
  e = nand_profile_unpack(blob, sizeof(blob), s->max_page_size, &p);
  if (e != NAND_PRF_OK) {
    reply(l, NAND_CMD_PUSH, e, NULL, 0);
    return NAND_SESS_MORE;
  }
  uint32_t crc = get_u32(blob + 6 + NAND_PROFILE_SIZE);
  uint8_t out[NAND_ECHO_SIZE];
  echo(s, &p, crc, out);

  // The key interlock: the profile is bound to the silicon in the socket.
  if (!bound_to_socket(s, &p)) {
    reply(l, NAND_CMD_PUSH, NAND_PRF_E_ID_MISMATCH, out, sizeof(out));
    return NAND_SESS_MORE;
  }
  s->staged = p;
  s->staged_crc = crc;
  s->has_staged = true;
  reply(l, NAND_CMD_PUSH, NAND_PRF_OK, out, sizeof(out));
  return NAND_SESS_MORE;
}

static nand_sess_action_t do_arm(nand_session_t *s, const nand_link_t *l) {
  uint8_t c[4];
  if (l->read(l->ctx, c, 4, s->timeout_ms) != 4) {
    reply(l, NAND_CMD_ARM, NAND_PRF_E_TIMEOUT, NULL, 0);
    return NAND_SESS_CLOSE;
  }
  if (!s->has_staged) {
    reply(l, NAND_CMD_ARM, NAND_PRF_E_NOT_STAGED, NULL, 0);
    return NAND_SESS_MORE;
  }
  if (get_u32(c) != s->staged_crc) {
    // The host checked a different echo than the one staged: disarm entirely.
    s->has_staged = false;
    reply(l, NAND_CMD_ARM, NAND_PRF_E_ARM_CRC, NULL, 0);
    return NAND_SESS_MORE;
  }
  s->armed = s->staged;
  s->has_staged = false;
  reply(l, NAND_CMD_ARM, NAND_PRF_OK, NULL, 0);
  return NAND_SESS_ARMED;
}

nand_sess_action_t nand_session_handle(nand_session_t *s, const nand_link_t *l, uint8_t cmd) {
  switch (cmd) {
    case NAND_CMD_INFO:
      do_info(s, l);
      return NAND_SESS_MORE;
    case NAND_CMD_PUSH:
      return do_push(s, l);
    case NAND_CMD_ARM:
      return do_arm(s, l);
    case NAND_CMD_GO:
      // A pushed profile that was never armed must not silently fall back to
      // whatever profile was active before: the host meant to dump that chip.
      if (s->has_staged) {
        reply(l, NAND_CMD_GO, NAND_PRF_E_NOT_ARMED, NULL, 0);
        return NAND_SESS_CLOSE;
      }
      return NAND_SESS_GO;
    case '\r': case '\n': case ' ':
      return NAND_SESS_MORE;   // stray whitespace from a hand-typed session
    default:
      reply(l, cmd, NAND_PRF_E_BAD_CMD, NULL, 0);
      return NAND_SESS_MORE;
  }
}
