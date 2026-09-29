#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

// XIAO ESP32S3 のユーザーLED（GPIO21）はアクティブLOW
const int LED_PIN = LED_BUILTIN;
const uint8_t BROADCAST_ADDR[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint32_t PING_INTERVAL_MS = 1000;
const int STATS_EVERY = 10;

enum : uint8_t { PING = 1, PONG = 2 };

struct __attribute__((packed)) Packet {
  uint8_t type;
  uint32_t seq;
  uint32_t sentUs;  // PING送信時刻（PONGではそのまま返す）
};

// 送信ペイロードのバイト数（-DPAYLOAD_SIZE=N で変更可。Packet の後ろは0埋め）
#ifndef PAYLOAD_SIZE
#define PAYLOAD_SIZE 9
#endif
static_assert(PAYLOAD_SIZE >= sizeof(Packet) && PAYLOAD_SIZE <= ESP_NOW_MAX_DATA_LEN,
              "PAYLOAD_SIZE must be 9..250");

// コールバック（Wi-Fiタスク）からloop()へ渡すイベント
struct Event {
  Packet pkt;
  uint32_t recvUs;
};
QueueHandle_t eventQueue;

uint32_t seq = 0;
bool waiting = false;
uint32_t ledOffAt = 0;

// STATS_EVERY 回ごとの統計
int pings = 0, lost = 0, got = 0;
uint32_t rttMin, rttMax;
uint64_t rttSum;

void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < (int)sizeof(Packet)) return;
  Event ev;
  ev.recvUs = micros();
  memcpy(&ev.pkt, data, sizeof(Packet));
  xQueueSend(eventQueue, &ev, 0);
}

void sendPacket(uint8_t type, uint32_t s, uint32_t sentUs) {
  Packet p = {type, s, sentUs};
  uint8_t buf[PAYLOAD_SIZE] = {};
  memcpy(buf, &p, sizeof(p));
  esp_now_send(BROADCAST_ADDR, buf, PAYLOAD_SIZE);
}

void resetStats() {
  pings = lost = got = 0;
  rttMin = UINT32_MAX;
  rttMax = 0;
  rttSum = 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // 消灯
  eventQueue = xQueueCreate(10, sizeof(Event));
  resetStats();

  WiFi.mode(WIFI_STA);
  Serial.print("my MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("esp_now_init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void loop() {
  static uint32_t lastPing = 0;
  uint32_t now = millis();

  if (now - lastPing >= PING_INTERVAL_MS) {
    lastPing = now;
    if (waiting) {
      lost++;
      Serial.printf("rtt seq=%lu lost\n", (unsigned long)seq);
    }
    if (pings == STATS_EVERY) {
      if (got > 0) {
        Serial.printf("stats n=%d min/avg/max=%lu/%lu/%lu us lost=%d\n", pings,
                      (unsigned long)rttMin, (unsigned long)(rttSum / got),
                      (unsigned long)rttMax, lost);
      } else {
        Serial.printf("stats n=%d lost=%d\n", pings, lost);
      }
      resetStats();
    }
    seq++;
    pings++;
    waiting = true;
    sendPacket(PING, seq, micros());
  }

  Event ev;
  while (xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
    if (ev.pkt.type == PING) {
      sendPacket(PONG, ev.pkt.seq, ev.pkt.sentUs);
    } else if (ev.pkt.type == PONG && waiting && ev.pkt.seq == seq) {
      uint32_t rtt = ev.recvUs - ev.pkt.sentUs;
      waiting = false;
      got++;
      rttSum += rtt;
      rttMin = min(rttMin, rtt);
      rttMax = max(rttMax, rtt);
      Serial.printf("rtt seq=%lu %lu us\n", (unsigned long)seq, (unsigned long)rtt);
      digitalWrite(LED_PIN, LOW);  // 点灯
      ledOffAt = now + 50;
    }
  }

  if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) {
    digitalWrite(LED_PIN, HIGH);  // 消灯
    ledOffAt = 0;
  }
}
