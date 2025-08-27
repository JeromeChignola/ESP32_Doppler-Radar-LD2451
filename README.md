# HLK-LD2451 — ESP32 Web + Passages + Config (pages séparées)

**Fonctions :**
- Page `/` : liste des passages + stats (rafraîchissement périodique).
- Page `/config` : configuration radar **sans rafraîchissement** (saisie confortable).
- CSV exportable (`/csv`), stockage persistant, presets, endpoints d’API.

## Câblage
- Radar **TXD → ESP32 RX2 (GPIO16)**
- Radar **RXD ← ESP32 TX2 (GPIO17)**
- **GND commun**
- Radar alimenté en **5 V**. Abaisser TXD(5V)→RX2(3V3) (diviseur ou level shifter).

## Wi‑Fi
Modifiez dans `src/main.cpp` :
```cpp
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"
```

## URLs utiles
- `http://<ip>/` — Passages & statistiques
- `http://<ip>/config` — Configuration radar (pas d’auto-refresh)
- `http://<ip>/csv` — Export CSV
- `http://<ip>/api/diag/ping` — Test ACK (Enable → ReadVersion → End)

## Notes
- Le protocole série fourni ne documente pas l’activation/désactivation du **BLE** via UART.
- LittleFS monte automatiquement la partition `spiffs` si la partition `littlefs` n’existe pas.
