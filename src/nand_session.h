#ifndef NAND_SESSION_H
#define NAND_SESSION_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nand_profile.h"

// Host <-> device command session (vendor-profile stage 3, design § 6).
//
// A client connection used to be one byte, 'G', followed by the dump stream.
// It is now a small command session, so the host can push a profile for a chip
// the firmware does not carry:
//
//   'I'                -> info: detected ID, how it resolved, active profile
//   'P' <PRF blob>     -> tiers 1-3 + ID cross-check; on success the profile is
//                         STAGED (not live) and echoed back for the host to check
//   'A' <crc32 u32 LE> -> arm: the staged profile goes live, but only if the
//                         CRC names the exact blob that was staged
//   'G'                -> dump (the 32-byte geometry header and pages follow)
//
// A bare 'G' still works exactly as before, so old dump.py clients and
// resident chips need no push. Every reply to I/P/A, and a refused G, is one
// response frame:
//
//   'NRSP' | cmd u8 | status u8 (nand_prf_err_t) | len u16 LE | payload | crc32 LE
//
// where the CRC covers everything before it. A successful G sends no frame;
// the dump header ("NANDMP...") is the reply.
//
// Fail-closed: any failed push discards the profile whole and clears whatever
// was staged; nothing reaches the read path without an explicit, CRC-bound ARM.
// Pure and Arduino-free: the transport is two callbacks, so the native tests
// drive the whole session from a byte buffer.

#define NAND_SESSION_VER      3      // v2: SPI NOR ID view + family in I/P replies
                                     // v3: EEPROM presence (I2C ACK scan, SPI RDSR)
#define NAND_RESP_MAGIC       "NRSP"
#define NAND_RESP_HDR_SIZE    8      // magic + cmd + status + len
#define NAND_RESP_MAX_PAYLOAD 64
#define NAND_INFO_SIZE        40     // v1 34; v2 38: + nor_id[3], family;
                                     // v3 40: + I2C ACK mask, SPI EEPROM status
#define NAND_ECHO_SIZE        53     // v1 was 52: + profile family

#define NAND_CMD_INFO 'I'
#define NAND_CMD_PUSH 'P'
#define NAND_CMD_ARM  'A'
#define NAND_CMD_GO   'G'

// How the detected chip got its active profile (the 'I' reply's state byte).
typedef enum {
  NAND_CHIP_RESIDENT  = 0,   // found in the compiled-in table
  NAND_CHIP_UNKNOWN   = 1,   // no match: running on manual menu geometry
  NAND_CHIP_AMBIGUOUS = 2,   // several resident matches: manual geometry until a push
  NAND_CHIP_PUSHED    = 3,   // a host-pushed profile was armed
  NAND_CHIP_SFDP      = 4,   // SPI NOR not in the table, profile built from its SFDP
  NAND_CHIP_PICKED    = 5,   // serial EEPROM picked by name (no ID to resolve)
} nand_chip_state_t;

typedef struct {
  // Read exactly n bytes, waiting at most timeout_ms; returns the count read
  // (less than n on timeout or disconnect).
  size_t (*read)(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms);
  size_t (*write)(void *ctx, const uint8_t *buf, size_t n);
  void *ctx;
} nand_link_t;

typedef struct {
  // Set by the caller.
  uint8_t  id[3];                     // mfr, dev, dev2: 9Fh + dummy byte (SPI NAND view)
  uint8_t  nor_id[3];                 // mfr, type, capacity: plain 9Fh (SPI NOR view)
  // Serial EEPROMs have no ID; what stands in for one is presence. A pushed
  // EEPROM profile is only staged if the part could really be there.
  uint8_t  i2c_ack_mask;              // bit i: 0x50 + i acknowledged (I2C EEPROM view)
  uint8_t  spi_ee_status;             // RDSR as read (SPI EEPROM view; 0xFF = nothing)
  nand_chip_state_t chip_state;
  const active_profile_t *active;     // the profile the read path runs on now
  uint32_t max_page_size;             // the firmware's page buffer bound
  uint32_t timeout_ms;                // per-command payload timeout
  // Owned by the session.
  active_profile_t staged;            // verified, echoed, waiting for ARM
  bool     has_staged;
  uint32_t staged_crc;                // CRC32 trailer of the staged blob
  active_profile_t armed;             // valid when nand_session_handle returns ARMED
} nand_session_t;

typedef enum {
  NAND_SESS_MORE = 0,   // keep reading commands
  NAND_SESS_ARMED,      // s->armed must become the active profile, then MORE
  NAND_SESS_GO,         // run the dump on the active profile
  NAND_SESS_CLOSE,      // drop the connection (the link can't be trusted to resync)
} nand_sess_action_t;

// Start a connection: anything staged on an earlier connection is forgotten.
void nand_session_begin(nand_session_t *s);

// Handle one command byte already read from the link.
nand_sess_action_t nand_session_handle(nand_session_t *s, const nand_link_t *l, uint8_t cmd);

// Build one response frame into `out` (NAND_RESP_HDR_SIZE + len + 4 bytes).
size_t nand_session_frame(uint8_t *out, uint8_t cmd, uint8_t status,
                          const uint8_t *payload, uint16_t len);

#endif // NAND_SESSION_H
