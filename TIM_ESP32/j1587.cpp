#include "j1587.h"

#include <driver/uart.h>

#include "config.h"
#include "uptime.h"

// J1708 framing: bytes of one message are at most 2 bit times apart; messages are separated by at least
// 10-12 bit times of idle. The UART RX timeout of 1 character time (10 bits) ends a frame.
// Checksum: all bytes of the frame, checksum included, add up to 0 (mod 256).
// J1587 PID data length: PID 0-127 one byte, 128-191 two bytes, 192-253 a count byte then that many bytes,
// 254 the rest of the message, 255 = the next byte is a page 2 PID.

static const uart_port_t kUart = UART_NUM_2;
static const int kPidSlots = kJ1587PidSlots;
static const int kCaptureSlots = 256;
static const uint8_t kCaptureBytes = 24;

struct CapturedFrame {
  uint32_t ms;
  uint8_t len;       // real length (may exceed kCaptureBytes)
  bool checksumOk;
  uint8_t b[kCaptureBytes];
};

static const int kDtcSlots = kJ1587DtcSlots;
static J1587Pid g_pids[kPidSlots];
static J1587Dtc g_dtcs[kDtcSlots];
static int g_numDtcs = 0;
static int g_numPids = 0;
static J1587Stats g_stats;
static CapturedFrame g_capture[kCaptureSlots];
static int g_capHead = 0;
static int g_capCount = 0;
static bool g_echo = false;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t g_uartQueue;

// ------------------------------------------------------------------ known PIDs
// Scaling from SAE J1587. Check each against the bus log before trusting it.
struct PidInfo {
  uint16_t pid;
  const char* name;
  const char* unit;
  float scale;
  bool isSigned;
  uint8_t decimals;
};

static const PidInfo kPidInfo[] = {
    {84, "Road speed", "mph", 0.5f, false, 1},
    {91, "Accelerator pedal", "%", 0.4f, false, 0},
    {92, "Engine load", "%", 0.5f, false, 0},
    {96, "Fuel level", "%", 0.5f, false, 0},
    {98, "Engine oil level", "%", 0.5f, false, 0},
    {100, "Engine oil pressure", "psi", 0.5f, false, 1},
    {102, "Boost pressure", "psi", 0.125f, false, 1},
    {105, "Intake manifold temp", "\xC2\xB0" "F", 1.0f, false, 0},
    {110, "Engine coolant temp", "\xC2\xB0" "F", 1.0f, false, 0},
    {111, "Coolant level", "%", 0.5f, false, 0},
    {168, "Battery potential", "V", 0.05f, false, 2},
    {171, "Ambient air temp", "\xC2\xB0" "F", 0.25f, true, 0},
    {173, "Exhaust gas temp", "\xC2\xB0" "F", 0.25f, true, 0},
    {175, "Engine oil temp", "\xC2\xB0" "F", 0.25f, true, 0},
    {177, "Transmission oil temp", "\xC2\xB0" "F", 0.25f, true, 0},
    {183, "Fuel rate", "gal/h", 1.0f / 64.0f, false, 2},
    {190, "Engine speed", "rpm", 0.25f, false, 0},
    {245, "Total vehicle distance", "mi", 0.1f, false, 1},
    {247, "Total engine hours", "h", 0.05f, false, 1},
};

static const PidInfo* findPid(uint16_t pid) {
  for (const PidInfo& p : kPidInfo) {
    if (p.pid == pid) {
      return &p;
    }
  }
  return nullptr;
}

const char* j1587PidName(uint16_t pid) {
  const PidInfo* p = findPid(pid);
  return p ? p->name : nullptr;
}

const char* j1587PidUnit(uint16_t pid) {
  const PidInfo* p = findPid(pid);
  return p ? p->unit : "";
}

uint8_t j1587PidDecimals(uint16_t pid) {
  const PidInfo* p = findPid(pid);
  return p ? p->decimals : 0;
}

const char* j1587MidName(uint8_t mid) {
  switch (mid) {
    case 128: return "Engine";
    case 130: return "Transmission";
    case 136: return "Brakes (ABS)";
    case 140: return "Instrument cluster";
    case 142: return "Vehicle management";
    default: return nullptr;
  }
}

