
#include "mqtt_cfg.h"
#include <Preferences.h>

namespace MqttCfg {
  static const char* NS        = "radar";
  static const char* K_EN      = "mqtt_en";
  static const char* K_HOST    = "mqtt_host";
  static const char* K_PORT    = "mqtt_port";
  static const char* K_USER    = "mqtt_user";
  static const char* K_PASS    = "mqtt_pass";
  static const char* K_BASE    = "mqtt_base";
  static const char* K_DISC    = "mqtt_disc";

  Settings load(){
    Preferences p;
    Settings s;
    if (p.begin(NS, false)){
      s.enabled   = p.getBool(K_EN, false);
      s.host      = p.getString(K_HOST, "");
      s.port      = p.getUShort(K_PORT, 1883);
      s.user      = p.getString(K_USER, "");
      s.pass      = p.getString(K_PASS, "");
      s.base      = p.getString(K_BASE, "");
      s.discovery = p.getBool(K_DISC, true);
      p.end();
    }
    return s;
  }

  bool save(const Settings& s){
    Preferences p;
    bool ok = false;
    if (p.begin(NS, false)){
      p.putBool(K_EN, s.enabled);
      p.putString(K_HOST, s.host);
      p.putUShort(K_PORT, s.port);
      p.putString(K_USER, s.user);
      p.putString(K_PASS, s.pass);
      p.putString(K_BASE, s.base);
      p.putBool(K_DISC, s.discovery);
      p.end();
      ok = true;
    }
    return ok;
  }

  void clear(){
    Preferences p;
    if (p.begin(NS, false)){
      p.remove(K_EN); p.remove(K_HOST); p.remove(K_PORT);
      p.remove(K_USER); p.remove(K_PASS); p.remove(K_BASE);
      p.remove(K_DISC);
      p.end();
    }
  }
}
