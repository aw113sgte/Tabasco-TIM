#include "web.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "adc.h"
#include "config.h"
#include "espnow_link.h"
#include "j1587.h"
#include "json_util.h"
#include "settings.h"
#include "signals.h"
#include "uptime.h"
#include "webpage.h"

static WebServer g_server(80);
static DNSServer g_dns;  // captive portal: every name on the hotspot resolves to the TIM
static String g_updateError;
static const char* const kEngineText[] = {"off", "cranking", "running", "unknown"};

// ------------------------------------------------------------------ Wi-Fi

// The ESP32 has one radio. While it hunts for a shop network it can't join, it hops across every channel
// and drags the hotspot (and ESP-NOW, which needs channel 1) with it. So a join attempt gets kStaTryMs;
// after that the TIM drops shop Wi-Fi, puts the hotspot back on channel 1 and only tries again every
// kStaRetryIdleMs while nobody is on the hotspot, or every kStaRetryBusyMs while someone is (a failed attempt
// can move the hotspot off channel 1 for up to 20 s). A phone that auto-joins the hotspot then can't keep the
// TIM off shop Wi-Fi for good, and a short drop (mesh roaming, router restart) heals within a minute.
enum StaState : uint8_t { STA_OFF, STA_TRYING, STA_CONNECTED, STA_GAVE_UP };
static const uint32_t kStaTryMs = 20000;
static const uint32_t kStaRetryIdleMs = 30000;
static const uint32_t kStaRetryBusyMs = 5UL * 60 * 1000;
static uint32_t g_staDrops = 0;     // times a joined shop Wi-Fi was lost since boot
static uint32_t g_staDropMs = 0;
static uint8_t g_staDropReason = 0;
static StaState g_sta = STA_OFF;
static uint32_t g_staSince = 0;
static volatile uint8_t g_staReason = 0;  // last disconnect reason from the Wi-Fi driver
static volatile bool g_staStopping = false;  // our own disconnect() also raises a "disconnected" event

static const char* staReasonText(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
      return "network not found (2.4 GHz only; the name is case-sensitive; with \"channel 1 only\" it must be on channel 1)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      return "network found but its security mode isn't supported (WPA3-only?)";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "network signal too weak";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "password rejected";
    case WIFI_REASON_ASSOC_FAIL:
      return "router refused the connection";
    case 0:
      return "no answer from the network";
    default:
      return nullptr;
  }
}

static String staReason() {
  const char* t = staReasonText(g_staReason);
  return t ? String(t) : "Wi-Fi reason code " + String(g_staReason);
}

static void startAp() {
  WiFi.softAP(g_wifi.apSsid.c_str(), g_wifi.apPass.c_str(), kWifiChannel);
}

static void staStart() {
  g_staReason = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);  // wifiLoop() decides when to retry
  // With "channel 1 only", join only an access point on channel 1, so the hotspot and the BMS ESP-NOW link
  // stay on channel 1 (a mesh or extender often has the same network name on several channels).
  WiFi.begin(g_wifi.staSsid.c_str(), g_wifi.staPass.c_str(), g_wifi.staCh1Only ? kWifiChannel : 0);
  g_sta = STA_TRYING;
  g_staSince = millis();
  Serial.printf("[TIM] WiFi: trying shop Wi-Fi \"%s\"%s for %lu s\n", g_wifi.staSsid.c_str(),
                g_wifi.staCh1Only ? " on channel 1" : "", (unsigned long)(kStaTryMs / 1000));
}

static void staStop(StaState next) {
  g_staStopping = true;
  g_sta = next;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  startAp();  // pins the hotspot (and ESP-NOW) back on channel 1
  g_staSince = millis();
  g_staStopping = false;
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setHostname(kHostname);
  WiFi.setSleep(false);  // no modem sleep: it delays or drops replies to the web page
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        if (!g_staStopping && g_sta != STA_OFF && g_sta != STA_GAVE_UP) {
          g_staReason = info.wifi_sta_disconnected.reason;
        }
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_AP);
  startAp();
  // Captive portal: answer every DNS lookup from hotspot clients with the TIM's address, so phones and
  // laptops see a "sign in to network" page and open the TIM page by themselves.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", WiFi.softAPIP());
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
  MDNS.addServiceTxt("http", "tcp", "path", "/");
  Serial.printf("[TIM] WiFi: hotspot \"%s\" at %s, channel %u\n", g_wifi.apSsid.c_str(),
                WiFi.softAPIP().toString().c_str(), (unsigned)kWifiChannel);
  if (g_wifi.staEnable && g_wifi.staSsid.length()) {
    staStart();
  }
}

