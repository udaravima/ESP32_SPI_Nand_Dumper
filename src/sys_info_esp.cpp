// ESP-only runtime capability queries + reporting. Uses only core APIs that
// exist across every modern ESP32 variant (ESP32/S2/S3/C3/C6), so the same
// firmware self-describes and self-sizes on any of them without board edits.
#include "sys_info.h"
#include <Arduino.h>
#include "esp_heap_caps.h"

size_t sys_free_dma_bytes(void) {
  return heap_caps_get_free_size(MALLOC_CAP_DMA);
}

void sys_info_report(void) {
  Serial.println("[*] Board capabilities (runtime-detected):");
  Serial.printf("    Chip: %s rev%d | %d core(s) @ %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), getCpuFrequencyMhz());
  Serial.printf("    Heap free: %u KB | DMA-capable free: %u KB\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(sys_free_dma_bytes() / 1024));
  size_t psram = ESP.getPsramSize();
  if (psram)
    Serial.printf("    PSRAM: %u KB (free %u KB)\n",
                  (unsigned)(psram / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  else
    Serial.println("    PSRAM: none");
  Serial.printf("    Flash: %u MB\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
}
