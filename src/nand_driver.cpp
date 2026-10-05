#include "nand_driver.h"
#include "nand_addr.h"
#include "nand_seq.h"
#include <string.h>
#include <Arduino.h>  // For Serial debug
#include "driver/gpio.h"

static spi_device_handle_t s_spi;
static spi_device_interface_config_t s_devcfg;  // kept so nand_set_clock can re-add the device
static spi_bus_config_t s_buscfg;               // kept so nand_bus_restore can re-init the bus
static uint8_t *s_verify_buf = NULL;

// Feature-register access, from the active profile (family defaults until then).
static uint8_t s_op_get_feat = 0x0F, s_op_set_feat = 0x1F;
static uint8_t s_status_addr = 0xC0, s_cfg_addr = 0xB0, s_ecc_en_mask = 0x10;

// SPI transport for the pure read-path sequencer (nand_seq). The sequencer
// decides opcode/address/plane; this only puts it on the wire.
static void spi_bus_xfer(void *, uint8_t cmd, uint32_t addr, uint8_t addr_bits,
                         uint8_t *rx, int rx_len, bool quad) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | (quad ? SPI_TRANS_MODE_QIO : 0);
  t.base.cmd = cmd;
  t.base.addr = addr;
  t.address_bits = addr_bits;
  t.base.rxlength = rx_len * 8;
  t.base.rx_buffer = rx;
  if (!s_spi) { if (rx) memset(rx, 0xFF, rx_len); return; }   // bus lent to I2C
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}
static void spi_bus_wait(void *) { nand_wait_ready(); }

// Raw transaction with a dummy phase, for the SPI NOR driver (nor_driver.cpp),
// which shares this bus and device. Replies of up to 4 bytes land in rx_data,
// so callers may pass small stack buffers.
void nand_spi_xfer(uint8_t cmd, uint32_t addr, uint8_t addr_bits, uint8_t dummy_bits,
                   uint8_t *rx, int rx_len, bool quad) {
  spi_transaction_ext_t t = {};
  bool small = rx_len > 0 && rx_len <= 4;
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY |
                 (quad ? SPI_TRANS_MODE_QIO : 0) | (small ? SPI_TRANS_USE_RXDATA : 0);
  t.base.cmd = cmd;
  t.base.addr = addr;
  t.address_bits = addr_bits;
  t.dummy_bits = dummy_bits;
  t.base.rxlength = rx_len * 8;
  t.base.rx_buffer = small ? NULL : rx;
  if (!s_spi) { if (rx) memset(rx, 0xFF, rx_len); return; }   // bus lent to I2C
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
  if (small) memcpy(rx, t.base.rx_data, rx_len);
}
static nand_seq_t s_seq;

esp_err_t nand_init(const nand_config_t *config, int max_page_size) {
  nand_bus_t bus = { spi_bus_xfer, NULL, spi_bus_wait };
  nand_seq_init(&s_seq, bus);
  nand_seq_set_quad(&s_seq, config->read_mode == NAND_READ_QUAD);

  s_buscfg = {};
  s_buscfg.mosi_io_num = config->pin_d0;
  s_buscfg.miso_io_num = config->pin_d1;
  s_buscfg.sclk_io_num = config->pin_clk;
  s_buscfg.quadwp_io_num = config->pin_d2;
  s_buscfg.quadhd_io_num = config->pin_d3;
  s_buscfg.max_transfer_sz = max_page_size + 16;

  s_devcfg = {};
  s_devcfg.clock_speed_hz = config->clock_hz;
  s_devcfg.mode = 0;
  s_devcfg.spics_io_num = config->pin_cs;
  s_devcfg.queue_size = 1;
  s_devcfg.flags = SPI_DEVICE_HALFDUPLEX;
  s_devcfg.command_bits = 8;
  s_devcfg.address_bits = 0;  // variable per transaction

  esp_err_t ret = spi_bus_initialize(NAND_SPI_HOST, &s_buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) return ret;
  // Pull MISO up so an empty socket (or a chip ignoring an opcode) reads 0xFF
  // rather than noise: detection then sees a flat ID, not a random one.
  gpio_pullup_en((gpio_num_t)config->pin_d1);
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

// Hand the clip's pins to another bus (I2C EEPROM, eeprom_driver.cpp) and
// take them back. The device keeps its clock; the verify buffer is kept.
esp_err_t nand_bus_release(void) {
  if (!s_spi) return ESP_OK;
  esp_err_t ret = spi_bus_remove_device(s_spi);
  if (ret != ESP_OK) return ret;
  s_spi = NULL;
  return spi_bus_free(NAND_SPI_HOST);
}

esp_err_t nand_bus_restore(void) {
  if (s_spi) return ESP_OK;
  esp_err_t ret = spi_bus_initialize(NAND_SPI_HOST, &s_buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK) return ret;
  gpio_pullup_en((gpio_num_t)s_buscfg.miso_io_num);
  return spi_bus_add_device(NAND_SPI_HOST, &s_devcfg, &s_spi);
}

void nand_apply_profile(const active_profile_t *p) {
  s_op_get_feat = p->op_get_feat;
  s_op_set_feat = p->op_set_feat;
  s_status_addr = p->op_status_addr;
  s_cfg_addr    = p->op_cfg_addr;
  s_ecc_en_mask = (uint8_t)(1u << p->ecc_en_bit);
  nand_seq_set_opcodes(&s_seq, p->op_page_read, p->op_read_cache, p->op_read_cache_x4);
}

void nand_reset(void) {
  spi_transaction_t t = {};
  t.cmd = 0xFF;
  if (!s_spi) return;
  spi_device_polling_transmit(s_spi, &t);
}

uint8_t nand_get_feature(uint8_t addr) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_RXDATA;
  t.base.cmd = s_op_get_feat;
  t.base.addr = addr;
  t.address_bits = 8;
  t.base.rxlength = 8;
  if (!s_spi) return 0xFF;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
  return t.base.rx_data[0];
}