void wifiLoop() {
  const uint32_t now = millis();
  switch (g_sta) {
    case STA_TRYING:
      if (WiFi.status() == WL_CONNECTED) {
        g_sta = STA_CONNECTED;  // mDNS follows the new interface by itself; restarting it broke service lookups
        Serial.printf("[TIM] WiFi: joined \"%s\" as %s (channel %u; hotspot and ESP-NOW moved with it)\n",
                      g_wifi.staSsid.c_str(), WiFi.localIP().toString().c_str(), (unsigned)WiFi.channel());
      } else if (now - g_staSince > kStaTryMs) {
        const String why = staReason();
        staStop(STA_GAVE_UP);
        Serial.printf("[TIM] WiFi: couldn't join \"%s\": %s. Hotspot back on channel %u, next try in %lu s\n",
                      g_wifi.staSsid.c_str(), why.c_str(), (unsigned)kWifiChannel,
                      (unsigned long)((WiFi.softAPgetStationNum() ? kStaRetryBusyMs : kStaRetryIdleMs) / 1000));
      }
      break;
    case STA_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[TIM] WiFi: lost \"%s\" (%s)\n", g_wifi.staSsid.c_str(), staReason().c_str());
        g_staDrops++;
        g_staDropMs = now;
        g_staDropReason = g_staReason;
        staStart();
      }
      break;
    case STA_GAVE_UP:
      if (now - g_staSince > (WiFi.softAPgetStationNum() ? kStaRetryBusyMs : kStaRetryIdleMs)) {
        staStart();
      }
      break;
    default:
      break;
  }
}

void wifiRetryNow() {
  if (g_wifi.staEnable && g_wifi.staSsid.length() && g_sta != STA_CONNECTED) {
    staStart();
  }
}

void wifiSetSta(bool enable) {
  g_wifi.staEnable = enable;
  saveWifi();
  if (enable) {
    wifiRetryNow();
  } else if (g_sta != STA_OFF) {
    staStop(STA_OFF);
  }
}

