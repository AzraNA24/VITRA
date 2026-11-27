#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <stdarg.h>

#include "vitra_lora.h"

// ---------------------------------------------------------------------
// 1. PIN DAN PARAMETER RADIO
// ---------------------------------------------------------------------
#ifndef LORA_PIN_SCK
#define LORA_PIN_SCK   8
#endif
#ifndef LORA_PIN_MOSI
#define LORA_PIN_MOSI  9
#endif
#ifndef LORA_PIN_MISO
#define LORA_PIN_MISO  0
#endif
#ifndef LORA_SS
#define LORA_SS        2       // dipakai juga oleh vitra_radio.h
#endif
#ifndef LORA_PIN_RST
#define LORA_PIN_RST   6
#endif
#ifndef LORA_PIN_DIO0
#define LORA_PIN_DIO0  -1
#endif

#ifndef LORA_FREQUENCY
#define LORA_FREQUENCY 433E6   // harus sama dengan Link dan Log
#endif

// SF HARUS SAMA di Sense, Link, dan Log (Link memakai 9).
#ifndef LORA_SF
#define LORA_SF 11

#endif

// ---------------------------------------------------------------------
// 2. PARAMETER PERILAKU
// ---------------------------------------------------------------------
#ifndef LORA_POLL_MS
#define LORA_POLL_MS          10     // jeda polling penerimaan
#endif
#ifndef LORA_RETRY_INIT_MS
#define LORA_RETRY_INIT_MS    5000   // jeda coba ulang kalau modul tidak terdeteksi
#endif
#ifndef LORA_ALERT_HOLD_MS
#define LORA_ALERT_HOLD_MS    5000   // pembacaan abnormal harus bertahan sekian ms sebelum dikirim di luar jadwal
#endif
#ifndef LORA_ALERT_RETRY_MS
#define LORA_ALERT_RETRY_MS   4000   // jeda kirim ulang selama WACK belum diterima
#endif
#ifndef LORA_ALERT_MAX_TRIES
#define LORA_ALERT_MAX_TRIES  5      // batas pengiriman alert per episode
#endif
#ifndef LORA_SOS_RETRY_MS
#define LORA_SOS_RETRY_MS     4000   // jeda kirim ulang SOS selama SACK belum diterima
#endif
#ifndef LORA_SOS_MAX_TRIES
#define LORA_SOS_MAX_TRIES    10     // batas pengiriman SOS per perubahan status
#endif
#ifndef LORA_DUMMY_DEFAULT
#define LORA_DUMMY_DEFAULT    -1     // -1 sensor asli | 0 dummy normal | 1 dummy waspada | 2 dummy kritis
#endif
#ifndef LORA_DUMMY_REFRESH_MS
#define LORA_DUMMY_REFRESH_MS 2000   // nilai dummy diacak ulang tiap sekian ms
#endif
#ifndef LORA_SERIAL_CMD
#define LORA_SERIAL_CMD       1      
#endif
#ifndef LORA_NOISE_FLOOR_AT_BOOT
#define LORA_NOISE_FLOOR_AT_BOOT 1   // 1 = ukur noise floor sekali saat boot (±0,7 detik)
#endif

// vitra_radio.h berisi fungsi static yang tidak semuanya dipakai di sini.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "vitra_radio.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------
// 3. STATE
// ---------------------------------------------------------------------
namespace {

enum TxKind : uint8_t { TX_SCHEDULED, TX_ALERT, TX_RETRY, TX_MANUAL };

struct LoraState {
  bool     ready = false;
  bool     spiStarted = false;
  bool     noiseShown = false;
  char     id[24] = "VITRA-000";
  uint8_t  seq = 0;                 // Q, melingkar 0..255

  uint32_t lastInitMs = 0;
  uint32_t lastPollMs = 0;

  // sesi terjadwal
  uint32_t sendsDone = 0;
  bool     sessionDone = false;
  uint32_t nextSendMs = 0;          // 0 = langsung jatuh tempo

  // alert
  uint32_t abnormalSince = 0;       // 0 = sedang tidak abnormal
  int      alertLevelSent = 0;
  uint8_t  alertTries = 0;
  bool     alertAcked = false;
  uint32_t lastAlertTxMs = 0;

  bool     restartRequested = false;

