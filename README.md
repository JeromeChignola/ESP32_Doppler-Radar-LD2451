# RADAR HLK‑LD2451 – Firmware ESP32 (V2.0+)

Projet **ESP32 + HLK‑LD2451** avec interface Web, configuration Wi‑Fi, MQTT (Home Assistant), et contrôles d’économie d’énergie (CPU/mDNS/sleep + override GPIO).  
Cible: **ESP32 Dev Module** (Arduino core 2.0.x, PlatformIO).

---

## ✨ Fonctionnalités

- **UI Web embarquée** (PROGMEM) : pages *Statut* et *Configuration*.
- **Wi‑Fi paramétrable** depuis l’UI (SSID/MdP), **persistant NVS**, **reboot auto** après sauvegarde.
- **MQTT configurable** via UI (broker, port, user/pass, base topic, découverte HA on/off).
- **Publication MQTT** :
  - `base/status` → `online` / `offline` (retain, LWT)
  - `base/count` → nombre de passages (retain)
  - `base/last` → dernier passage (JSON : `ts`, `dir` 0/1, `speed_kmh`, `dist_m`, `angle`, `snr`) (retain)
  - *(optionnel)* **HA Discovery** : capteurs vitesse / distance / angle / compteur
- **Endpoint test MQTT** : `GET /api/mqtt/test` (pousse un jeu de valeurs pour validation côté broker).
- **Contrôles Alimentation & Système** dans l’UI :
  - **CPU** : 80 / 160 / 240 MHz
  - **mDNS** : activable/désactivable
  - **Wi‑Fi sleep** (modem‑sleep) on/off
  - **Override via GPIO** : possibilité de **désactiver le sleep** par entrée externe (niveau configurable)
  - **Auto‑règle** : si le **LD2451 n’est pas détecté**, le **sleep est désactivé** automatiquement
- **Fallback Wi‑Fi** : au boot, essaie d’abord les creds **NVS**, sinon **config.h**, sinon **AP**.
- **Persistance** : Wi‑Fi, MQTT et Power sont stockés en **NVS/Preferences**.
- **Journal série** détaillé (diag MQTT, mDNS, réseau).

---

## 🧱 Arborescence

```
.
├─ platformio.ini
├─ include/
│  ├─ config.h        # SSID/MdP par défaut (fallback)
│  ├─ web_ui.h        # Déclarations des pages HTML (PROGMEM)
│  ├─ wifi_cfg.h      # Accès NVS aux creds Wi‑Fi
│  ├─ mqtt_cfg.h      # Accès NVS aux réglages MQTT
│  └─ power_cfg.h     # Accès NVS aux réglages CPU/mDNS/sleep/GPIO
├─ src/
│  ├─ main.cpp        # App principale + API HTTP + logique MQTT + power policy
│  ├─ web_ui.cpp      # Contenu HTML des pages (INDEX/CONFIG) en PROGMEM
│  ├─ wifi_cfg.cpp    # Implémentation NVS Wi‑Fi
│  ├─ mqtt_cfg.cpp    # Implémentation NVS MQTT
│  └─ power_cfg.cpp   # Implémentation NVS Power
└─ data/              # (optionnel) ressources statiques
```

---

## ⚙️ Build & Flash (PlatformIO)

- Environnement : `esp32dev`, Arduino core 2.0.11  
- Série : **115200 bauds**

`platformio.ini` (extrait) :
```ini
[env:esp32dev]
platform = espressif32 @ 6.4.0
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps = knolleary/PubSubClient @ ^2.8
```

**Étapes** :
1. Ouvre le dossier dans VS Code + PlatformIO.
2. (Optionnel) Édite `include/config.h` (SSID/MdP de secours).
3. Compile & flash : **PlatformIO: Upload**.
4. Ouvre le moniteur série pour l’IP/diagnostic.

---

## 🌐 Interface Web

