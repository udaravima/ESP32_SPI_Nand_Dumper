// Behavioral model of a multi-plane SPI NAND, for native tests of the read path.
//
// What it models (per the Micron M79A / MT29F2G01 datasheet):
// - Each plane has its own cache register. PAGE READ (13h) of a row loads the
//   page into the cache of the row's plane (plane = block LSBs, RA6 on a
//   64-page/block part).
// - READ FROM CACHE (0Bh/6Bh) returns bytes from the cache of the plane named
//   by the column's plane-select bit, starting at the column offset.
// Array contents are a deterministic function of (row, byte), so a test can
// tell exactly which page's data came back.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "nand_seq.h"

#define SIM_MAX_PAGE   256
#define SIM_MAX_PLANES 4

typedef struct {
  int planes, page_size, pages_per_block, page_addr_bits, plane_bit;
  uint8_t cache[SIM_MAX_PLANES][SIM_MAX_PAGE];
  int page_reads, cache_reads;
} sim_nand_t;

static inline uint8_t sim_byte(uint32_t row, int i) {
  return (uint8_t)(row * 31u + (uint32_t)i * 7u + (row >> 3));
}

static inline void sim_page(uint32_t row, uint8_t *out, int len) {
  for (int i = 0; i < len; i++) out[i] = sim_byte(row, i);
}

static inline void sim_init(sim_nand_t *n, int planes, int page_size,
                            int pages_per_block, int page_addr_bits, int plane_bit) {
  memset(n, 0, sizeof(*n));
  n->planes = planes; n->page_size = page_size;
  n->pages_per_block = pages_per_block; n->page_addr_bits = page_addr_bits;
  n->plane_bit = plane_bit;
  memset(n->cache, 0xFF, sizeof(n->cache));   // power-on cache contents
}

static void sim_xfer(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bits,
                     uint8_t *rx, int rx_len, bool quad) {
  (void)addr_bits; (void)quad;
  sim_nand_t *n = (sim_nand_t *)ctx;
  if (cmd == NAND_OP_PAGE_READ) {
    uint32_t row = addr;
    int plane = (int)((row >> n->page_addr_bits) & (uint32_t)(n->planes - 1));
    sim_page(row, n->cache[plane], n->page_size);
    n->page_reads++;
  } else if (cmd == NAND_OP_READ_CACHE || cmd == NAND_OP_READ_CACHE4) {
    uint32_t col = addr >> 8;                    // drop the dummy byte
    int plane = (int)((col >> n->plane_bit) & (uint32_t)(n->planes - 1));
    uint32_t off = col & ((1u << n->plane_bit) - 1u);
    for (int i = 0; i < rx_len; i++)
      rx[i] = (off + i < (uint32_t)n->page_size) ? n->cache[plane][off + i] : 0xFF;
    n->cache_reads++;
  }
}

static inline nand_bus_t sim_bus(sim_nand_t *n) {
  nand_bus_t b = { sim_xfer, n };
  return b;
}
