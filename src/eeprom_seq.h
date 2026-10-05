#ifndef EEPROM_SEQ_H
#define EEPROM_SEQ_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"   // active_profile_t, chip families
#include "nand_seq.h"       // nand_page_result_t

// Pure (hardware-free) read sequencing for serial EEPROMs, the sibling of
// nor_seq: I2C 24xx and SPI 25xx/FRAM (docs/superpowers/specs/
// 2026-10-05-eeprom-design.md). The driver owns the buses; this module only
// decides device addresses, word addresses and opcodes, so the native tests
// run it against simulated chips (test/test_eeprom).
//
// Read-only by design. On I2C the only write on the wire is the word address
// of a random read, always followed by a repeated START (never a data byte or
// a STOP), and the address-only probe (START, address + W, STOP). On SPI only
// READ (03h, or 0Bh for the upper half of a 25xx040) and RDSR (05h) are sent.

#define EEPROM_I2C_BASE      0x50    // 1010 A2 A1 A0
#define EEPROM_I2C_SCAN_N    8       // 0x50 .. 0x57
#define EEPROM_I2C_CHUNK     128     // bytes per I2C read transaction
#define EEPROM_SPI_OP_RDSR   0x05
#define EEPROM_SPI_A8_BIT    0x08    // 25xx040: A8 travels in opcode bit 3

typedef struct {
  // I2C random/sequential read: START, dev+W, w[wlen], repeated START, dev+R,
  // rx[rx_len] (NACK on the last byte), STOP. False if any byte was NACKed.
  bool (*i2c_read)(void *ctx, uint8_t dev7, const uint8_t *w, int wlen, uint8_t *rx,
                   int rx_len);
  // Address-only probe: START, dev+W, STOP. True if the address was ACKed.
  bool (*i2c_probe)(void *ctx, uint8_t dev7);
  // SPI: opcode, `addr_bytes` of address (0 for none), then rx_len bytes.
  void (*spi_xfer)(void *ctx, uint8_t cmd, uint32_t addr, uint8_t addr_bytes, uint8_t *rx,
                   int rx_len);
  void *ctx;
} eeprom_bus_t;

typedef struct {
  eeprom_bus_t bus;
  uint8_t  family;                  // CHIP_FAMILY_I2C_EEPROM / _SPI_EEPROM
  uint8_t  addr_bytes;              // word address width
  uint8_t  dev_addr_bits, dev_addr_shift;
  uint8_t  op_read;                 // SPI READ
  uint8_t  i2c_base;                // where the part answers (0x50..0x57)
  uint32_t size;
  uint32_t nacks;                   // I2C transactions refused this session
} eeprom_seq_t;

void eeprom_seq_init(eeprom_seq_t *s, eeprom_bus_t bus);
// Take family, address widths, the READ opcode and the size from a profile.
void eeprom_seq_apply(eeprom_seq_t *s, const active_profile_t *p);
void eeprom_seq_set_i2c_base(eeprom_seq_t *s, uint8_t base);

// Probe 0x50..0x57; bit i of the result is set if 0x50 + i acknowledged.
uint8_t eeprom_seq_i2c_scan(eeprom_seq_t *s);

// Where an I2C part answers, given the scan: the lowest base address whose
// whole set of addresses (one per value of the carried address bits)
// acknowledged, or -1. A 24C16 needs all eight; a 24C02 any one.
int eeprom_i2c_base(const active_profile_t *p, uint8_t ack_mask);

// SPI status register (RDSR). A 25xx/FRAM reads its unused bits 6..4 as 0,
// so 0xFF (MISO pulled up, nothing answering) or any of those bits set means
// no SPI EEPROM is in the socket.
uint8_t eeprom_seq_spi_status(eeprom_seq_t *s);
static inline bool eeprom_spi_status_plausible(uint8_t sr) { return (sr & 0x70) == 0; }

// The device address and word address for byte `addr` of the part, and how
// many bytes a transaction starting there may read before the carried bits
// change (the end of the word address range).
uint8_t  eeprom_i2c_dev(const eeprom_seq_t *s, uint32_t addr);
uint32_t eeprom_word_span(const eeprom_seq_t *s, uint32_t addr);
uint8_t  eeprom_spi_opcode(const eeprom_seq_t *s, uint32_t addr);

// Read `len` bytes from `addr`. False if the I2C part refused (buf is then
// filled with 0xFF from the first refused transaction on).
bool eeprom_seq_read(eeprom_seq_t *s, uint32_t addr, uint8_t *buf, int len);

// Read until two consecutive reads agree (at most max_retries extra reads),
// the same verify policy as the NAND/NOR paths. No quad, so the result is
// NAND_PAGE_OK or NAND_PAGE_UNSTABLE.
nand_page_result_t eeprom_seq_read_verified(eeprom_seq_t *s, uint32_t addr, uint8_t *buf,
                                            uint8_t *scratch, int len, int max_retries,
                                            uint32_t *retry_count);

#endif // EEPROM_SEQ_H