// Networks the TIM can hear (2.4 GHz only). Briefly takes the hotspot off channel 1.
int wifiScan(String names[], int8_t rssi[], uint8_t chan[], bool secure[], int max) {
  const int n = WiFi.scanNetworks(false, true);
  int k = 0;
  for (int i = 0; i < n && k < max; i++, k++) {
    names[k] = WiFi.SSID(i);
    rssi[k] = WiFi.RSSI(i);
    chan[k] = WiFi.channel(i);
    secure[k] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  if (g_sta == STA_OFF || g_sta == STA_GAVE_UP) {
    WiFi.mode(WIFI_AP);
    startAp();
  }
  return k;
}

String wifiStatusText() {
  String s = "hotspot " + g_wifi.apSsid + " " + WiFi.softAPIP().toString() + ", " + WiFi.softAPgetStationNum() +
             " connected, channel " + WiFi.channel();
  const String name = "\"" + g_wifi.staSsid + "\"";
  const uint32_t secs = (millis() - g_staSince) / 1000;
  switch (g_sta) {
    case STA_TRYING:
      s += ", trying shop Wi-Fi " + name + " (" + secs + " s)";
      break;
    case STA_CONNECTED:
      s += ", shop Wi-Fi " + name + " " + WiFi.localIP().toString() + " RSSI " + WiFi.RSSI();
      break;
    case STA_GAVE_UP:
      {
        const uint32_t wait = (WiFi.softAPgetStationNum() ? kStaRetryBusyMs : kStaRetryIdleMs) / 1000;
        s += ", shop Wi-Fi " + name + " failed: " + staReason() + "; next try in " + String(wait > secs ? wait - secs : 0) +
             " s (every 30 s, or 5 min while someone is on the hotspot)";
      }
      break;
    default:
      break;
  }
  if (g_staDrops) {
    const char* t = staReasonText(g_staDropReason);
    s += ". Shop Wi-Fi lost " + String(g_staDrops) + "x since restart, last " + String((millis() - g_staDropMs) / 1000) +
         " s ago (" + (t ? String(t) : "reason " + String(g_staDropReason)) + ")";
  }
  return s;
}

// ------------------------------------------------------------------ helpers

static bool auth() {
  if (g_server.authenticate("admin", g_wifi.apPass.c_str())) {
    return true;
  }
  g_server.requestAuthentication(BASIC_AUTH, "TIM", "Log in as admin with the hotspot password");
  return false;
}

static void sendJson(const String& body) {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(200, "application/json", body);
}

static void sendResult(const String& err) {
  String o = "{\"ok\":";
  o += err.length() ? "false,\"err\":" : "true";
  if (err.length()) {
    jStr(o, err.c_str());
  }
  o += '}';
  sendJson(o);
}

static uint32_t argEpoch() {
  return (uint32_t)strtoul(g_server.arg("t").c_str(), nullptr, 10);
}

static void currentValues(AdcSnapshot& a, SigValue s[kNumSig], EngineInfo& e) {
  adcGet(a);
  signalsCompute(a, s);
  e = engineInfo(s);
}

static void engineJson(String& o, const EngineInfo& e) {
  o += "{\"state\":\"";
  o += kEngineText[e.state];
  o += "\",\"reason\":";
  jStr(o, e.reason);
  o += ",\"rpm\":";
  jNum(o, e.rpmSeen ? e.rpm : NAN, 0);
  o += '}';
}

// ------------------------------------------------------------------ API

static void apiMeta() {
  String o;
  o.reserve(3000);
  o = "{\"fw\":\"" TIM_FW_VERSION "\",\"sig\":[";
  for (uint8_t i = 0; i < kNumSig; i++) {
    if (i) o += ',';
    o += "{\"id\":";
    jStr(o, kSig[i].id);
    o += ",\"name\":";
    jStr(o, kSig[i].name);
    o += ",\"unit\":";
    jStr(o, kSig[i].unit);
    o += ",\"dec\":";
    o += kSig[i].decimals;
    o += '}';
  }
  o += "],\"status\":[";
  for (uint8_t i = 0; i <= ST_RANGE; i++) {
    if (i) o += ',';
    jStr(o, kSigStatusText[i]);
  }
  o += "],\"dash\":";
  jStr(o, g_dash.c_str());
  o += ",\"wifi\":{\"ap_ssid\":";
  jStr(o, g_wifi.apSsid.c_str());
  o += ",\"sta_en\":";
  o += g_wifi.staEnable ? "true" : "false";
  o += ",\"sta_ssid\":";
  jStr(o, g_wifi.staSsid.c_str());
  o += ",\"sta_ch1\":";
  o += g_wifi.staCh1Only ? "true" : "false";
  o += ",\"status\":";
  jStr(o, wifiStatusText().c_str());
  o += "}}";
  sendJson(o);
}

static void apiLive() {
  AdcSnapshot a;
  SigValue s[kNumSig];
  EngineInfo e;
  currentValues(a, s, e);
  static SigMinMax mm[kNumSig];
  sigMinMaxGet(mm);
  static J1587Pid pids[kJ1587PidSlots];
  const int n = j1587CopyPids(pids, kJ1587PidSlots);
  String o;
  o.reserve(1500 + n * 190);
  o = "{\"up\":";
  o += uptimeSeconds();
  o += ",\"eng\":";
  engineJson(o, e);
  o += ",\"sig\":[";
  for (uint8_t i = 0; i < kNumSig; i++) {
    if (i) o += ',';
    o += '[';
    jNum(o, s[i].v, kSig[i].decimals + 1);
    o += ',';
    o += s[i].st;
    o += ',';
    jNum(o, mm[i].vmin, kSig[i].decimals + 1);
    o += ',';
    jNum(o, mm[i].vmax, kSig[i].decimals + 1);
    o += ',';
    o += mm[i].tmin;
    o += ',';
    o += mm[i].tmax;
    o += ']';
  }
  // Every J1587 parameter heard, as id "j<MID>.<PID>". Known PIDs are scaled; unknown 1-4 byte PIDs are
  // sent as their raw number (unit "raw") so they can still go on the dashboard.
  const uint32_t now = millis();
  o += "],\"j\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    const J1587Pid& p = pids[i];
    float v;
    bool known;
    if (!j1587Number(p, v, known)) continue;
    if (!first) o += ',';
    first = false;
    char id[16];
    snprintf(id, sizeof(id), "j%u.%u", p.mid, p.pid);
    o += "{\"id\":";
    jStr(o, id);
    o += ",\"name\":";
    const char* mn = j1587MidName(p.mid);
    const char* pn = j1587PidName(p.pid);
    String name = (pn ? String(pn) : "PID " + String(p.pid)) + " (" + (mn ? String(mn) : "MID " + String(p.mid)) + ")";
    jStr(o, name.c_str());
    o += ",\"unit\":";
    jStr(o, known ? j1587PidUnit(p.pid) : "raw");
    o += ",\"v\":";
    jNum(o, v, known ? j1587PidDecimals(p.pid) : 0);
    o += ",\"dec\":";
    o += known ? j1587PidDecimals(p.pid) : 0;
    o += ",\"age\":";
    o += now - p.lastMs;
    const uint8_t dec = known ? j1587PidDecimals(p.pid) : 0;
    o += ",\"mn\":";
    jNum(o, p.vmin, dec);
    o += ",\"mx\":";
    jNum(o, p.vmax, dec);
    o += ",\"tmn\":";
    o += p.tmin;
    o += ",\"tmx\":";
    o += p.tmax;
    o += '}';
  }
  o += "]}";
  sendJson(o);
}