  // darurat jatuh / tombol (SOS)
  uint8_t  sosType = 0;             // status yang diinginkan: 0 tidak ada, 1 jatuh, 2 manual
  uint8_t  sosSentType = 0;        
  uint8_t  sosTries = 0;
  bool     sosAcked = true;
  uint32_t lastSosMs = 0;

  // nilai vital terakhir yang valid (ikut dikirim bersama SOS)
  bool     haveVitals = false;
  int      lastHr = 0, lastSpo2 = 0, lastRr = 0;

  // mode dummy
  int      dummyMode = LORA_DUMMY_DEFAULT;   // -1 mati
  uint32_t dummyNextMs = 0;
  int      dummyHr = 80, dummySpo2 = 98, dummyRr = 16;

  bool     forceSend = false;       // perintah "lora kirim"

  uint32_t txCount = 0;
  uint32_t lastTxMs = 0;

  void (*serviceHook)() = nullptr;
  void (*restartCb)() = nullptr;
};

LoraState g;

// ---------------------------------------------------------------------
// 4. PENERIMAAN
// ---------------------------------------------------------------------
void onRx(const String &pkt, int rssi) {
  (void)rssi;

  if (pkt.startsWith("WACK|")) {
    if (strcmp(pkt.c_str() + 5, g.id) == 0) {
      g.alertAcked = true;
      Serial.println("[LORA] WACK diterima dari LINK");
    }
    return;
  }

  if (pkt.startsWith("SACK|")) {
    // SACK|<id>|T=<t>
    const char *p = pkt.c_str() + 5;
    size_t n = strlen(g.id);

    if (strncmp(p, g.id, n) == 0 && p[n] == '|' && strncmp(p + n + 1, "T=", 2) == 0) {
      const int t = atoi(p + n + 3);

      if (t == (int)g.sosSentType && !g.sosAcked) {
        g.sosAcked = true;
        Serial.print("[LORA] SACK diterima dari LINK (");
        Serial.print(t == 0 ? "darurat selesai" : (t == 1 ? "jatuh" : "tombol darurat"));
        Serial.println(")");
      }
    }
    return;
  }

  if (pkt.startsWith("RESTART|")) {
    if (strcmp(pkt.c_str() + 8, g.id) == 0) {
      g.restartRequested = true;
      Serial.println("[LORA] RESTART dari LINK, sesi akan diulang");
    }
    return;
  }

  // Paket lain bukan untuk Sense ini.
}

// Dipanggil vitraSend() selagi menunggu kanal kosong.
void idleHook() {
  if (g.serviceHook) {
    g.serviceHook();
  }
}

// ---------------------------------------------------------------------
// 5. INISIALISASI RADIO
// ---------------------------------------------------------------------
bool radioInit() {

  if (!g.spiStarted) {
    SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_SS);
    g.spiStarted = true;
  }

  LoRa.setPins(LORA_SS, LORA_PIN_RST, LORA_PIN_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    return false;
  }

  vitraRadioConfig();

  vitraRxHandler = onRx;
  vitraIdleHook = idleHook;

  LoRa.receive();

#if LORA_NOISE_FLOOR_AT_BOOT
  if (!g.noiseShown) {
    g.noiseShown = true;
    vitraPrintNoiseFloor();
  }
#endif

  return true;
}

// ---------------------------------------------------------------------
// 6. PENGIRIMAN
// ---------------------------------------------------------------------
void appendf(char *buf, size_t cap, size_t &len, const char *fmt, ...) {

  if (len >= cap - 1) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);

  if (w > 0) {
    len += (size_t)w;
  }

  if (len >= cap) {
    len = cap - 1;
  }
}

const char *prioName(uint8_t p) {
  switch (p) {
    case PRIO_CRIT:   return "CRIT";
    case PRIO_WARN:   return "WARN";
    case PRIO_REG:    return "REG";
    default:          return "NORMAL";
  }
}

const char *kindName(TxKind k) {
  switch (k) {
    case TX_SCHEDULED: return "terjadwal";
    case TX_ALERT:     return "alert";
    case TX_MANUAL:    return "manual (uji)";
    default:           return "ulang alert";
  }
}

