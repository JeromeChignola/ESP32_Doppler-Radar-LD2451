#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <ctime>
#include "esp_system.h"

// ========================= CONFIG WIFI =========================
#define WIFI_SSID   "Freebox-A2B4E1"
#define WIFI_PASS   "Jerome1986"
#define AP_SSID     "LD2451-EVerHM"
#define AP_PASS     "12345678"  // 8+ chars
static const char* TZ_EUROPE_PARIS = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ========================= UART RADAR ==========================
#define RADAR_RX 16  // ESP32 RX2  <= Radar TX
#define RADAR_TX 17  // ESP32 TX2  => Radar RX
static uint32_t g_uart_baud = 115200;

// =================== Protocole HLK-LD2451 ======================
static const uint8_t CMD_HDR[4]  = {0xFD,0xFC,0xFB,0xFA};
static const uint8_t CMD_TAIL[4] = {0x04,0x03,0x02,0x01};
static const uint8_t DAT_HDR[4]  = {0xF4,0xF3,0xF2,0xF1};
static const uint8_t DAT_TAIL[4] = {0xF8,0xF7,0xF6,0xF5};

enum : uint16_t {
  CMD_ENABLE_CFG   = 0x00FF,
  CMD_END_CFG      = 0x00FE, // payload 0x0001
  CMD_SET_DET      = 0x0002, // 4B
  CMD_GET_DET      = 0x0012, // +4B
  CMD_SET_SENS     = 0x0003, // 4B
  CMD_GET_SENS     = 0x0013, // +4B
  CMD_READ_VERSION = 0x00A1,
  CMD_SET_BAUD     = 0x00A0, // 2B index + reboot
  CMD_REBOOT       = 0x00A2,
  CMD_FACTORY_RST  = 0x00A3
};

// ====================== OPTIONS & ETAT =========================
static bool PRINT_EMPTY   = false;
static bool PRINT_RAW     = false;

static bool     ONLY_APPROACH     = false;
static uint8_t  MIN_SPEED         = 0;       // km/h mini
static uint32_t PASS_DEBOUNCE_MS  = 1500;

static bool g_applyAtBoot = true;
static int  g_baudIdxSaved = 5; // 115200

static struct { uint32_t bytes_rx=0, frames_data=0, frames_ack=0; } ST;

// ====================== LOGIQUE PASSAGES =======================
struct Passage { time_t ts; int8_t angle; uint8_t dist_m; uint8_t speed_kmh; uint8_t dir; uint8_t snr; };
static std::vector<Passage> g_passes;
static const size_t MAX_PASSES = 2000;
static uint32_t g_lastPassMs = 0;

// ======================= CONFIG COURANTE =======================
struct DetParams { uint8_t maxDist_m=20, dirMode=2, minSpeed_kmh=0, noTargetDelay_s=2; bool valid=false; };
struct SensParams{ uint8_t trigCount=1, snrLevel=4, ext1=0, ext2=0; bool valid=false; };
static DetParams  g_det;
static SensParams g_sens;

// ========================== UTILS ==============================
static String hex2(uint8_t b){ static const char* d="0123456789ABCDEF"; String s; s+=d[b>>4]; s+=d[b&0xF]; return s; }
static String toHex(const uint8_t* p, size_t n, const char* sep=" "){ String s; s.reserve(n*3); for(size_t i=0;i<n;i++){ s+=hex2(p[i]); if(i+1<n) s+=sep; } return s; }
static String fmtDate(time_t t){ if(!t) return F("-"); struct tm tm; localtime_r(&t,&tm); char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm); return String(buf); }
static time_t nowLocal(){ return time(nullptr); }
const char* resetToStr(esp_reset_reason_t r){
  switch(r){ case ESP_RST_POWERON:return "POWERON"; case ESP_RST_EXT:return "EXT"; case ESP_RST_SW:return "SW";
    case ESP_RST_PANIC:return "PANIC/WDT"; case ESP_RST_INT_WDT:return "INT_WDT"; case ESP_RST_TASK_WDT:return "TASK_WDT";
    case ESP_RST_BROWNOUT:return "BROWNOUT"; default:return "OTHER"; }
}
static uint32_t idxToBaud(int idx){
  switch(idx){ case 1:return 9600; case 2:return 19200; case 3:return 38400; case 4:return 57600; case 5:return 115200; case 6:return 230400; case 7:return 256000; case 8:return 460800; default:return 115200; }
}
static int baudToIdx(uint32_t b){
  switch(b){ case 9600:return 1; case 19200:return 2; case 38400:return 3; case 57600:return 4; case 115200:return 5; case 230400:return 6; case 256000:return 7; case 460800:return 8; default:return 5; }
}

// ======================== STOCKAGE CSV/CFG =====================
static const char* CSV_PATH = "/passes.csv";
static const char* CFG_PATH = "/config.txt";

// ---- LittleFS robust mount (tries both labels) ----
bool mountFS() {
  if (LittleFS.begin(false, "/littlefs", 10, "littlefs")) { Serial.println("[FS] mounted label=littlefs"); return true; }
  if (LittleFS.begin(false, "/littlefs", 10, "spiffs"))   { Serial.println("[FS] mounted label=spiffs");   return true; }
  if (LittleFS.begin(true,  "/littlefs", 10, "littlefs")) { Serial.println("[FS] formatted label=littlefs"); return true; }
  if (LittleFS.begin(true,  "/littlefs", 10, "spiffs"))   { Serial.println("[FS] formatted label=spiffs");   return true; }
  return false;
}

void appendCSV(const Passage& p){
  if (!LittleFS.exists(CSV_PATH)) {
    File f0 = LittleFS.open(CSV_PATH, FILE_WRITE);
    if (!f0) { Serial.println("[FS] cannot create passes.csv"); return; }
    f0.println("epoch,datetime,direction,speed_kmh,dist_m,angle_deg,snr"); f0.close();
  }
  File f = LittleFS.open(CSV_PATH, FILE_APPEND);
  if (!f) { Serial.println("[FS] cannot append passes.csv"); return; }
  f.printf("%ld,%s,%s,%u,%u,%d,%u\n",(long)p.ts, fmtDate(p.ts).c_str(), p.dir?"approach":"away", p.speed_kmh, p.dist_m, (int)p.angle, p.snr);
  f.close();
}

