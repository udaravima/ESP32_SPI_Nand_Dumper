#ifndef SYS_INFO_H
#define SYS_INFO_H
#include <stdint.h>
#include <stddef.h>

// Reserve this much RAM for stack/WiFi/LwIP when sizing the send buffer.
#define SYS_DMA_HEADROOM  32768   // 32 KB
#define SYS_BATCH_HARDCAP 64      // never batch more than this many pages/write

// Pure, host-testable: given the free DMA-capable bytes and one page-frame size
// (page_size + 4 CRC bytes), how many pages can we safely batch into one TCP
// write? Leaves SYS_DMA_HEADROOM free, always returns at least 1, and never
// exceeds `max_cap`. Runtime-computed so the same firmware sizes itself on any
// ESP32 variant regardless of how much RAM it has.
int sys_recommend_batch_pages(size_t free_dma_bytes, size_t frame_size, int max_cap);

// --- ESP-only (implemented in sys_info_esp.cpp; not built in the host test env) ---
void   sys_info_report(void);       // print chip model, cores, clock, heap, PSRAM, flash
size_t sys_free_dma_bytes(void);     // heap_caps_get_free_size(MALLOC_CAP_DMA)

#endif // SYS_INFO_H