void transmit(int hr, int spo2, int rr, const VitalLevel &v, TxKind kind) {

  const bool alert = v.lv >= 1;

  const uint8_t prio = (v.lv >= 2) ? PRIO_CRIT
                     : (v.lv == 1) ? PRIO_WARN
                                   : PRIO_NORMAL;

  char pkt[96];
  size_t len = 0;

  appendf(pkt, sizeof(pkt), len, "SENSE|%s|HR=%d|SPO2=%d", g.id, hr, spo2);

  if (rr > 0) {
    appendf(pkt, sizeof(pkt), len, "|RR=%d", rr);
  }

  appendf(pkt, sizeof(pkt), len, "|Q=%u", (unsigned)g.seq);

  if (g.sessionDone) {
    appendf(pkt, sizeof(pkt), len, "|LAST=1");
  }

  if (alert) {
    appendf(pkt, sizeof(pkt), len, "|A=1");
  }

  g.seq++;          // uint8_t: kembali ke 0 setelah 255, sesuai hitungan Link (modulo 256)
  g.txCount++;

  if (alert) {
    g.alertTries = (kind == TX_RETRY) ? (uint8_t)(g.alertTries + 1) : 1;
    g.alertAcked = false;     // sebelum kirim, supaya WACK yang datang sesudahnya tidak hilang

    if (v.lv > g.alertLevelSent) {
      g.alertLevelSent = v.lv;
    }
  }

  char why[48];
  vitraWhy(v, why, sizeof(why));

  Serial.print("[LORA] TX ");
  Serial.print(g.dummyMode >= 0 ? "[DUMMY] " : "");
  Serial.print(kindName(kind));
  Serial.print(" (");
  Serial.print(prioName(prio));
  Serial.print("): ");
  Serial.print(pkt);

  if (why[0]) {
    Serial.print("  <- ");
    Serial.print(why);
  }

  Serial.println();

  const bool ok = vitraSend(String(pkt), prio);

  g.lastTxMs = millis();

  if (alert) {
    g.lastAlertTxMs = g.lastTxMs;
  }

  char info[80];
  snprintf(info, sizeof(info), "[LORA] %s, tunggu kanal %lu ms, airtime ~%lu ms",
           ok ? "terkirim" : "GAGAL kirim",
           (unsigned long)vitraLastWaitMs,
           (unsigned long)vitraAirtimeMs((int)len));
  Serial.println(info);
}

const char *sosLabel(uint8_t t) {
  return t == 1 ? "JATUH" : (t == 2 ? "TOMBOL DARURAT" : "SELESAI");
}

void transmitSos(uint32_t now) {

  const uint8_t type = g.sosType;

  char pkt[96];
  size_t len = 0;

  appendf(pkt, sizeof(pkt), len, "SOS|%s|T=%u", g.id, (unsigned)type);

  if (type != 0 && g.haveVitals) {
    appendf(pkt, sizeof(pkt), len, "|HR=%d|SPO2=%d", g.lastHr, g.lastSpo2);

    if (g.lastRr > 0) {
      appendf(pkt, sizeof(pkt), len, "|RR=%d", g.lastRr);
    }
  }

  if (type != g.sosSentType) {
    g.sosTries = 0;            // status baru: hitung ulang
  }

  g.sosSentType = type;
  g.sosAcked = false;          // sebelum kirim, supaya SACK yang datang sesudahnya tidak hilang
  g.sosTries++;
  g.txCount++;

  Serial.println();
  Serial.print("[LORA] >>> SOS ");
  Serial.print(sosLabel(type));
  Serial.print(g.dummyMode >= 0 ? " [DUMMY]" : "");
  Serial.print(" -> LINK (percobaan ");
  Serial.print((int)g.sosTries);
  Serial.print("/");
  Serial.print((int)LORA_SOS_MAX_TRIES);
  Serial.println(")");
  Serial.print("[LORA] TX (CRIT): ");
  Serial.println(pkt);

  const bool ok = vitraSend(String(pkt), PRIO_CRIT);

  g.lastTxMs = millis();
  g.lastSosMs = g.lastTxMs;
  (void)now;

  char info[80];
  snprintf(info, sizeof(info), "[LORA] %s, tunggu kanal %lu ms, airtime ~%lu ms",
           ok ? "terkirim" : "GAGAL kirim",
           (unsigned long)vitraLastWaitMs,
           (unsigned long)vitraAirtimeMs((int)len));
  Serial.println(info);
}

const char *dummyName(int m) {
  return m == 0 ? "NORMAL" : (m == 1 ? "WASPADA" : (m == 2 ? "KRITIS" : "MATI (sensor asli)"));
}

