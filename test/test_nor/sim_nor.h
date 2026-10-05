// Behavioral model of a SPI NOR chip, for native tests of the NOR read path.
//
// What it models (JESD216 / common 25-series behavior, e.g. Winbond W25Q,
// Macronix MX25L):
// - 9Fh returns the 3-byte JEDEC ID; ABh wakes a part from deep power-down,
//   which ignores everything else until then.
// - 03h/0Bh/6Bh take a 3-byte address, or 4 bytes once B7h entered 4-byte
//   mode (E9h leaves it). 13h/0Ch/6Ch always take 4 bytes. With a 3-byte
//   address the chip only sees A[23:0], so a part above 16 MiB wraps.
// - 6Bh/6Ch drive IO2/IO3 only when the quad-enable bit is set (or the part
//   has no QE bit): otherwise the upper data lines float and reads are noise.
// - 5Ah reads the SFDP table (3-byte address, 8 dummy cycles).
// The sim also checks the wire format: a wrong address width or dummy count
// is a protocol error, and any write/erase/status-write opcode is recorded so
// tests can prove the dumper never sends one.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "nor_seq.h"

#define SIM_NOR_SFDP_MAX 256

typedef struct {
  uint32_t size;                 // bytes
  uint8_t  id[3];
  bool     needs_wren_for_4b;    // B7h ignored unless WEL is set
  bool     qe;                   // quad-enable bit as currently set
  bool     has_qe_bit;           // false: quad works without QE
  uint8_t  sr1, sr2;             // QE mirrored into sr1 bit 6 / sr2 bit 1 by qe_in_sr1
  bool     qe_in_sr1;            // Macronix-style QE (SR1 bit 6) vs SR2 bit 1
  bool     powered_down;
  bool     four_byte;            // in 4-byte address mode (B7h)
  bool     wel;
  bool     quad_noisy;           // marginal D2/D3: every quad read flips a bit
  uint8_t  sfdp[SIM_NOR_SFDP_MAX];
  int      sfdp_len;             // 0: the part has no SFDP (5Ah returns 0xFF)
  // Observations
  int      protocol_errors, writes, reads, quad_reads, id_reads, sfdp_reads;
  uint32_t last_addr;
  uint32_t noise_tick;
} sim_nor_t;

static inline uint8_t sim_nor_byte(uint32_t addr) {
  return (uint8_t)(addr * 13u + (addr >> 8) * 7u + (addr >> 16) * 3u + (addr >> 24));
}

static inline void sim_nor_init(sim_nor_t *c, uint32_t size, uint8_t mfr, uint8_t type,
                                uint8_t cap) {
  memset(c, 0, sizeof(*c));
  c->size = size;
  c->id[0] = mfr; c->id[1] = type; c->id[2] = cap;
  c->has_qe_bit = true;
}

static inline uint8_t sim_nor_sr1(const sim_nor_t *c) {
  return (uint8_t)(c->sr1 | (c->qe_in_sr1 && c->qe ? 0x40 : 0));
}
static inline uint8_t sim_nor_sr2(const sim_nor_t *c) {
  return (uint8_t)(c->sr2 | (!c->qe_in_sr1 && c->qe ? 0x02 : 0));
}