bool j1587Decode(const J1587Pid& p, float& value) {
  const PidInfo* info = findPid(p.pid);
  if (!info || p.len == 0 || p.len > 4) {
    return false;
  }
  uint32_t raw = 0;
  for (int i = p.len - 1; i >= 0; i--) {
    raw = (raw << 8) | p.data[i];  // J1587 is little-endian
  }
  const uint32_t allOnes = p.len == 4 ? 0xFFFFFFFFu : ((1u << (8 * p.len)) - 1);
  if (raw == allOnes) {
    return false;  // "not available"
  }
  if (info->isSigned) {
    const int32_t shift = 32 - 8 * p.len;
    value = (float)((int32_t)(raw << shift) >> shift) * info->scale;
  } else {
    value = raw * info->scale;
  }
  return true;
}

bool j1587Number(const J1587Pid& p, float& value, bool& known) {
  known = j1587Decode(p, value);
  if (known) {
    return true;
  }
  if (p.len == 0 || p.len > 4 || p.pid == 194 || p.pid == 254) {
    return false;
  }
  uint32_t raw = 0;
  for (int k = p.len - 1; k >= 0; k--) raw = (raw << 8) | p.data[k];
  value = (float)raw;
  return true;
}

const char* j1587FmiText(uint8_t fmi) {
  static const char* const kFmi[16] = {
      "above normal range", "below normal range", "erratic or intermittent data", "voltage high / shorted high",
      "voltage low / shorted low", "current low / open circuit", "current high / grounded",
      "mechanical system not responding", "abnormal frequency or pulse width", "abnormal update rate",
      "abnormal rate of change", "failure mode not identifiable", "bad device or component",
      "out of calibration", "special instructions", "reserved"};
  return kFmi[fmi & 0x0F];
}

// ------------------------------------------------------------------ frame handling

// PID 194 data: pairs of (PID/SID number, code character), each optionally followed by an occurrence count.
// Code character: bit 7 = count byte follows, bit 6 = inactive, bit 5 = standard code (0 = expansion, page 2),
// bit 4 = SID (0 = PID), bits 0-3 = FMI. Called with g_mux held.
static void storeDtcs(uint8_t mid, const uint8_t* d, uint8_t len, uint32_t now) {
  int i = 0;
  while (i + 1 < len) {
    const uint8_t id = d[i];
    const uint8_t cc = d[i + 1];
    i += 2;
    uint8_t count = 0;
    if (cc & 0x80) {
      if (i >= len) break;
      count = d[i++];
    }
    J1587Dtc c = {mid, (uint16_t)(id + ((cc & 0x20) ? 0 : 256)), (cc & 0x10) != 0, (uint8_t)(cc & 0x0F),
                  (cc & 0x40) == 0, count, now, now};
    J1587Dtc* slot = nullptr;
    for (int k = 0; k < g_numDtcs; k++) {
      J1587Dtc& x = g_dtcs[k];
      if (x.mid == c.mid && x.num == c.num && x.isSid == c.isSid && x.fmi == c.fmi) {
        slot = &x;
        break;
      }
    }
    if (!slot && g_numDtcs < kDtcSlots) {
      slot = &g_dtcs[g_numDtcs++];
      slot->firstMs = now;
    }
    if (!slot) {  // full: reuse the code not announced for the longest, if it has gone quiet
      J1587Dtc* old = &g_dtcs[0];
      for (int k = 1; k < g_numDtcs; k++) {
        if (now - g_dtcs[k].lastMs > now - old->lastMs) old = &g_dtcs[k];
      }
      if (now - old->lastMs < kJ1587StaleMs) {
        g_stats.dtcDropped++;
        continue;
      }
      slot = old;
      slot->firstMs = now;
    }
    c.firstMs = slot->firstMs;
    *slot = c;
  }
}

static void storePid(uint8_t mid, uint16_t pid, const uint8_t* data, uint8_t len, uint32_t now) {
  J1587Pid* slot = nullptr;
  for (int i = 0; i < g_numPids; i++) {
    if (g_pids[i].mid == mid && g_pids[i].pid == pid) {
      slot = &g_pids[i];
      break;
    }
  }
  bool fresh = false;
  if (!slot && g_numPids < kPidSlots) {
    slot = &g_pids[g_numPids++];
    fresh = true;
  } else if (!slot) {  // full: reuse the entry not heard for the longest, if it has gone quiet
    slot = &g_pids[0];
    for (int i = 1; i < g_numPids; i++) {
      if (now - g_pids[i].lastMs > now - slot->lastMs) slot = &g_pids[i];
    }
    if (now - slot->lastMs < kJ1587StaleMs) {
      g_stats.pidDropped++;
      return;
    }
    fresh = true;
  }
  if (fresh) {
    slot->mid = mid;
    slot->pid = pid;
    slot->count = 0;
    slot->vmin = slot->vmax = NAN;
  }
  slot->len = min<uint8_t>(len, sizeof(slot->data));
  memcpy(slot->data, data, slot->len);
  slot->lastMs = now;
  slot->count++;
  float v;
  bool known;
  if (j1587Number(*slot, v, known)) {
    const uint32_t t = uptimeSeconds();
    if (isnan(slot->vmin) || v < slot->vmin) {
      slot->vmin = v;
      slot->tmin = t;
    }
    if (isnan(slot->vmax) || v > slot->vmax) {
      slot->vmax = v;
      slot->tmax = t;
    }
  }
  if (pid == 194) {
    storeDtcs(mid, data, len, now);
  }
}

