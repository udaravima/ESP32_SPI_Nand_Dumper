#ifndef NOR_SEQ_H
#define NOR_SEQ_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"   // active_profile_t
#include "nand_seq.h"       // nand_page_result_t, NAND_QUAD_FALLBACK_LIMIT

// Pure (hardware-free) SPI NOR command sequencing for the read path, the NOR
// sibling of nand_seq. The driver owns the SPI bus; this module decides the
// opcode, address width and dummy cycles through a bus callback, so native
// tests can run it against a simulated chip (test/test_nor).
//
// Read-only by design: nothing here writes the array or a status register.
// The only state changes it can make are volatile ones (4-byte address mode
// on B7h parts, exited again by nor_seq_end, and the software reset).

typedef struct {
  // One half-duplex transaction: 8-bit opcode, `addr_bytes` of address (0 for
  // none), `dummy` clock cycles, then `rx_len` bytes read into `rx` (NULL/0
  // for none). `quad` puts the data phase on four lines (1-1-4).
  void (*xfer)(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bytes, uint8_t dummy,
               uint8_t *rx, int rx_len, bool quad);
  void *ctx;
} nor_bus_t;

// spi-nor family opcodes (db/families/spi-nor.yml).
#define NOR_OP_READ_ID      0x9F
#define NOR_OP_READ         0x03
#define NOR_OP_READ_X4      0x6B
#define NOR_OP_READ4        0x13
#define NOR_OP_READ4_X4     0x6C
#define NOR_OP_RDSR         0x05
#define NOR_OP_RDSR2        0x35   // status register 2 (Winbond, GigaDevice)
#define NOR_OP_RDSR2_ALT    0x3F   // status register 2, JESD216 QER 011b parts
#define NOR_OP_WREN         0x06
#define NOR_OP_EN4B         0xB7
#define NOR_OP_EX4B         0xE9
#define NOR_OP_RESET_EN     0x66
#define NOR_OP_RESET        0x99
#define NOR_OP_RELEASE_PD   0xAB
#define NOR_OP_READ_SFDP    0x5A
#define NOR_SR_WIP          0x01

typedef struct {
  nor_bus_t bus;
  uint8_t  op_read, op_read_x4;   // from the active profile
  uint8_t  op_rdsr;
  uint8_t  addr_bytes;            // 3 or 4
  uint8_t  addr4_mode;            // NOR_ADDR4_*
  uint8_t  dummy_x1, dummy_x4;
  uint8_t  qer;                   // JESD216 quad-enable requirement
  bool     in_4byte;              // B7h was sent and not yet undone
  bool     quad_on;               // current read mode (not `quad`: the ESP32 toolchain headers #define it)
  uint32_t quad_fallbacks;        // units recovered single this session
} nor_seq_t;

void nor_seq_init(nor_seq_t *s, nor_bus_t bus);
// Take opcodes, address width, dummy cycles and QER from a spi-nor profile.
void nor_seq_apply(nor_seq_t *s, const active_profile_t *p);
void nor_seq_set_quad(nor_seq_t *s, bool quad);

// Identification, safe on any chip in the socket (SPI NAND included).
void nor_seq_release_power_down(nor_seq_t *s);   // ABh: a part in deep power-down ignores 9Fh
void nor_seq_read_id(nor_seq_t *s, uint8_t id[3]);
// SFDP (5Ah): 3-byte address, 8 dummy cycles, single line (JESD216).
void nor_seq_read_sfdp(nor_seq_t *s, uint32_t addr, uint8_t *buf, int len);

uint8_t nor_seq_read_status(nor_seq_t *s, uint8_t op);
// Quad-enable bit as read back from the chip: 1 set, 0 clear, -1 when the
// profile's QER has no read command for it (or no QE bit is known).
int nor_seq_qe_state(nor_seq_t *s);

// Bracket a dump: enter 4-byte mode first if the part needs B7h, and leave it
// afterwards so the chip is back in its power-on addressing mode.
void nor_seq_begin(nor_seq_t *s);
void nor_seq_end(nor_seq_t *s);

// Read `len` bytes from `addr` (03h/13h, or 6Bh/6Ch when quad).
void nor_seq_read(nor_seq_t *s, uint32_t addr, uint8_t *buf, int len, bool quad);

// Read with read-back verification and quad -> single fallback, the same
// policy as nand_seq_read_verified: two consecutive reads must agree; if quad
// never settles the range is re-read single, and after
// NAND_QUAD_FALLBACK_LIMIT such units the session stays single.
nand_page_result_t nor_seq_read_verified(nor_seq_t *s, uint32_t addr, uint8_t *buf,
                                         uint8_t *scratch, int len, int max_retries,
                                         uint32_t *retry_count);

#endif // NOR_SEQ_H
