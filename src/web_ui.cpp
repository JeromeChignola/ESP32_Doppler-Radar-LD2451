#include <Arduino.h>

extern const char INDEX_HTML[] PROGMEM = R"HTML(

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

extern const char CONFIG_HTML[] PROGMEM = R"HTML(

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

  <div class="card" style="flex:1;min-width:320px">
    <h2>Wi‑Fi (mode Station)</h2>
    <p>Définir le SSID et le mot de passe utilisés au démarrage. Laisser le mot de passe vide si inchangé.</p>
    <label>SSID<br><input id="wifi_ssid" type="text" placeholder="mon-reseau"></label>
    <br><label>Mot de passe<br><input id="wifi_pass" type="password" placeholder="(inchangé si vide)"></label>
    <div style="margin-top:10px">
      <button onclick="wifiSave()">Sauver &amp; redémarrer</button>
      <span id="wifi_msg" style="margin-left:10px;color:#93c5fd"></span>
    </div>
  </div>


  <div class="card" style="flex:1;min-width:320px">
    <h2>MQTT (Home Assistant)</h2>
    <div class="switch"><input id="mqtt_enabled" type="checkbox"><label for="mqtt_enabled">Activer MQTT</label></div>
    <label>Hôte (broker)<br><input id="mqtt_host" type="text" placeholder="ha.local ou 192.168.x.x"></label><br>
    <label>Port<br><input id="mqtt_port" type="number" value="1883" min="1" max="65535" style="width:120px"></label><br>
    <label>Utilisateur<br><input id="mqtt_user" type="text" placeholder="(facultatif)"></label><br>
    <label>Mot de passe<br><input id="mqtt_pass" type="password" placeholder="(inchangé si vide)"></label><br>
    <label>Base topic<br><input id="mqtt_base" type="text" placeholder="radar/ld2451"></label>
    <div class="switch" style="margin-top:8px"><input id="mqtt_disc" type="checkbox" checked><label for="mqtt_disc">Découverte HA (auto-entities)</label></div>
    <div style="margin-top:10px">
      <button onclick="mqttSave()">Sauver &amp; redémarrer</button>
      <span id="mqtt_msg" style="margin-left:10px;color:#93c5fd"></span>
    </div>
  </div>


  <div class="card" style="flex:1;min-width:320px">
    <h2>Alimentation &amp; Système</h2>
    <label>CPU (MHz)<br>
      <select id="cpu">
        <option value="80">80</option>
        <option value="160">160</option>
        <option value="240" selected>240</option>
      </select>
    </label><br>
    <label>Mode de veille<br>
      <select id="slm">
        <option value="1">Modem sleep</option>
        <option value="2">Light sleep</option>
        <option value="3">Wi‑Fi OFF quand idle</option>
      </select>
    </label><br>

    <div class="switch"><input id="mdns" type="checkbox" checked><label for="mdns">Activer mDNS</label></div>
    <div class="switch"><input id="wsl" type="checkbox" checked><label for="wsl">Activer Wi‑Fi sleep</label></div>
    <label>GPIO override désactivation sleep<br><input id="gpio" type="number" value="-1" style="width:120px"></label><br>
    <div class="switch"><input id="gah" type="checkbox" checked><label for="gah">Override actif sur niveau HAUT</label></div>
    <small>Note&nbsp;: si le LD2451 n'est pas détecté, le sleep est désactivé automatiquement.</small>
    <div style="margin-top:10px">
      <button onclick="powerSave()">Sauver &amp; redémarrer</button>
      <span id="pmsg" style="margin-left:10px;color:#93c5fd"></span>
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

