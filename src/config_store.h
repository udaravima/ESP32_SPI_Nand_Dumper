#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H
#include <stdint.h>
#include <stdbool.h>

// Persisted application settings: transport + read behaviour only. Geometry is
// deliberately NOT here — it is auto-detected from the chip ID on every boot, so
// a stale saved geometry can never mask a correctly-detected chip.
typedef struct {
  char     ssid[64];
  char     pass[64];
  uint16_t tcp_port;
  int32_t  spi_clock_hz;
  uint8_t  read_mode;    // nand_read_mode_t: 0 = single x1, 1 = quad x4
  bool     verify;
  bool     ecc_on;
  int32_t  max_retries;
  int32_t  batch_pages;  // pages coalesced per TCP write (throughput knob; 1 = per-page)
} nand_app_config_t;

// Safe bounds enforced by config_validate().
#define CONFIG_CLOCK_MIN_HZ  100000      // 100 kHz
#define CONFIG_CLOCK_MAX_HZ  80000000    // 80 MHz
#define CONFIG_RETRIES_MAX   100
#define CONFIG_BATCH_MAX     64

// Fill with compiled defaults. WiFi credentials are intentionally EMPTY — they
// are entered once via the serial menu, then persisted to NVS, so a plaintext
// password never ships in the source tree.
void config_defaults(nand_app_config_t *c);

// Clamp a possibly-garbage config (e.g. a half-written or schema-mismatched NVS
// record) into safe ranges, in place. Pure — host-testable.
void config_validate(nand_app_config_t *c);

// NVS-backed persistence (ESP only; not compiled in the host test env).
// config_load returns false when nothing valid is stored yet.
bool config_load(nand_app_config_t *c);
void config_save(const nand_app_config_t *c);

#endif // CONFIG_STORE_H