void nand_set_feature(uint8_t addr, uint8_t value) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_USE_TXDATA;
  t.base.cmd = s_op_set_feat;
  t.base.addr = addr;
  t.address_bits = 8;
  t.base.length = 8;
  t.base.tx_data[0] = value;
  if (!s_spi) return;
  spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t);
}

// Toggle on-die ECC (configuration register, ECC-enable bit from the profile).
void nand_set_ecc(bool on) {
  uint8_t cfg = nand_get_feature(s_cfg_addr);
  if (on) cfg |= s_ecc_en_mask;
  else    cfg &= ~s_ecc_en_mask;
  nand_set_feature(s_cfg_addr, cfg);
}

// Bounded: a chip that never answers the NAND status read (a SPI NOR part, or
// an empty socket with MISO high) must not hang detection. Real SPI NAND
// operations finish in well under a millisecond (tRST, tRD).
bool nand_wait_ready(void) {
  uint32_t start = millis();
  while (nand_get_feature(s_status_addr) & NAND_STATUS_OIP)
    if (millis() - start > NAND_READY_TIMEOUT_MS) return false;
  return true;
}

void nand_set_plane_config(uint8_t planes, uint8_t page_addr_bits, uint32_t main_size) {
  nand_seq_set_planes(&s_seq, planes, page_addr_bits, main_size);
}

void nand_page_read_to_cache(uint32_t row_addr) {
  nand_seq_page_read(&s_seq, row_addr);
}

void nand_read_cache_single(uint8_t *buf, int len) {
  nand_seq_read_cache(&s_seq, buf, len, false);
}

// QIO puts the DATA phase on 4 lines; address stays single (1-1-4) — matches 6Bh.
void nand_read_cache_quad(uint8_t *buf, int len) {
  nand_seq_read_cache(&s_seq, buf, len, true);
}

void nand_read_cache(uint8_t *buf, int len) {
  nand_seq_read_cache(&s_seq, buf, len, s_seq.quad);
}

nand_page_result_t nand_read_page_verified(uint32_t row_addr, uint8_t *buf, int page_size,
                                           int max_retries, uint32_t *retry_count) {
  uint32_t before = retry_count ? *retry_count : 0;
  bool was_quad = s_seq.quad;
  nand_page_result_t r = nand_seq_read_verified(&s_seq, row_addr, buf, s_verify_buf,
                                                page_size, max_retries, retry_count);
  if (retry_count && *retry_count != before)
    Serial.printf("[!] SPI mismatch at row 0x%06X (%u retries)\n",
                  (unsigned)row_addr, (unsigned)(*retry_count - before));
  if (r == NAND_PAGE_OK_SINGLE)
    Serial.printf("[!] Quad read unstable at row 0x%06X; recovered with a single read\n",
                  (unsigned)row_addr);
  if (was_quad && !s_seq.quad)
    Serial.printf("[!] %u pages needed the single fallback; reading the rest single x1\n",
                  (unsigned)s_seq.quad_fallbacks);
  return r;
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

uint32_t nand_quad_fallbacks(void) { return s_seq.quad_fallbacks; }
nand_read_mode_t nand_get_read_mode(void) {
  return s_seq.quad ? NAND_READ_QUAD : NAND_READ_SINGLE;
}
void nand_set_read_mode(nand_read_mode_t m) { nand_seq_set_quad(&s_seq, m == NAND_READ_QUAD); }

// 9Fh + one zero address/dummy byte, then mfr, dev and one more byte (dev2).
// Read into a DMA-capable buffer: rx_data (USE_RXDATA) holds only 4 bytes, and
// the sequencer passes an rx pointer.
void nand_read_id(uint8_t id[3]) {
  nand_seq_read_id(&s_seq, s_verify_buf, 3);
  memcpy(id, s_verify_buf, 3);
}

uint8_t nand_get_status(void) {
  return nand_get_feature(s_status_addr);
}
