#include "MotionEsp32S3.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace {
const float kNoiseFloor = 2.0f;

uint8_t clampConfidence(float score, float threshold) {
  if (threshold <= 0.0f) {
    return 0;
  }
  const float raw = (score / threshold) * 100.0f;
  if (raw <= 0.0f) {
    return 0;
  }
  if (raw >= 100.0f) {
    return 100;
  }
  return static_cast<uint8_t>(raw + 0.5f);
}

String jsonBool(bool value) {
  return value ? F("true") : F("false");
}

void appendCsvLabel(String &out, const char *label) {
  if (label == nullptr) {
    return;
  }
  while (*label != '\0') {
    const char c = *label++;
    if (c == ',' || c == '"' || c == '\r' || c == '\n') {
      out += '_';
    } else {
      out += c;
    }
  }
}
}  // namespace

MotionEsp32S3::MotionEsp32S3() {
}

bool MotionEsp32S3::begin() {
  Config config;
  return begin(config);
}

bool MotionEsp32S3::begin(const Config &config) {
  end();

  _config = config;
  _startedMs = millis();
  _lastReportMs = _startedMs;
  _dropped = 0;
  _newReport = false;
  memset(_links, 0, sizeof(_links));
  _reading = Reading();
  _reading.threshold = _config.threshold;

  _queue = xQueueCreate(_config.queueDepth, sizeof(FeaturePacket));
  if (_queue == nullptr) {
    return false;
  }

  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);

  wifi_csi_config_t csiConfig;
  memset(&csiConfig, 0, sizeof(csiConfig));
  csiConfig.lltf_en = true;
  csiConfig.htltf_en = true;
  csiConfig.stbc_htltf2_en = true;
  csiConfig.ltf_merge_en = true;
  csiConfig.channel_filter_en = false;
  csiConfig.manu_scale = false;
  csiConfig.shift = 0;

  esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) {
    end();
    return false;
  }

  err = esp_wifi_set_csi_rx_cb(&MotionEsp32S3::onCsi, this);
  if (err != ESP_OK) {
    end();
    return false;
  }

  err = esp_wifi_set_csi_config(&csiConfig);
  if (err != ESP_OK) {
    end();
    return false;
  }

  err = esp_wifi_set_csi(true);
  if (err != ESP_OK) {
    end();
    return false;
  }

  _started = true;
  return true;
}

void MotionEsp32S3::end() {
  esp_wifi_set_csi(false);
  esp_wifi_set_csi_rx_cb(nullptr, nullptr);
  esp_wifi_set_promiscuous(false);

  if (_queue != nullptr) {
    vQueueDelete(static_cast<QueueHandle_t>(_queue));
    _queue = nullptr;
  }

  _started = false;
}

bool MotionEsp32S3::update() {
  if (!_started || _queue == nullptr) {
    return false;
  }

  FeaturePacket packet;
  uint8_t processed = 0;
  while (processed < _config.maxPacketsPerUpdate &&
         xQueueReceive(static_cast<QueueHandle_t>(_queue), &packet, 0) == pdTRUE) {
    processPacket(packet);
    processed++;
  }

  const uint32_t nowMs = millis();
  if (nowMs - _lastReportMs >= _config.reportIntervalMs) {
    rebuildReading(nowMs);
    _lastReportMs = nowMs;
    _newReport = true;
    return true;
  }

  _newReport = false;
  return false;
}

void MotionEsp32S3::resetBaseline() {
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    LinkState &link = _links[i];
    if (!link.active) {
      continue;
    }
    for (uint8_t bin = 0; bin < kBins; bin++) {
      link.baseline[bin] = link.last[bin];
      link.fast[bin] = link.last[bin];
      link.noise[bin] = kNoiseFloor;
    }
    link.firstSeenMs = nowMs;
    link.score = 0.0f;
    link.smoothed = 0.0f;
    link.motion = false;
  }
  _startedMs = nowMs;
  rebuildReading(nowMs);
}

void MotionEsp32S3::resetTracking(uint32_t nowMs) {
  if (_queue != nullptr) {
    xQueueReset(static_cast<QueueHandle_t>(_queue));
  }
  memset(_links, 0, sizeof(_links));
  _reading = Reading();
  _reading.threshold = _config.threshold;
  _startedMs = nowMs;
  _lastReportMs = nowMs;
}

const MotionEsp32S3::Reading &MotionEsp32S3::reading() const {
  return _reading;
}

bool MotionEsp32S3::linkReading(uint8_t index, LinkReading &out) const {
  uint8_t activeIndex = 0;
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    const LinkState &link = _links[i];
    if (!link.active || nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }
    if (activeIndex == index) {
      fillLinkReading(link, out, nowMs);
      return true;
    }
    activeIndex++;
  }
  return false;
}

uint8_t MotionEsp32S3::linkCount() const {
  uint8_t count = 0;
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    if (_links[i].active && nowMs - _links[i].lastSeenMs <= _config.staleLinkMs) {
      count++;
    }
  }
  return count;
}

bool MotionEsp32S3::motion() const {
  return _reading.motion;
}

float MotionEsp32S3::score() const {
  return _reading.score;
}

float MotionEsp32S3::threshold() const {
  return _config.threshold;
}

float MotionEsp32S3::sensitivity() const {
  return _config.sensitivity;
}

uint32_t MotionEsp32S3::packets() const {
  return _reading.packets;
}

uint32_t MotionEsp32S3::droppedPackets() const {
  return _dropped;
}

void MotionEsp32S3::clearMacFilter() {
  const bool restartCsi = _started;
  if (restartCsi) {
    esp_wifi_set_csi(false);
  }
  _config.filterByMac = false;
  _config.allowedMacCount = 0;
  memset(_config.targetMac, 0, sizeof(_config.targetMac));
  memset(_config.allowedMacs, 0, sizeof(_config.allowedMacs));
  resetTracking(millis());
  if (restartCsi) {
    esp_wifi_set_csi(true);
  }
}

