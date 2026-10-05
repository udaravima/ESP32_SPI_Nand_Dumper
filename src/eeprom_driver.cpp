#include "eeprom_driver.h"
#include "eeprom_seq.h"
#include "nand_driver.h"   // nand_spi_xfer, nand_bus_release/restore, pins
#include <string.h>
#include <Arduino.h>
#include <Wire.h>

static eeprom_seq_t s_ee;
static uint8_t *s_scratch = NULL;
static int s_max_unit = 0;
static bool s_i2c = false;

static bool i2c_read(void *, uint8_t dev7, const uint8_t *w, int wlen, uint8_t *rx,
                     int rx_len) {
  // Word address, then a repeated START (endTransmission(false)): never a STOP
  // after the address, so the part can't take it as a write.
  Wire.beginTransmission(dev7);
  Wire.write(w, (size_t)wlen);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)dev7, (size_t)rx_len, true) != (size_t)rx_len) return false;
  for (int i = 0; i < rx_len; i++) rx[i] = (uint8_t)Wire.read();
  return true;
}

static bool i2c_probe(void *, uint8_t dev7) {
  Wire.beginTransmission(dev7);
  return Wire.endTransmission(true) == 0;   // START, address + W, STOP: no data
}

static void spi_xfer(void *, uint8_t cmd, uint32_t addr, uint8_t addr_bytes, uint8_t *rx,
                     int rx_len) {
  nand_spi_xfer(cmd, addr, addr_bytes * 8, 0, rx, rx_len, false);
}

void eeprom_driver_init(int max_unit_size) {
  eeprom_bus_t bus = { i2c_read, i2c_probe, spi_xfer, NULL };
  eeprom_seq_init(&s_ee, bus);
  s_max_unit = max_unit_size;
  if (!s_scratch) s_scratch = (uint8_t *)heap_caps_malloc(max_unit_size, MALLOC_CAP_DMA);
}

bool eeprom_use_i2c(uint32_t clock_hz) {
  if (s_i2c) { Wire.setClock(clock_hz); return true; }
  if (nand_bus_release() != ESP_OK) return false;
  // The 24xx's strap pins sit on the other SPI lines: bias them weakly.
  pinMode(NAND_PIN_CS, INPUT_PULLUP);      // A0
  pinMode(NAND_PIN_D1, INPUT_PULLDOWN);    // A1
  pinMode(NAND_PIN_D2, INPUT_PULLUP);      // A2
  pinMode(NAND_PIN_D3, INPUT_PULLUP);      // WP: protected
  delay(1);
  s_i2c = Wire.begin(NAND_PIN_D0, NAND_PIN_CLK, clock_hz);
  if (!s_i2c) { nand_bus_restore(); return false; }
  Wire.setTimeOut(50);
  return true;
}

void eeprom_use_spi(void) {
  if (!s_i2c) return;
  Wire.end();
  s_i2c = false;
  nand_bus_restore();
}

bool eeprom_i2c_active(void) { return s_i2c; }

uint8_t eeprom_scan(void) { return s_i2c ? eeprom_seq_i2c_scan(&s_ee) : 0; }

uint8_t eeprom_spi_status(void) {
  if (s_i2c) return 0xFF;
  return eeprom_seq_spi_status(&s_ee);
}

void eeprom_apply_profile(const active_profile_t *p, uint8_t i2c_base) {
  eeprom_seq_apply(&s_ee, p);
  eeprom_seq_set_i2c_base(&s_ee, i2c_base);
}

bool eeprom_read(uint32_t addr, uint8_t *buf, int len) {
  return eeprom_seq_read(&s_ee, addr, buf, len);
}

nand_page_result_t eeprom_read_verified(uint32_t addr, uint8_t *buf, int len,
                                        int max_retries, uint32_t *retry_count) {
  if (!s_scratch || len > s_max_unit) return NAND_PAGE_UNSTABLE;
  uint32_t before = retry_count ? *retry_count : 0;
  uint32_t nacks = s_ee.nacks;
  nand_page_result_t r = eeprom_seq_read_verified(&s_ee, addr, buf, s_scratch, len,
                                                  max_retries, retry_count);
  if (s_ee.nacks != nacks)
    Serial.printf("[!] I2C part stopped answering at 0x%06X\n", (unsigned)addr);
  else if (retry_count && *retry_count != before)
    Serial.printf("[!] Read mismatch at 0x%06X (%u retries)\n", (unsigned)addr,
                  (unsigned)(*retry_count - before));
  return r;
}

uint32_t eeprom_nacks(void) { return s_ee.nacks; }
