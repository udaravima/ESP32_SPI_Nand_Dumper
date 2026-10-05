#include "nand_profile.h"
#include "nand_profiles_generated.h"
#include "dump_header.h"   // dump_crc32 (zlib-compatible)
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "nand_profile_unpack copies little-endian blob bytes straight into the struct"
#endif

const char *nand_prf_err_name(nand_prf_err_t e) {
  switch (e) {
    case NAND_PRF_OK:             return "OK";
    case NAND_PRF_E_BAD_MAGIC:    return "E_BAD_MAGIC";
    case NAND_PRF_E_SCHEMA_VER:   return "E_SCHEMA_VER";
    case NAND_PRF_E_BAD_LEN:      return "E_BAD_LEN";
    case NAND_PRF_E_BAD_CRC:      return "E_BAD_CRC";
    case NAND_PRF_E_STRUCTURE:    return "E_STRUCTURE";
    case NAND_PRF_E_GEOMETRY:     return "E_GEOMETRY";
    case NAND_PRF_E_PAGE_TOO_BIG: return "E_PAGE_TOO_BIG";
    case NAND_PRF_E_ID_MISMATCH:  return "E_ID_MISMATCH";
    case NAND_PRF_E_AMBIGUOUS_ID: return "E_AMBIGUOUS_ID";
    case NAND_PRF_E_UNKNOWN_ID:   return "E_UNKNOWN_ID";
    case NAND_PRF_E_NOT_STAGED:   return "E_NOT_STAGED";
    case NAND_PRF_E_ARM_CRC:      return "E_ARM_CRC";
    case NAND_PRF_E_NOT_ARMED:    return "E_NOT_ARMED";
    case NAND_PRF_E_BAD_CMD:      return "E_BAD_CMD";
    case NAND_PRF_E_TIMEOUT:      return "E_TIMEOUT";
  }
  return "E_?";
}

static bool regions_ok(const uint16_t *regs, uint8_t n, uint32_t spare) {
  if (n > 4) return false;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t off = regs[2 * i], len = regs[2 * i + 1];
    if (len == 0 || off + len > spare) return false;
  }
  return true;
}

static bool all_zero(const void *v, size_t n) {
  const uint8_t *b = (const uint8_t *)v;
  for (size_t i = 0; i < n; i++)
    if (b[i]) return false;
  return true;
}

// SPI NOR branch of nand_profile_check. Mirrors chipdb._check_nor().
static nand_prf_err_t check_nor(const active_profile_t *p, uint32_t max_page_size) {
  // Tier 2: every NAND-only field is zero, so a NOR profile can't half-drive
  // the NAND read path (and vice versa below).
  if (p->op_page_read || p->op_status_addr || p->op_cfg_addr || p->ecc_en_bit ||
      p->ecc_shift || p->ecc_mask || !all_zero(p->ecc_map, sizeof(p->ecc_map)) ||
      p->status2_reg || p->qe_addr || p->qe_bit || p->planes != 1 ||
      p->bbm_off || p->bbm_len || p->bbm_good || p->bbm_pages ||
      p->oob_free_n || p->oob_ecc_n || !all_zero(p->oob_free, sizeof(p->oob_free)) ||
      !all_zero(p->oob_ecc, sizeof(p->oob_ecc)))
    return NAND_PRF_E_STRUCTURE;
  if (!p->op_read_cache || !p->op_read_cache_x4 || !p->op_get_feat || !p->op_set_feat)
    return NAND_PRF_E_STRUCTURE;
  if (p->read_mode > NAND_READ_QUAD) return NAND_PRF_E_STRUCTURE;
  if (p->id_method != NAND_ID_METHOD_NONE || p->id_n_bytes != 3) return NAND_PRF_E_STRUCTURE;
  if (p->addr_bytes != 3 && p->addr_bytes != 4) return NAND_PRF_E_STRUCTURE;
  if (p->addr4_mode > NOR_ADDR4_ENTER_WREN) return NAND_PRF_E_STRUCTURE;
  if ((p->addr_bytes == 4) != (p->addr4_mode != NOR_ADDR4_NONE)) return NAND_PRF_E_STRUCTURE;
  if (p->dummy_x1 > NOR_MAX_DUMMY || p->dummy_x4 > NOR_MAX_DUMMY) return NAND_PRF_E_STRUCTURE;
  if (p->qer > NOR_QER_MAX) return NAND_PRF_E_STRUCTURE;

  // Tier 3: a linear array read in power-of-two frames, no spare area.
  uint32_t page = p->page_size, ppb = p->pages_per_block;
  if (p->spare_size != 0) return NAND_PRF_E_GEOMETRY;
  if (page == 0 || (page & (page - 1))) return NAND_PRF_E_GEOMETRY;
  if (ppb == 0 || (ppb & (ppb - 1)) || p->total_blocks == 0) return NAND_PRF_E_GEOMETRY;
  uint64_t size = (uint64_t)page * ppb * p->total_blocks;
  if (size >= (1ull << 32)) return NAND_PRF_E_GEOMETRY;
  // A 3-byte address reaches 16 MiB; above that the read would wrap silently.
  if (size > (1ull << 24) && p->addr_bytes != 4) return NAND_PRF_E_GEOMETRY;
  if (page > max_page_size) return NAND_PRF_E_PAGE_TOO_BIG;
  return NAND_PRF_OK;
}

