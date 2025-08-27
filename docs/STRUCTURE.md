# RADAR V2.0 — Code structuré

Ce ré-agencement isole :

- `include/config.h` — Vos identifiants Wi‑Fi/AP (éditables sans toucher au code).
- `include/web_ui.h` + `src/web_ui.cpp` — Les pages HTML (Passages et Config) en PROGMEM.
- `src/main.cpp` — Le cœur d'origine V2.0 (lecture capteur / API / stockage), inchangé hors `#include` ci‑dessus.
- `data/` — Ressources statiques (logo, copies HTML), utilisables en LittleFS au besoin.

> But: aucune logique n'a été modifiée. Le projet doit se compiler à l'identique.
