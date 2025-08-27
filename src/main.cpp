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
#include <PubSubClient.h>
#include "wifi_cfg.h"
#include "mqtt_cfg.h"

// ========================= CONFIG WIFI =========================
#include "config.h"
struct Passage;  // forward decl for prototype below
static void mqttPublishPass(const Passage& p);

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
  Passage p=*best; p.ts=nowLocal(); g_passes.push_back(p); mqttPublishPass(g_passes.back()); if (g_passes.size()>MAX_PASSES) g_passes.erase(g_passes.begin()); appendCSV(p); g_lastPassMs=nowMs;
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

// MQTT client
WiFiClient g_net;
PubSubClient g_mqtt(g_net);
static MqttCfg::Settings g_mq;
static uint32_t g_mqttNextTry = 0;

// Reboot scheduling after Wi‑Fi save
static bool g_rebootPending = false;
static uint32_t g_rebootAt = 0;

// ----------- PAGE 1 : PASSAGES & STATS (auto-
#include "web_ui.h"


// ---------------- MQTT helpers ------------------------------
static String devId(){
  String id = String((uint32_t)ESP.getEfuseMac(), HEX);
  id.toUpperCase();
  return id;
}
static String topic(const String& leaf){
  String base = g_mq.base.length()? g_mq.base : String("radar/") + devId();
  if (base.endsWith("/")) return base + leaf;
  return base + "/" + leaf;
}
static void publishJSON(const String& t, const String& json, bool retain=false){
  if (g_mqtt.connected()) g_mqtt.publish(t.c_str(), json.c_str(), retain);
}
static void publishStr(const String& t, const String& s, bool retain=false){
  if (g_mqtt.connected()) g_mqtt.publish(t.c_str(), s.c_str(), retain);
}
static void publishHAConfig(){
  if (!g_mq.discovery) return;
  String id = devId();
  String device = String("{\"identifiers\":[\"RADAR-")+id+"\"],\"name\":\"LD2451 Radar "+id+"\",\"manufacturer\":\"DIY\",\"model\":\"HLK-LD2451\"}";
  String cfgSpeed = String("{\"name\":\"Radar Speed\",\"uniq_id\":\"radar_")+id+String("_speed\",\"stat_t\":\"")+topic("last")+String("\",\"json_attr_t\":\"")+topic("last")+String("\",\"unit_of_meas\":\"km/h\",\"val_tpl\":\"{{ value_json.speed_kmh }}\",\"avty_t\":\"")+topic("status")+String("\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"device\":")+device+"}";
  publishStr(String("homeassistant/sensor/")+id+String("/speed/config"), cfgSpeed, true);
  String cfgDist = String("{\"name\":\"Radar Distance\",\"uniq_id\":\"radar_")+id+String("_dist\",\"stat_t\":\"")+topic("last")+String("\",\"unit_of_meas\":\"m\",\"val_tpl\":\"{{ value_json.dist_m }}\",\"avty_t\":\"")+topic("status")+String("\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"device\":")+device+"}";
  publishStr(String("homeassistant/sensor/")+id+String("/distance/config"), cfgDist, true);
  String cfgAng = String("{\"name\":\"Radar Angle\",\"uniq_id\":\"radar_")+id+String("_ang\",\"stat_t\":\"")+topic("last")+String("\",\"unit_of_meas\":\"°\",\"val_tpl\":\"{{ value_json.angle }}\",\"avty_t\":\"")+topic("status")+String("\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"device\":")+device+"}";
  publishStr(String("homeassistant/sensor/")+id+String("/angle/config"), cfgAng, true);
  String cfgCnt = String("{\"name\":\"Radar Passes\",\"uniq_id\":\"radar_")+id+String("_count\",\"stat_t\":\"")+topic("count")+String("\",\"avty_t\":\"")+topic("status")+String("\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"device\":")+device+"}";
  publishStr(String("homeassistant/sensor/")+id+String("/count/config"), cfgCnt, true);
}
static void mqttOnConnect(){
  publishStr(topic("status"), "online", true);
  publishHAConfig();
  publishStr(topic("count"), String((unsigned)g_passes.size()), true);
}
static void mqttEnsureConnected(){
  if (!g_mq.enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_mqtt.connected()) { g_mqtt.loop(); return; }
  uint32_t now = millis();
  if (now < g_mqttNextTry) return;
  g_mqtt.setServer(g_mq.host.c_str(), g_mq.port ? g_mq.port : 1883);
  String willTopic = topic("status");
  g_mqtt.connect(("RADAR-"+devId()).c_str(),
                 g_mq.user.length()? g_mq.user.c_str(): nullptr,
                 g_mq.user.length()? g_mq.pass.c_str(): nullptr,
                 willTopic.c_str(), 0, true, "offline");
  g_mqttNextTry = now + 5000;
  if (g_mqtt.connected()) mqttOnConnect();
}
static void mqttPublishPass(const Passage& p){
  if (!g_mqtt.connected()) return;
  String j = String("{\"ts\":\"")+fmtDate(p.ts)+"\",\"dir\":"+(p.dir?String(1):String(0))+
             ",\"speed_kmh\":"+String(p.speed_kmh)+",\"dist_m\":"+String(p.dist_m)+
             ",\"angle\":"+String((int)p.angle)+",\"snr\":"+String(p.snr)+"}";
  publishJSON(topic("last"), j, true);
  publishStr(topic("count"), String((unsigned)g_passes.size()), true);
}