async function wifiLoad(){
  try{
    const r = await fetch('/api/wifi/get'); 
    if(!r.ok) return;
    const j = await r.json();
    if (j.sleep_mode) document.getElementById('slm').value = String(j.sleep_mode);
    if(j && j.ssid!==undefined){
      document.getElementById('wifi_ssid').value = j.ssid || '';
    }
  }catch(e){}
}
async function wifiSave(){
  const ssid = document.getElementById('wifi_ssid').value.trim();
  const pass = document.getElementById('wifi_pass').value; // blank => unchanged
  const msg = document.getElementById('wifi_msg');
  msg.innerText = 'Sauvegarde...';
  const p = new URLSearchParams();
  p.set('ssid', ssid);
  p.set('pass', pass);
  const r = await fetch('/api/wifi/set?' + p.toString());
  if(r.ok){
    msg.innerText = 'OK, redémarrage...';
    setTimeout(()=>{ msg.innerText='Redémarrage en cours...'; }, 200);
  }else{
    msg.innerText = 'Erreur de sauvegarde';
    setTimeout(()=>msg.innerText='', 2500);
  }
}

window.addEventListener('load', wifiLoad);

async function mqttLoad(){
  try{
    const r = await fetch('/api/mqtt/get');
    if(!r.ok) return;
    const j = await r.json();
    if (j.sleep_mode) document.getElementById('slm').value = String(j.sleep_mode);
    document.getElementById('mqtt_enabled').checked = !!j.enabled;
    document.getElementById('mqtt_host').value = j.host||'';
    document.getElementById('mqtt_port').value = j.port||1883;
    document.getElementById('mqtt_user').value = j.user||'';
    document.getElementById('mqtt_base').value = j.base||'';
    document.getElementById('mqtt_disc').checked = !!j.discovery;
  }catch(e){}
}
async function mqttSave(){
  const msg = document.getElementById('mqtt_msg');
  const en = document.getElementById('mqtt_enabled').checked ? 1 : 0;
  const host = document.getElementById('mqtt_host').value.trim();
  const port = parseInt(document.getElementById('mqtt_port').value||'1883',10);
  const user = document.getElementById('mqtt_user').value.trim();
  const pass = document.getElementById('mqtt_pass').value;
  const base = document.getElementById('mqtt_base').value.trim();
  const disc = document.getElementById('mqtt_disc').checked ? 1 : 0;
  msg.innerText = 'Sauvegarde...';
  const p = new URLSearchParams();
  p.set('enabled', en);
  p.set('host', host);
  p.set('port', port);
  p.set('user', user);
  p.set('pass', pass);
  p.set('base', base);
  p.set('disc', disc);
  const r = await fetch('/api/mqtt/set?' + p.toString());
  if (r.ok){
    msg.innerText = 'OK, redémarrage...';
    setTimeout(()=>{ msg.innerText='Redémarrage en cours...'; }, 300);
  }else{
    msg.innerText = 'Erreur';
  }
}


async function powerLoad(){
  try{
    const r = await fetch('/api/power/get');
    if(!r.ok) return;
    const j = await r.json();
    if (j.sleep_mode) document.getElementById('slm').value = String(j.sleep_mode);
    const cpuSel = document.getElementById('cpu');
    if (j.cpu_mhz) cpuSel.value = String(j.cpu_mhz);
    document.getElementById('mdns').checked = !!j.mdns;
    document.getElementById('wsl').checked = !!j.wifi_sleep;
    document.getElementById('gpio').value = (j.sleep_gpio!==undefined? j.sleep_gpio : -1);
    document.getElementById('gah').checked = !!j.sleep_gpio_ah;
  }catch(e){}
}
async function powerSave(){
  const msg = document.getElementById('pmsg');
  msg.innerText = 'Sauvegarde...';
  const p = new URLSearchParams();
  p.set('cpu', document.getElementById('cpu').value);
  p.set('mdns', document.getElementById('mdns').checked ? 1 : 0);
  p.set('wsl', document.getElementById('wsl').checked ? 1 : 0);
  p.set('gpio', document.getElementById('gpio').value);
  p.set('gah', document.getElementById('gah').checked ? 1 : 0);
  p.set('slm', document.getElementById('slm').value);
  const r = await fetch('/api/power/set?' + p.toString());
  if (r.ok){
    msg.innerText = 'OK, redémarrage...';
    setTimeout(()=>{ msg.innerText='Redémarrage en cours...'; }, 300);
  }else{
    msg.innerText = 'Erreur';
  }
}

window.addEventListener('load', mqttLoad);
window.addEventListener('load', powerLoad);
</script>
</body></html>

)HTML";
