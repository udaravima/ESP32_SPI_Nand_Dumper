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
  // Poll until the chip is idle after a PAGE READ (OIP clear). NULL = no wait.
  void (*wait_ready)(void *ctx);
} nand_bus_t;

// After this many pages needed the single-line fallback in one session, stop
// trying quad and read the rest single (see nand_seq_read_verified).
#define NAND_QUAD_FALLBACK_LIMIT 3

typedef struct {
  nand_bus_t bus;
  uint8_t  planes;          // 1, 2 or 4
  uint8_t  page_addr_bits;  // log2(pages_per_block)
  uint8_t  plane_bit;       // column bit carrying the plane (nand_plane_bit)
  uint16_t cache_col;       // column of the last PAGE READ, plane bit included
  uint8_t  op_page_read, op_read_cache, op_read_cache_x4;   // from the active profile
  bool     quad;            // current read mode: data phase on 4 lines
  uint32_t quad_fallbacks;  // pages recovered by re-reading single this session
} nand_seq_t;

// spi-nand family defaults (db/families/spi-nand.yml); the active profile
// overrides them through nand_seq_set_opcodes.
#define NAND_OP_PAGE_READ    0x13
#define NAND_OP_READ_CACHE   0x0B   // x1, 1 dummy byte
#define NAND_OP_READ_CACHE4  0x6B   // x4 data, 1 dummy byte
#define NAND_OP_READ_ID      0x9F

void nand_seq_init(nand_seq_t *s, nand_bus_t bus);
void nand_seq_set_planes(nand_seq_t *s, uint8_t planes, uint8_t page_addr_bits,
                         uint32_t main_size);
void nand_seq_set_opcodes(nand_seq_t *s, uint8_t page_read, uint8_t read_cache,
                          uint8_t read_cache_x4);
// Select the read mode and clear the fallback counter (a new session).
void nand_seq_set_quad(nand_seq_t *s, bool quad);
// PAGE READ (13h) of `row` into the chip's cache; remembers the row's plane.
void nand_seq_page_read(nand_seq_t *s, uint32_t row);
// READ FROM CACHE (0Bh, or 6Bh when quad) of `len` bytes from column 0 of the
// plane last loaded by nand_seq_page_read.
void nand_seq_read_cache(nand_seq_t *s, uint8_t *buf, int len, bool quad);
// READ ID (9Fh) with one address/dummy byte of 0, `n` bytes into `out`. Both
// Linux read-ID methods (address byte, dummy byte) put the same 8 zero bits on
// a 1-bit bus, so id_method needs no separate wire sequence here.
void nand_seq_read_id(nand_seq_t *s, uint8_t *out, int n);

typedef enum {
  NAND_PAGE_OK = 0,        // two reads agreed in the current mode
  NAND_PAGE_OK_SINGLE,     // quad reads disagreed; two single reads agreed
  NAND_PAGE_UNSTABLE,      // no two reads agreed; buf holds the last read
} nand_page_result_t;

// Read one page with read-back verification and on-device quad -> single
// fallback. PAGE READ once, then read the cache in the current mode until two
// consecutive reads agree (at most max_retries extra reads). If quad never
// settles, the same cache is re-read single the same way. A page saved by
// single counts toward NAND_QUAD_FALLBACK_LIMIT; at the limit the session
// switches to single (s->quad = false) so the rest of the dump stops paying
// for failed quad reads. `scratch` must hold `len` bytes.
nand_page_result_t nand_seq_read_verified(nand_seq_t *s, uint32_t row, uint8_t *buf,
                                          uint8_t *scratch, int len, int max_retries,
                                          uint32_t *retry_count);

#endif // NAND_SEQ_H
