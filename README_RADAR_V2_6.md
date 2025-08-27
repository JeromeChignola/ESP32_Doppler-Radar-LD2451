# RADAR HLK‑LD2451 – Firmware ESP32 (V2.6)

Projet **ESP32 + HLK‑LD2451** avec interface Web, configuration Wi‑Fi, **MQTT (Home Assistant)** et **gestion avancée de l’énergie** :  
- Mode 1 **Modem sleep** (Wi‑Fi associé, PS MIN_MODEM)  
- Mode 2 **Light sleep** (siestes courtes CPU, Wi‑Fi associé)  
- Mode 3 **Wi‑Fi OFF quand idle** (coupure radio + serveur web après inactivité)

> Cible : **ESP32 Dev Module – Arduino core 2.0.x – PlatformIO**

---

## ✨ Points clés

- **UI Web embarquée** (pages *Statut* et *Configuration*).
- **Wi‑Fi STA** éditable depuis l’UI (persisté **NVS**), **reboot auto** après sauvegarde.
- **MQTT configurable** via UI (broker/port/user/pass/base topic) + **HA Discovery** optionnel.
- **Publications MQTT** : `status` (LWT), `count` (retain), `last` (JSON du dernier passage, retain).
- **Énergie** (UI → *Alimentation & Système*):
  - **CPU** : 80 / 160 / 240 MHz
  - **mDNS** : on/off
  - **Wi‑Fi sleep** : on/off
  - **Mode de veille (`slm`)** : 1=Modem, 2=Light, **3=Wi‑Fi OFF quand idle**
  - **Override GPIO** (permanent) pour **désactiver la veille**
  - **Sécurité** : si le **LD2451 n’est pas détecté**, la veille est **désactivée** (fiabilité)
- **Diag runtime** : endpoint `/api/power/diag` (JSON) pour vérifier CPU/mDNS/sleep/mode/GPIO/LD2451.

---

## 🧱 Arborescence (principaux fichiers)

```
.
├─ platformio.ini
├─ include/
│  ├─ config.h        # SSID/MdP par défaut (fallback)
│  ├─ web_ui.h        # Déclarations pages HTML
│  ├─ wifi_cfg.h      # NVS Wi‑Fi
│  ├─ mqtt_cfg.h      # NVS MQTT
│  └─ power_cfg.h     # NVS Power (CPU/mDNS/sleep/GPIO/Mode)
├─ src/
│  ├─ main.cpp        # App principale + API HTTP + MQTT + modes d’énergie
│  ├─ web_ui.cpp      # Contenus HTML (PROGMEM) + JS
│  ├─ wifi_cfg.cpp    # Implémentation NVS Wi‑Fi
│  ├─ mqtt_cfg.cpp    # Implémentation NVS MQTT
│  └─ power_cfg.cpp   # Implémentation NVS Power
└─ data/              # (optionnel) ressources statiques
```

---

## ⚙️ Build & Flash (PlatformIO)

- Environnement : `esp32dev`, Arduino core 2.0.11
- Dépendances : `knolleary/PubSubClient @ ^2.8`
- Moniteur série : **115200 bauds**

**Étapes**  
1. Ouvrir le dossier avec VS Code + PlatformIO.  
2. (Optionnel) Modifier `include/config.h` (SSID/MdP de secours).  
3. `Upload` pour compiler/flasher.  
4. Ouvrir le moniteur série pour l’IP/logs.  

---

## 🌐 UI Web – Configuration

### Wi‑Fi (STA)
- **SSID** / **Password** → Sauver & redémarrer (stockage NVS).  
- Fallback : NVS → `config.h` → AP (si échec).

### MQTT
- **Enabled**, **Host**, **Port**, **User**, **Pass**, **Base topic** (ex. `radar/ld2451`), **HA Discovery** on/off.  
- Endpoint de test : `GET /api/mqtt/test` publie un échantillon (`status`, `count`, `last`).

### Alimentation & Système
- **CPU** : 80 / 160 / 240 MHz  
- **mDNS** : on/off  
- **Wi‑Fi sleep** : on/off  
- **Mode de veille (`slm`)** :  
  - **1 – Modem sleep** : PS **MIN_MODEM**, l’UI reste accessible.  
  - **2 – Light sleep** : siestes CPU ~150 ms, Wi‑Fi associé ; l’UI reste accessible mais un peu plus lente.  
  - **3 – Wi‑Fi OFF quand idle** : coupe totalement Wi‑Fi + serveur web après **inactivité** ; se rallume automatiquement pendant une **fenêtre** après un **événement** (passage) ou si l’**override GPIO** est actif.
- **Override GPIO** : numéro de GPIO (ou `-1` pour désactiver) + **actif niveau haut/bas**.  
  **Important** : override **permanent** → si actif, **pas de veille** (quel que soit le mode).  
- **Règle LD2451** : tant que le radar n’a pas enregistré un premier **passage** (`ld2451_ok=false`), la veille est **bloquée**.

> **Fenêtres Mode 3** (valeurs build par défaut) :  
> **IDLE_OFF** = 60 s (coupe après 60 s sans activité) – **KEEP_ON** = 20 s (garde ON 20 s après un passage).

---

## 📡 MQTT – Topics & Home Assistant

