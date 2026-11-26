#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <math.h>

#ifndef LORA_SS
#error "Definisikan LORA_SS sebelum #include \"vitra_radio.h\""
#endif
#ifndef LORA_SF
#define LORA_SF 11
#endif

// ---------------------------------------------------------------------
// 1. AMBANG VITAL
// ---------------------------------------------------------------------
#define HR_CRIT_HI    130
#define HR_CRIT_LO     50
#define HR_WARN_HI    100
#define HR_WARN_LO     60

#define SPO2_CRIT      90
#define SPO2_WARN      95

#define RR_CRIT_LO     10
#define RR_CRIT_HI     30
#define RR_WARN_LO     12
#define RR_WARN_HI     20

struct VitalLevel {
  int8_t lh, ls, lv, lr;  // 0 normal, 1 waspada, 2 kritis (lv = terburuk dari HR/SpO2/RR; lr = RR)
};

static inline int vitraLevelHr(int hr) {
  if (hr > HR_CRIT_HI || hr < HR_CRIT_LO) return 2;
  if (hr > HR_WARN_HI || hr < HR_WARN_LO) return 1;
  return 0;
}

static inline int vitraLevelSpo2(int s) {
  if (s < SPO2_CRIT) return 2;
  if (s < SPO2_WARN) return 1;
  return 0;
}

static inline int vitraLevelRr(int rr) {
  if (rr <= 0) return 0;
  if (rr < RR_CRIT_LO || rr > RR_CRIT_HI) return 2;
  if (rr < RR_WARN_LO || rr > RR_WARN_HI) return 1;
  return 0;
}

static inline VitalLevel vitraClassify(int hr, int spo2, int rr = 0) {
  VitalLevel v;

  int lh = vitraLevelHr(hr);
  int ls = vitraLevelSpo2(spo2);
  int lr = vitraLevelRr(rr);

  int worst = (lh > ls) ? lh : ls;

  v.lh = lh;
  v.ls = ls;
  v.lr = lr;
  v.lv = (worst > lr) ? worst : lr;

  return v;
}

static void vitraWhy(const VitalLevel &v, char *out, size_t n) {
  const char *names[3] = { "HR", "SpO2", "RR" };
  int lv[3] = { v.lh, v.ls, v.lr };

  size_t len = 0;
  out[0] = 0;

  for (int i = 0; i < 3; i++) {
    if (lv[i] <= 0) continue;

    int w = snprintf(
      out + len,
      n - len,
      "%s%s %s",
      len ? "+" : "",
      names[i],
      lv[i] >= 2 ? "Kritis" : "Waspada"
    );

    if (w < 0) break;

    len += (size_t)w;
    if (len >= n) break;
  }
}

static void vitraSimValues(int mode, int &hr, int &spo2) {
  if (mode >= 2) {
    hr = random(140, 160);
    spo2 = random(82, 88);
  } else if (mode == 1) {
    hr = random(105, 126);
    spo2 = random(91, 95);
  } else {
    hr = random(70, 100);
    spo2 = random(95, 100);
  }
}

// Versi dengan RR (pasien simulasi). Versi 3 argumen di atas tetap ada.
static void vitraSimValues(int mode, int &hr, int &spo2, int &rr) {
  vitraSimValues(mode, hr, spo2);

  if (mode >= 2) {
    rr = random(2) ? (int)random(4, 10) : (int)random(31, 40);
  } else if (mode == 1) {
    rr = random(2) ? (int)random(21, 31) : (int)random(10, 12);
  } else {
    rr = (int)random(12, 21);
  }
}

#ifndef VITRA_REAL_TIME
#define VITRA_REAL_TIME 0
#endif

#if VITRA_REAL_TIME
  #define VITRA_SESSION_MS       7200000UL
  #define VITRA_SEND_INTERVAL_MS  900000UL
#else
  #define VITRA_SESSION_MS        120000UL
  #define VITRA_SEND_INTERVAL_MS   15000UL
#endif

#define VITRA_SESSION_SENDS (VITRA_SESSION_MS / VITRA_SEND_INTERVAL_MS)
#define VITRA_OFFLINE_S     (VITRA_SEND_INTERVAL_MS * 2UL / 1000UL + 10UL)

// ---------------------------------------------------------------------
// 2. RADIO
// ---------------------------------------------------------------------
#define PRIO_CRIT   0
#define PRIO_WARN   1
#define PRIO_REG    2
#define PRIO_NORMAL 3

struct VitraWin {
  uint16_t lo, hi, maxWait;
};

static const VitraWin VITRA_WIN[4] = {
  {   0,  30, 1500 },
  {  40, 120, 2500 },
  { 140, 300, 4000 },
  { 320, 900, 8000 },
};

#define VITRA_MIN_LISTEN_MS ((3UL << LORA_SF) / 125UL + 4UL)
#define VITRA_CCA_RSSI_DBM (-90)

