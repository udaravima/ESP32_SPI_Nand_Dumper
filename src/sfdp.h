#ifndef SFDP_H
#define SFDP_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"

// JESD216 Serial Flash Discoverable Parameters: the table a SPI NOR chip
// carries about itself (read with 5Ah). Parsing it lets the firmware dump a
// chip that is in neither the resident table nor the host's DB, standalone.
//
// Only the Basic Flash Parameter Table (BFPT) is read, and only the fields a
// read needs: density, address width, how to reach 4-byte addressing, the
// 1-1-4 fast read and the quad-enable requirement. Pure and Arduino-free.

#define SFDP_SIGNATURE   0x50444653u   // "SFDP", little-endian
#define SFDP_BFPT_MAX_DW 64            // longest BFPT this parser reads (JESD216 allows 255)

// Read `len` bytes of the SFDP space at `addr` (the device wraps nor_seq_read_sfdp).
typedef void (*sfdp_read_fn)(void *ctx, uint32_t addr, uint8_t *buf, int len);

typedef struct {
  uint8_t  major, minor;          // BFPT revision
  uint8_t  bfpt_dwords;           // BFPT length the chip declares
  uint32_t size_bytes;            // density (DWORD2), 0 if over 4 GiB
  uint8_t  addr_mode;             // DWORD1[18:17]: 0 3-byte only, 1 3 or 4, 2 4-byte only
  uint8_t  enter4;                // DWORD16[31:24] "enter 4-byte addressing" bitmap (0 if absent)
  bool     has_114;               // DWORD1[22]: 1-1-4 fast read
  uint8_t  op_114, dummy_114;     // DWORD3[31:16]: opcode, wait states + mode clocks
  uint8_t  qer;                   // DWORD15[22:20], 0 if the BFPT predates JESD216A
} sfdp_info_t;

// Read and parse the SFDP header and BFPT. Returns false if there is no valid
// signature/BFPT (a SPI NAND, a pre-SFDP NOR part, or no chip).
bool sfdp_probe(sfdp_read_fn rd, void *ctx, sfdp_info_t *out);

// Build a spi-nor profile from parsed SFDP and the detected JEDEC ID. Returns
// false (out untouched) if the chip can't be read safely with what SFDP says,
// e.g. above 16 MiB with no 4-byte method this firmware supports. The result
// passes nand_profile_check like any resident or pushed profile.
bool sfdp_build_profile(const sfdp_info_t *info, const uint8_t id[3], active_profile_t *out);

#endif // SFDP_H