void MotionEsp32S3::setMacFilter(const uint8_t mac[6]) {
  if (mac == nullptr) {
    clearMacFilter();
    return;
  }
  const bool restartCsi = _started;
  if (restartCsi) {
    esp_wifi_set_csi(false);
  }
  _config.filterByMac = true;
  _config.allowedMacCount = 0;
  memcpy(_config.targetMac, mac, 6);
  memset(_config.allowedMacs, 0, sizeof(_config.allowedMacs));
  resetTracking(millis());
  if (restartCsi) {
    esp_wifi_set_csi(true);
  }
}

bool MotionEsp32S3::filterToBestLink() {
  const uint32_t nowMs = millis();
  LinkState *best = nullptr;
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    LinkState &link = _links[i];
    if (!link.active || nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }
    if (best == nullptr || link.smoothed > best->smoothed) {
      best = &link;
    }
  }

  if (best == nullptr) {
    return false;
  }

  uint8_t mac[6];
  memcpy(mac, best->mac, 6);
  setMacFilter(mac);
  return true;
}

void MotionEsp32S3::setThreshold(float threshold) {
  _config.threshold = threshold;
  _reading.threshold = threshold;
}

void MotionEsp32S3::setSensitivity(float sensitivity) {
  _config.sensitivity = sensitivity;
}

String MotionEsp32S3::json() const {
  String out;
  out.reserve(512 + kMaxLinks * 160);
  out += F("{\"uptime_ms\":");
  out += _reading.uptimeMs;
  out += F(",\"motion\":");
  out += jsonBool(_reading.motion);
  out += F(",\"score\":");
  out += String(_reading.score, 2);
  out += F(",\"peak\":");
  out += String(_reading.peak, 2);
  out += F(",\"threshold\":");
  out += String(_reading.threshold, 2);
  out += F(",\"sensitivity\":");
  out += String(_config.sensitivity, 2);
  out += F(",\"filter_enabled\":");
  out += jsonBool(_config.filterByMac);
  out += F(",\"filter_mode\":\"");
  if (!_config.filterByMac) {
    out += F("all");
  } else if (_config.allowedMacCount > 0) {
    out += F("list");
  } else {
    out += F("single");
  }
  out += F("\",\"filter_mac\":\"");
  char filterMac[18];
  copyMacText(_config.targetMac, filterMac, sizeof(filterMac));
  out += filterMac;
  out += F("\",\"filter_count\":");
  out += static_cast<unsigned int>(_config.allowedMacCount);
  out += F(",\"heap_free\":");
  out += ESP.getFreeHeap();
  out += F(",\"heap_min\":");
  out += ESP.getMinFreeHeap();
  out += F(",\"confidence\":");
  out += static_cast<unsigned int>(_reading.confidence);
  out += F(",\"packets\":");
  out += _reading.packets;
  out += F(",\"dropped\":");
  out += _reading.dropped;
  out += F(",\"links\":");
  out += static_cast<unsigned int>(_reading.links);
  out += F(",\"best_mac\":\"");
  out += _reading.bestMac;
  out += F("\",\"rssi\":");
  out += static_cast<int>(_reading.rssi);
  out += F(",\"link_data\":[");

  bool first = true;
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    const LinkState &link = _links[i];
    if (!link.active || nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }

    char mac[18];
    copyMacText(link.mac, mac, sizeof(mac));

    if (!first) {
      out += ',';
    }
    first = false;
    out += F("{\"mac\":\"");
    out += mac;
    out += F("\",\"packets\":");
    out += link.packets;
    out += F(",\"age_ms\":");
    out += nowMs - link.lastSeenMs;
    out += F(",\"rssi\":");
    out += static_cast<int>(link.rssi);
    out += F(",\"channel\":");
    out += static_cast<unsigned int>(link.channel);
    out += F(",\"csi_len\":");
    out += link.csiLength;
    out += F(",\"score\":");
    out += String(link.smoothed, 2);
    out += F(",\"peak\":");
    out += String(link.peak, 2);
    out += F(",\"confidence\":");
    out += static_cast<unsigned int>(clampConfidence(link.smoothed, _config.threshold));
    out += F(",\"motion\":");
    out += jsonBool(link.motion);
    out += '}';
  }
  out += F("]}");
  return out;
}

String MotionEsp32S3::fingerprintCsv(const char *label) const {
  const LinkState *best = nullptr;
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    const LinkState &link = _links[i];
    if (!link.active || nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }
    if (best == nullptr || link.smoothed > best->smoothed) {
      best = &link;
    }
  }

  String out;
  out.reserve(360);
  appendFingerprintRow(out, best, nowMs, label);
  return out;
}

String MotionEsp32S3::fingerprintCsvAll(const char *label) const {
  const uint32_t nowMs = millis();
  String out;
  out.reserve(360 * kMaxLinks);

  bool first = true;
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    const LinkState &link = _links[i];
    if (!link.active || nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }
    if (!first) {
      out += '\n';
    }
    first = false;
    appendFingerprintRow(out, &link, nowMs, label);
  }

  if (first) {
    appendFingerprintRow(out, nullptr, nowMs, label);
  }
  return out;
}

String MotionEsp32S3::fingerprintCsvHeader() {
  String out = F("ms,label,mac,rssi,score,motion");
  for (uint8_t i = 0; i < kBins; i++) {
    out += F(",bin_");
    if (i < 10) {
      out += '0';
    }
    out += static_cast<unsigned int>(i);
  }
  return out;
}

String MotionEsp32S3::macToString(const uint8_t mac[6]) {
  char text[18];
  copyMacText(mac, text, sizeof(text));
  return String(text);
}

void IRAM_ATTR MotionEsp32S3::onCsi(void *ctx, wifi_csi_info_t *data) {
  if (ctx == nullptr || data == nullptr) {
    return;
  }
  static_cast<MotionEsp32S3 *>(ctx)->enqueueCsi(data);
}