#define VR_REG_OPMODE    0x01
#define VR_REG_IRQFLAGS  0x12
#define VR_REG_MODEMSTAT 0x18
#define VR_IRQ_RXDONE    0x40

struct VitraTxStats {
  uint32_t sent[4], deferred[4], forced[4], waitSum[4], waitMax[4];
  uint32_t airMs;
};

static VitraTxStats vitraTx;
static uint32_t vitraLastWaitMs = 0;

typedef void (*VitraRxHandler)(const String &pkt, int rssi);

static VitraRxHandler vitraRxHandler = nullptr;
static void (*vitraIdleHook)() = nullptr;

static uint8_t vitraReadReg(uint8_t reg) {
  SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));

  digitalWrite(LORA_SS, LOW);
  SPI.transfer(reg & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(LORA_SS, HIGH);

  SPI.endTransaction();
  return v;
}

static void vitraRadioConfig() {
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.enableCrc();
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN); 
}

static uint32_t vitraAirtimeMs(int payloadLen) {
  const float tsym = (float)(1UL << LORA_SF) / 125.0f;
  const int de = (LORA_SF >= 11) ? 1 : 0;

  float num = 8.0f * payloadLen - 4.0f * LORA_SF + 28.0f + 16.0f;
  float den = 4.0f * (LORA_SF - 2 * de);

  int blocks = (int)ceilf(num / den);
  if (blocks < 0) blocks = 0;

  float symbols = 12.25f + 8.0f + blocks * 5.0f;

  return (uint32_t)(symbols * tsym + 0.5f);
}

static void vitraArmRx() {
  if ((vitraReadReg(VR_REG_OPMODE) & 0x07) != 0x05) {
    LoRa.receive();
  }
}

static bool vitraPoll() {
  if (!(vitraReadReg(VR_REG_IRQFLAGS) & VR_IRQ_RXDONE)) {
    vitraArmRx();
    return false;
  }

  int n = LoRa.parsePacket();
  String s;
  int rssi = 0;

  if (n > 0) {
    s.reserve(n + 1);

    while (LoRa.available()) {
      s += (char)LoRa.read();
    }

    rssi = LoRa.packetRssi();
  }

  LoRa.receive();

  if (n <= 0) return false;

  s.trim();

  if (s.length() > 0 && vitraRxHandler) {
    vitraRxHandler(s, rssi);
  }

  return true;
}

static bool vitraChannelBusy() {
  if (vitraReadReg(VR_REG_MODEMSTAT) & 0x03) {
    return true;
  }

  return LoRa.rssi() > VITRA_CCA_RSSI_DBM;
}

static bool vitraSend(const String &msg, uint8_t prio) {
  static bool inSend = false;

  if (inSend) return false;

  inSend = true;

  if (prio > PRIO_NORMAL) prio = PRIO_NORMAL;

  const VitraWin &w = VITRA_WIN[prio];

  const uint32_t t0 = millis();
  uint32_t last = t0;
  uint32_t listened = 0;

  int32_t backoff = (int32_t)random(
    (long)w.lo,
    (long)w.hi + 1
  );

  bool deferred = false;
  bool forced = false;

  while (true) {
    vitraPoll();

    if (vitraIdleHook) {
      vitraIdleHook();
    }

    uint32_t now = millis();
    uint32_t dt = now - last;
    last = now;

    if (now - t0 >= w.maxWait) {
      forced = true;
      break;
    }

    if (vitraChannelBusy()) {
      deferred = true;
      listened = 0;
      delay(2);
      continue;
    }

    listened += dt;
    backoff -= (int32_t)dt;

    if (listened >= VITRA_MIN_LISTEN_MS && backoff <= 0) {
      break;
    }

    delay(2);
  }

  const uint32_t tTx = millis();

  LoRa.beginPacket();
  LoRa.print(msg);
  int ok = LoRa.endPacket();

  const uint32_t tEnd = millis();

  vitraLastWaitMs = tTx - t0;

  vitraTx.sent[prio]++;
  vitraTx.waitSum[prio] += vitraLastWaitMs;

  if (vitraLastWaitMs > vitraTx.waitMax[prio]) {
    vitraTx.waitMax[prio] = vitraLastWaitMs;
  }

  if (deferred) vitraTx.deferred[prio]++;
  if (forced) vitraTx.forced[prio]++;

  vitraTx.airMs += (tEnd - tTx);

  vitraArmRx();

  inSend = false;

  return ok == 1;
}

static void vitraPrintNoiseFloor() {
  LoRa.receive();
  delay(200);
  long s = 0;
  for (int i = 0; i < 50; i++) { s += LoRa.rssi(); delay(10); }
  Serial.println("[LoRa] Noise floor: " + String(s / 50) + " dBm");
}