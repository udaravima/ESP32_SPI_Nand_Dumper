#ifndef NAND_ADDR_H
#define NAND_ADDR_H
#include <stdint.h>

// Linear page (row) address: block concatenated with the page field.
// page_addr_bits = log2(pages_per_block). Return is 32-bit — NEVER narrow to 16.
static inline uint32_t nand_row_addr(uint32_t block, uint32_t page,
                                     uint8_t page_addr_bits) {
  uint32_t page_mask = (1u << page_addr_bits) - 1u;
  return (block << page_addr_bits) | (page & page_mask);
}

#endif // NAND_ADDR_H