void saveConfig(){
  File f = LittleFS.open(CFG_PATH, FILE_WRITE);
  if (!f) { Serial.println("[FS] cannot create config.txt"); return; }
  f.printf("options_approach=%d\n", ONLY_APPROACH?1:0);
  f.printf("options_minspd=%u\n", MIN_SPEED);
  f.printf("options_debounce=%lu\n", (unsigned long)PASS_DEBOUNCE_MS);
  f.printf("apply_at_boot=%d\n", g_applyAtBoot?1:0);
  f.printf("det_max=%u\n", g_det.maxDist_m);
  f.printf("det_dir=%u\n", g_det.dirMode);
  f.printf("det_minspd=%u\n", g_det.minSpeed_kmh);
  f.printf("det_delay=%u\n", g_det.noTargetDelay_s);
  f.printf("sens_trig=%u\n", g_sens.trigCount);
  f.printf("sens_snr=%u\n", g_sens.snrLevel);
  f.printf("baud_idx=%d\n", g_baudIdxSaved);
  f.close();
  Serial.println("[CFG] saved");
}
bool loadConfig(){
  if (!LittleFS.exists(CFG_PATH)) { Serial.println("[CFG] not found (defaults)"); return false; }
  File f = LittleFS.open(CFG_PATH, FILE_READ); if (!f) { Serial.println("[CFG] open fail"); return false; }
  while (f.available()){
    String line = f.readStringUntil('\n'); line.trim();
    int eq = line.indexOf('='); if (eq<0) continue;
    String k = line.substring(0,eq); String v = line.substring(eq+1);
    long n = v.toInt();
    if      (k=="options_approach") ONLY_APPROACH = (n!=0);
    else if (k=="options_minspd")   MIN_SPEED = (uint8_t)constrain(n,0,120);
    else if (k=="options_debounce") PASS_DEBOUNCE_MS = (uint32_t)constrain(n,200,10000);
    else if (k=="apply_at_boot")    g_applyAtBoot = (n!=0);
    else if (k=="det_max")          { g_det.maxDist_m = (uint8_t)constrain(n,1,120); g_det.valid=true; }
    else if (k=="det_dir")          { g_det.dirMode = (uint8_t)constrain(n,0,2); g_det.valid=true; }
    else if (k=="det_minspd")       { g_det.minSpeed_kmh = (uint8_t)constrain(n,0,120); g_det.valid=true; }
    else if (k=="det_delay")        { g_det.noTargetDelay_s = (uint8_t)constrain(n,0,255); g_det.valid=true; }
    else if (k=="sens_trig")        { g_sens.trigCount = (uint8_t)constrain(n,1,10); g_sens.valid=true; }
    else if (k=="sens_snr")         { g_sens.snrLevel = (uint8_t)constrain(n,0,8); g_sens.valid=true; }
    else if (k=="baud_idx")         g_baudIdxSaved = (int)constrain(n,1,8);
  }
  f.close();
  Serial.println("[CFG] loaded");
  return true;
}
void ensureFiles() {
  if (!LittleFS.exists(CFG_PATH)) { saveConfig(); Serial.println("[FS] created default config.txt"); }
  if (!LittleFS.exists(CSV_PATH)) {
    File f = LittleFS.open(CSV_PATH, FILE_WRITE);
    if (f) { f.println("epoch,datetime,direction,speed_kmh,dist_m,angle_deg,snr"); f.close(); }
    else { Serial.println("[FS] cannot create passes.csv"); }
  }
}

// ====================== UART / PARSING =========================
static std::vector<uint8_t> rx;
static std::vector<uint8_t> lastTx;
struct AckStore { uint16_t cmd=0xFFFF; uint16_t status=0xFFFF; std::vector<uint8_t> data; uint32_t ts=0; } g_ack;

void sendCmd(uint16_t cmd, const uint8_t* payload, uint16_t plen){
  // Spéc LD2451 : HDR + LEN(2+N) + CMD(2 LE) + VALUE(N) + TAIL ; LEN n'inclut pas le tail
  std::vector<uint8_t> f;
  uint16_t dataLen = uint16_t(2 + plen); // 2 pour "CMD" + N pour le payload

  // Header
  f.insert(f.end(), CMD_HDR, CMD_HDR+4);

  // Length
  f.push_back(uint8_t(dataLen & 0xFF));
  f.push_back(uint8_t(dataLen >> 8));

  // Commande (little-endian)
  f.push_back(uint8_t(cmd & 0xFF));
  f.push_back(uint8_t(cmd >> 8));

  // Valeur
  if (payload && plen) f.insert(f.end(), payload, payload + plen);

  // Tail
  f.insert(f.end(), CMD_TAIL, CMD_TAIL+4);

  lastTx = f;
  Serial.printf("[TX CMD] 0x%04X payload=%u raw:%s\n", cmd, plen, toHex(f.data(), f.size()).c_str());
  Serial2.write(f.data(), f.size());
  Serial2.flush();
}

void cmd_enableCfg(){ sendCmd(CMD_ENABLE_CFG, nullptr, 0); }
void cmd_endCfg(){ uint8_t v[2]={0x01,0x00}; sendCmd(CMD_END_CFG, v, 2); }
void cmd_readVersion(){ sendCmd(CMD_READ_VERSION, nullptr, 0); }
static inline uint16_t u16le(const uint8_t* p){ return uint16_t(p[0]) | (uint16_t(p[1])<<8); }

