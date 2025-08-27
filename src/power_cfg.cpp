
#include "power_cfg.h"
#include <Preferences.h>

namespace PowerCfg {
  static const char* NS    = "radar";
  static const char* K_CPU = "cpu_mhz";
  static const char* K_MDN = "mdns_en";
  static const char* K_WSL = "wifi_slp";
  static const char* K_PGN = "slp_gpio";
  static const char* K_PGA = "slp_gpio_ah";
  static const char* K_SLM = "sleep_mode";

  Settings load(){
    Preferences p;
    Settings s;
    if (p.begin(NS, false)){
      s.cpu_mhz = p.getUShort(K_CPU, 240);
      s.mdns    = p.getBool(K_MDN, true);
      s.wifi_sleep = p.getBool(K_WSL, true);
      s.sleep_gpio = (int8_t)p.getChar(K_PGN, -1);
      s.sleep_gpio_active_high = p.getBool(K_PGA, true);
      s.sleep_mode = (uint8_t)p.getUChar(K_SLM, 1);
      p.end();
    }
    if (s.cpu_mhz!=80 && s.cpu_mhz!=160 && s.cpu_mhz!=240) s.cpu_mhz = 240;
    return s;
  }

  bool save(const Settings& s){
    Preferences p;
    bool ok=false;
    if (p.begin(NS, false)){
      p.putUShort(K_CPU, (uint16_t)((s.cpu_mhz==80||s.cpu_mhz==160||s.cpu_mhz==240)? s.cpu_mhz:240));
      p.putBool(K_MDN, s.mdns);
      p.putBool(K_WSL, s.wifi_sleep);
      p.putChar(K_PGN, (char)s.sleep_gpio);
      p.putBool(K_PGA, s.sleep_gpio_active_high);
      p.putUChar(K_SLM, (unsigned char)(s.sleep_mode<=3 ? s.sleep_mode : 1));
      p.end();
      ok = true;
    }
    return ok;
  }

  void clear(){
    Preferences p;
    if (p.begin(NS, false)){
      p.remove(K_CPU); p.remove(K_MDN); p.remove(K_WSL);
      p.remove(K_PGN); p.remove(K_PGA);
      p.end();
    }
  }
}
