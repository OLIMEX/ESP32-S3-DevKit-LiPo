#include <MotionEsp32S3.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <esp_wifi.h>
#include <string.h>

// Multi-board receiver:
// - Flash this sketch on one Olimex ESP32-S3-DevKit-LiPo.
// - Flash MultiBoardTransmitterNode on one or more other ESP32 boards.
// - Connect a phone/laptop to the MOTION-RX access point and open http://192.168.4.1
//
// This mode is much more repeatable than one-board mode because the transmitters
// create steady packets on a fixed channel.

const char *AP_SSID = "MOTION-RX";
const char *AP_PASS = "motion1234";
const uint8_t AP_CHANNEL = 6;
const uint8_t MAX_CLIENTS = 8;
const uint16_t UDP_PORT = 4210;
const char *POSITION_LABEL = "empty";
const float DEFAULT_THRESHOLD = 1.5f;
const float DEFAULT_SENSITIVITY = 1.0f;

#define CSI_FILTER_NONE 0
#define CSI_FILTER_CUSTOM_LIST 2

// Multi-board default: listen to all transmitters, so several transmitter nodes
// can appear as separate sensing links. To ignore phones/laptops, use
// CSI_FILTER_CUSTOM_LIST and fill CSI_CUSTOM_MAC_ALLOWLIST with transmitter MACs.
// Three to five transmitter nodes are fine. MAX_CLIENTS also includes the phone
// or laptop used to view the dashboard.
#define CSI_FILTER_MODE CSI_FILTER_NONE
#define CSI_CUSTOM_MAC_COUNT 0
const uint8_t CSI_CUSTOM_MAC_ALLOWLIST[MotionEsp32S3::kMaxLinks][6] = {
  // {0xB0, 0x4E, 0x26, 0x6E, 0xB2, 0x73},
  // {0xF0, 0xF5, 0xBD, 0x02, 0x28, 0xA0},
};

WebServer server(80);
WiFiUDP udp;
MotionEsp32S3 motion;
Preferences preferences;
uint32_t lastUrlPrintMs = 0;

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
  Serial.println("CSI filter: none, listening to all transmitters on this channel.");
#endif
}

void printDashboardUrl() {
  Serial.print("Dashboard: http://");
  Serial.print(WiFi.softAPIP());
  Serial.println("/");
}

void drainUdp() {
  while (udp.parsePacket() > 0) {
    while (udp.available()) {
      udp.read();
    }
  }
}

void handleRoot() {
  server.send_P(200, "text/html", MotionEsp32S3::dashboardHtml());
}

void handleMetrics() {
  drainUdp();
  motion.update();
  server.send(200, "application/json", motion.json());
}

void handleCapabilities() {
  String out = F("{\"ap_filter\":false,\"position_logger\":true,\"idle_sample_ms\":8000,"
                 "\"motion_sample_ms\":20000,\"max_links\":");
  out += static_cast<unsigned int>(MotionEsp32S3::kMaxLinks);
  out += F(",\"max_clients\":");
  out += static_cast<unsigned int>(MAX_CLIENTS);
  out += '}';
  server.send(200, "application/json", out);
}

void handleReset() {
  motion.resetBaseline();
  server.send(200, "text/plain", "baseline reset");
}

bool readFloatArg(const char *name, float minValue, float maxValue, float &value) {
  if (!server.hasArg(name)) {
    server.send(400, "text/plain", "missing value");
    return false;
  }

  value = server.arg(name).toFloat();
  if (value < minValue || value > maxValue) {
    server.send(400, "text/plain", "value out of range");
    return false;
  }

  return true;
}

void handleThreshold() {
  float value = 0.0f;
  if (!readFloatArg("value", 0.05f, 50.0f, value)) {
    return;
  }
  motion.setThreshold(value);
  motion.update();
  server.send(200, "application/json", motion.json());
}

void handleSensitivity() {
  float value = 0.0f;
  if (!readFloatArg("value", 0.1f, 20.0f, value)) {
    return;
  }
  motion.setSensitivity(value);
  motion.update();
  server.send(200, "application/json", motion.json());
}

float loadFloatSetting(const char *key, float fallback) {
  preferences.begin("motion", true);
  float value = preferences.getFloat(key, fallback);
  preferences.end();
  return value;
}

void saveMotionSettings() {
  esp_wifi_set_csi(false);
  delay(25);
  preferences.begin("motion", false);
  preferences.putFloat("threshold", motion.threshold());
  preferences.putFloat("sensitivity", motion.sensitivity());
  preferences.end();
  esp_wifi_set_csi(true);
}

void handleSaveSettings() {
  saveMotionSettings();
  server.send(200, "text/plain", "settings saved");
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

void handleCsvHeader() {
  server.send(200, "text/plain", MotionEsp32S3::fingerprintCsvHeader());
}

void handleCsvSample() {
  drainUdp();
  motion.update();
  String label = server.hasArg("label") ? cleanLabel(server.arg("label")) : String(POSITION_LABEL);
  server.send(200, "text/plain", motion.fingerprintCsvAll(label.c_str()));
}

void handleFilter() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "missing mode");
    return;
  }

  String mode = server.arg("mode");
  mode.toLowerCase();

  if (mode == "all" || mode == "none") {
    motion.clearMacFilter();
  } else if (mode == "best") {
    if (!motion.filterToBestLink()) {
      server.send(404, "text/plain", "no active link to focus");
      return;
    }
  } else if (mode == "ap" || mode == "router") {
    server.send(400, "text/plain", "router/AP filter is only available in station-mode examples");
    return;
  } else {
    server.send(400, "text/plain", "unknown filter mode");
    return;
  }

  motion.update();
  server.send(200, "application/json", motion.json());
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
  config.threshold = loadFloatSetting("threshold", DEFAULT_THRESHOLD);
  config.reportIntervalMs = 150;
  config.calibrationMs = 8000;
  config.staleLinkMs = 2500;
  config.queueDepth = 32;
  config.maxPacketsPerUpdate = 120;
  config.sensitivity = loadFloatSetting("sensitivity", DEFAULT_SENSITIVITY);
  applyCsiFilter(config);

  if (!motion.begin(config)) {
    Serial.println("Failed to start CSI motion detector.");
    while (true) {
      delay(1000);
    }
  }

  server.on("/", handleRoot);
  server.on("/metrics", handleMetrics);
  server.on("/capabilities", handleCapabilities);
  server.on("/reset", handleReset);
  server.on("/threshold", handleThreshold);
  server.on("/sensitivity", handleSensitivity);
  server.on("/save", handleSaveSettings);
  server.on("/filter", handleFilter);
  server.on("/header", handleCsvHeader);
  server.on("/sample", handleCsvSample);
  server.begin();

  Serial.println("Receiver access point started.");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Password: ");
  Serial.println(AP_PASS);
  printDashboardUrl();
  lastUrlPrintMs = millis();
  Serial.println("Connect transmitter nodes first, then keep the room still and use dashboard calibration.");
}

void loop() {
  drainUdp();
  server.handleClient();

  if (millis() - lastUrlPrintMs >= 15000) {
    lastUrlPrintMs = millis();
    printDashboardUrl();
  }

  if (motion.update()) {
    const MotionEsp32S3::Reading &r = motion.reading();
    Serial.printf("score:%.2f threshold:%.2f motion:%d confidence:%u packets:%lu links:%u best:%s rssi:%d dropped:%lu\n",
                  r.score, r.threshold, r.motion, r.confidence,
                  static_cast<unsigned long>(r.packets), r.links, r.bestMac, r.rssi,
                  static_cast<unsigned long>(r.dropped));
  }
}
