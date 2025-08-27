
#pragma once
#include <Arduino.h>

namespace MqttCfg {
  struct Settings {
    bool   enabled = false;
    String host;
    uint16_t port = 1883;
    String user;
    String pass;
    String base;       // e.g., "radar/ld2451"
    bool   discovery = true;
  };

  Settings load();
  bool save(const Settings& s);
  void clear();
}