// ACK : HDR + LEN(2 = 2 octets (CMD|0x0100) + N) + (CMD|0x0100) + RETURN(N) + TAIL
bool waitAck(uint16_t cmd, uint32_t timeout_ms){
  uint32_t t0 = millis();
  g_ack.cmd = 0xFFFF; 
  g_ack.status = 0xFFFF; 
  g_ack.data.clear();

  while (millis() - t0 < timeout_ms) {
    // Alimente le buffer RX
    while (Serial2.available()){
      uint8_t b = (uint8_t)Serial2.read();
      rx.push_back(b);
      ST.bytes_rx++;
      if (rx.size() > 8192) rx.erase(rx.begin(), rx.begin() + 4096);
    }

    auto hdrEq = [&](const uint8_t* H){
      return rx.size() >= 4 && rx[0]==H[0] && rx[1]==H[1] && rx[2]==H[2] && rx[3]==H[3];
    };

    // --- Consomme les frames DATA pour ne pas bloquer ---
    if (hdrEq(DAT_HDR)){
      if (rx.size() < 10) { delay(1); continue; }
      uint16_t L  = u16le(rx.data() + 4);
      size_t  FL  = 4 + 2 + L + 4;            // L n'inclut pas le tail
      if (rx.size() < FL) { delay(1); continue; }
      if (std::equal(DAT_TAIL, DAT_TAIL + 4, rx.data() + FL - 4)) {
        ST.frames_data++;
        rx.erase(rx.begin(), rx.begin() + FL);
      } else {
        rx.erase(rx.begin());                 // désynchronisation : slide d'1 octet
      }
      continue;
    }

    // --- Frame de type ACK/Commande ---
    if (hdrEq(CMD_HDR)){
      // Filtre l’écho de nos propres trames (certains montages renvoient l'echo du TX)
      if (!lastTx.empty() && rx.size() >= lastTx.size() &&
          std::equal(rx.begin(), rx.begin() + lastTx.size(), lastTx.begin())){
        Serial.println("[UART] Echo of our TX ignored");
        rx.erase(rx.begin(), rx.begin() + lastTx.size());
        continue;
      }

      if (rx.size() < 12) { delay(1); continue; }
      uint16_t L  = u16le(rx.data() + 4);
      size_t  FL  = 4 + 2 + L + 4;
      if (rx.size() < FL) { delay(1); continue; }
      if (!std::equal(CMD_TAIL, CMD_TAIL + 4, rx.data() + FL - 4)) {
        rx.erase(rx.begin());                 // mauvais tail => désync, on jette 1 octet
        continue;
      }

      uint16_t ackCmd = u16le(rx.data() + 6);         // (cmd | 0x0100) selon la doc
      const uint8_t* ret = rx.data() + 8;
      size_t retLen = (L >= 2) ? (L - 2) : 0;         // 2 octets déjà pris par ackCmd

      g_ack.cmd = ackCmd;
      g_ack.status = (retLen >= 2) ? u16le(ret) : 0xFFFF;

      // Copie des octets de retour (hors status) en évitant assign(const*, non-const*)
      const uint8_t* first = ret + (retLen >= 2 ? 2 : 0);
      const uint8_t* last  = rx.data() + FL - 4;      // début du tail
      g_ack.data.clear();
      g_ack.data.insert(g_ack.data.end(), first, last);

      g_ack.ts = millis();
      ST.frames_ack++;

      rx.erase(rx.begin(), rx.begin() + FL);

      // On accepte l'ACK si cmd == ackCmd ou (cmd|0x0100) == ackCmd
      if (g_ack.cmd == cmd || g_ack.cmd == (cmd | 0x0100)) return true;

      continue;
    }

    // Pas d'en-tête reconnu : on décale d'1 octet
    if (!rx.empty()) rx.erase(rx.begin());
    delay(1);
  }
  return false; // timeout
}

// === Passages
static void maybeRecordPassageFromTargets(const std::vector<Passage>& candidates){
  if (candidates.empty()) return;
  uint32_t nowMs=millis(); if (nowMs - g_lastPassMs < PASS_DEBOUNCE_MS) return;
  const Passage* best=&candidates[0]; for (const auto& c: candidates) if (c.speed_kmh>best->speed_kmh) best=&c;
  Passage p=*best; p.ts=nowLocal(); g_passes.push_back(p); if (g_passes.size()>MAX_PASSES) g_passes.erase(g_passes.begin()); appendCSV(p); g_lastPassMs=nowMs;
  Serial.printf("[PASS] %s v=%u d=%u θ=%d @ %s\n", p.dir?"approach":"away", p.speed_kmh, p.dist_m, (int)p.angle, fmtDate(p.ts).c_str());
}
void parseDataFrame(const uint8_t* p, size_t n){
  if (n<10) return;
  uint16_t L = p[4] | (uint16_t(p[5])<<8);
  const uint8_t* payload = p+6; const uint8_t* tail=payload+L;
  if (!std::equal(DAT_TAIL,DAT_TAIL+4,tail)) return;
  if (L<2) { if(PRINT_EMPTY) Serial.println("[DATA] short"); ST.frames_data++; return; }
  uint8_t count = payload[0];
  const size_t PER=5; const uint8_t* tp=payload+2; const uint8_t* end=tail;
  std::vector<Passage> cand;
  for(uint8_t i=0;i<count;i++){ if (tp+PER>end) break;
    int8_t angle = int(tp[0]) - 0x80; uint8_t dist=tp[1]; uint8_t dir=tp[2]; uint8_t spd=tp[3]; uint8_t snr=tp[4];
    if ((!ONLY_APPROACH || dir==1) && spd>=MIN_SPEED && spd>0){ Passage c; c.ts=0; c.angle=angle; c.dist_m=dist; c.speed_kmh=spd; c.dir=dir; c.snr=snr; cand.push_back(c); }
    tp+=PER;
  }
  maybeRecordPassageFromTargets(cand); ST.frames_data++;
}
void parseAckFrame(const uint8_t* p, size_t n){ (void)p; (void)n; /* handled in waitAck() */ }

