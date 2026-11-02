#include "OtaBootSwitch.h"

#include <Logging.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <spi_flash_mmap.h>
#include <string.h>

#ifdef CONFIG_APP_ROLLBACK_ENABLE
// Arduino's weak C hook runs before setup(). Defer acceptance until the
// application has mounted storage and completed a normal UI render, so a
// failure in gpio/power/display/SD setup still leaves the image PENDING_VERIFY
// (crash, reboot, or OTADATA abort) for the bootloader to roll back.
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace ota_boot {

uint32_t computeSeqCrc(uint32_t seq) {
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), kOtaSeqCrcLen);
}

bool switchTo(const esp_partition_t* dest) {
  if (!dest) return false;

  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) {
    LOG_ERR("BOOT", "otadata partition not found");
    return false;
  }
  if (otadata->size < 2 * SPI_FLASH_SEC_SIZE) {
    LOG_ERR("BOOT", "otadata too small: %u", static_cast<unsigned>(otadata->size));
    return false;
  }

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata read failed");
    return false;
  }

  // Pick the slot with valid CRC and highest seq, ignoring INVALID/ABORTED.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != computeSeqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == kOtaImgInvalid || slots[i].ota_state == kOtaImgAborted) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }
  LOG_INF("BOOT", "otadata: active slot=%d seq=%u", activeIdx, static_cast<unsigned>(activeSeq));

  // ota_seq encoding: (seq - 1) % NUM_OTA_PARTITIONS picks the partition.
  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 15) {
    LOG_ERR("BOOT", "dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }

  // Find smallest seq > activeSeq such that (seq-1) % 2 == destOtaIdx,
  // assuming 2 OTA partitions (matches our partitions.csv with ota_0 + ota_1).
  uint32_t newSeq = activeSeq + 1;
  while (((newSeq - 1u) % 2u) != (destOtaIdx % 2u)) ++newSeq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = kOtaImgNew;
  next.crc = computeSeqCrc(next.ota_seq);

  // Write to the OTHER slot (so the bootloader sees a higher seq there).
  const int targetSlot = (activeIdx == 0) ? 1 : 0;
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;

  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
    LOG_ERR("BOOT", "otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) {
    LOG_ERR("BOOT", "otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  LOG_INF("BOOT", "otadata: wrote slot=%d seq=%u crc=0x%08x -> %s", targetSlot, static_cast<unsigned>(newSeq),
          static_cast<unsigned>(next.crc), dest->label);
  return true;
}

bool switchToOther() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = running ? esp_ota_get_next_update_partition(running) : nullptr;
  if (!other) {
    LOG_ERR("BOOT", "no alternate OTA app partition");
    return false;
  }

  // Raw otadata switching intentionally bypasses esp_image_verify for patched
  // Xteink images, so at least reject an erased/non-app alternate slot here.
  uint8_t magic = 0;
  if (esp_partition_read(other, 0, &magic, sizeof(magic)) != ESP_OK || magic != 0xE9) {
    LOG_ERR("BOOT", "alternate slot %s has no app image", other->label);
    return false;
  }

  LOG_INF("BOOT", "switching from %s to %s", running->label, other->label);
  return switchTo(other);
}

void acceptPendingUpdate() {
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  // Ordinary RAM latch, not RTC: it only collapses repeated render calls in
  // this boot into a single otadata write. Every new boot must prove health
  // again before the image is accepted.
  static bool attempted = false;
  if (attempted) return;
  attempted = true;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }

  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    LOG_INF("BOOT", "OTA app accepted after healthy UI render (%s)", running->label);
  } else {
    LOG_ERR("BOOT", "OTA app acceptance failed: %d", static_cast<int>(result));
  }
#endif
}

}  // namespace ota_boot