static void sim_nor_xfer(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bytes,
                         uint8_t dummy, uint8_t *rx, int rx_len, bool quad) {
  sim_nor_t *c = (sim_nor_t *)ctx;
  if (c->powered_down) {
    if (cmd == NOR_OP_RELEASE_PD) c->powered_down = false;
    if (rx) memset(rx, 0xFF, rx_len);     // MISO floats high (pull-up)
    return;
  }
  switch (cmd) {
    case NOR_OP_READ_ID:
      c->id_reads++;
      for (int i = 0; i < rx_len; i++) rx[i] = c->id[i % 3];
      return;
    case NOR_OP_RELEASE_PD: case NOR_OP_RESET_EN: case NOR_OP_RESET:
      return;
    case NOR_OP_WREN:
      c->wel = true;
      return;
    case NOR_OP_EN4B:
      if (!c->needs_wren_for_4b || c->wel) c->four_byte = true;
      c->wel = false;
      return;
    case NOR_OP_EX4B:
      c->four_byte = false;
      c->wel = false;
      return;
    case NOR_OP_RDSR: if (rx_len) rx[0] = sim_nor_sr1(c); return;
    case NOR_OP_RDSR2: if (rx_len) rx[0] = sim_nor_sr2(c); return;
    case NOR_OP_READ_SFDP:
      c->sfdp_reads++;
      if (addr_bytes != 3 || dummy != 8) c->protocol_errors++;
      for (int i = 0; i < rx_len; i++)
        rx[i] = (int)addr + i < c->sfdp_len ? c->sfdp[addr + i] : 0xFF;
      return;
    case 0x03: case 0x0B: case 0x6B: case 0x13: case 0x0C: case 0x6C: {
      bool native4 = cmd == 0x13 || cmd == 0x0C || cmd == 0x6C;
      uint8_t want_addr = native4 || c->four_byte ? 4 : 3;
      uint8_t want_dummy = (cmd == 0x03 || cmd == 0x13) ? 0 : 8;
      bool want_quad = cmd == 0x6B || cmd == 0x6C;
      if (addr_bytes != want_addr || dummy != want_dummy || quad != want_quad)
        c->protocol_errors++;
      uint32_t a = want_addr == 3 ? (addr & 0xFFFFFFu) : addr;
      c->last_addr = a;
      c->reads++;
      if (quad) c->quad_reads++;
      for (int i = 0; i < rx_len; i++) rx[i] = sim_nor_byte((a + (uint32_t)i) % c->size);
      if (quad && ((c->has_qe_bit && !c->qe) || c->quad_noisy) && rx_len > 0) {
        // IO2/IO3 floating (QE clear) or marginal: data differs on every read.
        c->noise_tick++;
        rx[c->noise_tick % (uint32_t)rx_len] ^= (uint8_t)(1u << (c->noise_tick % 8));
        if (c->has_qe_bit && !c->qe)
          for (int i = 0; i < rx_len; i += 7) rx[i] ^= 0x0C;
      }
      return;
    }
    default:
      // 01h/31h (status writes), 02h (program), 20h/52h/D8h/60h/C7h (erase)...
      c->writes++;
      return;
  }
}

static inline nor_bus_t sim_nor_bus(sim_nor_t *c) {
  nor_bus_t b = { sim_nor_xfer, c };
  return b;
}

// A JESD216B-style SFDP image: header + one BFPT (16 dwords).
static inline void sim_nor_set_sfdp(sim_nor_t *c, bool has_114, uint8_t qer, uint8_t enter4,
                                    uint8_t addr_mode) {
  uint8_t *t = c->sfdp;
  memset(t, 0xFF, SIM_NOR_SFDP_MAX);
  memcpy(t, "SFDP", 4);
  t[4] = 6; t[5] = 1; t[6] = 0; t[7] = 0xFF;            // rev 1.6, 1 parameter header
  t[8] = 0x00; t[9] = 6; t[10] = 1; t[11] = 16;          // BFPT 1.6, 16 dwords
  t[12] = 0x30; t[13] = 0; t[14] = 0; t[15] = 0xFF;      // at 0x30
  uint32_t dw[16];
  memset(dw, 0, sizeof(dw));
  dw[0] = 0x01 | (0x20u << 8) | ((uint32_t)addr_mode << 17) | (has_114 ? 1u << 22 : 0);
  dw[1] = c->size * 8u - 1u;                             // density in bits - 1
  dw[2] = has_114 ? (0x6Bu << 24) | (8u << 16) : 0;       // 1-1-4: 6Bh, 8 wait states
  dw[14] = (uint32_t)qer << 20;
  dw[15] = (uint32_t)enter4 << 24;
  for (int i = 0; i < 16; i++)
    for (int b = 0; b < 4; b++) t[0x30 + 4 * i + b] = (uint8_t)(dw[i] >> (8 * b));
  c->sfdp_len = 0x30 + 64;
}
