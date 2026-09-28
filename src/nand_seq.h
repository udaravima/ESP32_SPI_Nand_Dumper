#ifndef NAND_SEQ_H
#define NAND_SEQ_H
#include <stdint.h>
#include <stdbool.h>

// Pure (hardware-free) SPI NAND command sequencing for the read path.
//
// The driver owns the SPI bus; this module decides WHAT goes on it — opcode,
// address phase, and the plane-select column — through a bus callback. On the
// device the callback issues a real SPI transaction; in native tests it drives
// a simulated chip (test/test_sim), so the read path can be verified without
// hardware.

typedef struct {
  // One half-duplex transaction: 8-bit opcode, `addr_bits` of address (0 for
  // none), then `rx_len` bytes read into `rx` (NULL/0 for none). `quad` puts
  // the data phase on four lines (1-1-4).
  void (*xfer)(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bits,
               uint8_t *rx, int rx_len, bool quad);
  void *ctx;
} nand_bus_t;

typedef struct {
  nand_bus_t bus;
  uint8_t  planes;          // 1, 2 or 4
  uint8_t  page_addr_bits;  // log2(pages_per_block)
  uint8_t  plane_bit;       // column bit carrying the plane (nand_plane_bit)
  uint16_t cache_col;       // column of the last PAGE READ, plane bit included
} nand_seq_t;

#define NAND_OP_PAGE_READ    0x13
#define NAND_OP_READ_CACHE   0x0B   // x1, 1 dummy byte
#define NAND_OP_READ_CACHE4  0x6B   // x4 data, 1 dummy byte

void nand_seq_init(nand_seq_t *s, nand_bus_t bus);
void nand_seq_set_planes(nand_seq_t *s, uint8_t planes, uint8_t page_addr_bits,
                         uint32_t main_size);
// PAGE READ (13h) of `row` into the chip's cache; remembers the row's plane.
void nand_seq_page_read(nand_seq_t *s, uint32_t row);
// READ FROM CACHE (0Bh, or 6Bh when quad) of `len` bytes from column 0 of the
// plane last loaded by nand_seq_page_read.
void nand_seq_read_cache(nand_seq_t *s, uint8_t *buf, int len, bool quad);

#endif // NAND_SEQ_H
