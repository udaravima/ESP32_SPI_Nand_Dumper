#include "nand_profile.h"

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status) {
  uint8_t field = (uint8_t)((status >> p->ecc_shift) & p->ecc_mask);
  return (nand_severity_t)p->ecc_map[field]; // field is 0..15; ecc_map is 16 wide
}