size_t tryParseOne(){
  if (rx.size()<4) return 0;
  auto hdrEq = [&](const uint8_t* H){ return rx.size()>=4 && rx[0]==H[0]&&rx[1]==H[1]&&rx[2]==H[2]&&rx[3]==H[3]; };

  if (hdrEq(DAT_HDR)){
    if (rx.size()<10) return 0;
    uint16_t L = (uint16_t)rx[4] | ((uint16_t)rx[5]<<8);
    size_t frameLen = 4 + 2 + L + 4;
    if (rx.size()<frameLen) return 0;
    if (!std::equal(DAT_TAIL, DAT_TAIL+4, rx.data()+frameLen-4)){ rx.erase(rx.begin()); return 1; }
    parseDataFrame(rx.data(), frameLen);
    rx.erase(rx.begin(), rx.begin()+frameLen);
    return frameLen;
  }

  if (hdrEq(CMD_HDR)){
    if (!lastTx.empty() && rx.size()>=lastTx.size() &&
        std::equal(rx.begin(), rx.begin()+lastTx.size(), lastTx.begin())){
      Serial.println("[UART] Echo of our TX ignored");
      rx.erase(rx.begin(), rx.begin()+lastTx.size());
      return 1;
    }
    if (rx.size()<12) return 0;
    uint16_t L = (uint16_t)rx[4] | ((uint16_t)rx[5]<<8);
    size_t frameLen = 4 + 2 + L + 4;
    if (rx.size()<frameLen) return 0;
    if (!std::equal(CMD_TAIL, CMD_TAIL+4, rx.data()+frameLen-4)){ rx.erase(rx.begin()); return 1; }
    ST.frames_ack++;
    rx.erase(rx.begin(), rx.begin()+frameLen);
    return frameLen;
  }

  rx.erase(rx.begin());
  return 1;
}


// ========================= SERVEUR WEB =========================
WebServer server(80);