### Topics
- `radar/<base>/status` : `online` / `offline` (retain, LWT)
- `radar/<base>/count` : entier (retain)
- `radar/<base>/last` : JSON du dernier passage (retain) :
  ```json
  {
    "ts": "2025-08-28 19:02:41",
    "dir": 1,
    "speed_kmh": 42.0,
    "dist_m": 12.3,
    "angle": 5,
    "snr": 9
  }
  ```
> `<base>` = **Base topic** (UI). Si vide, fallback sur un identifiant dérivé du MAC.

### Home Assistant
- **Découverte auto** (optionnelle) : capteurs *speed*, *distance*, *angle*, *count* créés automatiquement.
- **YAML manuel** (ex.) :
  ```yaml
  sensor:
    - name: "Radar Speed"
      state_topic: "radar/ld2451/last"
      unit_of_measurement: "km/h"
      value_template: "{{ value_json.speed_kmh }}"
      availability:
        - topic: "radar/ld2451/status"
          payload_available: "online"
          payload_not_available: "offline"

    - name: "Radar Distance"
      state_topic: "radar/ld2451/last"
      unit_of_measurement: "m"
      value_template: "{{ value_json.dist_m }}"
      availability:
        - topic: "radar/ld2451/status"
          payload_available: "online"
          payload_not_available: "offline"

    - name: "Radar Angle"
      state_topic: "radar/ld2451/last"
      unit_of_measurement: "°"
      value_template: "{{ value_json.angle }}"
      availability:
        - topic: "radar/ld2451/status"
          payload_available: "online"
          payload_not_available: "offline"

    - name: "Radar Passes"
      state_topic: "radar/ld2451/count"
      availability:
        - topic: "radar/ld2451/status"
          payload_available: "online"
          payload_not_available: "offline"
  ```

---

## 🔋 Énergie & autonomie (résumé)

- **Modem sleep (1)** : Wi‑Fi associé, UI accessible, gain modéré.  
- **Light sleep (2)** : CPU doze, UI associée mais un peu plus lente, gain supplémentaire.  
- **Wi‑Fi OFF quand idle (3)** : plus gros gain ; **UI indisponible** quand le Wi‑Fi est coupé, se rallume **KEEP_ON** s après un passage.  
- **Override GPIO** actif ⇒ **pas de veille**.  
- **LD2451 non détecté** ⇒ veille **désactivée**.

> Rappel estimations (3S4P 18650 ~108 Wh utiles, CPU 80 MHz, LD2451 ~60–70 mA) :  
> **Très calme (duty ~5,6%)** ~16 j · **Modéré (33%)** ~13 j · **Soutenu (67%)** ~11 j.  
> Le **duty** dépend surtout de **KEEP_ON** (20 s par passage par défaut) et du trafic réel.

---

## 🧰 API HTTP (extraits)

- **Wi‑Fi** :  
  - `GET /api/wifi/get`  
  - `GET /api/wifi/set?ssid=...&pass=...` *(pass vide = inchangé, reboot auto)*
- **MQTT** :  
  - `GET /api/mqtt/get`  
  - `GET /api/mqtt/set?enabled=0|1&host=...&port=1883&user=...&pass=...&base=...&disc=0|1` *(reboot auto)*  
  - `GET /api/mqtt/test`
- **Power** :  
  - `GET /api/power/get` → `{"cpu_mhz":..., "mdns":..., "wifi_sleep":..., "sleep_gpio":..., "sleep_gpio_ah":..., "sleep_mode":1|2|3}`  
  - `GET /api/power/set?cpu=80|160|240&mdns=0|1&wsl=0|1&gpio=-1|xx&gah=0|1&slm=1|2|3` *(reboot auto)*  
  - `GET /api/power/diag` → ex.  
    ```json
    {
      "cpu_cfg": 80,
      "cpu_cur": 80,
      "mdns_cfg": false,
      "wifi_sleep_cfg": true,
      "wifi_sleep_rt": true,
      "wifi_ps": 1,
      "sleep_mode": 1,
      "ld2451_ok": true,
      "gpio": -1,
      "gpio_lvl": -1,
      "gpio_active": false,
      "wifi_off": false
    }
    ```

---

## 🧪 Dépannage rapide

- **mDNS OFF mais log indique `.local`** : vérifier que le start mDNS est bien sous `if (g_pw.mdns) ... else { "[mDNS] disabled" }`.
- **Rien dans MQTT** : vérifier Host/Port/User/Pass, **Base topic**, `homeassistant/#` (si discovery), tester `/api/mqtt/test`.
- **Veille non appliquée** : voir `/api/power/diag` → `ld2451_ok` doit être **true** (faire un passage) et **override GPIO** inactif.  
  - Modem sleep : `wifi_sleep_rt:true`, `wifi_ps:1`.  
  - Light sleep : `wifi_ps:0` (normal, légère latence perçue).  
  - Mode 3 : `wifi_off:true` quand idle (UI indisponible durant l’OFF).
- **LittleFS error** au boot : SPIFFS est monté, non bloquant (info).

---

## 🔐 Sécurité

- Les mots de passe **ne sont jamais renvoyés** par les APIs `.../get`.
- HTTP en clair (LAN) : éviter l’exposition Internet non protégée.

---

## 🗺️ Roadmap courte

- Durées **KEEP‑ON** / **IDLE‑OFF** configurables via l’UI (NVS).  
- Réglage **TX Power Wi‑Fi** dans l’UI.  
- OTA Web (upload binaire depuis l’UI).

---

## 📜 Licence

Projet DIY – usage personnel & expérimental. Respecter la réglementation locale.

