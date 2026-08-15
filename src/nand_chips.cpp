#include "nand_chips.h"
#include "nand_chips_generated.h"

const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id) {
  for (unsigned i = 0; i < CHIPS_COUNT; i++) {
    if (CHIPS[i].mfr_id == mfr_id && CHIPS[i].dev_id == dev_id) {
      return &CHIPS[i];
    }
  }
  return 0;
}