// ----------- PAGE 1 : PASSAGES & STATS (auto-refresh) ----------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LD2451 Radar — Passages</title>
<style>
body{font:14px system-ui,Segoe UI,Roboto,Arial;margin:0;background:#0b0f17;color:#e7eef9}
header{padding:16px 20px;background:#111827;border-bottom:1px solid #1f2937;display:flex;align-items:center;justify-content:space-between}
h1{margin:0;font-size:18px}
a.nav{color:#93c5fd;text-decoration:none;padding:6px 10px;border:1px solid #1f2937;border-radius:10px}
main{padding:16px 20px}
.card{background:#111827;border:1px solid #1f2937;border-radius:14px;padding:16px;margin:0 0 16px}
.row{display:flex;gap:16px;flex-wrap:wrap}
.card h2{margin:0 0 10px;font-size:16px}
small, label{color:#9aa4b2}
table{width:100%;border-collapse:collapse}
th,td{padding:8px 10px;border-bottom:1px solid #1f2937}
th{position:sticky;top:0;background:#0f1623}
.badge{display:inline-block;padding:2px 8px;border-radius:999px;font-weight:600}
.badge.approach{background:#0a3;color:#fff}.badge.away{background:#333;color:#ddd;border:1px solid #555}
.btn{background:#2563eb;color:#fff;border:none;padding:8px 12px;border-radius:10px;cursor:pointer}
.btn.secondary{background:#374151}
input[type="number"]{background:#0b1220;color:#e7eef9;border:1px solid #1f2937;border-radius:8px;padding:6px;width:90px}
.switch{display:inline-flex;align-items:center;gap:8px;margin-right:12px}
canvas{width:100%;height:220px;background:#0b1220;border-radius:12px;border:1px solid #1f2937}
footer{padding:16px 20px;color:#9aa4b2}
</style>
</head><body>
<header>
  <h1>LD2451 — Passages & Statistiques</h1>
  <a class="nav" href="/config">Configuration radar ⚙️</a>
</header>
<main>
  <div class="row">
    <div class="card" style="flex:1;min-width:280px">
      <h2>Options d’affichage</h2>
      <div class="switch"><input id="opt_approach" type="checkbox"><label for="opt_approach">Approche seulement</label></div>
      <div class="switch"><label for="opt_minspd">Vitesse mini</label><input id="opt_minspd" type="number" min="0" max="120" step="1" value="0"> km/h</div>
      <div class="switch"><label for="opt_deb">Anti-doublons</label><input id="opt_deb" type="number" min="200" max="5000" step="100" value="1500"> ms</div>
      <div style="margin-top:10px">
        <button class="btn" onclick="saveOpts()">Enregistrer</button>
        <button class="btn secondary" onclick="clearPasses()">Effacer la liste</button>
        <a class="btn" href="/csv">Télécharger CSV</a>
      </div>
      <div style="margin-top:8px"><small id="msg"></small></div>
    </div>
    <div class="card" style="flex:2;min-width:300px">
      <h2>Statistiques (live)</h2>
      <canvas id="chart_speed"></canvas><div style="height:12px"></div>
      <canvas id="chart_dir"></canvas>
    </div>
  </div>

  <div class="card">
    <h2>Derniers passages</h2>
    <div style="overflow:auto;max-height:50vh">
      <table id="tbl"><thead><tr>
        <th>Date/Heure</th><th>Direction</th><th>Vitesse</th><th>Distance</th><th>Angle</th><th>SNR</th>
      </tr></thead><tbody></tbody></table>
    </div>
  </div>
</main>
<footer><small>LD2451 Radar • ESP32</small></footer>

<script>
async function getJSON(u){const r=await fetch(u); return r.json();}
function badgeDir(d){return d?"<span class='badge approach'>approche</span>":"<span class='badge away'>éloign.</span>";}
function fmtDate(s){return s||'-';}

async function loadAll(){
  const cfg=await getJSON('/api/options');
  opt_approach.checked=!!cfg.approach; opt_minspd.value=cfg.minspd|0; opt_deb.value=cfg.debounce|0;

  const data = await getJSON('/api/passes'); const tb=document.querySelector('#tbl tbody'); tb.innerHTML='';
  for(const p of data){
    const tr=document.createElement('tr');
    tr.innerHTML = `<td>${fmtDate(p.datetime)}</td><td>${badgeDir(p.dir)}</td>
      <td>${p.speed_kmh} km/h</td><td>${p.dist_m} m</td><td>${p.angle_deg}°</td><td>${p.snr}</td>`;
    tb.appendChild(tr);
  }
  const st = await getJSON('/api/stats'); drawSpeedChart(st.speed_bins); drawDirChart(st.dir_counts);
}
async function saveOpts(){
  const a=opt_approach.checked?1:0, m=+opt_minspd.value||0, d=+opt_deb.value||1500;
  await fetch(`/api/options?approach=${a}&minspd=${m}&debounce=${d}`); msg.innerText='Options OK'; setTimeout(()=>msg.innerText='',1200);
  loadAll();
}
async function clearPasses(){ if(!confirm('Effacer tous les passages ?')) return; await fetch('/api/clear'); loadAll(); }

function drawSpeedChart(bins){
  const c = chart_speed, g=c.getContext('2d'); const W=c.clientWidth,H=c.clientHeight; c.width=W;c.height=H; g.clearRect(0,0,W,H);
  const labels=bins.map(b=>b.min+'-'+b.max), vals=bins.map(b=>b.count), maxV=Math.max(1,...vals), n=vals.length, barW=(W-20)/n;
  g.fillStyle='#93c5fd';
  vals.forEach((v,i)=>{ const h=(H-30)*v/maxV, x=10+i*barW, y=H-20-h; g.fillRect(x,y,barW*0.8,h); g.fillStyle='#9aa4b2'; g.fillText(labels[i], x, H-6); g.fillStyle='#93c5fd'; });
  g.fillStyle='#e7eef9'; g.fillText('Vitesses (km/h)', 10, 14);
}
function drawDirChart(dc){
  const c=chart_dir,g=c.getContext('2d'); const W=c.clientWidth,H=c.clientHeight; c.width=W;c.height=H; g.clearRect(0,0,W,H);
  const tot=Math.max(1,(dc.approach|0)+(dc.away|0)), a=(dc.approach|0)/tot, cx=W/2, cy=H/2, r=Math.min(W,H)/2-10;
  let start=-Math.PI/2, end=start+2*Math.PI*a;
  g.beginPath(); g.moveTo(cx,cy); g.arc(cx,cy,r,start,end); g.closePath(); g.fillStyle='#10b981'; g.fill();
  g.beginPath(); g.moveTo(cx,cy); g.arc(cx,cy,r,end,start+2*Math.PI); g.closePath(); g.fillStyle='#4b5563'; g.fill();
  g.fillStyle='#e7eef9'; g.fillText('Répartition sens', 10, 14);
  g.fillStyle='#10b981'; g.fillRect(10,H-18,10,10); g.fillStyle='#e7eef9'; g.fillText('Approche',26,H-10);
  g.fillStyle='#4b5563'; g.fillRect(100,H-18,10,10); g.fillStyle='#e7eef9'; g.fillText('Éloign.',116,H-10);
}
loadAll(); setInterval(loadAll, 1500);
</script>
</body></html>
)HTML";

// ----------- PAGE 2 : CONFIGURATION (pas d’auto-refresh) -------
const char CONFIG_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LD2451 — Configuration</title>
<style>
body{font:14px system-ui,Segoe UI,Roboto,Arial;margin:0;background:#0b0f17;color:#e7eef9}
header{padding:16px 20px;background:#111827;border-bottom:1px solid #1f2937;display:flex;align-items:center;justify-content:space-between}
h1{margin:0;font-size:18px}
a.nav{color:#93c5fd;text-decoration:none;padding:6px 10px;border:1px solid #1f2937;border-radius:10px}
main{padding:16px 20px}
.card{background:#111827;border:1px solid #1f2937;border-radius:14px;padding:16px;margin:0 0 16px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(180px,1fr));gap:10px}
small,label{color:#9aa4b2}
input[type="number"],select{background:#0b1220;color:#e7eef9;border:1px solid #1f2937;border-radius:8px;padding:6px}
input[type="number"]{width:110px}
.btn{background:#2563eb;color:#fff;border:none;padding:8px 12px;border-radius:10px;cursor:pointer}
.btn.secondary{background:#374151}
.pills .btn{margin-right:8px;margin-top:8px}
</style></head><body>
<header>
  <h1>LD2451 — Configuration radar</h1>
  <a class="nav" href="/">← Retour aux passages</a>
</header>
<main>
  <div class="card">
    <h2>Paramètres radar</h2>
    <div class="grid">
      <label>Distance max (m)<br><input id="cfg_max" type="number" min="1" max="120" step="1" value="20"></label>
      <label>Vitesse mini (km/h)<br><input id="cfg_minspd" type="number" min="0" max="120" step="1" value="0"></label>
      <label>Délai no-target (s)<br><input id="cfg_delay" type="number" min="0" max="255" step="1" value="2"></label>
      <label>Direction<br>
        <select id="cfg_dir">
          <option value="2">Tout (02)</option>
          <option value="1">Approche (01)</option>
          <option value="0">Éloignement (00)</option>
        </select>
      </label>
      <label>Déclenchements consécutifs<br><input id="cfg_trig" type="number" min="1" max="10" step="1" value="1"></label>
      <label>Niveau SNR (0=def,3..8=moins sensible)<br><input id="cfg_snr" type="number" min="0" max="8" step="1" value="4"></label>
      <label>Baud radar (reboot)<br>
        <select id="cfg_baud">
          <option value="1">9600</option><option value="2">19200</option><option value="3">38400</option>
          <option value="4">57600</option><option value="5" selected>115200 (def)</option>
          <option value="6">230400</option><option value="7">256000</option><option value="8">460800</option>
        </select>
      </label>
      <label><input id="cfg_applyboot" type="checkbox"> Appliquer au démarrage</label>
    </div>
    <div class="pills" style="margin-top:6px">
      <button class="btn secondary" onclick="preset('ped')">Profil Piéton</button>
      <button class="btn secondary" onclick="preset('car')">Profil Voiture</button>
    </div>
    <div style="margin-top:12px">
      <button class="btn" onclick="readCfg()">Lire config</button>
      <button class="btn" onclick="applyCfg()">Appliquer</button>
      <button class="btn secondary" onclick="setBaud()">Définir Baud</button>
      <button class="btn secondary" onclick="reboot()">Redémarrer module</button>
      <button class="btn secondary" onclick="factory()">Paramètres usine</button>
    </div>
    <div style="margin-top:10px"><small id="cfg_msg"></small></div>
  </div>

  <div class="card">
    <h2>BLE (économie d’énergie)</h2>
    <p><small>Le protocole série publié ne documente pas la désactivation BLE via UART. Le bouton ci-dessous retourne l’état de support.</small></p>
    <div>
      <button class="btn secondary" onclick="ble(0)">Désactiver BLE</button>
      <button class="btn secondary" onclick="ble(1)">Activer BLE</button>
      <small id="ble_msg" style="margin-left:10px"></small>
    </div>
  </div>
</main>
<script>
async function getJSON(u){const r=await fetch(u); return r.json();}
function setMsg(s){cfg_msg.innerText=s;}

async function loadCfg(){
  const meta=await getJSON('/api/cfg/get');
  if (meta.det){ cfg_max.value=meta.det.max; cfg_dir.value=meta.det.dir; cfg_minspd.value=meta.det.minspd; cfg_delay.value=meta.det.delay; }
  if (meta.sens){ cfg_trig.value=meta.sens.trig; cfg_snr.value=meta.sens.snr; }
  if (meta.baudIdx) cfg_baud.value=meta.baudIdx;
  cfg_applyboot.checked = !!meta.applyBoot;
}
async function readCfg(){ setMsg('Lecture...'); const j=await getJSON('/api/cfg/read'); setMsg(j.ok?'Lu.':'Échec lecture'); await loadCfg(); }
async function applyCfg(){
  const q=`max=${+cfg_max.value}&dir=${+cfg_dir.value}&minspd=${+cfg_minspd.value}&delay=${+cfg_delay.value}&trig=${+cfg_trig.value}&snr=${+cfg_snr.value}&applyboot=${cfg_applyboot.checked?1:0}`;
  setMsg('Application...'); const j=await getJSON('/api/cfg/set?'+q); setMsg(j.ok?'Appliqué.':'Échec appli.');
}
async function setBaud(){ const idx=+cfg_baud.value; setMsg('Changement de baud...'); const j=await getJSON('/api/cfg/baud?idx='+idx); setMsg(j.ok?('Baud='+j.baud):'Échec baud'); }
async function reboot(){ setMsg('Reboot...'); await getJSON('/api/reboot'); setMsg('Demande envoyée.'); }
async function factory(){ if(!confirm('Restaurer usine ?'))return; setMsg('Usine + reboot...'); await getJSON('/api/factory'); setMsg('Demande envoyée.'); }
async function preset(n){ setMsg('Profil...'); const j=await getJSON('/api/cfg/preset?name='+encodeURIComponent(n)); setMsg(j.ok?'Profil OK':'Échec profil'); await loadCfg(); }
async function ble(en){ ble_msg.innerText='Commande...'; const j=await getJSON('/api/cfg/ble?en='+en); ble_msg.innerText = j.supported? (j.ok?'OK':'Échec'): 'Non supporté par protocole'; }
loadCfg();
</script>
</body></html>
)HTML";

// ---------------- API Passages / Options -----------------------
void handlePasses(){
  String j="["; for(size_t i=0;i<g_passes.size();++i){ const auto& p=g_passes[i]; if(i) j+=','; j+="{\"epoch\":"+String((long)p.ts)+",\"datetime\":\""+fmtDate(p.ts)+"\",\"dir\":"+(p.dir?String(1):String(0))+
    ",\"speed_kmh\":"+String(p.speed_kmh)+",\"dist_m\":"+String(p.dist_m)+",\"angle_deg\":"+String((int)p.angle)+",\"snr\":"+String(p.snr)+"}"; } j+="]";
  server.send(200,"application/json",j);
}
void handleClear(){ g_passes.clear(); LittleFS.remove(CSV_PATH); server.send(200,"application/json","{\"ok\":1}"); }
void handleCSV(){
  if (!LittleFS.exists(CSV_PATH)) {
    File tmp=LittleFS.open(CSV_PATH, FILE_WRITE);
    if(!tmp){ server.send(500,"text/plain","CSV create error"); return; }
    tmp.println("epoch,datetime,direction,speed_kmh,dist_m,angle_deg,snr");
    for (const auto& p: g_passes) tmp.printf("%ld,%s,%s,%u,%u,%d,%u\n",(long)p.ts, fmtDate(p.ts).c_str(), p.dir?"approach":"away", p.speed_kmh, p.dist_m, (int)p.angle, p.snr);
    tmp.close();
  }
  File f=LittleFS.open(CSV_PATH, FILE_READ);
  if(!f){ server.send(500,"text/plain","CSV open error"); return; }
  server.sendHeader("Content-Disposition","attachment; filename=passes.csv"); server.streamFile(f,"text/csv"); f.close();
}
void handleOptionsGet(){
  String j = "{\"approach\":" + String(ONLY_APPROACH?1:0) + ",\"minspd\":" + String(MIN_SPEED) + ",\"debounce\":" + String(PASS_DEBOUNCE_MS) + "}";
  server.send(200,"application/json",j);
}
void handleOptionsSet(){
  if (server.hasArg("approach")) ONLY_APPROACH = (server.arg("approach")=="1");
  if (server.hasArg("minspd"))   MIN_SPEED = (uint8_t)constrain(server.arg("minspd").toInt(),0,120);
  if (server.hasArg("debounce")) PASS_DEBOUNCE_MS = (uint32_t)constrain(server.arg("debounce").toInt(),200,5000);
  saveConfig();
  handleOptionsGet();
}

// ---------------------- API CONFIG -----------------------------
bool readDetSync(DetParams& out){
  cmd_enableCfg(); if(!waitAck(CMD_ENABLE_CFG, 800)) return false;
  sendCmd(CMD_GET_DET,nullptr,0); if(!waitAck(CMD_GET_DET, 1500)) { cmd_endCfg(); waitAck(CMD_END_CFG, 800); return false; }
  cmd_endCfg(); waitAck(CMD_END_CFG, 800);
  if (g_ack.status!=0 || g_ack.data.size()<4) return false;
  out.maxDist_m=g_ack.data[0]; out.dirMode=g_ack.data[1]; out.minSpeed_kmh=g_ack.data[2]; out.noTargetDelay_s=g_ack.data[3]; out.valid=true; return true;
}
bool readSensSync(SensParams& out){
  cmd_enableCfg(); if(!waitAck(CMD_ENABLE_CFG, 800)) return false;
  sendCmd(CMD_GET_SENS,nullptr,0); if(!waitAck(CMD_GET_SENS, 1500)) { cmd_endCfg(); waitAck(CMD_END_CFG, 800); return false; }
  cmd_endCfg(); waitAck(CMD_END_CFG, 800);
  if (g_ack.status!=0 || g_ack.data.size()<4) return false;
  out.trigCount=g_ack.data[0]; out.snrLevel=g_ack.data[1]; out.ext1=g_ack.data[2]; out.ext2=g_ack.data[3]; out.valid=true; return true;
}
bool setDetSync(const DetParams& in){
  uint8_t v[4]={ in.maxDist_m, in.dirMode, in.minSpeed_kmh, in.noTargetDelay_s };
  cmd_enableCfg(); if(!waitAck(CMD_ENABLE_CFG, 800)) return false;
  sendCmd(CMD_SET_DET, v, 4); if(!waitAck(CMD_SET_DET, 1500)) { cmd_endCfg(); waitAck(CMD_END_CFG, 800); return false; }
  cmd_endCfg(); waitAck(CMD_END_CFG, 800);
  return g_ack.status==0;
}
bool setSensSync(const SensParams& in){
  uint8_t v[4]={ in.trigCount, in.snrLevel, in.ext1, in.ext2 };
  cmd_enableCfg(); if(!waitAck(CMD_ENABLE_CFG, 800)) return false;
  sendCmd(CMD_SET_SENS, v, 4); if(!waitAck(CMD_SET_SENS, 1500)) { cmd_endCfg(); waitAck(CMD_END_CFG, 800); return false; }
  cmd_endCfg(); waitAck(CMD_END_CFG, 800);
  return g_ack.status==0;
}
void handleCfgGet(){
  String j="{";
  if (g_det.valid) j += "\"det\":{\"max\":"+String(g_det.maxDist_m)+",\"dir\":"+String(g_det.dirMode)+",\"minspd\":"+String(g_det.minSpeed_kmh)+",\"delay\":"+String(g_det.noTargetDelay_s)+"},";
  else j+="\"det\":null,";
  if (g_sens.valid) j += "\"sens\":{\"trig\":"+String(g_sens.trigCount)+",\"snr\":"+String(g_sens.snrLevel)+"},";
  else j+="\"sens\":null,";
  j += "\"baudIdx\":"+String(g_baudIdxSaved)+",";
  j += "\"applyBoot\":" + String(g_applyAtBoot?1:0) + "}";
  server.send(200,"application/json",j);
}
void handleCfgRead(){ bool ok1=readDetSync(g_det); bool ok2=readSensSync(g_sens); if (ok1||ok2) saveConfig(); server.send(200,"application/json", String("{\"ok\":") + (ok1&&ok2?"1}":"0}")); }
void handleCfgSet(){
  DetParams d=g_det; SensParams s=g_sens;
  if (server.hasArg("max"))   d.maxDist_m       = (uint8_t)constrain(server.arg("max").toInt(), 1, 120);
  if (server.hasArg("dir"))   d.dirMode         = (uint8_t)constrain(server.arg("dir").toInt(), 0, 2);
  if (server.hasArg("minspd"))d.minSpeed_kmh    = (uint8_t)constrain(server.arg("minspd").toInt(), 0, 120);
  if (server.hasArg("delay")) d.noTargetDelay_s = (uint8_t)constrain(server.arg("delay").toInt(), 0, 255);
  if (server.hasArg("trig"))  s.trigCount       = (uint8_t)constrain(server.arg("trig").toInt(), 1, 10);
  if (server.hasArg("snr"))   s.snrLevel        = (uint8_t)constrain(server.arg("snr").toInt(), 0, 8);
  if (server.hasArg("applyboot")) g_applyAtBoot = (server.arg("applyboot")=="1");
  bool ok = setDetSync(d) && setSensSync(s); if (ok){ g_det=d; g_sens=s; saveConfig(); }
  server.send(200,"application/json", String("{\"ok\":") + (ok?"1}":"0}"));
}
void handleCfgBaud(){
  int idx = constrain(server.arg("idx").toInt(), 1, 8);
  uint8_t v[2] = { (uint8_t)(idx & 0xFF), (uint8_t)(idx>>8) };
  cmd_enableCfg(); waitAck(CMD_ENABLE_CFG, 800);
  sendCmd(CMD_SET_BAUD, v, 2); bool ok = waitAck(CMD_SET_BAUD, 2000);
  cmd_endCfg(); waitAck(CMD_END_CFG, 800);
  g_baudIdxSaved = idx; saveConfig();
  String j = String("{\"ok\":") + (ok?"1":"0") + ",\"baud\":"+idxToBaud(idx)+"}";
  server.send(200,"application/json", j);
}
void handleReboot(){ sendCmd(CMD_REBOOT,nullptr,0); server.send(200,"application/json","{\"ok\":1}"); }
void handleFactory(){ sendCmd(CMD_FACTORY_RST,nullptr,0); server.send(200,"application/json","{\"ok\":1}"); }

void applyPresetValues(const String& name, DetParams& d, SensParams& s){
  if (name=="ped"){ d.maxDist_m=8;  d.dirMode=2;  d.minSpeed_kmh=2;  d.noTargetDelay_s=2; s.trigCount=2; s.snrLevel=5; }
  else            { d.maxDist_m=20; d.dirMode=2;  d.minSpeed_kmh=10; d.noTargetDelay_s=1; s.trigCount=1; s.snrLevel=4; }
}
void handleCfgPreset(){
  String name = server.arg("name");
  if (name!="ped" && name!="car"){ server.send(400,"application/json","{\"ok\":0}"); return; }
  DetParams d=g_det; SensParams s=g_sens; applyPresetValues(name,d,s);
  bool ok = setDetSync(d) && setSensSync(s); if (ok){ g_det=d; g_sens=s; saveConfig(); }
  server.send(200,"application/json", String("{\"ok\":") + (ok?"1}":"0}"));
}

// BLE placeholder (non documenté via UART)
void handleCfgBle(){ server.send(200,"application/json","{\"supported\":0,\"ok\":0}"); }

// ---------------------- DIAG PING ------------------------------
void handleDiagPing(){
  cmd_enableCfg(); bool a = waitAck(CMD_ENABLE_CFG, 800);
  cmd_readVersion(); bool b = waitAck(CMD_READ_VERSION, 1500);
  cmd_endCfg(); bool c = waitAck(CMD_END_CFG, 800);
  char buf[160];
  snprintf(buf, sizeof(buf),
    "{\"ok\":%d,\"enable\":%d,\"readver\":%d,\"end\":%d,\"ack_cmd\":%u,\"status\":%u}",
    (a&&b&&c)?1:0, a, b, c, (unsigned)g_ack.cmd, (unsigned)g_ack.status);
  server.send(200, "application/json", buf);
}

// ========================= WIFI & NTP =========================
void setupWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WIFI] Connexion à %s ...\n", WIFI_SSID);
  uint32_t t0=millis(); bool ok=false; while (millis()-t0<10000){ if (WiFi.status()==WL_CONNECTED){ ok=true; break; } delay(200); }
  if (!ok){ WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS); IPAddress ip=WiFi.softAPIP(); Serial.printf("[AP] SSID=%s PASS=%s IP=%s\n", AP_SSID, AP_PASS, ip.toString().c_str()); }
  else {
    Serial.printf("[WIFI] OK  IP=%s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("ld2451")) Serial.println("[MDNS] http://ld2451.local");
    configTzTime(TZ_EUROPE_PARIS, "pool.ntp.org","time.google.com","time.cloudflare.com");
  }
}

// ============================ SETUP/LOOP =======================
void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n=== LD2451 Radar • ESP32 Web+Config+Persist (split pages) ===");
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[RESET] reason=%d (%s)\n", (int)rr, resetToStr(rr));

  if (!mountFS()) Serial.println("[FS] Mount fail");
  loadConfig(); ensureFiles();

  setupWiFi();
  Serial2.begin(g_uart_baud, SERIAL_8N1, RADAR_RX, RADAR_TX);
  Serial.printf("[UART] RX2=%d TX2=%d @ %u 8N1\n", RADAR_RX, RADAR_TX, (unsigned)g_uart_baud);

  if (g_applyAtBoot && g_det.valid && g_sens.valid) {
    Serial.println("[BOOT] Applying stored radar config...");
    bool ok = setDetSync(g_det) && setSensSync(g_sens);
    Serial.printf("[BOOT] apply %s\n", ok?"OK":"FAIL");
  }

  // Web routes
  server.on("/",        HTTP_GET, [](){ server.send_P(200,"text/html",INDEX_HTML); });
  server.on("/config",  HTTP_GET, [](){ server.send_P(200,"text/html",CONFIG_HTML); });

  server.on("/api/passes", HTTP_GET, handlePasses);
  server.on("/api/clear",  HTTP_GET, handleClear);
  server.on("/csv",        HTTP_GET, handleCSV);
  server.on("/api/options",HTTP_GET, [](){ if (server.hasArg("approach")||server.hasArg("minspd")||server.hasArg("debounce")) handleOptionsSet(); else handleOptionsGet(); });
  server.on("/api/stats",  HTTP_GET, [](){
    const int BIN_W=5, BIN_MAX=60, NB=(BIN_MAX/BIN_W)+1; int bins[NB]; for(int i=0;i<NB;i++) bins[i]=0; int dir_app=0, dir_away=0;
    for (const auto& p: g_passes){ int b=p.speed_kmh/BIN_W; if (b>=NB) b=NB-1; bins[b]++; if (p.dir) dir_app++; else dir_away++; }
    String j="{\"speed_bins\":[";
    for(int i=0;i<NB;i++){ if(i) j+=','; int mi=i*BIN_W, ma=(i==NB-1)?999:(mi+BIN_W-1); j+="{\"min\":"+String(mi)+",\"max\":"+String(ma)+",\"count\":"+String(bins[i])+"}"; }
    j+="],\"dir_counts\":{\"approach\":"+String(dir_app)+",\"away\":"+String(dir_away)+"}}"; server.send(200,"application/json",j);
  });

  // config API
  server.on("/api/cfg/get",    HTTP_GET, handleCfgGet);
  server.on("/api/cfg/read",   HTTP_GET, handleCfgRead);
  server.on("/api/cfg/set",    HTTP_GET, handleCfgSet);
  server.on("/api/cfg/baud",   HTTP_GET, handleCfgBaud);
  server.on("/api/cfg/preset", HTTP_GET, handleCfgPreset);
  server.on("/api/cfg/ble",    HTTP_GET, handleCfgBle);
  server.on("/api/reboot",     HTTP_GET, handleReboot);
  server.on("/api/factory",    HTTP_GET, handleFactory);

  // diag
  server.on("/api/diag/ping", HTTP_GET, handleDiagPing);

  server.begin(); Serial.println("[WEB] http server started");
}

void loop() {
  while (Serial2.available()){ uint8_t b=(uint8_t)Serial2.read(); rx.push_back(b); ST.bytes_rx++; if (rx.size()>4096) rx.erase(rx.begin(), rx.begin()+2048); }
  while (tryParseOne()) {}
  server.handleClient();
  static uint32_t hb=0; if (millis()-hb>30000){ hb=millis(); Serial.printf("[HB] bytes=%lu data=%lu ack=%lu pass=%u baud=%u\n",
    (unsigned long)ST.bytes_rx,(unsigned long)ST.frames_data,(unsigned long)ST.frames_ack,(unsigned)g_passes.size(),(unsigned)g_uart_baud); }
}
