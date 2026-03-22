#ifndef NAND_DRIVER_H
#define NAND_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

// ============ NAND Geometry ============
#define NAND_PAGE_SIZE       2112   // 2048 main + 64 spare
#define NAND_PAGES_PER_BLOCK 64
#define NAND_TOTAL_BLOCKS    1024
#define NAND_TOTAL_PAGES     (NAND_TOTAL_BLOCKS * NAND_PAGES_PER_BLOCK)
#define NAND_TOTAL_BYTES     ((uint32_t)NAND_TOTAL_PAGES * NAND_PAGE_SIZE)

// ============ Pin Defaults ============
#define NAND_PIN_CLK  18
#define NAND_PIN_D0   23   // MOSI / SIO0
#define NAND_PIN_D1   19   // MISO / SIO1
#define NAND_PIN_D2   22   // WP#  / SIO2 (quad only)
#define NAND_PIN_D3   21   // HOLD# / SIO3 (quad only)
#define NAND_PIN_CS   5

// ============ Feature Register Addresses ============
#define NAND_FEATURE_BLOCK_LOCK  0xA0
#define NAND_FEATURE_OTP         0xB0
#define NAND_FEATURE_STATUS      0xC0
#define NAND_FEATURE_DRIVER_STR  0xD0

// Status register bits (C0h)
#define NAND_STATUS_OIP          0x01  // Operation In Progress
#define NAND_STATUS_WEL          0x02  // Write Enable Latch
#define NAND_STATUS_EFAIL        0x04  // Erase Fail
#define NAND_STATUS_PFAIL        0x08  // Program Fail
#define NAND_STATUS_ECC_S0       0x10  // ECC Status bit 0
#define NAND_STATUS_ECC_S1       0x20  // ECC Status bit 1

// OTP register bits (B0h)
#define NAND_OTP_QE              0x01  // Quad Enable

// Read mode
typedef enum {
  NAND_READ_SINGLE = 0,   // 0x0B — standard 1-line read
  NAND_READ_QUAD   = 1,   // 0x6B — quad 4-line read (x4)
} nand_read_mode_t;

// Configuration
typedef struct {
  int pin_clk;
  int pin_d0;
  int pin_d1;
  int pin_d2;       // -1 to disable quad
  int pin_d3;       // -1 to disable quad
  int pin_cs;
  int clock_hz;
  nand_read_mode_t read_mode;
} nand_config_t;

// Default config
#define NAND_DEFAULT_CONFIG() { \
  .pin_clk   = NAND_PIN_CLK,   \
  .pin_d0    = NAND_PIN_D0,    \
  .pin_d1    = NAND_PIN_D1,    \
  .pin_d2    = NAND_PIN_D2,    \
  .pin_d3    = NAND_PIN_D3,    \
  .pin_cs    = NAND_PIN_CS,    \
  .clock_hz  = 5000000,        \
  .read_mode = NAND_READ_QUAD, \
}

// ============ API ============

// Initialize the SPI bus and NAND device. Returns ESP_OK on success.
esp_err_t nand_init(const nand_config_t *config);

// Reset the NAND chip (FFh)
void nand_reset();

// Wait for OIP (Operation In Progress) to clear
void nand_wait_ready();

// Get/Set feature registers
uint8_t nand_get_feature(uint8_t addr);
void    nand_set_feature(uint8_t addr, uint8_t value);

// Load a page from NAND array into the internal cache (13h)
void nand_page_read_to_cache(uint16_t row_addr);

// Read data from the cache into buffer (uses configured read mode)
void nand_read_cache(uint8_t *buf, int len);

// Force a specific read mode for this call
void nand_read_cache_single(uint8_t *buf, int len);
void nand_read_cache_quad(uint8_t *buf, int len);

// Read a full page with automatic verify-retry
// Returns true if verified, false if still mismatched after max_retries
bool nand_read_page_verified(uint16_t row_addr, uint8_t *buf,
                             int max_retries, uint32_t *retry_count);

// Get the current read mode
nand_read_mode_t nand_get_read_mode();

// Read NAND ID (9Fh) — returns manufacturer + device ID
uint16_t nand_read_id();

// Get ECC status from the last read (from status register)
uint8_t nand_get_ecc_status();

#endif // NAND_DRIVER_H
