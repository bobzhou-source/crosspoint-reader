#pragma once
#include <cstdint>
inline int esp_efuse_mac_get_default(uint8_t* value) {
  for (int i = 0; i < 6; ++i) value[i] = i + 1;
  return 0;
}
