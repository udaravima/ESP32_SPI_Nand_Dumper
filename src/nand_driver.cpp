#include "nand_driver.h"
#include <string.h>
#include <Arduino.h>  // For Serial debug

static spi_device_handle_t s_spi;
static nand_read_mode_t s_read_mode = NAND_READ_QUAD;
static uint8_t *s_verify_buf = NULL;

// ---- Low-level SPI transactions ----

esp_err_t nand_init(const nand_config_t *config) {
  s_read_mode = config->read_mode;

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = config->pin_d0;
  buscfg.miso_io_num = config->pin_d1;
  buscfg.sclk_io_num = config->pin_clk;
  buscfg.quadwp_io_num = config->pin_d2;
  buscfg.quadhd_io_num = config->pin_d3;
  buscfg.max_transfer_sz = NAND_PAGE_SIZE + 64;

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = config->clock_hz;
  devcfg.mode = 0;
  devcfg.spics_io_num = config->pin_cs;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_HALFDUPLEX;
  devcfg.command_bits = 8;
  devcfg.address_bits = 0;  // Variable per transaction

  esp_err_t ret = spi_bus_initialize(VSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) return ret;

  ret = spi_bus_add_device(VSPI_HOST, &devcfg, &s_spi);
  if (ret != ESP_OK) return ret;

  // Allocate verify buffer (DMA-capable)
  s_verify_buf = (uint8_t*)heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
  if (!s_verify_buf) return ESP_ERR_NO_MEM;

  // Reset and enable quad if needed
  nand_reset();
  nand_wait_ready();

  if (s_read_mode == NAND_READ_QUAD) {
    uint8_t b0 = nand_get_feature(NAND_FEATURE_OTP);
    b0 |= NAND_OTP_QE;
    nand_set_feature(NAND_FEATURE_OTP, b0);

    b0 = nand_get_feature(NAND_FEATURE_OTP);
    if (!(b0 & NAND_OTP_QE)) {
      Serial.println("[!] WARNING: QE bit did not set!");
      return ESP_FAIL;
    }
    Serial.printf("[*] Quad mode enabled (B0h=0x%02X)\n", b0);
  }

  return ESP_OK;
}

void nand_reset() {
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
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
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
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
}

void nand_wait_ready() {
  uint8_t status;
  do {
    status = nand_get_feature(NAND_FEATURE_STATUS);
  } while (status & NAND_STATUS_OIP);
}

void nand_page_read_to_cache(uint16_t row_addr) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR;
  t.base.cmd = 0x13;
  t.base.addr = row_addr;  // Sent as 24 bits (upper 8 = dummy)
  t.address_bits = 24;
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
}

void nand_read_cache_single(uint8_t *buf, int len) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR;
  t.base.cmd = 0x0B;         // READ FROM CACHE (fast)
  t.base.addr = 0x000000;    // Col addr(16) + dummy(8) = 24 bits
  t.address_bits = 24;
  t.base.rxlength = len * 8;
  t.base.rx_buffer = buf;
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
}

void nand_read_cache_quad(uint8_t *buf, int len) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_MODE_QIO;
  t.base.cmd = 0x6B;         // READ FROM CACHE x4
  t.base.addr = 0x000000;    // Col addr(16) + dummy(8) = 24 bits
  t.address_bits = 24;
  t.base.rxlength = len * 8;
  t.base.rx_buffer = buf;
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
}

void nand_read_cache(uint8_t *buf, int len) {
  if (s_read_mode == NAND_READ_QUAD) {
    nand_read_cache_quad(buf, len);
  } else {
    nand_read_cache_single(buf, len);
  }
}

bool nand_read_page_verified(uint16_t row_addr, uint8_t *buf,
                              int max_retries, uint32_t *retry_count) {
  nand_page_read_to_cache(row_addr);
  nand_wait_ready();
  nand_read_cache(buf, NAND_PAGE_SIZE);

  for (int attempt = 0; attempt < max_retries; attempt++) {
    // Re-read from cache — no PAGE READ needed, cache is still loaded
    nand_read_cache(s_verify_buf, NAND_PAGE_SIZE);

    if (memcmp(buf, s_verify_buf, NAND_PAGE_SIZE) == 0) {
      return true;  // Verified
    }

    if (retry_count) (*retry_count)++;
    Serial.printf("[!] SPI mismatch at row 0x%04X (retry %d)\n",
                  row_addr, attempt + 1);
    memcpy(buf, s_verify_buf, NAND_PAGE_SIZE);
  }
  return false;  // Failed after retries
}

nand_read_mode_t nand_get_read_mode() {
  return s_read_mode;
}

uint16_t nand_read_id() {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_RXDATA;
  t.base.cmd = 0x9F;
  t.base.addr = 0x00;  // 8-bit dummy
  t.address_bits = 8;
  t.base.rxlength = 16;
  spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
  return (t.base.rx_data[0] << 8) | t.base.rx_data[1];
}

uint8_t nand_get_ecc_status() {
  uint8_t status = nand_get_feature(NAND_FEATURE_STATUS);
  return (status >> 4) & 0x03;  // ECC_S1:ECC_S0
}
