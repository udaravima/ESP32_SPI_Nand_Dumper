// Pure config logic (no Arduino/ESP-IDF deps) so it compiles in the host test
// env. The NVS-backed load/save live in config_nvs.cpp (ESP only).
#include "config_store.h"
#include <string.h>

void config_defaults(nand_app_config_t *c) {
  memset(c, 0, sizeof(*c));
  c->ssid[0]       = '\0';   // entered on first boot, then persisted
  c->pass[0]       = '\0';
  c->tcp_port      = 3333;
  c->spi_clock_hz  = 1000000;  // 1 MHz — safe default; the menu raises it
  c->read_mode     = 0;        // single x1
  c->verify        = true;
  c->ecc_on        = false;    // raw by default
  c->max_retries   = 5;
}

void config_validate(nand_app_config_t *c) {
  if (c->tcp_port == 0) c->tcp_port = 3333;
  if (c->spi_clock_hz < CONFIG_CLOCK_MIN_HZ) c->spi_clock_hz = CONFIG_CLOCK_MIN_HZ;
  if (c->spi_clock_hz > CONFIG_CLOCK_MAX_HZ) c->spi_clock_hz = CONFIG_CLOCK_MAX_HZ;
  if (c->read_mode > 1) c->read_mode = 0;
  if (c->max_retries < 0) c->max_retries = 0;
  if (c->max_retries > CONFIG_RETRIES_MAX) c->max_retries = CONFIG_RETRIES_MAX;
  // Guarantee NUL-termination for strings pulled out of NVS.
  c->ssid[sizeof(c->ssid) - 1] = '\0';
  c->pass[sizeof(c->pass) - 1] = '\0';
}
