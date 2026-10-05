#include "eeprom_seq.h"
#include <string.h>

void eeprom_seq_init(eeprom_seq_t *s, eeprom_bus_t bus) {
  memset(s, 0, sizeof(*s));
  s->bus = bus;
  s->i2c_base = EEPROM_I2C_BASE;
}

void eeprom_seq_apply(eeprom_seq_t *s, const active_profile_t *p) {
  s->family = p->family;
  s->addr_bytes = p->addr_bytes;
  s->dev_addr_bits = p->dev_addr_bits;
  s->dev_addr_shift = p->dev_addr_shift;
  s->op_read = p->op_read_cache;
  s->size = nand_profile_bytes(p);
  s->nacks = 0;
}

void eeprom_seq_set_i2c_base(eeprom_seq_t *s, uint8_t base) { s->i2c_base = base; }

uint8_t eeprom_seq_i2c_scan(eeprom_seq_t *s) {
  uint8_t mask = 0;
  for (int i = 0; i < EEPROM_I2C_SCAN_N; i++)
    if (s->bus.i2c_probe(s->bus.ctx, (uint8_t)(EEPROM_I2C_BASE + i))) mask |= 1u << i;
  return mask;
}

int eeprom_i2c_base(const active_profile_t *p, uint8_t ack_mask) {
  if (p->family != CHIP_FAMILY_I2C_EEPROM || p->dev_addr_bits > 3) return -1;
  uint8_t carry = 0;
  for (unsigned k = 0; k < (1u << p->dev_addr_bits); k++)
    carry |= (uint8_t)(k << p->dev_addr_shift);
  for (int i = 0; i < EEPROM_I2C_SCAN_N; i++) {
    if (i & carry) continue;              // a base has the carried bits clear
    bool all = true;
    for (unsigned k = 0; k < (1u << p->dev_addr_bits) && all; k++)
      all = (ack_mask >> (i | (k << p->dev_addr_shift))) & 1;
    if (all) return EEPROM_I2C_BASE + i;
  }
  return -1;
}

uint8_t eeprom_seq_spi_status(eeprom_seq_t *s) {
  uint8_t sr = 0xFF;
  s->bus.spi_xfer(s->bus.ctx, EEPROM_SPI_OP_RDSR, 0, 0, &sr, 1);
  return sr;
}

static uint32_t word_bits(const eeprom_seq_t *s) { return 8u * s->addr_bytes; }

uint8_t eeprom_i2c_dev(const eeprom_seq_t *s, uint32_t addr) {
  uint32_t hi = (addr >> word_bits(s)) & ((1u << s->dev_addr_bits) - 1);
  return (uint8_t)(s->i2c_base | (hi << s->dev_addr_shift));
}

uint32_t eeprom_word_span(const eeprom_seq_t *s, uint32_t addr) {
  uint32_t range = 1u << word_bits(s);
  return range - (addr & (range - 1));
}

uint8_t eeprom_spi_opcode(const eeprom_seq_t *s, uint32_t addr) {
  if (s->dev_addr_bits && ((addr >> word_bits(s)) & 1)) return s->op_read | EEPROM_SPI_A8_BIT;
  return s->op_read;
}

bool eeprom_seq_read(eeprom_seq_t *s, uint32_t addr, uint8_t *buf, int len) {
  uint32_t mask = (1u << word_bits(s)) - 1;
  while (len > 0) {
    // Never let one transaction run past the end of the word address range:
    // the carried bits (device address or opcode bit 3) change there.
    uint32_t n = eeprom_word_span(s, addr);
    if (n > (uint32_t)len) n = (uint32_t)len;
    if (s->family == CHIP_FAMILY_SPI_EEPROM) {
      s->bus.spi_xfer(s->bus.ctx, eeprom_spi_opcode(s, addr), addr & mask, s->addr_bytes,
                      buf, (int)n);
    } else {
      if (n > EEPROM_I2C_CHUNK) n = EEPROM_I2C_CHUNK;
      uint8_t w[2];
      uint32_t word = addr & mask;
      if (s->addr_bytes == 2) { w[0] = (uint8_t)(word >> 8); w[1] = (uint8_t)word; }
      else                    { w[0] = (uint8_t)word; }
      if (!s->bus.i2c_read(s->bus.ctx, eeprom_i2c_dev(s, addr), w, s->addr_bytes, buf,
                           (int)n)) {
        s->nacks++;
        memset(buf, 0xFF, (size_t)len);
        return false;
      }
    }
    addr += n; buf += n; len -= (int)n;
  }
  return true;
}

nand_page_result_t eeprom_seq_read_verified(eeprom_seq_t *s, uint32_t addr, uint8_t *buf,
                                            uint8_t *scratch, int len, int max_retries,
                                            uint32_t *retry_count) {
  bool ok = eeprom_seq_read(s, addr, buf, len);
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    bool ok2 = eeprom_seq_read(s, addr, scratch, len);
    if (ok && ok2 && memcmp(buf, scratch, len) == 0) return NAND_PAGE_OK;
    if (retry_count) (*retry_count)++;
    memcpy(buf, scratch, len);
    ok = ok2;
  }
  return NAND_PAGE_UNSTABLE;
}