nand_prf_err_t nand_profile_check(const active_profile_t *p, uint32_t max_page_size) {
  // Tier 2: structural. Mirrors chipdb.check_flat().
  if (memchr(p->name, '\0', sizeof(p->name)) == NULL) return NAND_PRF_E_STRUCTURE;
  if (!all_zero(p->_pad1, sizeof(p->_pad1))) return NAND_PRF_E_STRUCTURE;
  if (p->family == CHIP_FAMILY_SPI_NOR) return check_nor(p, max_page_size);
  if (p->family != CHIP_FAMILY_SPI_NAND) return NAND_PRF_E_STRUCTURE;
  if (p->addr_bytes || p->addr4_mode || p->dummy_x1 || p->dummy_x4 || p->qer)
    return NAND_PRF_E_STRUCTURE;
  if (p->ecc_shift > 7) return NAND_PRF_E_STRUCTURE;
  if (p->ecc_mask != 0x1 && p->ecc_mask != 0x3 && p->ecc_mask != 0x7 && p->ecc_mask != 0xF)
    return NAND_PRF_E_STRUCTURE;
  for (int i = 0; i < 16; i++)
    if (p->ecc_map[i] > NAND_ECC_UNCORRECTABLE) return NAND_PRF_E_STRUCTURE;
  if (!p->op_page_read || !p->op_read_cache || !p->op_read_cache_x4 || !p->op_get_feat ||
      !p->op_set_feat || !p->op_status_addr || !p->op_cfg_addr)
    return NAND_PRF_E_STRUCTURE;
  if (p->ecc_en_bit > 7 || p->read_mode > NAND_READ_QUAD) return NAND_PRF_E_STRUCTURE;
  if (p->id_method > NAND_ID_METHOD_DUMMY || p->id_n_bytes < 1 || p->id_n_bytes > 4)
    return NAND_PRF_E_STRUCTURE;
  if (p->planes != 1 && p->planes != 2 && p->planes != 4) return NAND_PRF_E_STRUCTURE;
  if (!regions_ok(p->oob_free, p->oob_free_n, p->spare_size) ||
      !regions_ok(p->oob_ecc, p->oob_ecc_n, p->spare_size))
    return NAND_PRF_E_STRUCTURE;
  if (p->bbm_len == 0 || (uint32_t)p->bbm_off + p->bbm_len > p->spare_size)
    return NAND_PRF_E_STRUCTURE;
  if (p->bbm_pages == 0 || (p->bbm_pages & ~0x07)) return NAND_PRF_E_STRUCTURE;

  // Tier 3: physical sanity. Only hard bounds that protect the device itself.
  if (p->spare_size == 0 || p->spare_size >= p->page_size) return NAND_PRF_E_GEOMETRY;
  uint32_t ppb = p->pages_per_block;
  if (ppb == 0 || (ppb & (ppb - 1))) return NAND_PRF_E_GEOMETRY;
  if (p->total_blocks == 0 || p->total_blocks % p->planes) return NAND_PRF_E_GEOMETRY;
  if ((uint64_t)p->page_size * ppb * p->total_blocks >= (1ull << 32)) return NAND_PRF_E_GEOMETRY;
  if (p->page_size > max_page_size) return NAND_PRF_E_PAGE_TOO_BIG;
  return NAND_PRF_OK;
}