void setDummy(int mode) {

  if (mode < -1 || mode > 2) {
    mode = -1;
  }

  g.dummyMode = mode;
  g.dummyNextMs = 0;

  Serial.print("[LORA] Mode dummy: ");
  Serial.println(dummyName(mode));

  if (mode >= 0) {
    Serial.println("[LORA] Data sensor asli diabaikan oleh LoRa selama dummy aktif");
  }
}

void printHelp() {
  Serial.println("[LORA] Perintah Serial Monitor (kirim dengan Enter):");
  Serial.println("  lora dummy normal|waspada|kritis|off   data dummy sebagai pengganti sensor");
  Serial.println("  lora kirim                             kirim paket SENSE sekarang");
  Serial.println("  lora jatuh                             kirim SOS jatuh");
  Serial.println("  lora sos                               kirim SOS tombol darurat");
  Serial.println("  lora selesai                           akhiri SOS");
  Serial.println("  lora reset                             ulangi sesi dari awal");
  Serial.println("  lora status                            tampilkan status LoRa");
}

void resetSession();

void printStatus() {

  Serial.print("[LORA] Status: ");
  Serial.print(g.ready ? "SIAP" : "MODUL TIDAK TERDETEKSI");
  Serial.print(" | ID ");
  Serial.print(g.id);
  Serial.print(" | sesi ");
  Serial.print((int)g.sendsDone);
  Serial.print("/");
  Serial.print((int)VITRA_SESSION_SENDS);
  Serial.print(g.sessionDone ? " (selesai)" : "");
  Serial.print(" | TX ");
  Serial.print((int)g.txCount);
  Serial.print(" | SOS ");
  Serial.print(g.sosType == 0 ? "tidak ada" : sosLabel(g.sosType));
  Serial.print(g.sosType != 0 || g.sosSentType != 0 ? (g.sosAcked ? " (SACK ok)" : " (menunggu SACK)") : "");
  Serial.print(" | dummy ");
  Serial.println(dummyName(g.dummyMode));
}

void handleCommand(char *line) {

  for (char *c = line; *c; c++) {
    if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
  }

  if (strncmp(line, "lora", 4) != 0 || (line[4] != 0 && line[4] != ' ')) {
    return;                       // bukan untuk modul ini
  }

  const char *a = line + 4;
  while (*a == ' ') a++;

  if (strncmp(a, "dummy", 5) == 0) {
    const char *m = a + 5;
    while (*m == ' ') m++;

    if (!strcmp(m, "normal") || !strcmp(m, "0")) setDummy(0);
    else if (!strcmp(m, "waspada") || !strcmp(m, "1")) setDummy(1);
    else if (!strcmp(m, "kritis") || !strcmp(m, "2")) setDummy(2);
    else if (!strcmp(m, "off") || !strcmp(m, "mati")) setDummy(-1);
    else Serial.println("[LORA] Pakai: lora dummy normal|waspada|kritis|off");
  }
  else if (!strcmp(a, "kirim"))   { g.forceSend = true; }
  else if (!strcmp(a, "jatuh"))   { vitraLoraSetEmergency(1); }
  else if (!strcmp(a, "sos"))     { vitraLoraSetEmergency(2); }
  else if (!strcmp(a, "selesai")) { vitraLoraSetEmergency(0); }
  else if (!strcmp(a, "reset"))   { g.restartRequested = true; Serial.println("[LORA] Sesi akan diulang dari awal"); }
  else if (!strcmp(a, "status"))  { printStatus(); }
  else                            { printHelp(); }
}

// Baca Serial tanpa memblokir; satu baris = satu perintah.
void pollSerialCommands() {

  static char line[40];
  static uint8_t n = 0;

  while (Serial.available() > 0) {

    const int c = Serial.read();

    if (c < 0) break;

    if (c == '\n' || c == '\r') {
      line[n] = 0;
      n = 0;

      if (line[0]) {
        handleCommand(line);
      }
    }
    else if (n < sizeof(line) - 1) {
      line[n++] = (char)c;
    }
  }
}

void resetSession() {
  g.sendsDone = 0;
  g.sessionDone = false;
  g.nextSendMs = 0;
  g.abnormalSince = 0;
  g.alertLevelSent = 0;
  g.alertTries = 0;
  g.alertAcked = false;
}

}  // namespace

