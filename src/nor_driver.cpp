#include "nor_driver.h"
#include "nor_seq.h"
#include "nand_driver.h"   // nand_spi_xfer
#include <string.h>
#include <Arduino.h>

static nor_seq_t s_nor;
static uint8_t *s_scratch = NULL, *s_quad = NULL;
static int s_max_unit = 0;

static void xfer(void *, uint8_t cmd, uint32_t addr, uint8_t addr_bytes, uint8_t dummy,
                 uint8_t *rx, int rx_len, bool quad) {
  nand_spi_xfer(cmd, addr, addr_bytes * 8, dummy, rx, rx_len, quad);
}

void nor_init(int max_unit_size) {
  nor_bus_t bus = { xfer, NULL };
  nor_seq_init(&s_nor, bus);
  s_max_unit = max_unit_size;
  if (!s_scratch) s_scratch = (uint8_t *)heap_caps_malloc(max_unit_size, MALLOC_CAP_DMA);
}

void nor_apply_profile(const active_profile_t *p) { nor_seq_apply(&s_nor, p); }
void nor_release_power_down(void) {
  nor_seq_release_power_down(&s_nor);
  delayMicroseconds(50);   // tRES1: up to 3 us on most parts, 30 us on a few
}
void nor_read_id(uint8_t id[3]) { nor_seq_read_id(&s_nor, id); }

static void sfdp_rd(void *, uint32_t addr, uint8_t *buf, int len) {
  // SFDP reads are small; bounce through the DMA-capable scratch buffer.
  if (!s_scratch || len > s_max_unit) { memset(buf, 0xFF, len); return; }
  nor_seq_read_sfdp(&s_nor, addr, s_scratch, len);
  memcpy(buf, s_scratch, len);
}
bool nor_probe_sfdp(sfdp_info_t *out) { return sfdp_probe(sfdp_rd, NULL, out); }

int nor_qe_state(void) { return nor_seq_qe_state(&s_nor); }
void nor_begin(void) { nor_seq_begin(&s_nor); }
void nor_end(void) { nor_seq_end(&s_nor); }

void nor_read(uint32_t addr, uint8_t *buf, int len) {
  nor_seq_read(&s_nor, addr, buf, len, s_nor.quad);
}

nand_page_result_t nor_read_verified(uint32_t addr, uint8_t *buf, int len,
                                     int max_retries, uint32_t *retry_count) {
  uint32_t before = retry_count ? *retry_count : 0;
  bool was_quad = s_nor.quad;
  nand_page_result_t r = nor_seq_read_verified(&s_nor, addr, buf, s_scratch, len,
                                               max_retries, retry_count);
  if (retry_count && *retry_count != before)
    Serial.printf("[!] SPI mismatch at 0x%08X (%u retries)\n", (unsigned)addr,
                  (unsigned)(*retry_count - before));
  if (r == NAND_PAGE_OK_SINGLE)
    Serial.printf("[!] Quad read unstable at 0x%08X; recovered with a single read\n",
                  (unsigned)addr);
  if (was_quad && !s_nor.quad)
    Serial.printf("[!] %u units needed the single fallback; reading the rest single x1\n",
                  (unsigned)s_nor.quad_fallbacks);
  return r;
}

// Read `len` bytes at `addr` single and quad; quad is usable if they match.
bool nor_quad_selftest(uint32_t addr, int len) {
  if (!s_quad) s_quad = (uint8_t *)heap_caps_malloc(s_max_unit, MALLOC_CAP_DMA);
  if (!s_quad || !s_scratch || len > s_max_unit) return false;
  nor_begin();
  nor_seq_read(&s_nor, addr, s_scratch, len, false);
  nor_seq_read(&s_nor, addr, s_quad, len, true);
  bool same = memcmp(s_scratch, s_quad, len) == 0;
  nor_seq_read(&s_nor, addr, s_quad, len, true);   // and quad agrees with itself
  same = same && memcmp(s_scratch, s_quad, len) == 0;
  nor_end();
  return same;
}

void nor_set_read_mode(nand_read_mode_t m) { nor_seq_set_quad(&s_nor, m == NAND_READ_QUAD); }
nand_read_mode_t nor_get_read_mode(void) {
  return s_nor.quad ? NAND_READ_QUAD : NAND_READ_SINGLE;
}
uint32_t nor_quad_fallbacks(void) { return s_nor.quad_fallbacks; }
