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

// Bit position of the plane-select bit in the READ FROM CACHE column address.
// It sits just above the column range that addresses the page's main area,
// i.e. fls(main_size): bit 12 for a 2048-byte main area (Micron M79A/MT29F2G01
// datasheet, "Plane select" in the 03h/0Bh/3Bh/6Bh timing diagrams; the same
// rule mainline Linux spinand applies in spinand_read_from_cache_op()).
static inline uint8_t nand_plane_bit(uint32_t main_size) {
  uint8_t bit = 0;
  while (main_size) { bit++; main_size >>= 1; }
  return bit;
}

// Column address for READ FROM CACHE after PAGE READ of `row`. On a
// multi-plane die the plane is the low bits of the block address (RA6 on a
// 64-page-per-block part), and the cache read MUST carry the same plane or the
// chip returns the other plane's cache register. Single-plane parts get 0.
static inline uint16_t nand_cache_column(uint32_t row, uint8_t page_addr_bits,
                                         uint8_t planes, uint8_t plane_bit) {
  if (planes <= 1) return 0;
  uint32_t plane = (row >> page_addr_bits) & (uint32_t)(planes - 1u);
  return (uint16_t)(plane << plane_bit);
}

#endif // NAND_ADDR_H