// ---------------------------------------------------------------------
// 7. API
// ---------------------------------------------------------------------
bool vitraLoraBegin(uint16_t nodeId) {

  snprintf(g.id, sizeof(g.id), "VITRA-%03u", (unsigned)nodeId);

  resetSession();

  g.lastInitMs = millis();
  g.ready = radioInit();

  Serial.print("LoRa SX127x: ");
  Serial.println(g.ready ? "TERDETEKSI" : "TIDAK TERDETEKSI");

  if (g.ready) {
    Serial.print("LoRa ID: ");
    Serial.println(g.id);
    Serial.print("LoRa: ");
    Serial.print((int)(LORA_FREQUENCY / 1E6));
    Serial.print(" MHz, SF");
    Serial.println(LORA_SF);
    Serial.print("LoRa pin: SCK=");
    Serial.print(LORA_PIN_SCK);
    Serial.print(" MISO=");
    Serial.print(LORA_PIN_MISO);
    Serial.print(" MOSI=");
    Serial.print(LORA_PIN_MOSI);
    Serial.print(" SS=");
    Serial.print(LORA_SS);
    Serial.print(" RST=");
    Serial.println(LORA_PIN_RST);
    Serial.print("LoRa jadwal: ");
    Serial.print((int)VITRA_SESSION_SENDS);
    Serial.print(" kiriman, tiap ");
    Serial.print((int)(VITRA_SEND_INTERVAL_MS / 1000UL));
    Serial.println(" detik");
    Serial.println("LoRa aktif.");
  } else {
    Serial.println("LoRa nonaktif, VITRA-Sense tetap berjalan (dicoba lagi tiap 5 detik).");
  }

  if (g.dummyMode >= 0) {
    Serial.print("LoRa data DUMMY: ");
    Serial.println(dummyName(g.dummyMode));
  }

#if LORA_SERIAL_CMD
  Serial.println("LoRa perintah serial: ketik \"lora help\"");
#endif

  return g.ready;
}

