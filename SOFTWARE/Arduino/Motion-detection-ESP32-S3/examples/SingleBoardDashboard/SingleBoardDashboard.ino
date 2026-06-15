#include <MotionEsp32S3.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

// One-board mode:
// - The ESP32-S3 joins your normal Wi-Fi.
// - The web dashboard keeps traffic flowing through the AP, and the board reads CSI
//   from received AP packets.
// - Put the AP/router and ESP32-S3 on opposite sides of the area you want to sense.

const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
const float DEFAULT_THRESHOLD = 1.1f;
const float DEFAULT_SENSITIVITY = 1.0f;

#define CSI_FILTER_NONE 0
#define CSI_FILTER_CONNECTED_AP 1
#define CSI_FILTER_CUSTOM_LIST 2

// Single-board default: filter to the router/AP that this ESP32 joined.
// Use CSI_FILTER_NONE to see every Wi-Fi transmitter on the channel.
// Use CSI_FILTER_CUSTOM_LIST and fill CSI_CUSTOM_MAC_ALLOWLIST to lock to known MACs.
#define CSI_FILTER_MODE CSI_FILTER_CONNECTED_AP
#define CSI_CUSTOM_MAC_COUNT 0
const uint8_t CSI_CUSTOM_MAC_ALLOWLIST[MotionEsp32S3::kMaxLinks][6] = {
  // {0xB0, 0x4E, 0x26, 0x6E, 0xB2, 0x73},
};

WebServer server(80);
MotionEsp32S3 motion;
Preferences preferences;
uint32_t lastUrlPrintMs = 0;

void printMac(const uint8_t mac[6]) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void applyCsiFilter(MotionEsp32S3::Config &config) {
#if CSI_FILTER_MODE == CSI_FILTER_CONNECTED_AP
  const uint8_t *bssid = WiFi.BSSID();
  if (bssid != nullptr) {
    config.filterByMac = true;
    memcpy(config.targetMac, bssid, 6);
    Serial.print("CSI filter: connected AP ");
    printMac(config.targetMac);
    Serial.println();
  }
#elif CSI_FILTER_MODE == CSI_FILTER_CUSTOM_LIST
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
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void handleRoot() {
  server.send_P(200, "text/html", MotionEsp32S3::dashboardHtml());
}

void handleMetrics() {
  motion.update();
  server.send(200, "application/json", motion.json());
}

void handleCapabilities() {
  String out = F("{\"ap_filter\":true,\"position_logger\":false,\"idle_sample_ms\":8000,"
                 "\"motion_sample_ms\":20000,\"max_links\":");
  out += static_cast<unsigned int>(MotionEsp32S3::kMaxLinks);
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

bool setConnectedApFilter() {
  const uint8_t *bssid = WiFi.BSSID();
  if (bssid == nullptr) {
    return false;
  }
  motion.setMacFilter(bssid);
  return true;
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
    if (!setConnectedApFilter()) {
      server.send(500, "text/plain", "connected AP BSSID unavailable");
      return;
    }
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

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  printDashboardUrl();

  esp_wifi_set_ps(WIFI_PS_NONE);

  MotionEsp32S3::Config config;
  config.threshold = loadFloatSetting("threshold", DEFAULT_THRESHOLD);
  config.reportIntervalMs = 200;
  config.calibrationMs = 8000;
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
  server.begin();

  lastUrlPrintMs = millis();
  Serial.println("Keep the room still for 8 seconds for baseline calibration.");
}

void loop() {
  server.handleClient();

  if (millis() - lastUrlPrintMs >= 15000) {
    lastUrlPrintMs = millis();
    printDashboardUrl();
  }

  if (motion.update()) {
    const MotionEsp32S3::Reading &r = motion.reading();
    Serial.printf("score:%.2f threshold:%.2f motion:%d confidence:%u packets:%lu links:%u best:%s rssi:%d\n",
                  r.score, r.threshold, r.motion, r.confidence,
                  static_cast<unsigned long>(r.packets), r.links, r.bestMac, r.rssi);
  }
}
