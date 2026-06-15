#include <MotionEsp32S3.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <esp_wifi.h>
#include <string.h>

// Static-position experiment helper:
// Run this as the receiver, connect your phone to MOTION-RX, open
// http://192.168.4.1, choose a label, and record/download CSV from the browser.
//
// For repeatable data, use one or more MultiBoardTransmitterNode boards around the room.

const char *AP_SSID = "MOTION-RX";
const char *AP_PASS = "motion1234";
const uint8_t AP_CHANNEL = 6;
const uint8_t MAX_CLIENTS = 8;
const uint16_t UDP_PORT = 4210;
const char *POSITION_LABEL = "empty";
const float DEFAULT_THRESHOLD = 1.5f;
const uint32_t SERIAL_URL_INTERVAL_MS = 15000;

// Set to 1 if you also want CSV rows printed over USB Serial.
#define PRINT_SERIAL_CSV 0

#define CSI_FILTER_NONE 0
#define CSI_FILTER_CUSTOM_LIST 2

// CSV logger default: listen to all transmitter nodes.
// Three to five transmitter nodes are fine; use the custom allowlist if phones
// or other clients appear as unwanted links.
// To ignore phones/laptops or lock to known nodes, use CSI_FILTER_CUSTOM_LIST
// and fill CSI_CUSTOM_MAC_ALLOWLIST with transmitter STA MAC addresses.
#define CSI_FILTER_MODE CSI_FILTER_NONE
#define CSI_CUSTOM_MAC_COUNT 0
const uint8_t CSI_CUSTOM_MAC_ALLOWLIST[MotionEsp32S3::kMaxLinks][6] = {
  // {0xB0, 0x4E, 0x26, 0x6E, 0xB2, 0x73},
  // {0xF0, 0xF5, 0xBD, 0x02, 0x28, 0xA0},
};

WebServer server(80);
WiFiUDP udp;
MotionEsp32S3 motion;
uint32_t lastCsvMs = 0;
uint32_t lastUrlPrintMs = 0;