void vitraLoraLoop(uint32_t now, bool valid, int hr, int spo2, int rr) {

#if LORA_SERIAL_CMD
  pollSerialCommands();
#endif

  // Modul belum siap: coba lagi berkala, jangan memblokir.
  if (!g.ready) {

    if ((uint32_t)(now - g.lastInitMs) >= LORA_RETRY_INIT_MS) {
      g.lastInitMs = now;
      g.ready = radioInit();

      if (g.ready) {
        Serial.println("LoRa SX127x: TERDETEKSI");
        Serial.println("LoRa aktif.");
      }
    }

    return;
  }

  // Penerimaan.
  if ((uint32_t)(now - g.lastPollMs) >= LORA_POLL_MS) {
    g.lastPollMs = now;
    vitraPoll();
  }

  if (g.restartRequested) {
    g.restartRequested = false;
    resetSession();

    if (g.restartCb) {
      g.restartCb();
    }
  }

  // Mode dummy: nilai acak sesuai kelas yang dipilih menggantikan sensor.
  if (g.dummyMode >= 0) {

    if ((int32_t)(now - g.dummyNextMs) >= 0) {
      g.dummyNextMs = now + LORA_DUMMY_REFRESH_MS;
      vitraSimValues(g.dummyMode, g.dummyHr, g.dummySpo2, g.dummyRr);
    }

    valid = true;
    hr = g.dummyHr;
    spo2 = g.dummySpo2;
    rr = g.dummyRr;
  }

  g.haveVitals = valid;

  if (valid) {
    g.lastHr = hr;
    g.lastSpo2 = spo2;
    g.lastRr = rr;
  }

  // Darurat jatuh / tombol: tidak bergantung pada pembacaan vital.
  if (g.sosType != g.sosSentType) {
    transmitSos(now);
    return;
  }

  if (!g.sosAcked && (uint32_t)(now - g.lastSosMs) >= LORA_SOS_RETRY_MS) {

    if (g.sosTries >= LORA_SOS_MAX_TRIES) {
      g.sosAcked = true;       // berhenti; status baru akan mulai dari percobaan 1
      Serial.println("[LORA] SACK tidak diterima, berhenti mengulang SOS");
    } else {
      transmitSos(now);
      return;
    }
  }

  // Tanpa pembacaan valid (jari dilepas, belum stabil) tidak ada yang dikirim.
  if (!valid) {
    g.abnormalSince = 0;

    if (g.forceSend) {
      g.forceSend = false;
      Serial.println("[LORA] Tidak ada data valid untuk dikirim (jari belum terdeteksi?). Coba: lora dummy normal");
    }

    return;
  }

  const VitalLevel v = vitraClassify(hr, spo2, rr);

  if (g.forceSend) {
    g.forceSend = false;
    transmit(hr, spo2, rr, v, TX_MANUAL);
    return;
  }

  if (v.lv >= 1) {

    if (g.abnormalSince == 0) {
      g.abnormalSince = now ? now : 1;
    }

  } else {
    // Kembali normal: episode alert berikutnya mulai dari awal.
    g.abnormalSince = 0;
    g.alertLevelSent = 0;
    g.alertTries = 0;
    g.alertAcked = false;
  }

  // 1) Pengiriman terjadwal. Kalau pembacaan abnormal, paket ini sudah membawa A=1.
  const bool due =
    g.sendsDone < (uint32_t)VITRA_SESSION_SENDS &&
    (int32_t)(now - g.nextSendMs) >= 0;

  if (due) {

    g.sendsDone++;

    if (g.sendsDone >= (uint32_t)VITRA_SESSION_SENDS) {
      g.sessionDone = true;           // paket ini membawa LAST=1
    }

    g.nextSendMs = now + (uint32_t)VITRA_SEND_INTERVAL_MS;

    transmit(hr, spo2, rr, v, TX_SCHEDULED);
    return;
  }

  // 2) Alert di luar jadwal: kondisi memburuk dan sudah bertahan cukup lama.
  const bool alertNew =
    g.abnormalSince != 0 &&
    (uint32_t)(now - g.abnormalSince) >= LORA_ALERT_HOLD_MS &&
    v.lv > g.alertLevelSent;

  if (alertNew) {
    transmit(hr, spo2, rr, v, TX_ALERT);
    return;
  }

  // 3) Ulangi alert sampai WACK diterima.
  const bool alertRetry =
    v.lv >= 1 &&
    g.alertTries > 0 &&
    !g.alertAcked &&
    (uint32_t)(now - g.lastAlertTxMs) >= LORA_ALERT_RETRY_MS;

  if (alertRetry) {

    if (g.alertTries >= LORA_ALERT_MAX_TRIES) {
      // Berhenti mengulang; episode berikutnya (level naik / kembali normal dulu) akan mengirim lagi.
      g.alertTries = 0;
      g.alertLevelSent = v.lv;
      Serial.println("[LORA] WACK tidak diterima, berhenti mengulang alert");
      return;
    }

    transmit(hr, spo2, rr, v, TX_RETRY);
  }
}

void vitraLoraSetEmergency(uint8_t type) {

  if (type > 2) {
    type = 0;
  }

  if (type == g.sosType) {
    return;
  }

  g.sosType = type;

  Serial.print("[LORA] Status darurat: ");
  Serial.println(sosLabel(type));
}

void vitraLoraSetDummy(int mode) {
  setDummy(mode);
}

int vitraLoraLevel(int hr, int spo2, int rr) {
  return vitraClassify(hr, spo2, rr).lv;
}

void vitraLoraSetServiceHook(void (*hook)()) {
  g.serviceHook = hook;
}

void vitraLoraSetRestartHandler(void (*handler)()) {
  g.restartCb = handler;
}

void vitraLoraRestartSession() {
  g.restartRequested = true;
}

bool vitraLoraReady() {
  return g.ready;
}

const char *vitraLoraId() {
  return g.id;
}

VitraLoraStatus vitraLoraStatus() {

  VitraLoraStatus s;

  s.ready = g.ready;
  s.sendsDone = g.sendsDone;
  s.sendsTotal = (uint32_t)VITRA_SESSION_SENDS;
  s.sessionDone = g.sessionDone;
  s.alertActive = g.alertLevelSent > 0;
  s.alertAcked = g.alertAcked;
  s.txCount = g.txCount;
  s.lastTxMs = g.lastTxMs;
  s.emergency = g.sosType;
  s.emergencyAcked = g.sosAcked && g.sosSentType == g.sosType;
  s.dummyMode = g.dummyMode;

  return s;
}
