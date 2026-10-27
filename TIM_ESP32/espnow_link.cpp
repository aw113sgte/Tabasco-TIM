#include "espnow_link.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "config.h"
#include "j1587.h"
#include "json_util.h"

static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const int kMaxNodes = 8;

static NowNode g_nodes[kMaxNodes];
static int g_numNodes = 0;
static NowStats g_stats;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
  volatile bool ready;
  char text[64];
  uint8_t mac[6];
} g_pending;

static const char* const kEngineText[] = {"off", "cranking", "running", "unknown"};

static bool ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) {
    return true;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // current channel (the hotspot's, channel 1)
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

static void sendTo(const uint8_t* mac, const String& payload) {
  if (!g_stats.ready) {
    return;
  }
  const size_t len = min<size_t>(payload.length(), 250);
  if (!ensurePeer(mac) || esp_now_send(mac, (const uint8_t*)payload.c_str(), len) != ESP_OK) {
    g_stats.txFail++;
  } else {
    g_stats.tx++;
  }
}

static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || len <= 0) {
    return;
  }
  portENTER_CRITICAL(&g_mux);
  g_stats.rx++;
  NowNode* n = nullptr;
  for (int i = 0; i < g_numNodes; i++) {
    if (memcmp(g_nodes[i].mac, info->src_addr, 6) == 0) {
      n = &g_nodes[i];
      break;
    }
  }
  if (!n && g_numNodes < kMaxNodes) {
    n = &g_nodes[g_numNodes++];
    memset(n, 0, sizeof(*n));
    memcpy(n->mac, info->src_addr, 6);
  }
  if (n) {
    const int m = min(len, (int)sizeof(n->last) - 1);
    memcpy(n->last, data, m);
    n->last[m] = '\0';
    n->rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    n->lastMs = millis();
    n->count++;
    const char* t = strstr(n->last, "\"type\":\"");
    if (data[0] == '{' && t) {
      t += 8;
      int k = 0;
      while (t[k] && t[k] != '"' && k < (int)sizeof(n->type) - 1) {
        n->type[k] = t[k];
        k++;
      }
      n->type[k] = '\0';
    }
  }
  portEXIT_CRITICAL(&g_mux);

  if (data[0] != '{' && !g_pending.ready) {  // plain text = a command for us
    const int m = min(len, (int)sizeof(g_pending.text) - 1);
    memcpy(g_pending.text, data, m);
    g_pending.text[m] = '\0';
    memcpy(g_pending.mac, info->src_addr, 6);
    g_pending.ready = true;
  }
}

void espnowBegin() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("[TIM] NOW: init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  g_stats.ready = ensurePeer(kBroadcastMac);
  Serial.printf("[TIM] NOW: %s on channel %u, MAC %s\n", g_stats.ready ? "ready" : "peer add failed",
                (unsigned)WiFi.channel(), WiFi.softAPmacAddress().c_str());
}

void espnowPackets(const SigValue s[kNumSig], const EngineInfo& e, String& volts, String& press) {
  volts = "{\"type\":\"tim_v\"";
  for (uint8_t i = S_V1437; i <= S_SENS5V; i++) {
    volts += ',';
    jKey(volts, kSig[i].id);
    jNum(volts, s[i].st == ST_NO_ADC ? NAN : s[i].v, i == S_SHUNT ? 0 : 2);
  }
  volts += '}';

  J1587Stats js;
  j1587GetStats(js);
  press = "{\"type\":\"tim_p\",\"fw\":\"" TIM_FW_VERSION "\"";
  for (uint8_t i = S_C59_PSIA; i <= S_AIR; i++) {
    press += ',';
    jKey(press, kSig[i].id);
    jNum(press, s[i].st == ST_OK ? s[i].v : NAN, 2);
  }
  press += ",\"c58\":";
  jNum(press, s[S_C58].v, 0);
  press += ",\"engine\":\"";
  press += kEngineText[e.state];
  press += "\",\"rpm\":";
  jNum(press, e.rpmSeen ? e.rpm : NAN, 0);
  press += ",\"j1587_frames\":";
  press += js.frames;
  press += '}';
}

static void handleCommand(const SigValue s[kNumSig], const EngineInfo& e) {
  char text[sizeof(g_pending.text)];
  uint8_t mac[6];
  memcpy(text, g_pending.text, sizeof(text));
  memcpy(mac, g_pending.mac, 6);
  g_pending.ready = false;
  g_stats.commands++;
  for (char* p = text; *p; p++) {
    if (*p == '\r' || *p == '\n') {
      *p = '\0';
      break;
    }
  }
  if (strcasecmp(text, "STATUS") == 0 || strcasecmp(text, "GET") == 0) {
    String v, p;
    espnowPackets(s, e, v, p);
    sendTo(mac, v);
    sendTo(mac, p);
    return;
  }
  String ack = "{\"type\":\"ack\",\"cmd\":";
  jStr(ack, text);
  if (strcasecmp(text, "HELP") == 0 || strcasecmp(text, "PING") == 0) {
    ack += ",\"ok\":true,\"fw\":\"" TIM_FW_VERSION "\",\"cmds\":\"STATUS GET PING HELP\"}";
  } else {
    ack += ",\"ok\":false,\"err\":\"unknown_command\"}";
  }
  sendTo(mac, ack);
}

void espnowLoop(const SigValue s[kNumSig], const EngineInfo& e) {
  static uint32_t lastTx = 0;
  if (g_pending.ready) {
    handleCommand(s, e);
  }
  if (millis() - lastTx >= kEspNowPeriodMs) {
    lastTx = millis();
    String v, p;
    espnowPackets(s, e, v, p);
    sendTo(kBroadcastMac, v);
    sendTo(kBroadcastMac, p);
  }
}

int espnowNodes(NowNode* out, int max) {
  portENTER_CRITICAL(&g_mux);
  const int n = min(max, g_numNodes);
  memcpy(out, g_nodes, n * sizeof(NowNode));
  portEXIT_CRITICAL(&g_mux);
  return n;
}

NowStats espnowStats() {
  return g_stats;
}