const char LOGGER_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CSI Fingerprint Logger</title>
<style>
:root{color-scheme:dark;--bg:#101317;--panel:#181d22;--line:#2b333b;--text:#e9eef2;--muted:#91a0ad;--ok:#2dd4bf;--hot:#ef4444;--blue:#60a5fa}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,Segoe UI,sans-serif}
main{width:min(820px,100%);margin:0 auto;padding:14px}header{display:flex;justify-content:space-between;align-items:flex-end;gap:12px;border-bottom:1px solid var(--line);padding-bottom:12px}
h1{font-size:22px;margin:0}.sub{color:var(--muted);font-size:13px;margin-top:4px}.state{min-width:96px;text-align:right;font-weight:800;color:var(--ok)}.state.hot{color:var(--hot)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:12px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px}
.metric{display:flex;justify-content:space-between;border-top:1px solid var(--line);padding-top:9px;margin-top:9px;font-size:14px}.metric:first-child{border-top:0;margin-top:0;padding-top:0}.metric span{color:var(--muted)}
label{display:block;color:var(--muted);font-size:13px;margin-bottom:6px}input,select{width:100%;background:#0b0e11;color:var(--text);border:1px solid var(--line);border-radius:6px;padding:10px;font-size:16px}
.chips{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:8px}.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-top:10px}
button{width:100%;min-width:0;min-height:40px;background:#223042;color:var(--text);border:1px solid #34465b;border-radius:6px;padding:10px 10px;cursor:pointer;font-size:14px;white-space:normal;overflow-wrap:anywhere;line-height:1.2}button.primary{background:#164e63;border-color:#1b7891}button.danger{background:#3b1717;border-color:#7f1d1d}button:hover{background:#2a3a50}
.status{color:var(--muted);font-size:13px;line-height:1.35;margin-top:10px;min-height:36px}.wide{grid-column:1/-1}
@media(max-width:680px){.grid{grid-template-columns:1fr}.chips{grid-template-columns:repeat(2,minmax(0,1fr))}header{align-items:flex-start}.state{text-align:left}}
</style>
</head>
<body>
<main>
  <header>
    <div><h1>CSI Fingerprint Logger</h1><div class="sub" id="meta">Waiting for receiver...</div></div>
    <div class="state" id="state">IDLE</div>
  </header>
  <section class="grid">
    <div class="panel">
      <label for="labelInput">Position label</label>
      <input id="labelInput" value="empty" title="Label for this training run, such as empty, door, desk, bed, or center.">
      <div class="chips">
        <button onclick="setLabel('empty')" title="Room empty / no person present.">empty</button>
        <button onclick="setLabel('door')" title="Person near the door zone.">door</button>
        <button onclick="setLabel('desk')" title="Person near the desk zone.">desk</button>
        <button onclick="setLabel('center')" title="Person in the center zone.">center</button>
        <button onclick="setLabel('bed')" title="Person near the bed zone.">bed</button>
        <button onclick="setLabel('sofa')" title="Person near the sofa zone.">sofa</button>
      </div>
      <div class="actions">
        <button class="primary" onclick="startRun(60)" title="Record this label for 60 seconds. Keep board positions fixed.">Start 60s</button>
        <button onclick="startRun(120)" title="Record this label for 120 seconds for a stronger dataset.">Start 120s</button>
        <button onclick="stopRun()" title="Stop recording but keep collected rows in the browser.">Stop</button>
        <button onclick="resetBaseline()" title="Reset CSI baseline. Keep the room still for a few seconds afterward.">Reset baseline</button>
        <button onclick="downloadCsv()" title="Download all rows collected in this browser session. Each active transmitter link can add one row per sample.">Download CSV</button>
        <button class="danger" onclick="clearRows()" title="Clear rows stored in this browser session.">Clear</button>
      </div>
      <div class="status" id="runStatus">Keep boards fixed. Each active transmitter link can add one CSV row per sample.</div>
    </div>
    <div class="panel">
      <div class="metric"><span>Rows</span><b id="rows">0</b></div>
      <div class="metric"><span>Score</span><b id="score">0.00</b></div>
      <div class="metric"><span>Motion</span><b id="motion">0</b></div>
      <div class="metric"><span>Links</span><b id="links">0</b></div>
      <div class="metric"><span>Best link</span><b id="best">--</b></div>
      <div class="metric"><span>RSSI</span><b id="rssi">--</b></div>
      <div class="metric"><span>Packets</span><b id="packets">0</b></div>
      <div class="metric"><span>Dropped</span><b id="dropped">0</b></div>
      <div class="metric"><span>Heap</span><b id="heap">--</b></div>
    </div>
  </section>
</main>
<script>
let header='', rows=[], recording=false, runEnds=0, lastSample=0, busy=false;
function cleanLabel(v){return (v||'empty').replace(/[^a-zA-Z0-9 _-]/g,'_').trim()||'empty';}
function setLabel(v){document.getElementById('labelInput').value=v;}
function status(text){document.getElementById('runStatus').textContent=text;}
async function ensureHeader(){
  if(header) return;
  header=await (await fetch('/header',{cache:'no-store'})).text();
}
async function startRun(seconds){
  await ensureHeader();
  recording=true; runEnds=Date.now()+seconds*1000;
  status(`Recording ${cleanLabel(document.getElementById('labelInput').value)} for ${seconds}s.`);
}
function stopRun(){recording=false; status('Recording stopped. You can change label, continue, or download CSV.');}
function clearRows(){rows=[]; document.getElementById('rows').textContent='0'; status('Rows cleared in browser.');}
async function resetBaseline(){await fetch('/reset',{cache:'no-store'}); status('Baseline reset. Keep the room still briefly.');}
async function sample(){
  const label=cleanLabel(document.getElementById('labelInput').value);
  const text=await (await fetch(`/sample?label=${encodeURIComponent(label)}`,{cache:'no-store'})).text();
  text.trim().split(/\r?\n/).filter(Boolean).forEach(line=>rows.push(line));
  document.getElementById('rows').textContent=rows.length;
}
async function metrics(){
  try{
    const m=await (await fetch('/metrics',{cache:'no-store'})).json();
    document.getElementById('score').textContent=m.score.toFixed(2);
    document.getElementById('motion').textContent=m.motion?1:0;
    document.getElementById('links').textContent=m.links;
    document.getElementById('best').textContent=m.best_mac||'--';
    document.getElementById('rssi').textContent=m.rssi?`${m.rssi} dBm`:'--';
    document.getElementById('packets').textContent=m.packets;
    document.getElementById('dropped').textContent=m.dropped;
    document.getElementById('heap').textContent=m.heap_free?`${Math.round(m.heap_free/1024)}K / ${Math.round((m.heap_min||0)/1024)}K`:'--';
    document.getElementById('meta').textContent=`${m.links} link(s), uptime ${(m.uptime_ms/1000).toFixed(0)}s`;
    const st=document.getElementById('state'); st.textContent=recording?'REC':(m.motion?'MOTION':'IDLE'); st.className='state '+(m.motion?'hot':'');
  }catch(e){document.getElementById('meta').textContent='Disconnected';}
}
async function tick(){
  if(busy) return;
  busy=true;
  try{
    await metrics();
    if(recording){
      if(Date.now()>=runEnds){stopRun(); return;}
      if(Date.now()-lastSample>=200){lastSample=Date.now(); await sample();}
      const left=Math.max(0,Math.ceil((runEnds-Date.now())/1000));
      status(`Recording ${cleanLabel(document.getElementById('labelInput').value)}: ${left}s left.`);
    }
  }finally{
    busy=false;
  }
}
async function downloadCsv(){
  await ensureHeader();
  const body=(header||'')+'\n'+rows.join('\n')+'\n';
  const label=cleanLabel(document.getElementById('labelInput').value);
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([body],{type:'text/csv'}));
  a.download=`csi-${label}-${new Date().toISOString().replace(/[:.]/g,'-')}.csv`;
  a.click(); URL.revokeObjectURL(a.href);
}
setInterval(tick,250); tick();
</script>
</body>
</html>
)HTML";