// ----------- PAGE 2 : CONFIGURATION (pas d’au
// ---------------- API Passages / Options -----------------------
void handleWifiGet();
void handleWifiSet();
void handleMqttGet();
void handleMqttSet();
static void mqttPublishPass(const Passage& p);
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
  WiFi.mode(WIFI_STA); auto _c = WifiCfg::load();
  String _ssid = _c.ssid.length()? _c.ssid : String(WIFI_SSID);
  String _pass = _c.ssid.length()? _c.pass : String(WIFI_PASS);
  WiFi.begin(_ssid.c_str(), _pass.c_str());
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
  server.on("/api/mqtt/get", HTTP_GET, handleMqttGet);
  server.on("/api/mqtt/set", HTTP_GET, handleMqttSet);

    server.on("/api/wifi/get", HTTP_GET, handleWifiGet);
  server.on("/api/wifi/set", HTTP_GET, handleWifiSet);
  g_mq = MqttCfg::load();
  server.begin(); Serial.println("[WEB] http server started");
}

void loop() {
  while (Serial2.available()){ uint8_t b=(uint8_t)Serial2.read(); rx.push_back(b); ST.bytes_rx++; if (rx.size()>4096) rx.erase(rx.begin(), rx.begin()+2048); }
  while (tryParseOne()) {}
  server.handleClient();
  mqttEnsureConnected();
  if (g_rebootPending && millis() >= g_rebootAt) { ESP.restart(); }
  static uint32_t hb=0; if (millis()-hb>30000){ hb=millis(); Serial.printf("[HB] bytes=%lu data=%lu ack=%lu pass=%u baud=%u\n",
    (unsigned long)ST.bytes_rx,(unsigned long)ST.frames_data,(unsigned long)ST.frames_ack,(unsigned)g_passes.size(),(unsigned)g_uart_baud); }
}


// ---------------- Wi‑Fi credentials API ------------------------
void handleWifiGet(){
  auto c = WifiCfg::load();
  String j = String("{\"ssid\":\"") + (c.ssid.length()?c.ssid:String("")) + "\"}";
  server.send(200, "application/json", j);
}
void handleWifiSet(){
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  auto cur = WifiCfg::load();
  if (pass.length() == 0) pass = cur.pass;
  bool ok = WifiCfg::save(ssid, pass);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
  if (ok) { g_rebootPending = true; g_rebootAt = millis() + 800; }
}


// ---------------- MQTT API (definitions) -----------------------
void handleMqttGet(){
  g_mq = MqttCfg::load();
  String j = String("{\"enabled\":") + (g_mq.enabled?"true":"false") +
             ",\"host\":\""+ g_mq.host + "\"" +
             ",\"port\":"+ String((unsigned)g_mq.port) +
             ",\"user\":\""+ g_mq.user + "\"" +
             ",\"base\":\""+ g_mq.base + "\"" +
             ",\"discovery\":"+ (g_mq.discovery?"true":"false") +
             "}";
  server.send(200, "application/json", j);
}

void handleMqttSet(){
  MqttCfg::Settings s = MqttCfg::load();
  if (server.hasArg("enabled")) s.enabled = (server.arg("enabled")=="1");
  if (server.hasArg("host"))    s.host = server.arg("host");
  if (server.hasArg("port"))    s.port = (uint16_t)constrain(server.arg("port").toInt(), 1, 65535);
  if (server.hasArg("user"))    s.user = server.arg("user");
  if (server.hasArg("pass")){   String np = server.arg("pass"); if (np.length()>0) s.pass = np; }
  if (server.hasArg("base"))    s.base = server.arg("base");
  if (server.hasArg("disc"))    s.discovery = (server.arg("disc")=="1");
  bool ok = MqttCfg::save(s);
  server.send(ok?200:500, "text/plain", ok?"OK":"ERR");
  if (ok) { g_rebootPending = true; g_rebootAt = millis() + 800; }
}

