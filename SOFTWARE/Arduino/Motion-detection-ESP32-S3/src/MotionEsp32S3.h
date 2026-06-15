#pragma once

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_wifi.h>

class MotionEsp32S3 {
public:
  static constexpr uint8_t kBins = 32;
  static constexpr uint8_t kMaxLinks = 8;
  static constexpr uint16_t kMaxCsiBytes = 384;

  struct Config {
    uint16_t reportIntervalMs = 200;
    uint16_t calibrationMs = 8000;
    uint16_t staleLinkMs = 4000;
    uint8_t queueDepth = 24;
    uint8_t maxPacketsPerUpdate = 80;
    float threshold = 1.1f;
    float sensitivity = 1.0f;
    float fastAlpha = 0.35f;
    float baselineAlpha = 0.010f;
    float calibrationAlpha = 0.080f;
    float noiseAlpha = 0.050f;
    bool filterByMac = false;
    uint8_t targetMac[6] = {0, 0, 0, 0, 0, 0};
    uint8_t allowedMacCount = 0;
    uint8_t allowedMacs[kMaxLinks][6] = {{0}};
  };

  struct Reading {
    uint32_t uptimeMs = 0;
    uint32_t packets = 0;
    uint32_t dropped = 0;
    uint8_t links = 0;
    int8_t rssi = 0;
    float score = 0.0f;
    float peak = 0.0f;
    float threshold = 0.0f;
    uint8_t confidence = 0;
    bool motion = false;
    char bestMac[18] = "00:00:00:00:00:00";
  };

  struct LinkReading {
    bool active = false;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    char macText[18] = "00:00:00:00:00:00";
    uint32_t packets = 0;
    uint32_t ageMs = 0;
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint16_t csiLength = 0;
    float score = 0.0f;
    float peak = 0.0f;
    uint8_t confidence = 0;
    bool motion = false;
  };

  MotionEsp32S3();

  bool begin();
  bool begin(const Config &config);
  void end();

  bool update();
  void resetBaseline();

  const Reading &reading() const;
  bool linkReading(uint8_t index, LinkReading &out) const;
  uint8_t linkCount() const;

  bool motion() const;
  float score() const;
  float threshold() const;
  float sensitivity() const;
  uint32_t packets() const;
  uint32_t droppedPackets() const;

  void clearMacFilter();
  void setMacFilter(const uint8_t mac[6]);
  bool filterToBestLink();
  void setThreshold(float threshold);
  void setSensitivity(float sensitivity);

  String json() const;
  String fingerprintCsv(const char *label = "") const;
  String fingerprintCsvAll(const char *label = "") const;

  static String fingerprintCsvHeader();
  static String macToString(const uint8_t mac[6]);
  static const char *dashboardHtml();

private:
  struct FeaturePacket {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    bool firstWordInvalid;
    uint16_t csiLength;
    uint32_t rxTimestampUs;
    int8_t csi[kMaxCsiBytes];
  };

  struct LinkState {
    bool active = false;
    bool initialized = false;
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    uint32_t packets = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint16_t csiLength = 0;
    float fast[kBins] = {0};
    float baseline[kBins] = {0};
    float noise[kBins] = {0};
    float last[kBins] = {0};
    float score = 0.0f;
    float smoothed = 0.0f;
    float peak = 0.0f;
    bool motion = false;
  };

  static void IRAM_ATTR onCsi(void *ctx, wifi_csi_info_t *data);
  void IRAM_ATTR enqueueCsi(wifi_csi_info_t *data);
  void processPacket(const FeaturePacket &packet);
  int findOrCreateLink(const uint8_t mac[6], uint32_t nowMs);
  void resetTracking(uint32_t nowMs);
  void rebuildReading(uint32_t nowMs);
  void fillLinkReading(const LinkState &link, LinkReading &out, uint32_t nowMs) const;
  void appendFingerprintRow(String &out, const LinkState *link, uint32_t nowMs, const char *label) const;
  static void copyMacText(const uint8_t mac[6], char *out, size_t outSize);

  Config _config;
  void *_queue = nullptr;
  bool _started = false;
  bool _newReport = false;
  uint32_t _startedMs = 0;
  uint32_t _lastReportMs = 0;
  volatile uint32_t _dropped = 0;
  LinkState _links[kMaxLinks];
  Reading _reading;
};
