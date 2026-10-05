#ifndef CHIP_DETECT_H
#define CHIP_DETECT_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"
#include "nand_session.h"   // nand_chip_state_t

// Which chip is in the socket: SPI NAND or SPI NOR, and which profile.
//
// The two families answer READ ID (9Fh) differently. SPI NAND wants one
// address/dummy byte before the ID; SPI NOR starts its 3-byte JEDEC ID right
// after the opcode. The device issues both reads (each is harmless to the
// other family) and resolves each view only against its own family's resident
// entries. A NOR chip in neither table can still describe itself through SFDP;
// that step needs the bus, so main.cpp runs it when this returns UNKNOWN with
// a plausible NOR ID (chip_detect_nor_id_plausible).
//
// Fail-closed: if both views match, or either view is ambiguous within its
// family, nothing is picked.

typedef struct {
  nand_chip_state_t state;          // RESIDENT, UNKNOWN or AMBIGUOUS
  nand_prf_err_t    err;            // why not RESIDENT
  const active_profile_t *hit;      // the resident entry when RESIDENT
  uint8_t           family;         // chip_family_t of hit (or of the ambiguity)
} chip_detect_t;

chip_detect_t chip_detect_resident(const active_profile_t *table, unsigned n,
                                   const uint8_t nand_id[3], const uint8_t nor_id[3]);

// An ID worth asking SFDP about: a real manufacturer byte, not a floating or
// shorted bus (all 0x00 / all 0xFF).
bool chip_detect_nor_id_plausible(const uint8_t nor_id[3]);

#endif // CHIP_DETECT_H
