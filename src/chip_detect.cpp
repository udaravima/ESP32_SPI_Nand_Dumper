#include "chip_detect.h"

static bool flat(const uint8_t id[3], uint8_t v) {
  return id[0] == v && id[1] == v && id[2] == v;
}

bool chip_detect_nor_id_plausible(const uint8_t nor_id[3]) {
  return !flat(nor_id, 0x00) && !flat(nor_id, 0xFF) && nor_id[0] != 0x00 && nor_id[0] != 0xFF;
}

chip_detect_t chip_detect_resident(const active_profile_t *table, unsigned n,
                                   const uint8_t nand_id[3], const uint8_t nor_id[3]) {
  chip_detect_t r = {NAND_CHIP_UNKNOWN, NAND_PRF_E_UNKNOWN_ID, NULL, CHIP_FAMILY_SPI_NAND};
  nand_prf_err_t e_nand, e_nor;
  const active_profile_t *a = nand_profile_find_family(table, n, CHIP_FAMILY_SPI_NAND,
                                                       nand_id[0], nand_id[1], nand_id[2],
                                                       &e_nand);
  const active_profile_t *b = NULL;
  e_nor = NAND_PRF_E_UNKNOWN_ID;
  if (chip_detect_nor_id_plausible(nor_id))
    b = nand_profile_find_family(table, n, CHIP_FAMILY_SPI_NOR, nor_id[0], nor_id[1],
                                 nor_id[2], &e_nor);
  if (a && b) {                       // both families claim the chip: refuse to pick
    r.state = NAND_CHIP_AMBIGUOUS;
    r.err = NAND_PRF_E_AMBIGUOUS_ID;
    return r;
  }
  if (a || b) {
    r.state = NAND_CHIP_RESIDENT;
    r.err = NAND_PRF_OK;
    r.hit = a ? a : b;
    r.family = r.hit->family;
    return r;
  }
  if (e_nand == NAND_PRF_E_AMBIGUOUS_ID || e_nor == NAND_PRF_E_AMBIGUOUS_ID) {
    r.state = NAND_CHIP_AMBIGUOUS;
    r.err = NAND_PRF_E_AMBIGUOUS_ID;
    r.family = e_nand == NAND_PRF_E_AMBIGUOUS_ID ? CHIP_FAMILY_SPI_NAND : CHIP_FAMILY_SPI_NOR;
  }
  return r;
}
