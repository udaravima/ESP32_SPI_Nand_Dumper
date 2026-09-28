#include "nand_driver.h"
#include "nand_addr.h"
#include <string.h>
#include <Arduino.h>  // For Serial debug

static spi_device_handle_t s_spi;
static spi_device_interface_config_t s_devcfg;  // kept so nand_set_clock can re-add the device
static nand_read_mode_t s_read_mode = NAND_READ_SINGLE;
static uint8_t *s_verify_buf = NULL;
// Plane selection for multi-plane dies (see nand_cache_column()).
static uint8_t  s_planes = 1, s_page_addr_bits = 6, s_plane_bit = 12;
static uint16_t s_cache_col = 0;   // column (incl. plane bit) of the last PAGE READ

esp_err_t nand_init(const nand_config_t *config, int max_page_size) {
  s_read_mode = config->read_mode;

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = config->pin_d0;
  buscfg.miso_io_num = config->pin_d1;
  buscfg.sclk_io_num = config->pin_clk;
  buscfg.quadwp_io_num = config->pin_d2;
  buscfg.quadhd_io_num = config->pin_d3;
  buscfg.max_transfer_sz = max_page_size + 16;

  s_devcfg = {};
  s_devcfg.clock_speed_hz = config->clock_hz;
  s_devcfg.mode = 0;
  s_devcfg.spics_io_num = config->pin_cs;
  s_devcfg.queue_size = 1;
  s_devcfg.flags = SPI_DEVICE_HALFDUPLEX;
  s_devcfg.command_bits = 8;
  s_devcfg.address_bits = 0;  // variable per transaction

  esp_err_t ret = spi_bus_initialize(NAND_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) return ret;
  ret = spi_bus_add_device(NAND_SPI_HOST, &s_devcfg, &s_spi);
  if (ret != ESP_OK) return ret;

  // Verify buffer sized to the largest page we might read.
  s_verify_buf = (uint8_t *)heap_caps_malloc(max_page_size, MALLOC_CAP_DMA);
  if (!s_verify_buf) return ESP_ERR_NO_MEM;

  nand_reset();
  nand_wait_ready();
  return ESP_OK;
}

// Re-create the SPI device at a new clock. The device is first created at a
// slow, safe speed (for chip detection); call this after the user picks a
// speed so the dump actually runs at it. Without this the bus is stuck at the
// init clock no matter what the menu shows. Returns ESP_OK on success.
esp_err_t nand_set_clock(int clock_hz) {
  if (!s_spi) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = spi_bus_remove_device(s_spi);
  if (ret != ESP_OK) return ret;
  s_spi = NULL;
  s_devcfg.clock_speed_hz = clock_hz;
  return spi_bus_add_device(NAND_SPI_HOST, &s_devcfg, &s_spi);
}

void nand_reset(void) {
  spi_transaction_t t = {};
  t.cmd = 0xFF;
  spi_device_polling_transmit(s_spi, &t);
}

uint8_t nand_get_feature(uint8_t addr) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_RXDATA;
  t.base.cmd = 0x0F;
  t.base.addr = addr;
  t.address_bits = 8;
  t.base.rxlength = 8;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
  return t.base.rx_data[0];
}

void nand_set_feature(uint8_t addr, uint8_t value) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_TXDATA;
  t.base.cmd = 0x1F;
  t.base.addr = addr;
  t.address_bits = 8;
  t.base.length = 8;
  t.base.tx_data[0] = value;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

// Toggle on-die ECC (Configuration register B0h, bit4 ECC_EN).
void nand_set_ecc(bool on) {
  uint8_t cfg = nand_get_feature(NAND_FEATURE_CONFIG);
  if (on) cfg |= NAND_CONFIG_ECC_EN;
  else    cfg &= ~NAND_CONFIG_ECC_EN;
  nand_set_feature(NAND_FEATURE_CONFIG, cfg);
}

