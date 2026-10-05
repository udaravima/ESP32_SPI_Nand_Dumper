#ifndef NOR_DRIVER_H
#define NOR_DRIVER_H
#include <stdint.h>
#include <stdbool.h>
#include "nand_profile.h"
#include "nand_seq.h"    // nand_page_result_t
#include "sfdp.h"

// SPI NOR on the same bus and device as the SPI NAND driver (nand_driver.h
// brings the bus up). Thin wrapper: the sequencing is nor_seq, the wire is
// nand_spi_xfer. Read-only, like nor_seq.

void nor_init(int max_unit_size);              // after nand_init
void nor_apply_profile(const active_profile_t *p);
void nor_release_power_down(void);
void nor_read_id(uint8_t id[3]);
bool nor_probe_sfdp(sfdp_info_t *out);
int  nor_qe_state(void);                       // 1 set, 0 clear, -1 unknown
void nor_begin(void);                          // enter 4-byte mode if the profile needs it
void nor_end(void);
void nor_read(uint32_t addr, uint8_t *buf, int len);
nand_page_result_t nor_read_verified(uint32_t addr, uint8_t *buf, int len,
                                     int max_retries, uint32_t *retry_count);
bool nor_quad_selftest(uint32_t addr, int len);
void nor_set_read_mode(nand_read_mode_t m);
nand_read_mode_t nor_get_read_mode(void);
uint32_t nor_quad_fallbacks(void);

#endif // NOR_DRIVER_H
