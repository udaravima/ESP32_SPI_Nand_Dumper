#ifndef NAND_CHIPS_H
#define NAND_CHIPS_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { NAND_READ_SINGLE = 0, NAND_READ_QUAD = 1 } nand_read_mode_t;

typedef struct {
  const char *name;
  uint8_t  mfr_id, dev_id;
  uint16_t page_size;        // total bytes/page (main + spare)
  uint16_t spare_size;
  uint16_t pages_per_block;
  uint16_t total_blocks;
  uint8_t  page_addr_bits;   // log2(pages_per_block)
  uint8_t  bad_block_mark;
  bool     has_qe_bit;
  uint8_t  qe_feature_addr;
  uint8_t  qe_bit;
  bool     ecc_default_on;
  uint16_t vcc_mv;
  uint8_t  planes;           // 1, or 2 for multi-plane parts (plane = block LSBs)
} nand_chip_t;

const nand_chip_t *nand_chip_lookup(uint8_t mfr_id, uint8_t dev_id);

#endif // NAND_CHIPS_H