void nand_wait_ready(void) {
  uint8_t status;
  do {
    status = nand_get_feature(NAND_FEATURE_STATUS);
  } while (status & NAND_STATUS_OIP);
}

void nand_set_plane_config(uint8_t planes, uint8_t page_addr_bits, uint32_t main_size) {
  s_planes = planes ? planes : 1;
  s_page_addr_bits = page_addr_bits;
  s_plane_bit = nand_plane_bit(main_size);
  s_cache_col = 0;
}

void nand_page_read_to_cache(uint32_t row_addr) {
  s_cache_col = nand_cache_column(row_addr, s_page_addr_bits, s_planes, s_plane_bit);
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR;
  t.base.cmd = 0x13;
  t.base.addr = row_addr & 0xFFFFFF;  // 24-bit row (17 used on 2Gbit)
  t.address_bits = 24;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

void nand_read_cache_single(uint8_t *buf, int len) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR;
  t.base.cmd = 0x0B;         // READ FROM CACHE (fast)
  t.base.addr = (uint32_t)s_cache_col << 8;  // col addr(16, plane bit incl.) + dummy(8)
  t.address_bits = 24;
  t.base.rxlength = len * 8;
  t.base.rx_buffer = buf;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

void nand_read_cache_quad(uint8_t *buf, int len) {
  spi_transaction_ext_t t = {};
  // QIO puts the DATA phase on 4 lines; address stays single (1-1-4) — matches 6Bh.
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_MODE_QIO;
  t.base.cmd = 0x6B;         // READ FROM CACHE x4
  t.base.addr = (uint32_t)s_cache_col << 8;  // col addr(16, plane bit incl.) + dummy(8)
  t.address_bits = 24;
  t.base.rxlength = len * 8;
  t.base.rx_buffer = buf;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

void nand_read_cache(uint8_t *buf, int len) {
  if (s_read_mode == NAND_READ_QUAD) nand_read_cache_quad(buf, len);
  else nand_read_cache_single(buf, len);
}

bool nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size,
                             int max_retries, uint32_t *retry_count) {
  nand_page_read_to_cache(row_addr);
  nand_wait_ready();
  nand_read_cache(buf, page_size);

  // A matching first pair passes regardless of max_retries (fixes retries==0).
  for (int attempt = 0; attempt <= max_retries; attempt++) {
    nand_read_cache(s_verify_buf, page_size);  // cache still loaded, no re-PAGE-READ
    if (memcmp(buf, s_verify_buf, page_size) == 0) return true;
    if (retry_count) (*retry_count)++;
    Serial.printf("[!] SPI mismatch at row 0x%06X (retry %d)\n",
                  (unsigned)row_addr, attempt + 1);
    memcpy(buf, s_verify_buf, page_size);
  }
  return false;
}

// Read probe_row via single and via quad; return true if they match.
bool nand_quad_selftest(uint32_t probe_row, int page_size) {
  nand_page_read_to_cache(probe_row);
  nand_wait_ready();
  nand_read_cache_single(s_verify_buf, page_size);
  static uint8_t *q = NULL;
  if (!q) q = (uint8_t *)heap_caps_malloc(page_size, MALLOC_CAP_DMA);
  if (!q) return false;
  nand_read_cache_quad(q, page_size);
  return memcmp(s_verify_buf, q, page_size) == 0;
}

nand_read_mode_t nand_get_read_mode(void) { return s_read_mode; }
void nand_set_read_mode(nand_read_mode_t m) { s_read_mode = m; }

uint16_t nand_read_id(void) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_RXDATA;
  t.base.cmd = 0x9F;
  t.base.addr = 0x00;  // 8-bit dummy
  t.address_bits = 8;
  t.base.rxlength = 16;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
  return (t.base.rx_data[0] << 8) | t.base.rx_data[1];
}

uint8_t nand_get_ecc_status(void) {
  return (nand_get_feature(NAND_FEATURE_STATUS) >> 4) & 0x07;  // ECCS0..2
}