void IRAM_ATTR MotionEsp32S3::enqueueCsi(wifi_csi_info_t *data) {
  if (_queue == nullptr || data->buf == nullptr || data->len < 8) {
    return;
  }

  if (_config.filterByMac) {
    bool allowed = false;
    if (_config.allowedMacCount > 0) {
      const uint8_t count = _config.allowedMacCount > kMaxLinks ? kMaxLinks : _config.allowedMacCount;
      for (uint8_t i = 0; i < count; i++) {
        if (memcmp(data->mac, _config.allowedMacs[i], 6) == 0) {
          allowed = true;
          break;
        }
      }
    } else {
      allowed = memcmp(data->mac, _config.targetMac, 6) == 0;
    }

    if (!allowed) {
      return;
    }
  }

  FeaturePacket packet;
  memset(&packet, 0, sizeof(packet));
  memcpy(packet.mac, data->mac, sizeof(packet.mac));
  packet.rssi = data->rx_ctrl.rssi;
  packet.channel = data->rx_ctrl.channel;
  packet.firstWordInvalid = data->first_word_invalid;
  packet.csiLength = data->len;
  packet.rxTimestampUs = data->rx_ctrl.timestamp;
  if (packet.csiLength > kMaxCsiBytes) {
    packet.csiLength = kMaxCsiBytes;
  }
  memcpy(packet.csi, data->buf, packet.csiLength);

  if (xQueueSend(static_cast<QueueHandle_t>(_queue), &packet, 0) != pdTRUE) {
    _dropped++;
  }
}

void MotionEsp32S3::processPacket(const FeaturePacket &packet) {
  const uint32_t nowMs = millis();
  const int index = findOrCreateLink(packet.mac, nowMs);
  if (index < 0) {
    _dropped++;
    return;
  }

  LinkState &link = _links[index];
  link.packets++;
  link.lastSeenMs = nowMs;
  link.rssi = packet.rssi;
  link.channel = packet.channel;
  link.csiLength = packet.csiLength;

  const int start = packet.firstWordInvalid ? 4 : 0;
  const int usable = packet.csiLength - start;
  const int pairCount = usable / 2;
  if (pairCount <= 0) {
    return;
  }

  uint16_t bins[kBins] = {0};
  uint32_t sums[kBins] = {0};
  uint8_t counts[kBins] = {0};

  for (int pair = 0; pair < pairCount; pair++) {
    const int offset = start + pair * 2;
    const int8_t imag = packet.csi[offset];
    const int8_t real = packet.csi[offset + 1];
    const uint16_t mag = static_cast<uint16_t>(abs(real)) + static_cast<uint16_t>(abs(imag));
    uint8_t bin = static_cast<uint8_t>((pair * kBins) / pairCount);
    if (bin >= kBins) {
      bin = kBins - 1;
    }
    sums[bin] += mag;
    counts[bin]++;
  }

  for (uint8_t bin = 0; bin < kBins; bin++) {
    bins[bin] = counts[bin] == 0 ? 0 : static_cast<uint16_t>(sums[bin] / counts[bin]);
  }

  if (!link.initialized) {
    for (uint8_t bin = 0; bin < kBins; bin++) {
      const float sample = static_cast<float>(bins[bin]);
      link.last[bin] = sample;
      link.fast[bin] = sample;
      link.baseline[bin] = sample;
      link.noise[bin] = kNoiseFloor;
    }
    link.initialized = true;
    return;
  }

  float rawScore = 0.0f;
  for (uint8_t bin = 0; bin < kBins; bin++) {
    const float sample = static_cast<float>(bins[bin]);
    link.last[bin] = sample;
    link.fast[bin] += _config.fastAlpha * (sample - link.fast[bin]);
    const float delta = fabsf(link.fast[bin] - link.baseline[bin]);
    rawScore += delta / (link.noise[bin] + kNoiseFloor);
  }

  rawScore = (rawScore / static_cast<float>(kBins)) * _config.sensitivity;

  const bool calibrating = (nowMs - _startedMs < _config.calibrationMs) ||
                           (nowMs - link.firstSeenMs < _config.calibrationMs) ||
                           (link.packets < 30);
  float baselineAlpha = calibrating ? _config.calibrationAlpha : _config.baselineAlpha;
  const bool likelyMotion = rawScore > _config.threshold;
  if (likelyMotion && !calibrating) {
    baselineAlpha *= 0.12f;
  }

  for (uint8_t bin = 0; bin < kBins; bin++) {
    const float sample = static_cast<float>(bins[bin]);
    const float delta = fabsf(link.fast[bin] - link.baseline[bin]);
    link.baseline[bin] += baselineAlpha * (sample - link.baseline[bin]);
    if (!likelyMotion || calibrating) {
      link.noise[bin] += _config.noiseAlpha * (delta - link.noise[bin]);
      if (link.noise[bin] < kNoiseFloor) {
        link.noise[bin] = kNoiseFloor;
      }
    }
  }

  link.score = rawScore;
  link.smoothed = (0.78f * link.smoothed) + (0.22f * rawScore);
  link.motion = link.smoothed > _config.threshold;
  if (link.smoothed > link.peak) {
    link.peak = link.smoothed;
  }
}

int MotionEsp32S3::findOrCreateLink(const uint8_t mac[6], uint32_t nowMs) {
  int freeIndex = -1;
  int stalestIndex = 0;
  uint32_t stalestAge = 0;

  for (uint8_t i = 0; i < kMaxLinks; i++) {
    LinkState &link = _links[i];
    if (link.active && memcmp(link.mac, mac, 6) == 0) {
      return i;
    }
    if (!link.active && freeIndex < 0) {
      freeIndex = i;
    }
    const uint32_t age = link.active ? nowMs - link.lastSeenMs : UINT32_MAX;
    if (age > stalestAge) {
      stalestAge = age;
      stalestIndex = i;
    }
  }

  const int index = freeIndex >= 0 ? freeIndex : stalestIndex;
  LinkState &link = _links[index];
  memset(&link, 0, sizeof(link));
  link.active = true;
  memcpy(link.mac, mac, 6);
  link.firstSeenMs = nowMs;
  link.lastSeenMs = nowMs;
  return index;
}

void MotionEsp32S3::rebuildReading(uint32_t nowMs) {
  Reading next;
  next.uptimeMs = nowMs - _startedMs;
  next.dropped = _dropped;
  next.threshold = _config.threshold;

  const LinkState *best = nullptr;
  for (uint8_t i = 0; i < kMaxLinks; i++) {
    const LinkState &link = _links[i];
    if (!link.active) {
      continue;
    }
    next.packets += link.packets;
    if (nowMs - link.lastSeenMs > _config.staleLinkMs) {
      continue;
    }
    next.links++;
    if (best == nullptr || link.smoothed > best->smoothed) {
      best = &link;
    }
  }

  if (best != nullptr) {
    next.score = best->smoothed;
    next.peak = best->peak;
    next.motion = best->motion;
    next.confidence = clampConfidence(best->smoothed, _config.threshold);
    next.rssi = best->rssi;
    copyMacText(best->mac, next.bestMac, sizeof(next.bestMac));
  }

  _reading = next;
}

