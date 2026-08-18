#ifndef NAND_ECC_H
#define NAND_ECC_H
#include <stdint.h>
#include <stdbool.h>

// On-die ECC status field ECCS[2:0] (status register C0h, bits 6:4).
// Values from the MT29F2G01 datasheet, Table 9 "ECC Status Register Bit
// Descriptions". ECCS is only valid when ECC is ENABLED (invalid with ECC off).
#define NAND_ECCS_NO_ERRORS      0x00  // 000: no bit errors
#define NAND_ECCS_CORRECTED_1_3  0x01  // 001: 1-3 bit errors corrected
#define NAND_ECCS_UNCORRECTABLE  0x02  // 010: >8 bit errors, NOT corrected
#define NAND_ECCS_CORRECTED_4_6  0x03  // 011: 4-6 corrected, refresh might be needed
#define NAND_ECCS_CORRECTED_7_8  0x05  // 101: 7-8 corrected, refresh required

// The chip could NOT correct the page — its data is untrustworthy. Upper status
// bits (CRBSY etc.) are masked off. Pure; host-testable.
static inline bool nand_ecc_uncorrectable(uint8_t eccs) {
  return (eccs & 0x07) == NAND_ECCS_UNCORRECTABLE;
}

// The page WAS corrected, but the block is wearing and should be rewritten.
static inline bool nand_ecc_refresh_recommended(uint8_t eccs) {
  uint8_t e = eccs & 0x07;
  return e == NAND_ECCS_CORRECTED_4_6 || e == NAND_ECCS_CORRECTED_7_8;
}

#endif // NAND_ECC_H
