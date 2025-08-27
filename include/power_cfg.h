
#pragma once
#include <Arduino.h>

namespace PowerCfg {
  struct Settings {
    uint16_t cpu_mhz = 240;   // 80/160/240
    bool mdns = true;
    bool wifi_sleep = true;
    uint8_t sleep_mode = 1; // 1=modem, 2=light, 3=wifi-off-idle
    int8_t sleep_gpio = -1;   // -1 = disabled
    bool sleep_gpio_active_high = true;
  };
  Settings load();
  bool save(const Settings& s);
  void clear();
}
