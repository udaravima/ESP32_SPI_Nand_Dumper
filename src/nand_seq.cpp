#include "nand_seq.h"
#include "nand_addr.h"
#include <string.h>

void nand_seq_init(nand_seq_t *s, nand_bus_t bus) {
  s->bus = bus;
  s->planes = 1;
  s->page_addr_bits = 6;
  s->plane_bit = 12;
  s->cache_col = 0;
  nand_seq_set_opcodes(s, NAND_OP_PAGE_READ, NAND_OP_READ_CACHE, NAND_OP_READ_CACHE4);
  nand_seq_set_quad(s, false);
}

void nand_seq_set_planes(nand_seq_t *s, uint8_t planes, uint8_t page_addr_bits,
                         uint32_t main_size) {
  s->planes = planes ? planes : 1;
  s->page_addr_bits = page_addr_bits;
  s->plane_bit = nand_plane_bit(main_size);
  s->cache_col = 0;
}

void nand_seq_set_opcodes(nand_seq_t *s, uint8_t page_read, uint8_t read_cache,
                          uint8_t read_cache_x4) {
  s->op_page_read = page_read;
  s->op_read_cache = read_cache;
  s->op_read_cache_x4 = read_cache_x4;
}

void nand_seq_set_quad(nand_seq_t *s, bool quad) {
  s->quad = quad;
  s->quad_fallbacks = 0;
}

void nand_seq_page_read(nand_seq_t *s, uint32_t row) {
  s->cache_col = nand_cache_column(row, s->page_addr_bits, s->planes, s->plane_bit);
  s->bus.xfer(s->bus.ctx, s->op_page_read, row & 0xFFFFFF, 24, 0, 0, false);
}

void nand_seq_read_cache(nand_seq_t *s, uint8_t *buf, int len, bool quad) {
  // Address phase: 16-bit column (plane bit included) + 8-bit dummy.
  s->bus.xfer(s->bus.ctx, quad ? s->op_read_cache_x4 : s->op_read_cache,
              (uint32_t)s->cache_col << 8, 24, buf, len, quad);
}

void nand_seq_read_id(nand_seq_t *s, uint8_t *out, int n) {
  s->bus.xfer(s->bus.ctx, NAND_OP_READ_ID, 0x00, 8, out, n, false);
}

// Read the loaded cache until two consecutive reads agree. A matching first
// pair passes regardless of max_retries (retries == 0 still verifies once).
static bool settle(nand_seq_t *s, uint8_t *buf, uint8_t *scratch, int len,
                   int max_retries, bool quad, uint32_t *retry_count) {
  nand_seq_read_cache(s, buf, len, quad);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    nand_seq_read_cache(s, scratch, len, quad);
    if (memcmp(buf, scratch, len) == 0) return true;
    if (retry_count) (*retry_count)++;
    memcpy(buf, scratch, len);
  }
  return false;
}

nand_page_result_t nand_seq_read_verified(nand_seq_t *s, uint32_t row, uint8_t *buf,
                                          uint8_t *scratch, int len, int max_retries,
                                          uint32_t *retry_count) {
  nand_seq_page_read(s, row);
  if (s->bus.wait_ready) s->bus.wait_ready(s->bus.ctx);
  if (settle(s, buf, scratch, len, max_retries, s->quad, retry_count)) return NAND_PAGE_OK;
  if (!s->quad) return NAND_PAGE_UNSTABLE;

  // Quad never settled. The cache still holds the page, so re-read it single.
  if (!settle(s, buf, scratch, len, max_retries, false, retry_count))
    return NAND_PAGE_UNSTABLE;
  if (++s->quad_fallbacks >= NAND_QUAD_FALLBACK_LIMIT) s->quad = false;
  return NAND_PAGE_OK_SINGLE;
}
