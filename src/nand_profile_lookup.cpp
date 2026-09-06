#include "nand_profile.h"
#include "nand_profiles_generated.h"

const active_profile_t *nand_profile_lookup(uint8_t mfr, uint8_t dev) {
  for (unsigned i = 0; i < PROFILES_COUNT; i++) {
    if (PROFILE_IDS[i].mfr == mfr && PROFILE_IDS[i].dev == dev) {
      return &PROFILES[i];
    }
  }
  return 0;
}