- Page **/config** :
  - **Wi‑Fi (STA)** : SSID, MdP (laisse vide pour conserver), *Sauver & redémarrer*.
  - **MQTT (Home Assistant)** : host, port, user, pass (vide = inchangé), base topic, *découverte HA*.
  - **Alimentation & Système** :
    - CPU : 80/160/240 MHz
    - mDNS : on/off
    - Wi‑Fi sleep : on/off
    - GPIO override (n° et “actif niveau haut/bas”)
    - *Note : si LD2451 non détecté → sleep OFF automatiquement*

- Endpoints utiles :
  - `GET /api/wifi/get` / `GET /api/wifi/set?ssid=...&pass=...`
  - `GET /api/mqtt/get` / `GET /api/mqtt/set?...` / `GET /api/mqtt/test`
  - `GET /api/power/get` / `GET /api/power/set?...`
  - `GET /api/reboot`

> Après un `*Set`, l’ESP redémarre **automatiquement** (confirmation UI).

---

## 📡 MQTT – Topics & HA

### Topics publiés
- `radar/<base>/status` : `online` / `offline` *(retain, LWT)*
- `radar/<base>/count`  : entier *(retain)*
- `radar/<base>/last`   : JSON *(retain)*
  ```json
  {
    "ts": "2024-05-12 18:02:41",
    "dir": 1,
    "speed_kmh": 42.0,
    "dist_m": 12.3,
    "angle": 5,
    "snr": 9
  }
  ```

> `<base>` = valeur de **Base topic** dans l’UI (ex: `ld2451` ⇒ `radar/ld2451/...`).  
> Si vide, un identifiant basé sur l’EFuse MAC est utilisé.

### Intégration Home Assistant

**Auto‑découverte** (optionnel) : coche “Découverte HA” → le firmware publie les *config topics* nécessaires et l’appareil apparaît automatiquement dans HA (capteurs: `speed`, `distance`, `angle`, `count`).

**Configuration manuelle** (exemple YAML) :
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

> Pour afficher le **sens de passage** sans changer le firmware, ajoute un capteur texte :
```yaml
  - name: "Radar Direction"
    state_topic: "radar/ld2451/last"
    value_template: >-
      {% if value_json.dir|int == 1 %}Approche{% else %}Éloignement{% endif %}
    availability:
      - topic: "radar/ld2451/status"
        payload_available: "online"
        payload_not_available: "offline"
```

---

## 🔋 Énergie & autonomie

- **Mode STA‑only** (AP coupé), **modem‑sleep** activé si possible.
- **CPU 80 MHz** recommandé sur batterie (si performance OK).
- **mDNS** désactivable (petit gain).
- **Override GPIO** pour forcer le *performance mode* (sleep OFF) à la demande.
- Si le **LD2451** n’est pas détecté, le firmware **coupe le sleep** automatiquement (fiabilité).

Estimation avec **power‑bank 20 000 mAh** (≈60 Wh utiles) :
- Radar 50–70 mA + ESP optimisé 10–25 mA ⇒ **5–7 jours** typiques (selon modèle LD2451).
- Radar 90 mA + ESP optimisé ⇒ ~**4,5–5 jours**.

---

## 🧪 Dépannage rapide

- **Réseau/MQTT** : regarde les logs série
  - `[MQTT] connect to host:port user=...` / `[MQTT] connected` / `state=X`
  - `publish fail topic=... len=...` → augmente `setBufferSize()` (déjà 1024 par défaut ici)
- **Rien dans HA** :
  - Vérifie la **base topic** (la même que dans HA).
  - Teste `GET /api/mqtt/test` pour publier un échantillon.
  - Si découverte HA activée : écouter `homeassistant/#` pour voir les *config topics*.

---

## 🔐 Sécurité

- Les mots de passe ne sont jamais renvoyés par les APIs `GET /.../get`.
- En **HTTP clair** sur réseau local : évite Internet/port‑forwarding sans protection.

---

## 🗺️ Roadmap (idées)

- OTA Web (upload binaire depuis l’UI).
- Réglage **TX Power Wi‑Fi** depuis l’UI.
- Export/Import `config.json`.
- Capteurs/commandes supplémentaires côté MQTT (snr, dir texte, enable/disable radar, etc.).

---

## 📜 Licence

Projet DIY — usage personnel & expérimental. Vérifie la conformité locale avant tout usage “terrain”.

