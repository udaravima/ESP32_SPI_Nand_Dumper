// Behavioral models of serial EEPROMs, for native tests of the EEPROM read path.
//
// I2C 24xx (AT24C / 24LC / M24C behavior):
// - The part answers at 1010 A2 A1 A0. Address pins it uses for memory
//   address bits (24C04..16 P0..P2, 24CM01/02 A16/A17, 24xx1025 block bit at
//   A2) are ignored as pins; the others must match the strapped pin levels.
//   A 24xx1025 also needs its A2 pin high, or it never answers.
// - A random read sends exactly `addr_bytes` of word address, then a
//   repeated START. A byte beyond the word address is a data byte for the
//   page buffer: the sim records it (`page_bytes`), and since every
//   transaction here ends with a repeated START rather than a STOP, a real
//   part would discard it. `writes` counts transactions that would have
//   committed (data bytes followed by STOP), which the dumper never sends.
// - Sequential reads roll over at the end of the array.
//
// SPI 25xx / FRAM: 03h + address (1, 2 or 3 bytes); on a 25xx040 bit 3 of
// the opcode is A8. 05h reads the status register (bits 6..4 read 0). Any
// write-type opcode (WREN, WRITE, WRSR) is counted in `writes`; anything else
// is a protocol error.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static inline uint8_t sim_ee_byte(uint32_t addr) {
  return (uint8_t)(addr * 29u + (addr >> 8) * 11u + (addr >> 16) * 5u + 0x5A);
}

typedef struct {
  uint32_t size;
  uint8_t  addr_bytes, dev_addr_bits, dev_addr_shift;
  uint8_t  pins;                 // A2..A0 strap levels
  bool     needs_a2_high;        // 24xx1025
  bool     present;
  uint8_t  *mem;
  // Observations
  int      protocol_errors, writes, page_bytes, transactions, probes;
  int      max_rx;               // longest single read
  uint32_t flip_every;           // noisy bus: flip a bit every Nth read (0 = never)
  uint32_t reads;
} sim_i2c_ee_t;

static inline void sim_i2c_ee_init(sim_i2c_ee_t *c, uint32_t size, uint8_t addr_bytes,
                                   uint8_t dev_bits, uint8_t dev_shift, uint8_t pins) {
  memset(c, 0, sizeof(*c));
  c->size = size; c->addr_bytes = addr_bytes;
  c->dev_addr_bits = dev_bits; c->dev_addr_shift = dev_shift;
  c->pins = pins; c->present = true;
  c->mem = (uint8_t *)malloc(size);
  for (uint32_t i = 0; i < size; i++) c->mem[i] = sim_ee_byte(i);
}
static inline void sim_i2c_ee_free(sim_i2c_ee_t *c) { free(c->mem); c->mem = NULL; }

static inline uint8_t sim_i2c_ee_carry(const sim_i2c_ee_t *c) {
  uint8_t m = 0;
  for (unsigned k = 0; k < (1u << c->dev_addr_bits); k++) m |= (uint8_t)(k << c->dev_addr_shift);
  return m;
}

// Does the part answer at dev7?
static inline bool sim_i2c_ee_match(const sim_i2c_ee_t *c, uint8_t dev7) {
  if (!c->present || (dev7 & 0x78) != 0x50) return false;
  if (c->needs_a2_high && !(c->pins & 0x4)) return false;
  uint8_t carry = sim_i2c_ee_carry(c);
  return ((dev7 & 0x07) & ~carry) == (c->pins & ~carry & 0x07);
}

static inline bool sim_i2c_ee_probe(void *ctx, uint8_t dev7) {
  sim_i2c_ee_t *c = (sim_i2c_ee_t *)ctx;
  c->probes++;
  return sim_i2c_ee_match(c, dev7);
}

static inline bool sim_i2c_ee_read(void *ctx, uint8_t dev7, const uint8_t *w, int wlen,
                                   uint8_t *rx, int rx_len) {
  sim_i2c_ee_t *c = (sim_i2c_ee_t *)ctx;
  c->transactions++;
  if (!sim_i2c_ee_match(c, dev7)) return false;
  if (wlen < c->addr_bytes) { c->protocol_errors++; return false; }
  if (wlen > c->addr_bytes) c->page_bytes += wlen - c->addr_bytes;   // discarded at Sr
  uint32_t word = 0;
  for (int i = 0; i < c->addr_bytes; i++) word = (word << 8) | w[i];
  uint8_t carry = sim_i2c_ee_carry(c);
  uint32_t hi = 0;
  for (unsigned b = 0; b < c->dev_addr_bits; b++)
    if (dev7 & (1u << (c->dev_addr_shift + b))) hi |= 1u << b;
  (void)carry;
  uint32_t ptr = ((hi << (8 * c->addr_bytes)) | word) % c->size;
  if (rx_len > c->max_rx) c->max_rx = rx_len;
  for (int i = 0; i < rx_len; i++) rx[i] = c->mem[(ptr + i) % c->size];
  c->reads++;
  if (c->flip_every && c->reads % c->flip_every == 0 && rx_len) rx[rx_len / 2] ^= 0x10;
  return true;
}

typedef struct {
  uint32_t size;
  uint8_t  addr_bytes;
  bool     a8_in_opcode;
  bool     present;
  uint8_t  sr;
  uint8_t  *mem;
  int      protocol_errors, writes, reads, status_reads;
  uint32_t flip_every;
} sim_spi_ee_t;

static inline void sim_spi_ee_init(sim_spi_ee_t *c, uint32_t size, uint8_t addr_bytes,
                                   bool a8) {
  memset(c, 0, sizeof(*c));
  c->size = size; c->addr_bytes = addr_bytes; c->a8_in_opcode = a8; c->present = true;
  c->mem = (uint8_t *)malloc(size);
  for (uint32_t i = 0; i < size; i++) c->mem[i] = sim_ee_byte(i);
}
static inline void sim_spi_ee_free(sim_spi_ee_t *c) { free(c->mem); c->mem = NULL; }

static inline void sim_spi_ee_xfer(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bytes,
                                   uint8_t *rx, int rx_len) {
  sim_spi_ee_t *c = (sim_spi_ee_t *)ctx;
  if (!c->present) { if (rx) memset(rx, 0xFF, rx_len); return; }   // MISO pulled up
  switch (cmd) {
    case 0x05:
      c->status_reads++;
      if (addr_bytes) c->protocol_errors++;
      if (rx_len) rx[0] = c->sr & 0x8F;
      return;
    case 0x06: case 0x02: case 0x01: case 0x0A: case 0x09:
      c->writes++;
      return;
    case 0x03: case 0x0B: {
      if (cmd == 0x0B && !c->a8_in_opcode) { c->protocol_errors++; return; }
      if (addr_bytes != c->addr_bytes) { c->protocol_errors++; return; }
      uint32_t a = addr & ((1u << (8 * c->addr_bytes)) - 1);
      if (cmd == 0x0B) a |= 0x100;
      for (int i = 0; i < rx_len; i++) rx[i] = c->mem[(a + i) % c->size];
      c->reads++;
      if (c->flip_every && c->reads % c->flip_every == 0 && rx_len) rx[rx_len / 2] ^= 0x01;
      return;
    }
    default:
      c->protocol_errors++;
      if (rx) memset(rx, 0xFF, rx_len);
  }
}