nand_prf_err_t nand_profile_unpack(const uint8_t *blob, size_t len,
                                   uint32_t max_page_size, active_profile_t *out) {
  if (len < 10 || memcmp(blob, "PRF", 3) != 0) return NAND_PRF_E_BAD_MAGIC;
  if (blob[3] != NAND_PROFILE_SCHEMA_VER) return NAND_PRF_E_SCHEMA_VER;
  uint16_t body = (uint16_t)(blob[4] | (blob[5] << 8));
  if (body != NAND_PROFILE_SIZE || len != NAND_PROFILE_BLOB_SIZE) return NAND_PRF_E_BAD_LEN;
  const uint8_t *c = blob + 6 + NAND_PROFILE_SIZE;
  uint32_t crc = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) |
                 ((uint32_t)c[3] << 24);
  if (dump_crc32(blob, 6 + NAND_PROFILE_SIZE) != crc) return NAND_PRF_E_BAD_CRC;

  active_profile_t tmp;
  memcpy(&tmp, blob + 6, sizeof(tmp));
  nand_prf_err_t e = nand_profile_check(&tmp, max_page_size);
  if (e == NAND_PRF_OK) *out = tmp;
  return e;
}

bool nand_profile_id_matches(const active_profile_t *p, uint8_t mfr, uint8_t dev,
                             uint8_t dev2) {
  if (p->id_mfr != mfr || p->id_dev != dev) return false;
  return !(p->id_flags & NAND_PROFILE_ID_HAS_DEV2) || p->id_dev2 == dev2;
}

const active_profile_t *nand_profile_find_family(const active_profile_t *table, unsigned n,
                                                 uint8_t family, uint8_t mfr, uint8_t dev,
                                                 uint8_t dev2, nand_prf_err_t *err) {
  // A candidate shares (mfr, dev) and, if it declares dev2, matches it too: a
  // chip whose dev2 differs is a different chip (a W25Q256 is not a W25Q128).
  const active_profile_t *any = NULL, *by_dev2 = NULL;
  unsigned n_any = 0, n_dev2 = 0;
  for (unsigned i = 0; i < n; i++) {
    const active_profile_t *t = &table[i];
    if (family != CHIP_FAMILY_ANY && t->family != family) continue;
    if (t->id_mfr != mfr || t->id_dev != dev) continue;
    bool has_dev2 = t->id_flags & NAND_PROFILE_ID_HAS_DEV2;
    if (has_dev2 && t->id_dev2 != dev2) continue;
    any = t; n_any++;
    if (has_dev2) { by_dev2 = t; n_dev2++; }
  }
  if (n_any == 0) { if (err) *err = NAND_PRF_E_UNKNOWN_ID; return NULL; }
  if (n_any == 1) { if (err) *err = NAND_PRF_OK; return any; }
  // Several share (mfr, dev): only one declared, matching dev2 decides.
  if (n_dev2 == 1) { if (err) *err = NAND_PRF_OK; return by_dev2; }
  if (err) *err = NAND_PRF_E_AMBIGUOUS_ID;
  return NULL;
}

const active_profile_t *nand_profile_find(const active_profile_t *table, unsigned n,
                                          uint8_t mfr, uint8_t dev, uint8_t dev2,
                                          nand_prf_err_t *err) {
  return nand_profile_find_family(table, n, CHIP_FAMILY_ANY, mfr, dev, dev2, err);
}

const active_profile_t *nand_profile_pick(const active_profile_t *table, unsigned n,
                                          uint8_t mfr, uint8_t dev, const char *name) {
  if (!name || !name[0]) return NULL;
  for (unsigned i = 0; i < n; i++)
    if (table[i].id_mfr == mfr && table[i].id_dev == dev &&
        strncmp(table[i].name, name, sizeof(table[i].name)) == 0)
      return &table[i];
  return NULL;
}

