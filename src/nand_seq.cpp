#include "nand_seq.h"
#include "nand_addr.h"

void nand_seq_init(nand_seq_t *s, nand_bus_t bus) {
  s->bus = bus;
  s->planes = 1;
  s->page_addr_bits = 6;
  s->plane_bit = 12;
  s->cache_col = 0;
}

void nand_seq_set_planes(nand_seq_t *s, uint8_t planes, uint8_t page_addr_bits,
                         uint32_t main_size) {
  s->planes = planes ? planes : 1;
  s->page_addr_bits = page_addr_bits;
  s->plane_bit = nand_plane_bit(main_size);
  s->cache_col = 0;
}

void nand_seq_page_read(nand_seq_t *s, uint32_t row) {
  s->cache_col = nand_cache_column(row, s->page_addr_bits, s->planes, s->plane_bit);
  s->bus.xfer(s->bus.ctx, NAND_OP_PAGE_READ, row & 0xFFFFFF, 24, 0, 0, false);
}

void nand_seq_read_cache(nand_seq_t *s, uint8_t *buf, int len, bool quad) {
  // Address phase: 16-bit column (plane bit included) + 8-bit dummy.
  s->bus.xfer(s->bus.ctx, quad ? NAND_OP_READ_CACHE4 : NAND_OP_READ_CACHE,
              (uint32_t)s->cache_col << 8, 24, buf, len, quad);
}