static void apiRaw() {
  AdcSnapshot a;
  adcGet(a);
  String o;
  o.reserve(3000);
  o = "{\"ok\":[";
  for (int i = 0; i < 4; i++) {
    if (i) o += ',';
    o += a.ok[i] ? "true" : "false";
  }
  o += "],\"err\":[";
  for (int i = 0; i < 4; i++) {
    if (i) o += ',';
    o += a.errors[i];
  }
  o += "],\"hz\":";
  jNum(o, a.scanHz, 1);
  o += ",\"heap\":";
  o += ESP.getFreeHeap();
  o += ",\"up\":";
  o += uptimeSeconds();
  o += ",\"wifi\":";
  jStr(o, wifiStatusText().c_str());
  o += ",\"ch\":[";
  for (uint8_t ch = 0; ch < kNumAdcCh; ch++) {
    if (ch) o += ',';
    o += "{\"n\":";
    jStr(o, kAdcChName[ch]);
    o += ",\"pin\":";
    jStr(o, kAdcChPin[ch]);
    o += ",\"v\":";
    jNum(o, a.v[ch], 4);
    o += ",\"min\":";
    jNum(o, a.vmin[ch], 4);
    o += ",\"max\":";
    jNum(o, a.vmax[ch], 4);
    // Input-side value: FE-A volts, FE-B volts, or shunt amps before calibration.
    float in = NAN, r = NAN;
    const bool feB = (ch >= CH_S_C59 && ch <= CH_C58);
    if (ch == CH_SHUNT_I) {
      in = isnan(a.v[ch]) ? NAN : (a.v[ch] - kShuntIdleV) / kShuntVPerA;
    } else if (feB) {
      in = a.v[ch] / kFeBGain;
      if (ch <= CH_S_C63) r = sensorRatio(a, ch);
    } else {
      in = isnan(a.v[ch]) ? NAN : (a.v[ch] - kFeAOffset) / kFeAGain;
    }
    o += ",\"in\":";
    jNum(o, in, ch == CH_SHUNT_I ? 1 : 3);
    o += ",\"r\":";
    jNum(o, r, 4);
    o += '}';
  }
  o += "]}";
  sendJson(o);
}

