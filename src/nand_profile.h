#ifndef NAND_PROFILE_H
#define NAND_PROFILE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The flat chip profile the firmware runs against (vendor-profile design,
// docs/superpowers/specs/2026-08-23-vendor-profile-architecture-design.md § 4).
//
// The host flattens family -> profile -> chip into this one struct
// (tools/chipdb.py); the device never resolves layers. Resident chips are
// compiled in from db/ by tools/gen_profiles.py; a pushed profile (stage 3)
// arrives as the same bytes inside a 'PRF' blob (src/nand_session.h). Pure and Arduino-free, so the
// native tests link it.
//
// Schema v2 adds a `family` byte: the same struct describes a SPI NAND or a
// SPI NOR chip (docs/superpowers/specs/2026-10-05-spi-nor-design.md). For
// NOR, page_size is the dump frame (one 4 KiB read unit), spare_size is 0, the
// NAND-only fields (page read, feature addresses, ECC, BBM, OOB) are zero, and
// the tail carries the address width, dummy cycles and quad-enable location.

typedef enum { NAND_READ_SINGLE = 0, NAND_READ_QUAD = 1 } nand_read_mode_t;

// ECC severity, as stored in ecc_map. Order matters: higher is worse.
typedef enum {
  NAND_ECC_OK = 0,
  NAND_ECC_CORRECTED = 1,
  NAND_ECC_CORRECTED_REFRESH = 2,
  NAND_ECC_UNCORRECTABLE = 3,
} nand_ecc_sev_t;

#define NAND_PROFILE_SCHEMA_VER  2
#define NAND_PROFILE_ID_HAS_DEV2 0x01   // id_flags: id_dev2 is meaningful
#define NAND_ID_METHOD_ADDR      0
#define NAND_ID_METHOD_DUMMY     1
#define NAND_ID_METHOD_NONE      2      // 9Fh, then data: SPI NOR's plain JEDEC read

typedef enum { CHIP_FAMILY_SPI_NAND = 0, CHIP_FAMILY_SPI_NOR = 1 } chip_family_t;
#define CHIP_FAMILY_ANY 0xFF            // nand_profile_find_family: no filter

// addr4_mode: how a SPI NOR part above 16 MiB is addressed.
#define NOR_ADDR4_NONE       0          // 3-byte part
#define NOR_ADDR4_NATIVE     1          // 4-byte read opcodes (13h/6Ch), no mode change
#define NOR_ADDR4_ENTER      2          // B7h enters 4-byte mode (volatile)
#define NOR_ADDR4_ENTER_WREN 3          // WREN, then B7h
#define NOR_QER_MAX          6          // JESD216 quad-enable requirement codes 0..6
#define NOR_MAX_DUMMY        32
#define NAND_BBM_PAGE_FIRST      0x01   // bbm_pages bits
#define NAND_BBM_PAGE_SECOND     0x02
#define NAND_BBM_PAGE_LAST       0x04

// Field order, sizes and padding == tools/chipdb.py LAYOUT (little-endian).
// Every field is naturally aligned, so no packing attribute is needed; the
// static_asserts below and the golden-blob native test pin it.
typedef struct {
  char     name[24];                  // NUL-terminated, strlen <= 23
  uint8_t  id_mfr, id_dev, id_dev2, id_flags;
  uint32_t page_size;                 // main + spare
  uint32_t spare_size;
  uint32_t pages_per_block;
  uint32_t total_blocks;
  uint8_t  op_page_read, op_read_cache, op_read_cache_x4;
  uint8_t  op_get_feat, op_set_feat, op_status_addr, op_cfg_addr;
  uint8_t  ecc_en_bit;                // bit in the config register that enables on-die ECC
  uint8_t  ecc_shift, ecc_mask;       // status field = (status >> shift) & mask
  uint8_t  ecc_map[16];               // field value -> nand_ecc_sev_t
  uint8_t  status2_reg;               // 0 = none (exact-count read deferred)
  uint8_t  id_method, id_n_bytes;
  uint8_t  qe_addr, qe_bit;           // qe_addr 0 = no quad-enable bit
  uint8_t  read_mode;                 // nand_read_mode_t default
  uint8_t  planes;                    // 1, 2 or 4
  uint8_t  family;                    // chip_family_t (schema v2; v1 padding)
  uint16_t vcc_mv;
  uint8_t  bbm_off, bbm_len, bbm_good;
  uint8_t  oob_free_n, oob_ecc_n;
  uint8_t  bbm_pages;                 // NAND_BBM_PAGE_* bits
  uint16_t oob_free[8];               // up to 4 x (offset, length) in the spare
  uint16_t oob_ecc[8];
  // v2 tail: SPI NOR only, zero for SPI NAND.
  uint8_t  addr_bytes;                // 3 or 4
  uint8_t  addr4_mode;                // NOR_ADDR4_*
  uint8_t  dummy_x1, dummy_x4;        // dummy cycles after the address, x1 / x4 read
  uint8_t  qer;                       // JESD216 quad-enable requirement (0 = no QE bit)
  uint8_t  _pad1[5];
} active_profile_t;

#define NAND_PROFILE_SIZE 128
static_assert(sizeof(active_profile_t) == NAND_PROFILE_SIZE,
              "active_profile_t drifted from tools/chipdb.py LAYOUT");
static_assert(offsetof(active_profile_t, page_size) == 28, "layout drift");
static_assert(offsetof(active_profile_t, ecc_map) == 54, "layout drift");
static_assert(offsetof(active_profile_t, vcc_mv) == 78, "layout drift");
static_assert(offsetof(active_profile_t, oob_free) == 86, "layout drift");
static_assert(offsetof(active_profile_t, family) == 77, "layout drift");
static_assert(offsetof(active_profile_t, addr_bytes) == 118, "layout drift");

