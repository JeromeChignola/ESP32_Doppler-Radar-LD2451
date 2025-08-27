
#include "wifi_cfg.h"
#include <Preferences.h>

namespace WifiCfg {
  static const char* NS = "radar";
  static const char* KEY_SSID = "wifi_ssid";
  static const char* KEY_PASS = "wifi_pass";

  Creds load(){
    Preferences p;
    Creds c;
    // NOT_FOUND guard: open in RW so the namespace is created on first use
    if(p.begin(NS, false)){
      c.ssid = p.getString(KEY_SSID, "");
      c.pass = p.getString(KEY_PASS, "");
      p.end();
    }
    return c;
  }

  bool save(const String& ssid, const String& pass){
    Preferences p;
    bool ok = false;
    if(p.begin(NS, false)){
      if (ssid.length() == 0) {
        p.remove(KEY_SSID);
      } else {
        p.putString(KEY_SSID, ssid);
      }
      // Note: allow empty password (open Wi-Fi) -> we store empty string
      p.putString(KEY_PASS, pass);
      p.end();
      ok = true;
    }
    return ok;
  }

  void clear(){
    Preferences p;
    if(p.begin(NS, false)){
      p.remove(KEY_SSID);
      p.remove(KEY_PASS);
      p.end();
    }
  }
}
