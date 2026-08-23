#ifndef NAND_PROFILE_H
#define NAND_PROFILE_H
#include <stdint.h>

#define NAND_PROFILE_SIZE 110

typedef enum {
  NAND_SEV_OK = 0,
  NAND_SEV_CORRECTED = 1,
  NAND_SEV_CORRECTED_REFRESH = 2,
  NAND_SEV_UNCORRECTABLE = 3,
} nand_severity_t;

// Flat, packed wire struct. Host flattens the 3 YAML layers into this; the
// device holds one copy in RAM. Packed so the Python '<' packer (no alignment
// padding) matches byte-for-byte; GCC emits safe byte-wise member access.
typedef struct __attribute__((packed)) {
  char     name[24];                       // NUL-terminated; strlen <= 23
  uint32_t page_size, spare_size, pages_per_block, total_blocks;
  uint8_t  op_page_read, op_read_cache, op_get_feat, op_set_feat, op_status_addr, op_cfg_addr;
  uint8_t  ecc_en_bit;                     // config (B0h) bit toggling on-die ECC
  uint8_t  ecc_shift, ecc_mask;
  uint8_t  ecc_map[16];                    // field value -> nand_severity_t; unmapped => UNCOR
  uint8_t  status2_reg;                    // 0=none; 0x30 Winbond / 0xF0 GigaDevice (deferred)
  uint8_t  id_method, id_n_bytes;          // 0=addr,1=dummy; ID byte count
  uint8_t  qe_addr, qe_bit;                // 0 if none
  uint8_t  read_mode;
  uint16_t vcc_mv;
  uint8_t  bbm_off, bbm_len, bbm_good;
  uint8_t  oob_free_n, oob_ecc_n;          // valid region counts, each <= 4
  uint16_t oob_free[8];                    // up to 4 x (off,len)
  uint16_t oob_ecc[8];                     // up to 4 x (off,len)
} active_profile_t;

static_assert(sizeof(active_profile_t) == NAND_PROFILE_SIZE,
              "active_profile_t layout drifted from the 110-byte wire contract");

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status);

#endif // NAND_PROFILE_H