const active_profile_t *nand_profile_resident(unsigned *n) {
  *n = NAND_RESIDENT_COUNT;
  return NAND_RESIDENT;
}

void nand_profile_manual(active_profile_t *p, uint32_t page_size, uint32_t spare_size,
                         uint32_t pages_per_block, uint32_t total_blocks, uint8_t planes) {
  static const uint8_t generic2[16] = {
    NAND_ECC_OK, NAND_ECC_CORRECTED, NAND_ECC_UNCORRECTABLE, NAND_ECC_UNCORRECTABLE,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
  memset(p, 0, sizeof(*p));
  strncpy(p->name, "MANUAL", sizeof(p->name) - 1);
  p->page_size = page_size; p->spare_size = spare_size;
  p->pages_per_block = pages_per_block; p->total_blocks = total_blocks;
  // spi-nand family defaults (db/families/spi-nand.yml)
  p->op_page_read = 0x13; p->op_read_cache = 0x0B; p->op_read_cache_x4 = 0x6B;
  p->op_get_feat = 0x0F; p->op_set_feat = 0x1F;
  p->op_status_addr = 0xC0; p->op_cfg_addr = 0xB0; p->ecc_en_bit = 4;
  p->ecc_shift = 4; p->ecc_mask = 0x3;
  memcpy(p->ecc_map, generic2, sizeof(generic2));
  p->id_method = NAND_ID_METHOD_DUMMY; p->id_n_bytes = 2;
  p->read_mode = NAND_READ_SINGLE; p->planes = planes ? planes : 1;
  p->vcc_mv = 3300;
  p->bbm_off = 0; p->bbm_len = 1; p->bbm_good = 0xFF; p->bbm_pages = NAND_BBM_PAGE_FIRST;
}

void nand_profile_manual_nor(active_profile_t *p, uint32_t size_bytes) {
  // spi-nor family defaults (db/families/spi-nor.yml).
  memset(p, 0, sizeof(*p));
  strncpy(p->name, "MANUAL-NOR", sizeof(p->name) - 1);
  p->family = CHIP_FAMILY_SPI_NOR;
  p->page_size = size_bytes < 4096 ? size_bytes : 4096;
  p->spare_size = 0;
  uint32_t units = p->page_size ? size_bytes / p->page_size : 0;
  p->pages_per_block = units < 16 ? (units ? units : 1) : 16;
  p->total_blocks = size_bytes / (p->page_size * p->pages_per_block);
  p->op_read_cache = 0x03; p->op_read_cache_x4 = 0x6B;
  p->op_get_feat = 0x05; p->op_set_feat = 0x01;
  p->id_method = NAND_ID_METHOD_NONE; p->id_n_bytes = 3;
  p->read_mode = NAND_READ_SINGLE; p->planes = 1; p->vcc_mv = 3300;
  p->addr_bytes = size_bytes > (1u << 24) ? 4 : 3;
  p->addr4_mode = p->addr_bytes == 4 ? NOR_ADDR4_ENTER : NOR_ADDR4_NONE;
  p->dummy_x1 = 0; p->dummy_x4 = 8;
}

bool nand_profile_is_bbm_page(const active_profile_t *p, uint32_t page_in_block,
                              uint32_t pages_per_block) {
  return ((p->bbm_pages & NAND_BBM_PAGE_FIRST) && page_in_block == 0) ||
         ((p->bbm_pages & NAND_BBM_PAGE_SECOND) && page_in_block == 1) ||
         ((p->bbm_pages & NAND_BBM_PAGE_LAST) && page_in_block + 1 == pages_per_block);
}

bool nand_profile_marker_bad(const active_profile_t *p, const uint8_t *page,
                             uint32_t page_size, uint32_t spare_size) {
  if ((uint32_t)p->bbm_off + p->bbm_len > spare_size || spare_size >= page_size) return false;
  const uint8_t *m = page + (page_size - spare_size) + p->bbm_off;
  for (uint8_t i = 0; i < p->bbm_len; i++)
    if (m[i] != p->bbm_good) return true;
  return false;
}