static void apiJ1587() {
  J1587Stats st;
  j1587GetStats(st);
  static J1587Pid pids[kJ1587PidSlots];
  const int n = j1587CopyPids(pids, kJ1587PidSlots);
  const uint32_t now = millis();
  String o;
  o.reserve(200 + n * 120);
  o = "{\"st\":{\"bytes\":";
  o += st.bytes;
  o += ",\"frames\":";
  o += st.frames;
  o += ",\"bad\":";
  o += st.badChecksum;
  o += ",\"malformed\":";
  o += st.malformed;
  o += ",\"long\":";
  o += st.tooLong;
  o += ",\"uart\":";
  o += st.uartErrors;
  o += ",\"ign\":";
  o += st.ignoredMid;
  o += ",\"pdrop\":";
  o += st.pidDropped;
  o += ",\"ddrop\":";
  o += st.dtcDropped;
  o += ",\"slots\":";
  o += n;
  o += ",\"slotmax\":";
  o += kJ1587PidSlots;
  o += ",\"last\":";
  if (st.lastFrameMs) {
    o += now - st.lastFrameMs;
  } else {
    o += "null";
  }
  o += "},\"pids\":[";
  for (int i = 0; i < n; i++) {
    if (i) o += ',';
    const J1587Pid& p = pids[i];
    o += "{\"mid\":";
    o += p.mid;
    o += ",\"midn\":";
    jStr(o, j1587MidName(p.mid) ? j1587MidName(p.mid) : "");
    o += ",\"pid\":";
    o += p.pid;
    o += ",\"name\":";
    jStr(o, j1587PidName(p.pid) ? j1587PidName(p.pid) : "");
    o += ",\"unit\":";
    jStr(o, j1587PidUnit(p.pid));
    float v;
    o += ",\"v\":";
    jNum(o, j1587Decode(p, v) ? v : NAN, j1587PidDecimals(p.pid));
    o += ",\"hex\":\"";
    char hx[4];
    for (int k = 0; k < p.len; k++) {
      snprintf(hx, sizeof(hx), k ? " %02X" : "%02X", p.data[k]);
      o += hx;
    }
    o += "\",\"age\":";
    o += now - p.lastMs;
    o += ",\"n\":";
    o += p.count;
    o += '}';
  }
  static J1587Dtc dtcs[kJ1587DtcSlots];
  const int nd = j1587CopyDtcs(dtcs, kJ1587DtcSlots);
  o += "],\"dtc\":[";
  for (int i = 0; i < nd; i++) {
    const J1587Dtc& d = dtcs[i];
    if (i) o += ',';
    o += "{\"mid\":";
    o += d.mid;
    o += ",\"midn\":";
    jStr(o, j1587MidName(d.mid) ? j1587MidName(d.mid) : "");
    o += ",\"num\":";
    o += d.num;
    o += ",\"sid\":";
    o += d.isSid ? "true" : "false";
    o += ",\"name\":";
    jStr(o, (!d.isSid && j1587PidName(d.num)) ? j1587PidName(d.num) : "");
    o += ",\"fmi\":";
    o += d.fmi;
    o += ",\"fmin\":";
    jStr(o, j1587FmiText(d.fmi));
    o += ",\"active\":";
    o += d.active ? "true" : "false";
    o += ",\"count\":";
    o += d.count;
    o += ",\"age\":";
    o += now - d.lastMs;
    o += ",\"first\":";
    o += now - d.firstMs;
    o += '}';
  }
  o += "]}";
  sendJson(o);
}

static void apiCal() {
  AdcSnapshot a;
  SigValue s[kNumSig];
  EngineInfo e;
  currentValues(a, s, e);
  String o;
  o.reserve(1500);
  o = "{\"eng\":";
  engineJson(o, e);
  o += ",\"vgain\":[";
  for (uint8_t i = 0; i < kNumVGain; i++) {
    if (i) o += ',';
    jNum(o, g_cal.vgain[i], 5);
  }
  o += "],\"vraw\":[";
  for (uint8_t i = 0; i < kNumVGain; i++) {
    if (i) o += ',';
    jNum(o, voltageUncorrected(a, i), 3);
  }
  o += "],\"shunt_zero\":";
  jNum(o, g_cal.shuntZeroV, 4);
  o += ",\"shunt_sign\":";
  o += g_cal.shuntSign;
  o += ",\"baro\":";
  jNum(o, g_cal.baroPsia, 3);
  o += ",\"zero\":[";
  for (uint8_t i = 0; i < kNumCurves; i++) {
    if (i) o += ',';
    jNum(o, g_cal.zero[i], 3);
  }
  o += "],\"curve\":[";
  for (uint8_t i = 0; i < kNumCurves; i++) {
    const SensorCurve& c = g_cal.curve[i];
    if (i) o += ',';
    o += '[';
    jNum(o, c.rLo, 4);
    o += ',';
    jNum(o, c.rHi, 4);
    o += ',';
    jNum(o, c.pLo, 3);
    o += ',';
    jNum(o, c.pHi, 3);
    o += ']';
  }
  o += "],\"ratio\":[";
  static const uint8_t kCh[kNumCurves] = {CH_S_C59, CH_S_C61, CH_S_C62, CH_S_C63};
  for (uint8_t i = 0; i < kNumCurves; i++) {
    if (i) o += ',';
    jNum(o, sensorRatio(a, kCh[i]), 4);
  }
  o += "],\"unc\":[";
  for (uint8_t i = 0; i < kNumCurves; i++) {
    if (i) o += ',';
    jNum(o, sensorUncorrected(a, i), 3);
  }
  o += "],\"epoch\":[";
  for (uint8_t i = 0; i < kNumVGain + 2 + kNumCurves; i++) {
    if (i) o += ',';
    o += g_cal.calEpoch[i];
  }
  o += "]}";
  sendJson(o);
}

