// ESP-NOW on Wi-Fi channel 1, unencrypted, the same conventions as the BMS v2 (BMS_v2_interface.md):
// JSON status broadcasts once a second, plain-ASCII commands to the TIM's MAC with replies to the sender.
// The TIM is not the hub. It also lists every node it hears, which checks the radio link.
#pragma once

#include <Arduino.h>

#include "signals.h"

struct NowNode {
  uint8_t mac[6];
  char type[16];      // "type" field of the last JSON packet
  char last[251];     // last payload as text
  int8_t rssi;
  uint32_t lastMs;
  uint32_t count;
};

struct NowStats {
  uint32_t tx, txFail, rx, commands;
  bool ready;
};

void espnowBegin();
void espnowLoop(const SigValue s[kNumSig], const EngineInfo& e);  // call often from loop()
int espnowNodes(NowNode* out, int max);
NowStats espnowStats();
// Builds the two status packets; also used by the serial STATUS command.
void espnowPackets(const SigValue s[kNumSig], const EngineInfo& e, String& volts, String& press);