void printDashboardUrl() {
  Serial.print("Logger: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");
}

void applyCsiFilter(MotionEsp32S3::Config &config) {
#if CSI_FILTER_MODE == CSI_FILTER_CUSTOM_LIST
  config.filterByMac = true;
  config.allowedMacCount = CSI_CUSTOM_MAC_COUNT;
  if (config.allowedMacCount > MotionEsp32S3::kMaxLinks) {
    config.allowedMacCount = MotionEsp32S3::kMaxLinks;
  }
  for (uint8_t i = 0; i < config.allowedMacCount; i++) {
    memcpy(config.allowedMacs[i], CSI_CUSTOM_MAC_ALLOWLIST[i], 6);
  }
  Serial.print("CSI filter: custom MAC allowlist, count ");
  Serial.println(config.allowedMacCount);
#else
  Serial.println("CSI filter: none, logging all transmitters on this channel.");
#endif
}

void drainUdp() {
  while (udp.parsePacket() > 0) {
    while (udp.available()) {
      udp.read();
    }
  }
}

String cleanLabel(String label) {
  label.trim();
  if (label.length() == 0) {
    label = POSITION_LABEL;
  }
  for (size_t i = 0; i < label.length(); i++) {
    const char c = label.charAt(i);
    const bool ok = isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
    if (!ok) {
      label.setCharAt(i, '_');
    }
  }
  return label;
}

void handleRoot() {
  server.send_P(200, "text/html", LOGGER_HTML);
}

void handleHeader() {
  server.send(200, "text/plain", MotionEsp32S3::fingerprintCsvHeader());
}

void handleSample() {
  drainUdp();
  motion.update();
  String label = server.hasArg("label") ? cleanLabel(server.arg("label")) : String(POSITION_LABEL);
  server.send(200, "text/plain", motion.fingerprintCsvAll(label.c_str()));
}

void handleMetrics() {
  drainUdp();
  motion.update();
  server.send(200, "application/json", motion.json());
}

void handleReset() {
  motion.resetBaseline();
  server.send(200, "text/plain", "baseline reset");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false, MAX_CLIENTS);
  if (!apOk) {
    Serial.println("Failed to start SoftAP.");
    while (true) {
      delay(1000);
    }
  }
  esp_wifi_set_ps(WIFI_PS_NONE);
  udp.begin(UDP_PORT);

  MotionEsp32S3::Config config;
  config.threshold = DEFAULT_THRESHOLD;
  config.reportIntervalMs = 100;
  config.calibrationMs = 8000;
  config.staleLinkMs = 2500;
  config.queueDepth = 32;
  config.maxPacketsPerUpdate = 120;
  applyCsiFilter(config);

  if (!motion.begin(config)) {
    Serial.println("Failed to start CSI motion detector.");
    while (true) {
      delay(1000);
    }
  }

  server.on("/", handleRoot);
  server.on("/header", handleHeader);
  server.on("/sample", handleSample);
  server.on("/metrics", handleMetrics);
  server.on("/reset", handleReset);
  server.begin();

  Serial.println(MotionEsp32S3::fingerprintCsvHeader());
  Serial.println("Connect transmitter nodes first, then keep the room still during calibration.");
  Serial.print("Motion threshold used for the CSV motion column: ");
  Serial.println(DEFAULT_THRESHOLD, 2);
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  printDashboardUrl();
  lastUrlPrintMs = millis();
}

void loop() {
  drainUdp();
  server.handleClient();
  motion.update();

#if PRINT_SERIAL_CSV
  const uint32_t nowMs = millis();
  if (nowMs - lastCsvMs >= 200) {
    lastCsvMs = nowMs;
    Serial.println(motion.fingerprintCsvAll(POSITION_LABEL));
  }
#endif

  if (millis() - lastUrlPrintMs >= SERIAL_URL_INTERVAL_MS) {
    lastUrlPrintMs = millis();
    printDashboardUrl();
  }
}