static void apiNodes() {
  static NowNode nodes[8];
  const int n = espnowNodes(nodes, 8);
  const NowStats st = espnowStats();
  const uint32_t now = millis();
  String o = "{\"me\":\"";
  o += WiFi.softAPmacAddress();
  o += "\",\"ready\":";
  o += st.ready ? "true" : "false";
  o += ",\"tx\":";
  o += st.tx;
  o += ",\"fail\":";
  o += st.txFail;
  o += ",\"rx\":";
  o += st.rx;
  o += ",\"nodes\":[";
  for (int i = 0; i < n; i++) {
    if (i) o += ',';
    o += "{\"mac\":";
    jMac(o, nodes[i].mac);
    o += ",\"type\":";
    jStr(o, nodes[i].type);
    o += ",\"rssi\":";
    o += nodes[i].rssi;
    o += ",\"age\":";
    o += now - nodes[i].lastMs;
    o += ",\"n\":";
    o += nodes[i].count;
    o += ",\"last\":";
    jStr(o, nodes[i].last);
    o += '}';
  }
  o += "]}";
  sendJson(o);
}

// ------------------------------------------------------------------ firmware update

static void updateDone() {
  if (!auth()) return;
  const bool ok = !Update.hasError() && g_updateError.isEmpty();
  sendResult(ok ? "" : (g_updateError.length() ? g_updateError : String(Update.errorString())));
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

static void updateChunk() {
  HTTPUpload& up = g_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_updateError = "";
    if (!g_server.authenticate("admin", g_wifi.apPass.c_str())) {
      g_updateError = "not logged in";
      return;
    }
    Serial.printf("[TIM] OTA: receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      g_updateError = Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_updateError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) {
      g_updateError = Update.errorString();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_updateError.isEmpty() && !Update.end(true)) {
      g_updateError = Update.errorString();
    }
    Serial.printf("[TIM] OTA: %u bytes, %s\n", (unsigned)up.totalSize,
                  g_updateError.isEmpty() ? "ok, restarting" : g_updateError.c_str());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_updateError = "upload aborted";
  }
}

// ------------------------------------------------------------------ routes

