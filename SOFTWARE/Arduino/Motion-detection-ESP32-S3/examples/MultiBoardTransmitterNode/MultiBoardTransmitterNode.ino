#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

// Multi-board transmitter:
// - Flash MultiBoardReceiverNode on one ESP32-S3 board first.
// - Flash this sketch on one or more transmitter boards.
// - Give each transmitter a different NODE_ID.
// - Put transmitters around the room so people cross one or more radio paths.

const char *RX_SSID = "MOTION-RX";
const char *RX_PASS = "motion1234";
const char *NODE_ID = "node-1";

const IPAddress RX_IP(192, 168, 4, 1);
const uint16_t RX_PORT = 4210;
const uint16_t TX_LOCAL_PORT = 4211;
// Use 45 for one to three transmitters. If 4 or 5 transmitters make the
// receiver Dropped counter rise, try 25 to 35 on every transmitter.
const uint16_t PACKETS_PER_SECOND = 45;

WiFiUDP udp;
uint32_t sequenceNumber = 0;
uint32_t lastPacketMs = 0;
uint32_t lastPrintMs = 0;

void connectToReceiver() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Serial.print("Node MAC: ");
  Serial.println(WiFi.macAddress());
  WiFi.begin(RX_SSID, RX_PASS);

  Serial.print("Connecting to receiver AP");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  Serial.print("Node IP: ");
  Serial.println(WiFi.localIP());
  esp_wifi_set_ps(WIFI_PS_NONE);
}

void sendPacket() {
  char payload[96];
  snprintf(payload, sizeof(payload),
           "motion-node,%s,%lu,%lu,0123456789abcdef0123456789abcdef",
           NODE_ID, static_cast<unsigned long>(sequenceNumber++),
           static_cast<unsigned long>(millis()));

  udp.beginPacket(RX_IP, RX_PORT);
  udp.write(reinterpret_cast<const uint8_t *>(payload), strlen(payload));
  udp.endPacket();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  connectToReceiver();
  udp.begin(TX_LOCAL_PORT);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToReceiver();
  }

  const uint32_t nowMs = millis();
  const uint32_t intervalMs = 1000UL / PACKETS_PER_SECOND;
  if (nowMs - lastPacketMs >= intervalMs) {
    lastPacketMs = nowMs;
    sendPacket();
  }

  if (nowMs - lastPrintMs >= 2000) {
    lastPrintMs = nowMs;
    Serial.printf("%s sent %lu packets, RSSI %d dBm\n",
                  NODE_ID, static_cast<unsigned long>(sequenceNumber), WiFi.RSSI());
  }
}
