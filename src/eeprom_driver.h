#ifndef EEPROM_DRIVER_H
#define EEPROM_DRIVER_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"
#include "nand_seq.h"    // nand_page_result_t

// Serial EEPROMs in the same SOIC-8 clip as SPI NAND/NOR. Thin wrapper: the
// sequencing is eeprom_seq, the wires are nand_spi_xfer (25xx) or the I2C
// controller (24xx). Read-only, like eeprom_seq.
//
// A 24xx has the same footprint as a 25xx, so in the clip its pins land on the
// SPI lines (pin -> 24xx function <- SPI line):
//
//   1 A0  <- CS          weak pull-up (also keeps any SPI part deselected)
//   2 A1  <- D1 / MISO   weak pull-down
//   3 A2  <- D2 / WP     weak pull-up (a 24xx1025 needs A2 high)
//   5 SDA <- D0 / MOSI   I2C data, open drain + pull-up
//   6 SCL <- CLK         I2C clock, open drain + pull-up
//   7 WP  <- D3 / HOLD   weak pull-up: write-protected while it is held high
//
// Only weak pulls touch A0..A2 and WP, so an in-circuit part's own straps win
// and the ACK scan finds wherever it really answers. I2C mode frees the SPI
// bus (nand_bus_release) and SPI mode brings it back (nand_bus_restore).

void eeprom_driver_init(int max_unit_size);   // after nand_init
bool eeprom_use_i2c(uint32_t clock_hz);       // lend the clip's pins to I2C
void eeprom_use_spi(void);                    // give them back to SPI
bool eeprom_i2c_active(void);
uint8_t eeprom_scan(void);                    // ACK mask over 0x50..0x57 (I2C mode)
uint8_t eeprom_spi_status(void);              // RDSR (SPI mode)
void eeprom_apply_profile(const active_profile_t *p, uint8_t i2c_base);
bool eeprom_read(uint32_t addr, uint8_t *buf, int len);
nand_page_result_t eeprom_read_verified(uint32_t addr, uint8_t *buf, int len,
                                        int max_retries, uint32_t *retry_count);
uint32_t eeprom_nacks(void);

#endif // EEPROM_DRIVER_H