static void handleFrame(const uint8_t* f, int len) {
  if (len < 2) {
    return;  // a lone byte is noise or a collision fragment
  }
  const uint32_t now = millis();
  uint8_t sum = 0;
  for (int i = 0; i < len; i++) {
    sum += f[i];
  }
  const bool ok = sum == 0;

  portENTER_CRITICAL(&g_mux);
  CapturedFrame& c = g_capture[g_capHead];
  c.ms = now;
  c.len = len;
  c.checksumOk = ok;
  memcpy(c.b, f, min<int>(len, kCaptureBytes));
  g_capHead = (g_capHead + 1) % kCaptureSlots;
  g_capCount = min(g_capCount + 1, kCaptureSlots);
  if (!ok) {
    g_stats.badChecksum++;
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  g_stats.frames++;
  g_stats.lastFrameMs = now;
  if (len > 21) {
    g_stats.tooLong++;
  }
  const uint8_t mid = f[0];
  if (mid < 128 || mid == 255) {  // J1587 MIDs are 128-254; anything else is a corrupted frame that passed the checksum
    g_stats.ignoredMid++;
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  const int end = len - 1;  // checksum byte
  int i = 1;
  bool malformed = false;
  uint16_t page = 0;
  while (i < end) {
    uint8_t pid = f[i++];
    if (pid == 255) {
      page = 256;
      continue;
    }
    const uint16_t fullPid = page + pid;
    page = 0;
    int n;
    const uint8_t* data = &f[i];
    if (pid < 128) {
      n = 1;
    } else if (pid < 192) {
      n = 2;
    } else if (pid < 254) {
      if (i >= end) {
        malformed = true;
        break;
      }
      n = f[i++];
      data = &f[i];
    } else {
      n = end - i;  // PID 254: data link escape, the rest of the message
    }
    if (i + n > end) {
      malformed = true;
      break;
    }
    storePid(mid, fullPid, data, n, now);
    i += n;
  }
  if (malformed) {
    g_stats.malformed++;
  }
  portEXIT_CRITICAL(&g_mux);

  digitalWrite(kPinLedBus, HIGH);
  if (g_echo) {
    char line[16 + 3 * kJ1708MaxFrame];
    int p = snprintf(line, sizeof(line), "[J1587] %8lu", (unsigned long)now);
    for (int k = 0; k < len && p < (int)sizeof(line) - 4; k++) {
      p += snprintf(line + p, sizeof(line) - p, " %02X", f[k]);
    }
    Serial.println(line);
  }
}

static void j1708Task(void*) {
  uint8_t frame[kJ1708MaxFrame];
  int len = 0;
  bool overflow = false;
  uart_event_t ev;
  uint8_t buf[128];
  for (;;) {
    if (xQueueReceive(g_uartQueue, &ev, pdMS_TO_TICKS(50)) != pdTRUE) {
      digitalWrite(kPinLedBus, LOW);
      continue;
    }
    switch (ev.type) {
      case UART_DATA: {
        int remaining = ev.size;
        while (remaining > 0) {
          const int n = uart_read_bytes(kUart, buf, min<int>(remaining, sizeof(buf)), 0);
          if (n <= 0) {
            break;
          }
          remaining -= n;
          portENTER_CRITICAL(&g_mux);
          g_stats.bytes += n;
          portEXIT_CRITICAL(&g_mux);
          for (int k = 0; k < n; k++) {
            if (len < kJ1708MaxFrame) {
              frame[len++] = buf[k];
            } else {
              overflow = true;
            }
          }
        }
        if (ev.timeout_flag) {
          if (!overflow) {
            handleFrame(frame, len);
          } else {
            portENTER_CRITICAL(&g_mux);
            g_stats.malformed++;
            portEXIT_CRITICAL(&g_mux);
          }
          len = 0;
          overflow = false;
        }
        break;
      }
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        uart_flush_input(kUart);
        xQueueReset(g_uartQueue);
        // fall through
      case UART_FRAME_ERR:
      case UART_PARITY_ERR:
      case UART_BREAK:
        portENTER_CRITICAL(&g_mux);
        g_stats.uartErrors++;
        portEXIT_CRITICAL(&g_mux);
        len = 0;
        overflow = false;
        break;
      default:
        break;
    }
  }
}

void j1587Begin() {
  // Transmitter off for good in this firmware: IO17 is a plain GPIO held low (DE low = bus released).
  pinMode(kPinJ1708De, OUTPUT);
  digitalWrite(kPinJ1708De, LOW);
  pinMode(kPinLedBus, OUTPUT);
  digitalWrite(kPinLedBus, LOW);

  uart_config_t cfg = {};
  cfg.baud_rate = kJ1708Baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  uart_driver_install(kUart, 1024, 0, 32, &g_uartQueue, 0);
  uart_param_config(kUart, &cfg);
  uart_set_pin(kUart, UART_PIN_NO_CHANGE, kPinJ1708Rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_set_rx_timeout(kUart, 1);
  xTaskCreatePinnedToCore(j1708Task, "j1708", 4096, nullptr, 4, nullptr, 1);
}

void j1587GetStats(J1587Stats& out) {
  portENTER_CRITICAL(&g_mux);
  out = g_stats;
  portEXIT_CRITICAL(&g_mux);
}

int j1587CopyPids(J1587Pid* out, int max) {
  portENTER_CRITICAL(&g_mux);
  const int n = min(max, g_numPids);
  memcpy(out, g_pids, n * sizeof(J1587Pid));
  portEXIT_CRITICAL(&g_mux);
  return n;
}

bool j1587Value(uint8_t mid, uint16_t pid, float& value, uint32_t maxAgeMs) {
  J1587Pid p;
  bool found = false;
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < g_numPids; i++) {
    if (g_pids[i].mid == mid && g_pids[i].pid == pid) {
      p = g_pids[i];
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&g_mux);
  if (!found || millis() - p.lastMs > maxAgeMs) {
    return false;
  }
  return j1587Decode(p, value);
}

int j1587CopyDtcs(J1587Dtc* out, int max) {
  portENTER_CRITICAL(&g_mux);
  const int n = min(max, g_numDtcs);
  memcpy(out, g_dtcs, n * sizeof(J1587Dtc));
  portEXIT_CRITICAL(&g_mux);
  return n;
}

void j1587CaptureText(String& out) {
  static CapturedFrame copy[kCaptureSlots];
  int count, head;
  portENTER_CRITICAL(&g_mux);
  memcpy(copy, g_capture, sizeof(copy));
  count = g_capCount;
  head = g_capHead;
  portEXIT_CRITICAL(&g_mux);
  out.reserve(count * 72 + 200);
  out = "# TIM J1708 capture, firmware " TIM_FW_VERSION "\n# ms_since_boot len checksum bytes(hex)\n";
  char line[24 + 3 * kCaptureBytes];
  for (int k = 0; k < count; k++) {
    const CapturedFrame& c = copy[(head - count + k + kCaptureSlots) % kCaptureSlots];
    int p = snprintf(line, sizeof(line), "%lu %u %s", (unsigned long)c.ms, c.len, c.checksumOk ? "ok" : "BAD");
    for (int i = 0; i < min<int>(c.len, kCaptureBytes); i++) {
      p += snprintf(line + p, sizeof(line) - p, " %02X", c.b[i]);
    }
    out += line;
    out += c.len > kCaptureBytes ? " ...\n" : "\n";
  }
}

void j1587Clear() {
  portENTER_CRITICAL(&g_mux);
  g_numPids = 0;
  g_numDtcs = 0;
  g_capCount = 0;
  g_capHead = 0;
  memset(&g_stats, 0, sizeof(g_stats));
  portEXIT_CRITICAL(&g_mux);
}

void j1587ResetMinMax() {
  portENTER_CRITICAL(&g_mux);
  for (int i = 0; i < g_numPids; i++) {
    g_pids[i].vmin = g_pids[i].vmax = NAN;
  }
  portEXIT_CRITICAL(&g_mux);
}

void j1587SetEcho(bool on) {
  g_echo = on;
}

bool j1587Echo() {
  return g_echo;
}
