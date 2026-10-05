// A stand-in device for tests/test_dump_session.py: the real session module
// (src/nand_session.cpp) on stdin/stdout, with the detected ID from argv
// (the NAND view, then optionally the NOR view, then optionally the I2C ACK
// mask and the SPI EEPROM status byte).
// It lets dump.py's client talk to the firmware's own C code, not a mock.
#include "nand_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static size_t rd(void *, uint8_t *b, size_t n, uint32_t) {
  size_t got = 0;
  while (got < n) {
    ssize_t k = read(0, b + got, n - got);
    if (k <= 0) break;
    got += (size_t)k;
  }
  return got;
}
static size_t wr(void *, const uint8_t *b, size_t n) {
  ssize_t k = write(1, b, n);
  return k < 0 ? 0 : (size_t)k;
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  active_profile_t active;
  nand_profile_manual(&active, 2112, 64, 64, 1024, 1);
  nand_session_t s = {};
  for (int i = 0; i < 3; i++) s.id[i] = (uint8_t)strtol(argv[1 + i], NULL, 16);
  if (argc >= 7)
    for (int i = 0; i < 3; i++) s.nor_id[i] = (uint8_t)strtol(argv[4 + i], NULL, 16);
  s.spi_ee_status = 0xFF;
  if (argc >= 9) {
    s.i2c_ack_mask = (uint8_t)strtol(argv[7], NULL, 16);
    s.spi_ee_status = (uint8_t)strtol(argv[8], NULL, 16);
  }
  s.chip_state = NAND_CHIP_UNKNOWN;
  s.active = &active;
  s.max_page_size = 8192;
  s.timeout_ms = 100;
  nand_session_begin(&s);
  nand_link_t l = {rd, wr, NULL};
  uint8_t c;
  while (read(0, &c, 1) == 1) {
    switch (nand_session_handle(&s, &l, c)) {
      case NAND_SESS_ARMED:
        active = s.armed;
        s.chip_state = NAND_CHIP_PUSHED;
        break;
      case NAND_SESS_GO:
        // Stand-in for the dump: the active profile's name, NUL-padded.
        wr(NULL, (const uint8_t *)active.name, sizeof(active.name));
        return 0;
      case NAND_SESS_CLOSE:
        return 1;
      default:
        break;
    }
  }
  return 0;
}