// Fail-closed error codes (design § 6).
typedef enum {
  NAND_PRF_OK = 0,
  NAND_PRF_E_BAD_MAGIC,
  NAND_PRF_E_SCHEMA_VER,
  NAND_PRF_E_BAD_LEN,
  NAND_PRF_E_BAD_CRC,
  NAND_PRF_E_STRUCTURE,       // tier 2: a field is out of its encodable range
  NAND_PRF_E_GEOMETRY,        // tier 3: physically impossible geometry
  NAND_PRF_E_PAGE_TOO_BIG,    // tier 3: page_size > the firmware's page buffer
  NAND_PRF_E_ID_MISMATCH,
  NAND_PRF_E_AMBIGUOUS_ID,
  NAND_PRF_E_UNKNOWN_ID,
  // Push session (stage 3, src/nand_session.h). Values travel on the wire, so
  // new codes are only ever appended.
  NAND_PRF_E_NOT_STAGED,      // ARM with no verified profile waiting
  NAND_PRF_E_ARM_CRC,         // ARM named a different blob than the one staged
  NAND_PRF_E_NOT_ARMED,       // GO while a pushed profile is still unarmed
  NAND_PRF_E_BAD_CMD,         // unknown session command byte
  NAND_PRF_E_TIMEOUT,         // the link went quiet mid-command
} nand_prf_err_t;

const char *nand_prf_err_name(nand_prf_err_t e);

// Blob framing: 'PRF' | schema_ver u8 | len u16 LE | struct | crc32 LE over all before.
#define NAND_PROFILE_BLOB_SIZE (3 + 1 + 2 + NAND_PROFILE_SIZE + 4)

// ECC decode, pure data: ecc_map[(status >> shift) & mask]. The index is also
// clamped to 4 bits so a corrupt mask can never read past ecc_map.
static inline nand_ecc_sev_t nand_profile_ecc_severity(const active_profile_t *p,
                                                       uint8_t status) {
  return (nand_ecc_sev_t)p->ecc_map[((status >> p->ecc_shift) & p->ecc_mask) & 0x0F];
}

// Tier 2 (structure) and tier 3 (physical sanity) checks. The device runs them
// on every resident entry at boot and on every pushed profile.
nand_prf_err_t nand_profile_check(const active_profile_t *p, uint32_t max_page_size);

// Tier 1 (framing) then tiers 2-3. `out` is written only on NAND_PRF_OK, so a
// bad blob never partially replaces a good profile.
nand_prf_err_t nand_profile_unpack(const uint8_t *blob, size_t len,
                                   uint32_t max_page_size, active_profile_t *out);

// The detected ID must match the profile's expected ID (design § 6).
bool nand_profile_id_matches(const active_profile_t *p, uint8_t mfr, uint8_t dev,
                             uint8_t dev2);

// Resolve a detected JEDEC ID in `table` (design § 5): candidates by
// (mfr, dev); an entry that declares dev2 is a candidate only if dev2 matches.
// If several remain, the dev2-declaring ones win. Returns the single match, or
// NULL with *err = NAND_PRF_E_UNKNOWN_ID / NAND_PRF_E_AMBIGUOUS_ID. Never guesses.
const active_profile_t *nand_profile_find(const active_profile_t *table, unsigned n,
                                          uint8_t mfr, uint8_t dev, uint8_t dev2,
                                          nand_prf_err_t *err);
// The same, limited to one chip_family_t (or CHIP_FAMILY_ANY).
const active_profile_t *nand_profile_find_family(const active_profile_t *table, unsigned n,
                                                 uint8_t family, uint8_t mfr, uint8_t dev,
                                                 uint8_t dev2, nand_prf_err_t *err);

// The design § 5 tiebreak for a standalone device: the entry called `name`
// (the user's saved choice), but only if it really carries the detected
// (mfr, dev). A choice saved for another chip, or one no longer in the table
// after a reflash, returns NULL and the ID stays ambiguous.
const active_profile_t *nand_profile_pick(const active_profile_t *table, unsigned n,
                                          uint8_t mfr, uint8_t dev, const char *name);

// The resident table compiled from db/ (src/nand_profiles_generated.h).
const active_profile_t *nand_profile_resident(unsigned *n);

// A profile for a chip that is not in the table, geometry entered by hand:
// spi-nand family opcodes, generic 2-bit ECC decode, 1-byte BBM at spare[0].
void nand_profile_manual(active_profile_t *p, uint32_t page_size, uint32_t spare_size,
                         uint32_t pages_per_block, uint32_t total_blocks, uint8_t planes);

// A SPI NOR profile for a chip that is not in the table: plain 03h read (no
// dummy, any clock the part allows), 6Bh for quad, 4-byte addressing through
// B7h above 16 MiB. size_bytes must be a power of two >= 4 KiB.
void nand_profile_manual_nor(active_profile_t *p, uint32_t size_bytes);

// Total bytes the profile covers (page_size * pages_per_block * total_blocks).
static inline uint32_t nand_profile_bytes(const active_profile_t *p) {
  return p->page_size * p->pages_per_block * p->total_blocks;
}

// Does page `page_in_block` carry the factory bad-block marker?
bool nand_profile_is_bbm_page(const active_profile_t *p, uint32_t page_in_block,
                              uint32_t pages_per_block);

// Is the bad-block marker in this raw page (main + spare) set? Any marker byte
// that differs from bbm_good marks the block bad (as Linux spinand does).
bool nand_profile_marker_bad(const active_profile_t *p, const uint8_t *page,
                             uint32_t page_size, uint32_t spare_size);

#endif // NAND_PROFILE_H
