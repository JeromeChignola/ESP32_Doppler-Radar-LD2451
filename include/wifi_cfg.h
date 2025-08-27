
#pragma once
#include <Arduino.h>

namespace WifiCfg {
  struct Creds {
    String ssid;
    String pass;
  };

  // Load saved credentials from NVS (Preferences). Empty strings if not set.
  Creds load();

  // Save credentials (ssid may be empty to clear). Returns true on success.
  bool save(const String& ssid, const String& pass);

  // Clear saved credentials.
  void clear();
}
