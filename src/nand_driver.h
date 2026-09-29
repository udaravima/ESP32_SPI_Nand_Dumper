#ifndef NAND_DRIVER_H
#define NAND_DRIVER_H
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "nand_profile.h"   // active_profile_t, nand_read_mode_t
#include "nand_seq.h"       // nand_page_result_t
#include "board_pins.h"

#define NAND_STATUS_OIP          0x01

typedef struct {
  int pin_clk, pin_d0, pin_d1, pin_d2, pin_d3, pin_cs;
  int clock_hz;
  nand_read_mode_t read_mode;
} nand_config_t;

#define NAND_DEFAULT_CONFIG() { \
  .pin_clk = NAND_PIN_CLK, .pin_d0 = NAND_PIN_D0, .pin_d1 = NAND_PIN_D1, \
  .pin_d2 = NAND_PIN_D2, .pin_d3 = NAND_PIN_D3, .pin_cs = NAND_PIN_CS, \
  .clock_hz = 1000000, .read_mode = NAND_READ_SINGLE }

esp_err_t nand_init(const nand_config_t *config, int max_page_size);
esp_err_t nand_set_clock(int clock_hz);       // re-clock the device post-detection
// Take opcodes, feature addresses and the ECC-enable bit from the active
// profile. Until called, the spi-nand family defaults are used (for READ ID).
void      nand_apply_profile(const active_profile_t *p);
void      nand_reset(void);
void      nand_wait_ready(void);
uint8_t   nand_get_feature(uint8_t addr);
void      nand_set_feature(uint8_t addr, uint8_t value);
void      nand_set_ecc(bool on);
uint8_t   nand_get_status(void);              // raw status register; decode with the profile
void      nand_read_id(uint8_t id[3]);        // mfr, dev, dev2 (third byte for disambiguation)
// Multi-plane geometry: page reads remember the row's plane and the next
// cache read selects it. planes = 1 (the default) keeps column 0.
void      nand_set_plane_config(uint8_t planes, uint8_t page_addr_bits, uint32_t main_size);
void      nand_page_read_to_cache(uint32_t row_addr);
void      nand_read_cache(uint8_t *buf, int len);
void      nand_read_cache_single(uint8_t *buf, int len);
void      nand_read_cache_quad(uint8_t *buf, int len);
// Verified read with on-device quad -> single fallback (nand_seq_read_verified).
nand_page_result_t nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size,
                                           int max_retries, uint32_t *retry_count);
bool      nand_quad_selftest(uint32_t probe_row, int page_size);
// Pages recovered by the single fallback since the read mode was last set.
uint32_t  nand_quad_fallbacks(void);
nand_read_mode_t nand_get_read_mode(void);
void      nand_set_read_mode(nand_read_mode_t m);

#endif // NAND_DRIVER_H