void MotionEsp32S3::fillLinkReading(const LinkState &link, LinkReading &out, uint32_t nowMs) const {
  out = LinkReading();
  out.active = link.active;
  memcpy(out.mac, link.mac, 6);
  copyMacText(link.mac, out.macText, sizeof(out.macText));
  out.packets = link.packets;
  out.ageMs = nowMs - link.lastSeenMs;
  out.rssi = link.rssi;
  out.channel = link.channel;
  out.csiLength = link.csiLength;
  out.score = link.smoothed;
  out.peak = link.peak;
  out.confidence = clampConfidence(link.smoothed, _config.threshold);
  out.motion = link.motion;
}

void MotionEsp32S3::appendFingerprintRow(String &out, const LinkState *link, uint32_t nowMs, const char *label) const {
  out += nowMs;
  out += ',';
  appendCsvLabel(out, label);
  out += ',';

  if (link == nullptr) {
    out += F("00:00:00:00:00:00,0,0,0");
    for (uint8_t i = 0; i < kBins; i++) {
      out += F(",0");
    }
    return;
  }

  char mac[18];
  copyMacText(link->mac, mac, sizeof(mac));
  out += mac;
  out += ',';
  out += static_cast<int>(link->rssi);
  out += ',';
  out += String(link->smoothed, 3);
  out += ',';
  out += (link->motion ? '1' : '0');
  for (uint8_t i = 0; i < kBins; i++) {
    out += ',';
    out += String(link->last[i], 2);
  }
}

