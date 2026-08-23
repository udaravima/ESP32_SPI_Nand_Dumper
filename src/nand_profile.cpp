#include "nand_profile.h"

nand_severity_t nand_profile_severity(const active_profile_t *p, uint8_t status) {
  (void)p; (void)status;
  return NAND_SEV_UNCORRECTABLE; // conservative stub; real decode in Task 2
}
