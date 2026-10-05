// NVS-backed persistence for nand_app_config_t (ESP only). Stores the struct as
// one blob under a schema version, so presence and layout are both checked
// before a load is trusted. Excluded from the host test env's build filter.
#include "config_store.h"
#include <Preferences.h>

static const char *CFG_NS   = "nandcfg";
static const char *CFG_VER  = "ver";
static const char *CFG_BLOB = "blob";
static const uint8_t CFG_SCHEMA = 2;   // bump when nand_app_config_t layout changes

bool config_load(nand_app_config_t *c) {
  Preferences p;
  if (!p.begin(CFG_NS, true)) return false;   // read-only; missing namespace
  bool ok = (p.getUChar(CFG_VER, 0) == CFG_SCHEMA &&
             p.getBytesLength(CFG_BLOB) == sizeof(*c) &&
             p.getBytes(CFG_BLOB, c, sizeof(*c)) == sizeof(*c));
  p.end();
  if (ok) config_validate(c);   // never trust stored bytes verbatim
  return ok;
}

static const char *CFG_CHIP = "chip";

bool config_load_chip_choice(nand_chip_choice_t *c) {
  Preferences p;
  if (!p.begin(CFG_NS, true)) return false;
  bool ok = p.getBytesLength(CFG_CHIP) == sizeof(*c) &&
            p.getBytes(CFG_CHIP, c, sizeof(*c)) == sizeof(*c);
  p.end();
  if (ok) c->name[sizeof(c->name) - 1] = '\0';
  return ok;
}

void config_save_chip_choice(const nand_chip_choice_t *c) {
  Preferences p;
  if (!p.begin(CFG_NS, false)) return;
  p.putBytes(CFG_CHIP, c, sizeof(*c));
  p.end();
}

void config_save(const nand_app_config_t *c) {
  Preferences p;
  if (!p.begin(CFG_NS, false)) return;
  p.putUChar(CFG_VER, CFG_SCHEMA);
  p.putBytes(CFG_BLOB, c, sizeof(*c));
  p.end();
}