void webBegin() {
  g_server.on("/", HTTP_GET, [] {
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send_P(200, "text/html", kIndexHtml);
  });
  g_server.on("/api/meta", HTTP_GET, apiMeta);
  g_server.on("/api/live", HTTP_GET, apiLive);
  g_server.on("/api/raw", HTTP_GET, apiRaw);
  g_server.on("/api/j1587", HTTP_GET, apiJ1587);
  g_server.on("/api/cal", HTTP_GET, apiCal);
  g_server.on("/api/nodes", HTTP_GET, apiNodes);
  g_server.on("/j1587.txt", HTTP_GET, [] {
    String t;
    j1587CaptureText(t);
    g_server.sendHeader("Content-Disposition", "attachment; filename=\"tim_j1708_capture.txt\"");
    g_server.send(200, "text/plain", t);
  });

  g_server.on("/api/j1587/clear", HTTP_POST, [] {
    j1587Clear();
    sendResult("");
  });
  g_server.on("/api/minmax/reset", HTTP_POST, [] {
    adcResetMinMax();
    sigMinMaxReset();
    j1587ResetMinMax();
    sendResult("");
  });
  g_server.on("/api/dash", HTTP_POST, [] {
    if (!auth()) return;
    g_dash = g_server.arg("ids");
    saveDash();
    sendResult("");
  });
  g_server.on("/api/cal/zero", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calZero(g_server.arg("what").c_str(), argEpoch()));
  });
  g_server.on("/api/cal/meter", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calMeter(g_server.arg("id").c_str(), g_server.arg("v").toFloat(), argEpoch()));
  });
  g_server.on("/api/cal/curve", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calCurve(g_server.arg("c").toInt(), g_server.arg("rlo").toFloat(), g_server.arg("rhi").toFloat(),
                        g_server.arg("plo").toFloat(), g_server.arg("phi").toFloat(), argEpoch()));
  });
  g_server.on("/api/cal/reset", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calReset(g_server.arg("what").c_str()));
  });
  g_server.on("/api/cal/undo", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calUndo() ? "" : "nothing to undo since the last restart");
  });
  g_server.on("/api/cal/shuntsign", HTTP_POST, [] {
    if (!auth()) return;
    sendResult(calShuntSign(g_server.arg("s").toInt()));
  });
  g_server.on("/api/wifi", HTTP_POST, [] {
    if (!auth()) return;
    const String ssid = g_server.arg("ap_ssid");
    const String pass = g_server.arg("ap_pass");
    if (ssid.length() < 1 || ssid.length() > 31) return sendResult("hotspot name must be 1-31 characters");
    if (pass.length() && (pass.length() < 8 || pass.length() > 63)) {
      return sendResult("hotspot password must be 8-63 characters");
    }
    g_wifi.apSsid = ssid;
    if (pass.length()) g_wifi.apPass = pass;  // blank = keep the current password
    g_wifi.staEnable = g_server.arg("sta_en") == "1";
    g_wifi.staSsid = g_server.arg("sta_ssid");
    g_wifi.staSsid.trim();
    g_wifi.staCh1Only = g_server.arg("sta_ch1") == "1";
    if (g_server.hasArg("sta_pass") && g_server.arg("sta_pass").length()) g_wifi.staPass = g_server.arg("sta_pass");
    saveWifi();
    sendResult("");
    delay(500);
    ESP.restart();
  });
  g_server.on("/api/wifi/retry", HTTP_POST, [] {
    if (!auth()) return;
    wifiRetryNow();
    sendResult("");
  });
  g_server.on("/api/wifi/scan", HTTP_GET, [] {
    static String names[20];
    static int8_t rssi[20];
    static uint8_t chan[20];
    static bool secure[20];
    const int n = wifiScan(names, rssi, chan, secure, 20);
    String o = "{\"nets\":[";
    for (int i = 0; i < n; i++) {
      if (i) o += ',';
      o += "{\"ssid\":";
      jStr(o, names[i].c_str());
      o += ",\"rssi\":";
      o += rssi[i];
      o += ",\"ch\":";
      o += chan[i];
      o += ",\"secure\":";
      o += secure[i] ? "true" : "false";
      o += '}';
    }
    o += "]}";
    sendJson(o);
  });
  g_server.on("/api/reboot", HTTP_POST, [] {
    if (!auth()) return;
    sendResult("");
    delay(500);
    ESP.restart();
  });
  g_server.on("/api/factory", HTTP_POST, [] {
    if (!auth()) return;
    if (g_server.arg("confirm") != "yes") return sendResult("confirm=yes missing");
    factoryReset();
    sendResult("");
    delay(500);
    ESP.restart();
  });
  g_server.on("/update", HTTP_POST, updateDone, updateChunk);
  // Captive-portal checks (Android /generate_204, Apple /hotspot-detect.html, Windows /connecttest.txt, ...)
  // and any other address typed on the hotspot: send the browser to the TIM page.
  g_server.onNotFound([] {
    const String host = g_server.hostHeader();
    if (host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString() || host == String(kHostname) + ".local") {
      g_server.send(404, "text/plain", "not found");
      return;
    }
    g_server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    g_server.send(302, "text/plain", "");
  });
  g_server.begin();
}

void webLoop() {
  wifiLoop();
  g_dns.processNextRequest();
  g_server.handleClient();
}