void MotionEsp32S3::copyMacText(const uint8_t mac[6], char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *MotionEsp32S3::dashboardHtml() {
  static const char html[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CSI Motion and Position Demo</title>
<style>
:root{color-scheme:dark;--bg:#101317;--panel:#181d22;--line:#2b333b;--text:#e9eef2;--muted:#91a0ad;--ok:#2dd4bf;--warn:#f59e0b;--hot:#ef4444;--blue:#60a5fa}
*{box-sizing:border-box}html{overflow-y:scroll}body{margin:0;overflow-x:hidden;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,Segoe UI,sans-serif}
.hidden{display:none!important}
main{width:min(1120px,100%);margin:0 auto;padding:18px}.top{display:flex;gap:12px;align-items:flex-end;justify-content:space-between;flex-wrap:wrap;border-bottom:1px solid var(--line);padding-bottom:14px}.top>div:first-child{min-width:0}.topRight{display:flex;align-items:flex-end;justify-content:flex-end;gap:12px;flex-wrap:wrap}
h1{font-size:22px;margin:0}.sub{color:var(--muted);font-size:13px;margin-top:4px}.modes{display:flex;gap:6px}.modeBtn{width:auto;min-width:76px}.modeBtn.active{background:#164e63;border-color:#1b7891}.status{font-size:18px;font-weight:700;color:var(--ok);width:96px;text-align:right;display:inline-block}.status.hot{color:var(--hot)}.status.rec{color:var(--blue)}
.grid{display:grid;grid-template-columns:320px 1fr;gap:14px;margin-top:14px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}
.big{font-size:56px;line-height:1;font-weight:800}.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}.row{display:flex;justify-content:space-between;gap:16px;border-top:1px solid var(--line);padding-top:10px;margin-top:10px;font-size:14px}
canvas{width:100%;height:270px;display:block;background:#0b0e11;border:1px solid var(--line);border-radius:6px}.bar{height:10px;background:#0b0e11;border-radius:999px;overflow:hidden;margin-top:8px}.fill{height:100%;width:0;background:linear-gradient(90deg,var(--ok),var(--warn),var(--hot))}
button{width:100%;min-width:0;min-height:38px;background:#223042;color:var(--text);border:1px solid #34465b;border-radius:6px;padding:9px 10px;cursor:pointer;white-space:normal;overflow-wrap:anywhere;line-height:1.2}button:hover{background:#2a3a50}button.primary{background:#164e63;border-color:#1b7891}
input{width:100%;min-width:0;background:#0b0e11;color:var(--text);border:1px solid var(--line);border-radius:6px;padding:8px 9px}.ctrl{display:grid;grid-template-columns:82px minmax(0,1fr) minmax(62px,68px);gap:8px;align-items:center;margin-top:8px}.ctrl label{color:var(--muted);font-size:13px}.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-top:10px}.subline{color:var(--muted);font-size:12px;line-height:1.35;margin-top:10px;min-height:32px}.wide{width:100%}
.chips{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:8px}
table{width:100%;table-layout:fixed;border-collapse:collapse;margin-top:10px;font-size:13px}th,td{text-align:left;padding:8px;border-bottom:1px solid var(--line);overflow:hidden;text-overflow:ellipsis}th{color:var(--muted);font-weight:600}th:last-child,td:last-child{width:86px;text-align:center}.pill{display:inline-block;width:72px;text-align:center;border-radius:999px;padding:3px 0;background:#143832;color:var(--ok)}.pill.hot{background:#3b1717;color:#fecaca}
@media(max-width:780px){.grid{grid-template-columns:1fr}.big{font-size:46px}main{padding:12px}canvas{height:230px}.ctrl{grid-template-columns:1fr 1fr}.ctrl label{grid-column:1/-1}.ctrl button{grid-column:auto}.actions{grid-template-columns:1fr}.chips{grid-template-columns:repeat(2,minmax(0,1fr))}.topRight{justify-content:flex-start}.status{text-align:left}}
</style>
</head>
<body>
<main>
  <section class="top">
    <div><h1>CSI Motion and Position Demo</h1><div class="sub" id="meta">Waiting for CSI packets...</div></div>
    <div class="topRight">
      <div class="modes hidden" id="modeTabs">
        <button class="modeBtn active" id="motionTab" onclick="setUiMode('motion')" title="Show live motion score, calibration, and per-link state.">Motion</button>
        <button class="modeBtn" id="positionTab" onclick="setUiMode('position')" title="Record labeled CSI rows for room-zone or static-position experiments.">Position</button>
      </div>
      <div class="status" id="state">CALM</div>
    </div>
  </section>
  <section class="grid" id="motionView">
    <div class="panel">
      <div class="label">Motion Score</div>
      <div class="big" id="score">0.0</div>
      <div class="bar"><div class="fill" id="bar"></div></div>
      <div class="row"><span>Threshold</span><b id="threshold">0.0</b></div>
      <div class="row"><span>Sensitivity</span><b id="sensitivity">1.00x</b></div>
      <div class="row"><span>Confidence</span><b id="confidence">0%</b></div>
      <div class="row"><span>Best link</span><b id="best">--</b></div>
      <div class="row"><span>Filter</span><b id="filter">all links</b></div>
      <div class="row"><span>RSSI</span><b id="rssi">--</b></div>
      <div class="row"><span>Packets</span><b id="packets">0</b></div>
      <div class="row"><span>Dropped</span><b id="dropped">0</b></div>
      <div class="row"><span>Heap</span><b id="heap">--</b></div>
      <div class="ctrl"><label for="thresholdInput">Threshold</label><input id="thresholdInput" type="number" min="0.05" max="50" step="0.05" title="Motion is ON when score is above this value. Lower detects smaller movement but can create false alarms."><button onclick="applyThreshold()" title="Apply this threshold immediately. Use Save settings if you want it after reboot.">Apply</button></div>
      <div class="ctrl"><label for="sensitivityInput">Sensitivity</label><input id="sensitivityInput" type="number" min="0.1" max="20" step="0.1" title="Multiplier for the score. Prefer threshold tuning first; sensitivity is mostly for scaling the displayed score."><button onclick="applySensitivity()" title="Apply this sensitivity multiplier immediately. Use Save settings if you want it after reboot.">Apply</button></div>
      <div class="actions"><button onclick="setFilter('all')" title="Accept CSI packets from every Wi-Fi transmitter heard on this channel. Useful for discovery, noisier for detection.">Show all</button><button onclick="setFilter('best')" title="Lock to the currently highest-scoring active link. Useful after you identify the path that reacts to movement.">Focus best</button></div>
      <div class="row hidden" id="apFilterRow"><button class="wide" onclick="setFilter('ap')" title="Single-board mode only: lock CSI to the connected router/AP BSSID and ignore phones or other nearby Wi-Fi devices.">Router/AP only</button></div>
      <div class="actions"><button onclick="startSample('idle')" title="Collect a still-room score sample. Keep people and moving objects out of the sensing path.">Learn idle</button><button onclick="startSample('motion')" title="Collect a longer sample while moving through the path you want to detect.">Learn motion</button></div>
      <div class="row"><span>Suggested</span><b id="suggested">--</b></div>
      <div class="actions"><button class="primary" onclick="applySuggested()" title="Use the dashboard suggestion based on idle p95 and motion p80 samples.">Apply suggested</button><button onclick="resetBaseline()" title="Reset the adaptive baseline. Keep the room still for a few seconds after pressing this.">Reset baseline</button></div>
      <div class="actions"><button onclick="saveSettings()" title="Save threshold and sensitivity in ESP32 flash. The temporary GUI filter choice is not saved.">Save settings</button><button onclick="downloadCsv()" title="Download recent dashboard samples for offline inspection.">Save CSV</button></div>
      <div class="subline" id="calStatus">Keep still after reset, then learn idle and motion.</div>
    </div>
    <div class="panel">
      <div class="label">Live Score</div>
      <canvas id="chart" width="900" height="270"></canvas>
      <table>
        <thead><tr><th>Link</th><th>Score</th><th>RSSI</th><th>Packets</th><th>Age</th><th>State</th></tr></thead>
        <tbody id="links"></tbody>
      </table>
    </div>
  </section>
  <section class="grid hidden" id="positionView">
    <div class="panel">
      <div class="label">Position Label</div>
      <input id="positionLabel" value="empty" title="Label written into CSV rows for this training run. Keep it stable while recording one room state.">
      <div class="chips">
        <button onclick="setPositionLabel('empty')" title="Room empty / no person present.">empty</button>
        <button onclick="setPositionLabel('door')" title="Person near the door zone.">door</button>
        <button onclick="setPositionLabel('desk')" title="Person near the desk zone.">desk</button>
        <button onclick="setPositionLabel('center')" title="Person in the center zone.">center</button>
        <button onclick="setPositionLabel('bed')" title="Person near the bed zone.">bed</button>
        <button onclick="setPositionLabel('sofa')" title="Person near the sofa zone.">sofa</button>
      </div>
      <div class="actions">
        <button class="primary" onclick="startPositionRun(60)" title="Record this label for 60 seconds. Each active transmitter link can add one CSV row per sample.">Start 60s</button>
        <button onclick="startPositionRun(120)" title="Record this label for 120 seconds for a larger dataset.">Start 120s</button>
        <button onclick="stopPositionRun()" title="Stop recording but keep collected rows in browser memory.">Stop</button>
        <button onclick="resetBaseline()" title="Reset the CSI baseline. Keep the room still for a few seconds afterward.">Reset baseline</button>
        <button onclick="downloadPositionCsv()" title="Download labeled CSI rows collected in this browser session.">Download CSV</button>
        <button onclick="clearPositionRows()" title="Clear labeled rows stored in this browser session.">Clear</button>
      </div>
      <div class="subline" id="positionStatus">Collect at least two labels, then live guess updates here.</div>
    </div>
    <div class="panel">
      <div class="label">Position Dataset</div>
      <div class="row"><span>Live guess</span><b id="positionGuess">--</b></div>
      <div class="row"><span>Confidence</span><b id="positionConfidence">--</b></div>
      <div class="row"><span>Trained labels</span><b id="positionLabels">--</b></div>
      <div class="row"><span>Model rows</span><b id="positionModelRows">0</b></div>
      <div class="row"><span>Rows</span><b id="positionRows">0</b></div>
      <div class="row"><span>Links</span><b id="positionLinks">0</b></div>
      <div class="row"><span>Best link</span><b id="positionBest">--</b></div>
      <div class="row"><span>Score</span><b id="positionScore">0.00</b></div>
      <div class="row"><span>Motion</span><b id="positionMotion">0</b></div>
      <div class="row"><span>Packets</span><b id="positionPackets">0</b></div>
      <div class="row"><span>Dropped</span><b id="positionDropped">0</b></div>
      <div class="row"><span>Heap</span><b id="positionHeap">--</b></div>
      <table>
        <thead><tr><th>Link</th><th>Score</th><th>RSSI</th><th>Packets</th><th>Age</th><th>State</th></tr></thead>
        <tbody id="positionLinkRows"></tbody>
      </table>
    </div>
  </section>
</main>
<script>
const N=240, hist=[], csv=[];
const c=document.getElementById('chart'), ctx=c.getContext('2d');
let lastMetrics=null, suggestedThreshold=null, sampleMode='', sampleUntil=0;
let caps={ap_filter:true,position_logger:false,idle_sample_ms:8000,motion_sample_ms:20000,max_links:8};
let uiMode='motion', tickBusy=false;
let positionHeader='', positionRows=[], positionRecording=false, positionUntil=0, lastPositionSample=0;
let positionModel=null, lastPositionLiveSample=0;
const samples={idle:[],motion:[]};
function draw(th){
  ctx.clearRect(0,0,c.width,c.height);
  ctx.strokeStyle='#26303a'; ctx.lineWidth=1;
  for(let i=1;i<5;i++){let y=i*c.height/5;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(c.width,y);ctx.stroke();}
  const max=Math.max(th*1.6, 1, ...hist.map(v=>v*1.15));
  const ty=c.height-(th/max)*c.height;
  ctx.strokeStyle='#f59e0b'; ctx.setLineDash([6,5]); ctx.beginPath(); ctx.moveTo(0,ty); ctx.lineTo(c.width,ty); ctx.stroke(); ctx.setLineDash([]);
  ctx.strokeStyle='#60a5fa'; ctx.lineWidth=2; ctx.beginPath();
  hist.forEach((v,i)=>{const x=i*(c.width/(N-1)); const y=c.height-(v/max)*c.height; i?ctx.lineTo(x,y):ctx.moveTo(x,y);});
  ctx.stroke();
}
function linkRows(items){
  if(!items || !items.length) return '<tr><td colspan="6">No active CSI links yet</td></tr>';
  return items.map(l=>`<tr><td>${l.mac}</td><td>${l.score.toFixed(2)}</td><td>${l.rssi}</td><td>${l.packets}</td><td>${l.age_ms} ms</td><td><span class="pill ${l.motion?'hot':''}">${l.motion?'MOTION':'calm'}</span></td></tr>`).join('');
}
function filterText(m){
  if(!m.filter_enabled) return 'all links';
  if(m.filter_mode==='list') return `custom list (${m.filter_count||0})`;
  return m.filter_mac || 'single link';
}
function pct(values,p){
  if(!values.length) return null;
  const sorted=[...values].sort((a,b)=>a-b);
  const i=Math.min(sorted.length-1,Math.max(0,Math.round((sorted.length-1)*p)));
  return sorted[i];
}
function avg(values){
  return values.length ? values.reduce((a,b)=>a+b,0)/values.length : null;
}
function showCal(text){
  document.getElementById('calStatus').textContent=text;
}
function showPosition(text){
  document.getElementById('positionStatus').textContent=text;
}
function setUiMode(mode){
  if(mode==='position' && !caps.position_logger) return;
  uiMode=mode;
  document.getElementById('motionView').classList.toggle('hidden', mode!=='motion');
  document.getElementById('positionView').classList.toggle('hidden', mode!=='position');
  document.getElementById('motionTab').classList.toggle('active', mode==='motion');
  document.getElementById('positionTab').classList.toggle('active', mode==='position');
}
function cleanPositionLabel(v){
  return (v||'empty').replace(/[^a-zA-Z0-9 _-]/g,'_').trim()||'empty';
}
function setPositionLabel(v){
  document.getElementById('positionLabel').value=v;
}
async function ensurePositionHeader(){
  if(positionHeader) return;
  const r=await fetch('/header',{cache:'no-store'});
  if(!r.ok) throw new Error(await r.text());
  positionHeader=await r.text();
}
async function startPositionRun(seconds){
  try{
    await ensurePositionHeader();
    positionRecording=true;
    positionUntil=Date.now()+seconds*1000;
    lastPositionSample=0;
    showPosition(`Recording ${cleanPositionLabel(document.getElementById('positionLabel').value)} for ${seconds}s.`);
  }catch(e){
    showPosition('Position logger endpoint is not available in this sketch.');
  }
}
function stopPositionRun(){
  positionRecording=false;
  showPosition('Recording stopped. You can change label, continue, or download CSV.');
}
function clearPositionRows(){
  positionRows=[];
  document.getElementById('positionRows').textContent='0';
  positionModel=null;
  updatePositionModelUi();
  showPosition('Rows cleared in browser memory.');
}
function parseFingerprintLine(line){
  const p=(line||'').trim().split(',');
  if(p.length < 38) return null;
  const bins=[];
  for(let i=6;i<38;i++){
    const v=parseFloat(p[i]);
    if(!Number.isFinite(v)) return null;
    bins.push(v);
  }
  return {label:p[1]||'',mac:p[2]||'',bins};
}
function updatePositionModelUi(){
  const labels=positionModel ? positionModel.labels : [];
  document.getElementById('positionLabels').textContent=labels.length ? labels.join(', ') : '--';
  document.getElementById('positionModelRows').textContent=positionModel ? positionModel.rows : 0;
  if(!positionModel || labels.length < 2){
    document.getElementById('positionGuess').textContent='--';
    document.getElementById('positionConfidence').textContent='--';
  }
}
function rebuildPositionModel(){
  const groups={}, labels={};
  let rows=0;
  positionRows.forEach(line=>{
    const r=parseFingerprintLine(line);
    if(!r || !r.label || r.label==='live' || r.mac==='00:00:00:00:00:00') return;
    const key=r.label+'|'+r.mac;
    if(!groups[key]) groups[key]={label:r.label,mac:r.mac,count:0,bins:Array(32).fill(0)};
    for(let i=0;i<32;i++) groups[key].bins[i]+=r.bins[i];
    groups[key].count++;
    labels[r.label]=true;
    rows++;
  });
  const centroids={};
  Object.keys(groups).forEach(key=>{
    const g=groups[key];
    centroids[key]={label:g.label,mac:g.mac,count:g.count,bins:g.bins.map(v=>v/g.count)};
  });
  positionModel={centroids,labels:Object.keys(labels).sort(),rows};
  if(positionModel.labels.length < 2) positionModel=null;
  updatePositionModelUi();
}
async function samplePosition(){
  const label=cleanPositionLabel(document.getElementById('positionLabel').value);
  const r=await fetch(`/sample?label=${encodeURIComponent(label)}`,{cache:'no-store'});
  if(!r.ok) throw new Error(await r.text());
  const text=await r.text();
  text.trim().split(/\r?\n/).filter(Boolean).forEach(line=>positionRows.push(line));
  document.getElementById('positionRows').textContent=positionRows.length;
  rebuildPositionModel();
}
async function updatePositionRecording(){
  if(!positionRecording) return;
  if(Date.now()>=positionUntil){
    stopPositionRun();
    return;
  }
  if(Date.now()-lastPositionSample>=200){
    lastPositionSample=Date.now();
    await samplePosition();
  }
  const left=Math.max(0,Math.ceil((positionUntil-Date.now())/1000));
  showPosition(`Recording ${cleanPositionLabel(document.getElementById('positionLabel').value)}: ${left}s left.`);
}
async function downloadPositionCsv(){
  try{
    await ensurePositionHeader();
    const body=(positionHeader||'')+'\n'+positionRows.join('\n')+'\n';
    const label=cleanPositionLabel(document.getElementById('positionLabel').value);
    const a=document.createElement('a');
    a.href=URL.createObjectURL(new Blob([body],{type:'text/csv'}));
    a.download=`csi-${label}-${new Date().toISOString().replace(/[:.]/g,'-')}.csv`;
    a.click();
    URL.revokeObjectURL(a.href);
  }catch(e){
    showPosition('Download failed because the position logger endpoint is unavailable.');
  }
}
function updatePositionPanel(m){
  document.getElementById('positionLinks').textContent=m.links;
  document.getElementById('positionBest').textContent=m.best_mac||'--';
  document.getElementById('positionScore').textContent=m.score.toFixed(2);
  document.getElementById('positionMotion').textContent=m.motion?1:0;
  document.getElementById('positionPackets').textContent=m.packets;
  document.getElementById('positionDropped').textContent=m.dropped;
  document.getElementById('positionHeap').textContent=m.heap_free ? `${Math.round(m.heap_free/1024)}K / ${Math.round((m.heap_min||0)/1024)}K` : '--';
  document.getElementById('positionLinkRows').innerHTML=linkRows(m.link_data);
}
function meanAbsDistance(a,b){
  let sum=0;
  for(let i=0;i<32;i++) sum+=Math.abs(a[i]-b[i]);
  return sum/32;
}
function classifyPositionRows(text){
  if(!positionModel || positionModel.labels.length < 2) return;
  const live=text.trim().split(/\r?\n/).map(parseFingerprintLine).filter(Boolean);
  if(!live.length) return;
  const scores=[];
  positionModel.labels.forEach(label=>{
    let sum=0,count=0;
    live.forEach(row=>{
      const c=positionModel.centroids[label+'|'+row.mac];
      if(!c) return;
      sum+=meanAbsDistance(row.bins,c.bins);
      count++;
    });
    if(count) scores.push({label,score:sum/count,count});
  });
  scores.sort((a,b)=>a.score-b.score);
  if(!scores.length){
    document.getElementById('positionGuess').textContent='--';
    document.getElementById('positionConfidence').textContent='--';
    showPosition('No trained MAC matches the current live links.');
    return;
  }
  const best=scores[0], second=scores[1];
  let confidence=second ? Math.round(Math.max(0,Math.min(100,((second.score-best.score)/(second.score+0.001))*100))) : 50;
  document.getElementById('positionGuess').textContent=best.label;
  document.getElementById('positionConfidence').textContent=confidence+'%';
  if(!positionRecording) showPosition(`Live guess: ${best.label}, distance ${best.score.toFixed(1)} from ${best.count} link(s).`);
}
async function updatePositionClassifier(){
  if(uiMode!=='position' || positionRecording || !positionModel) return;
  if(Date.now()-lastPositionLiveSample < 600) return;
  lastPositionLiveSample=Date.now();
  try{
    const r=await fetch('/sample?label=live',{cache:'no-store'});
    if(r.ok) classifyPositionRows(await r.text());
  }catch(e){}
}
function updateSuggested(){
  const idle95=pct(samples.idle,0.95);
  const motion80=pct(samples.motion,0.80);
  const idleMean=avg(samples.idle);
  const motionMean=avg(samples.motion);
  const el=document.getElementById('suggested');
  if(idle95===null){
    suggestedThreshold=null; el.textContent='--';
    showCal('Learn idle first with the room still.');
    return;
  }
  if(motion80===null){
    suggestedThreshold=Math.max(0.05,idle95*1.6+0.05);
    el.textContent=suggestedThreshold.toFixed(2);
    showCal(`Idle mean ${idleMean.toFixed(2)}, idle p95 ${idle95.toFixed(2)}. Now learn motion.`);
    return;
  }
  suggestedThreshold = motion80 > idle95 ? idle95 + (motion80-idle95)*0.45 : idle95*1.6+0.05;
  suggestedThreshold = Math.max(0.05, Math.min(50, suggestedThreshold));
  el.textContent=suggestedThreshold.toFixed(2);
  const gap=motion80-idle95;
  showCal(`Idle p95 ${idle95.toFixed(2)}, motion p80 ${motion80.toFixed(2)}, gap ${gap.toFixed(2)}.`);
}
function updateSampling(m){
  if(!sampleMode) return;
  samples[sampleMode].push(m.score);
  const left=Math.max(0,Math.ceil((sampleUntil-Date.now())/1000));
  showCal(`${sampleMode==='idle'?'Idle':'Motion'} sample: ${left}s`);
  if(Date.now()>=sampleUntil){
    const finished=sampleMode;
    sampleMode='';
    updateSuggested();
    if(finished==='idle') showCal(document.getElementById('calStatus').textContent+' Walk through the path and learn motion.');
  }
}
async function tick(){
  if(tickBusy) return;
  tickBusy=true;
  try{
    const r=await fetch('/metrics',{cache:'no-store'});
    const m=await r.json();
    lastMetrics=m;
    hist.push(m.score); if(hist.length>N) hist.shift();
    csv.push([Date.now(),m.score,m.threshold,m.sensitivity||1,m.motion?1:0,m.confidence,m.best_mac,m.rssi,m.packets,m.dropped].join(','));
    if(csv.length>5000) csv.shift();
    document.getElementById('score').textContent=m.score.toFixed(1);
    document.getElementById('threshold').textContent=m.threshold.toFixed(1);
    document.getElementById('sensitivity').textContent=(m.sensitivity||1).toFixed(2)+'x';
    document.getElementById('confidence').textContent=m.confidence+'%';
    document.getElementById('best').textContent=m.best_mac || '--';
    document.getElementById('filter').textContent=filterText(m);
    document.getElementById('rssi').textContent=m.rssi ? `${m.rssi} dBm` : '--';
    document.getElementById('packets').textContent=m.packets;
    document.getElementById('dropped').textContent=m.dropped;
    document.getElementById('heap').textContent=m.heap_free ? `${Math.round(m.heap_free/1024)}K / ${Math.round((m.heap_min||0)/1024)}K` : '--';
    document.getElementById('meta').textContent=`${m.links} active link(s), uptime ${(m.uptime_ms/1000).toFixed(0)} s`;
    document.getElementById('bar').style.width=Math.min(100,(m.score/Math.max(m.threshold,0.05))*100)+'%';
    const st=document.getElementById('state'); st.textContent=positionRecording?'REC':(m.motion?'MOTION':'CALM'); st.className='status '+(m.motion?'hot':'');
    document.getElementById('links').innerHTML=linkRows(m.link_data);
    updatePositionPanel(m);
    const thInput=document.getElementById('thresholdInput');
    const seInput=document.getElementById('sensitivityInput');
    if(document.activeElement!==thInput) thInput.value=m.threshold.toFixed(2);
    if(document.activeElement!==seInput) seInput.value=(m.sensitivity||1).toFixed(2);
    updateSampling(m);
    await updatePositionRecording();
    await updatePositionClassifier();
    draw(m.threshold);
  }catch(e){document.getElementById('meta').textContent='Dashboard disconnected';}
  finally{tickBusy=false;}
}
async function loadCapabilities(){
  try{
    const r=await fetch('/capabilities',{cache:'no-store'});
    if(r.ok) caps=Object.assign(caps, await r.json());
  }catch(e){}
  const apRow=document.getElementById('apFilterRow');
  if(apRow) apRow.classList.toggle('hidden', !caps.ap_filter);
  const modeTabs=document.getElementById('modeTabs');
  if(modeTabs) modeTabs.classList.toggle('hidden', !caps.position_logger);
  if(!caps.position_logger) setUiMode('motion');
}
async function setValue(path,value){
  const r=await fetch(`${path}?value=${encodeURIComponent(value)}`,{cache:'no-store'});
  if(!r.ok) throw new Error(await r.text());
  return r.json();
}
async function applyThreshold(){
  const v=parseFloat(document.getElementById('thresholdInput').value);
  if(Number.isFinite(v)){
    await setValue('/threshold',v);
    showCal(`Applied threshold ${v.toFixed(2)}. Save settings to keep it after reboot.`);
  }
}
async function applySensitivity(){
  const v=parseFloat(document.getElementById('sensitivityInput').value);
  if(Number.isFinite(v)){
    await setValue('/sensitivity',v);
    showCal(`Applied sensitivity ${v.toFixed(2)}x. Save settings to keep it after reboot.`);
  }
}
async function setFilter(mode){
  const r=await fetch(`/filter?mode=${encodeURIComponent(mode)}`,{cache:'no-store'});
  if(!r.ok){
    showCal(await r.text());
    return;
  }
  const m=await r.json();
  hist.length=0;
  showCal(`Filter set: ${filterText(m)}. Keep still briefly, then recalibrate if needed.`);
}
async function applySuggested(){
  if(suggestedThreshold===null) return;
  document.getElementById('thresholdInput').value=suggestedThreshold.toFixed(2);
  await setValue('/threshold',suggestedThreshold);
  showCal(`Applied threshold ${suggestedThreshold.toFixed(2)}. Save settings to keep it after reboot.`);
}
function startSample(mode){
  sampleMode=mode;
  const duration=mode==='motion' ? (caps.motion_sample_ms||20000) : (caps.idle_sample_ms||8000);
  sampleUntil=Date.now()+duration;
  samples[mode]=[];
  if(mode==='idle'){
    samples.motion=[];
    suggestedThreshold=null;
    document.getElementById('suggested').textContent='--';
  }
  showCal(`${mode==='idle'?'Idle':'Motion'} sample: ${Math.ceil(duration/1000)}s`);
}
async function resetBaseline(){
  await fetch('/reset');
  hist.length=0;
  samples.idle=[]; samples.motion=[];
  sampleMode=''; suggestedThreshold=null;
  document.getElementById('suggested').textContent='--';
  showCal('Baseline reset. Keep still, then learn idle.');
}
async function saveSettings(){
  const r=await fetch('/save',{cache:'no-store'});
  showCal(r.ok ? 'Threshold/sensitivity saved on the ESP32. Filter mode follows the sketch setting after reboot.' : 'Save failed.');
}
function downloadCsv(){
  const body='epoch_ms,score,threshold,sensitivity,motion,confidence,best_mac,rssi,packets,dropped\n'+csv.join('\n');
  const a=document.createElement('a'); a.href=URL.createObjectURL(new Blob([body],{type:'text/csv'})); a.download='esp32-csi-motion.csv'; a.click(); URL.revokeObjectURL(a.href);
}
loadCapabilities();
setInterval(tick,200); tick();
</script>
</body>
</html>
)HTML";
  return html;
}
