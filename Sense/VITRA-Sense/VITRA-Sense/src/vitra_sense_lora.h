#pragma once
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------
// PIN ESP32-C3 SuperMini <-> SX1278
// Pin terpakai di main.cpp: 1, 3, 4, 5, 10 (GPIO) dan 20/21 (I2C).
// DIO0 tidak dipakai (penerimaan memakai polling register).
// ---------------------------------------------------------------------
#define LORA_PIN_SCK   8
#define LORA_PIN_MOSI  9
#define LORA_PIN_MISO  0
#define LORA_SS        2      
#define LORA_PIN_RST   6
#define LORA_PIN_DIO0  -1

#define LORA_FREQUENCY 433E6
#define LORA_SF        9     

#include "vitra_radio.h"


// ---------------------------------------------------------------------
// PARAMETER
// ---------------------------------------------------------------------
// DATA DUMMY: 1 = Sense langsung membuat data HR/SpO2/RR normal acak tanpa sensor dan
// sesi langsung berjalan saat boot (berguna tanpa jari di MAX30102). Bisa juga
// dinyalakan lewat Serial dengan perintah 'd', dimatikan dengan 'x'.
#ifndef SENSE_DUMMY_DEFAULT
#define SENSE_DUMMY_DEFAULT      0
#endif
#define SENSE_LORA_TERMINAL      1          // 1 = perintah uji lewat Serial Monitor
#define SENSE_ACK_WAIT_MS        4000UL     // tunggu WACK setelah paket terkirim
#define SENSE_ACK_MAX_TRIES      5
#define SENSE_URGENT_CONFIRM_MS  3000UL     // perubahan level harus bertahan selama ini
#define SENSE_URGENT_MIN_GAP_MS  8000UL     // jarak minimum kirim-segera non-kritis
#define SENSE_RADIO_RETRY_MS     10000UL    // coba init ulang LoRa bila gagal
#define SENSE_TEMP_NEUTRAL       36.6f      // Sense tidak punya sensor suhu

// Bila saat jadwal kirim belum ada pembacaan valid, tunggu sebentar sebelum
// mengirim paket "tanpa sinyal" (HR/SpO2/RR = 0).
static const uint32_t SENSE_SCHED_GRACE_MS =
  (VITRA_SEND_INTERVAL_MS / 3 < 20000UL) ? (VITRA_SEND_INTERVAL_MS / 3) : 20000UL;


// ---------------------------------------------------------------------
// DATA DARI main.cpp
// ---------------------------------------------------------------------
struct SenseSnapshot {
  bool    valid;        // dataSensorValid
  int     hr, spo2, rr;
  uint8_t triLevel;     // 0 hijau, 1 kuning, 2 merah (hasil hitungTriasePPG)
  bool    fall;         // jatuh terdeteksi (gerakanDarurat)
  bool    sos;          // emergency manual (emergencyAktif && !gerakanDarurat)
};


// ---------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------
enum SenseSession : uint8_t { SS_IDLE, SS_SENSING, SS_DONE };

struct SenseTxJob {
  char     msg[112];
  uint8_t  prio;
  bool     wantAck;
  bool     used;
  uint32_t enq;
};

#define SENSE_TXQ 4

static char          slId[16];
static bool          slRadioOk = false;
static uint32_t      slRadioRetryAt = 0;

static SenseSession  slState = SS_IDLE;
static uint32_t      slStart = 0;
static uint32_t      slSent = 0;            // pengiriman terjadwal pada sesi ini
static bool          slDueActive = false;
static uint32_t      slDueSince = 0;
static uint8_t       slSeq = 0;

static int8_t        slLastLevel = 0;       // level pada pembacaan terakhir yang dikirim
static int8_t        slCandLevel = -1;
static uint32_t      slCandSince = 0;
static uint32_t      slLastUrgent = 0;
static bool          slPrevFall = false;
static bool          slPrevSos = false;

static bool          slRestartReq = false;

// Konfirmasi WACK
static bool          slAckPending = false;
static bool          slAckArmed = false;    // true setelah paket benar-benar terkirim
static char          slAckMsg[112];
static uint8_t       slAckPrio = 0;
static uint8_t       slAckTries = 0;
static uint32_t      slAckDeadline = 0;

// Mesin kirim
static SenseTxJob    slQ[SENSE_TXQ];
static int           slCur = -1;
static bool          slTxBusy = false;
static uint32_t      slAttT0 = 0, slAttLast = 0, slListened = 0, slTxStart = 0, slTxTimeout = 0;
static int32_t       slBackoff = 0;
static bool          slDeferred = false;

// Simulasi lewat terminal
static int8_t        slSimLevel = -1;       // -1 = pakai sensor asli
static bool          slSimFall = false;
static uint32_t      slSimNext = 0;
static int           slSimHr = 0, slSimSpo2 = 0, slSimRr = 0;


// ---------------------------------------------------------------------
// UTIL
// ---------------------------------------------------------------------
static uint8_t slLevel(const SenseSnapshot &s) {

  if (!s.valid) {
    return 0;
  }

  uint8_t l = s.triLevel;

  // Ambang bersama dengan Link (HR, SpO2). Sense asli tidak punya sensor suhu,
  // jadi dipakai suhu netral agar suhu tidak ikut menaikkan level.
  int v = vitraClassify(s.hr, s.spo2, SENSE_TEMP_NEUTRAL).lv;

  if (v > (int)l) {
    l = (uint8_t)v;
  }

  return l;
}

static const char *slEvName(const SenseSnapshot &s) {
  if (s.fall) return "FALL";
  if (s.sos)  return "SOS";
  return nullptr;
}


// ---------------------------------------------------------------------
// ANTRIAN + KIRIM NON-BLOCKING (CSMA: dengar dulu, backoff per kelas)
// ---------------------------------------------------------------------
static bool slEnqueue(const char *msg, uint8_t prio, bool wantAck) {

  for (int i = 0; i < SENSE_TXQ; i++) {
    if (slQ[i].used && strcmp(slQ[i].msg, msg) == 0) {
      return true;                       // pesan identik sudah mengantri
    }
  }

  int slot = -1;

  for (int i = 0; i < SENSE_TXQ; i++) {
    if (!slQ[i].used) { slot = i; break; }
  }

  if (slot < 0) {
    // Penuh: korbankan job berprioritas paling rendah, asal lebih rendah dari yang baru
    int worst = -1;

    for (int i = 0; i < SENSE_TXQ; i++) {
      if (slTxBusy && i == slCur) continue;
      if (slQ[i].prio > prio && (worst < 0 || slQ[i].prio > slQ[worst].prio)) worst = i;
    }

    if (worst < 0) {
      Serial.println("[LORA] Antrian penuh, paket dibuang");
      return false;
    }

    slot = worst;

    if (slot == slCur) slCur = -1;
  }

  strncpy(slQ[slot].msg, msg, sizeof(slQ[slot].msg) - 1);
  slQ[slot].msg[sizeof(slQ[slot].msg) - 1] = 0;
  slQ[slot].prio = prio;
  slQ[slot].wantAck = wantAck;
  slQ[slot].enq = millis();
  slQ[slot].used = true;

  return true;
}

static void slStartAttempt(int idx, uint32_t now) {

  const VitraWin &w = VITRA_WIN[slQ[idx].prio];

  slCur = idx;
  slAttT0 = now;
  slAttLast = now;
  slListened = 0;
  slBackoff = (int32_t)random((long)w.lo, (long)w.hi + 1);
  slDeferred = false;
}

static void slTxService() {

  uint32_t now = millis();

  // --- paket sedang di udara: jangan sentuh mode radio (jangan panggil vitraPoll) ---
  if (slTxBusy) {

    bool still = ((vitraReadReg(VR_REG_OPMODE) & 0x07) == 0x03);   // mode TX (isTransmitting() private di library)

    if (still && (uint32_t)(now - slTxStart) < slTxTimeout) {
      return;
    }

    SenseTxJob &j = slQ[slCur];

    vitraTx.airMs += (now - slTxStart);

    if (j.wantAck && slAckPending) {
      slAckArmed = true;
      slAckDeadline = now + SENSE_ACK_WAIT_MS;
    }

    j.used = false;
    slCur = -1;
    slTxBusy = false;

    vitraArmRx();
    return;
  }

  // --- dengarkan (paket dari Link: WACK, RESTART) ---
  vitraPoll();

  // --- pilih job terbaik: prioritas tertinggi, lalu yang paling lama ---
  int best = -1;

  for (int i = 0; i < SENSE_TXQ; i++) {

    if (!slQ[i].used) continue;

    if (best < 0 ||
        slQ[i].prio < slQ[best].prio ||
        (slQ[i].prio == slQ[best].prio && (int32_t)(slQ[i].enq - slQ[best].enq) < 0)) {
      best = i;
    }
  }

  if (best < 0) {
    slCur = -1;
    return;
  }

  // Job yang lebih penting menyela yang sedang menunggu giliran
  if (slCur < 0 || !slQ[slCur].used || slQ[best].prio < slQ[slCur].prio) {
    slStartAttempt(best, now);
  }

  const VitraWin &w = VITRA_WIN[slQ[slCur].prio];

  uint32_t dt = now - slAttLast;
  slAttLast = now;

  bool forced = false;

  if ((uint32_t)(now - slAttT0) >= w.maxWait) {

    forced = true;                       // kanal tak kunjung sepi: kirim juga

  } else if (vitraChannelBusy()) {

    slDeferred = true;
    slListened = 0;
    return;

  } else {

    slListened += dt;
    slBackoff -= (int32_t)dt;

    if (slListened < VITRA_MIN_LISTEN_MS || slBackoff > 0) {
      return;
    }
  }

  // --- kirim (async) ---
  SenseTxJob &j = slQ[slCur];

  if (!LoRa.beginPacket()) {
    return;                              // radio masih sibuk; coba lagi putaran berikut
  }

  LoRa.print(j.msg);
  LoRa.endPacket(true);

  slTxStart = millis();
  slTxTimeout = vitraAirtimeMs((int)strlen(j.msg)) + 300;
  slTxBusy = true;

  vitraLastWaitMs = slTxStart - slAttT0;

  vitraTx.sent[j.prio]++;
  vitraTx.waitSum[j.prio] += vitraLastWaitMs;
  if (vitraLastWaitMs > vitraTx.waitMax[j.prio]) vitraTx.waitMax[j.prio] = vitraLastWaitMs;
  if (slDeferred) vitraTx.deferred[j.prio]++;
  if (forced) vitraTx.forced[j.prio]++;

  Serial.printf("[LORA] TX P%d tunggu=%lums > %s\n",
                j.prio, (unsigned long)vitraLastWaitMs, j.msg);
}


// ---------------------------------------------------------------------
// PAKET MASUK DARI LINK
// ---------------------------------------------------------------------
static void slOnPacket(const String &pkt, int rssi) {

  (void)rssi;

  const char *s = pkt.c_str();

  if (strncmp(s, "WACK|", 5) == 0) {

    if (strcmp(s + 5, slId) == 0 && slAckPending) {
      slAckPending = false;
      slAckArmed = false;
      Serial.println("[LORA] Link mengonfirmasi warning (WACK)");
    }

  } else if (strncmp(s, "RESTART|", 8) == 0) {

    if (strcmp(s + 8, slId) == 0) {
      slRestartReq = true;               // diproses di senseLoraTick
    }
  }
}


// ---------------------------------------------------------------------
// SESI
// ---------------------------------------------------------------------
static void slStartSession(const char *why) {

  slState = SS_SENSING;
  slStart = millis();
  slSent = 0;
  slDueActive = false;
  slLastLevel = 0;
  slCandLevel = -1;

  // Buang kiriman rutin lama yang masih antre (yang sedang di udara dibiarkan)
  for (int i = 0; i < SENSE_TXQ; i++) {
    if (slQ[i].used && slQ[i].prio == PRIO_NORMAL && !(slTxBusy && i == slCur)) {
      slQ[i].used = false;
    }
  }

  Serial.printf("[SESI] %s mulai dari awal (%s): %d pengiriman, tiap %lu dtk\n",
                slId, why, (int)VITRA_SESSION_SENDS,
                (unsigned long)(VITRA_SEND_INTERVAL_MS / 1000UL));
}

// Susun dan antrikan satu paket. scheduled = bagian dari 8 pengiriman terjadwal.
static void slSendPacket(const SenseSnapshot &s, bool scheduled, bool last, const char *why) {

  int hr = 0, spo2 = 0, rr = 0;

  if (s.valid) {
    hr = s.hr;
    spo2 = s.spo2;
    rr = s.rr;
  }

  uint8_t lvl = slLevel(s);
  const char *ev = slEvName(s);

  uint8_t prio = (ev || lvl >= 2) ? PRIO_CRIT : (lvl == 1) ? PRIO_WARN : PRIO_NORMAL;
  bool askAck = (ev != nullptr) || (lvl >= 1);

  // Sesi sudah selesai -> setiap paket (mis. jatuh) tetap menandai LAST=1 supaya
  // dashboard Link tidak menganggap Sense hidup kembali / offline.
  bool markLast = last || (slState == SS_DONE);

  char msg[112];

  int n = snprintf(msg, sizeof(msg), "SENSE|%s|HR=%d|SPO2=%d|RR=%d|Q=%u",
                   slId, hr, spo2, rr, (unsigned)slSeq++);

  if (ev && n > 0 && n < (int)sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, "|EV=%s", ev);
  if (askAck && n > 0 && n < (int)sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, "|A=1");
  if (markLast && n > 0 && n < (int)sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, "|LAST=1");

  if (s.valid) {
    slLastLevel = (int8_t)lvl;
  }

  // Paket baru menggantikan konfirmasi lama: bila paket baru tidak darurat,
  // pengulangan warning lama dibatalkan (agar tidak membunyikan alarm basi).
  if (askAck) {
    slAckPending = true;
    slAckArmed = false;
    strncpy(slAckMsg, msg, sizeof(slAckMsg) - 1);
    slAckMsg[sizeof(slAckMsg) - 1] = 0;
    slAckPrio = prio;
    slAckTries = 1;
  } else {
    slAckPending = false;
    slAckArmed = false;
  }

  Serial.printf("[LORA] %s -> %s (level %d%s%s)\n",
                scheduled ? "Terjadwal" : "SEGERA", why, (int)lvl,
                ev ? ", " : "", ev ? ev : "");

  slEnqueue(msg, prio, askAck);
}


// ---------------------------------------------------------------------
// TERMINAL UJI
// ---------------------------------------------------------------------
#if SENSE_LORA_TERMINAL
static void slPrintStats() {

  const char *nm[4] = { "CRIT", "WARN", "REG", "NORMAL" };
  const char *st = (slState == SS_IDLE) ? "IDLE (menunggu data pasien)"
                 : (slState == SS_SENSING) ? "SENSING" : "SELESAI";

  Serial.println();
  Serial.println("---- STATUS LoRa SENSE ----");
  Serial.printf("ID %s | radio %s | sesi %s", slId, slRadioOk ? "OK" : "GAGAL", st);

  if (slState == SS_SENSING) {
    Serial.printf(" | terjadwal %lu/%lu", (unsigned long)slSent, (unsigned long)VITRA_SESSION_SENDS);
  }

  Serial.println();
  Serial.println("kelas   kirim  tunda  paksa  tunggu_rata  tunggu_maks");

  for (int i = 0; i < 4; i++) {

    if (vitraTx.sent[i] == 0) continue;

    Serial.printf("%-6s %6lu %6lu %6lu %9lu ms %9lu ms\n",
                  nm[i],
                  (unsigned long)vitraTx.sent[i],
                  (unsigned long)vitraTx.deferred[i],
                  (unsigned long)vitraTx.forced[i],
                  (unsigned long)(vitraTx.waitSum[i] / vitraTx.sent[i]),
                  (unsigned long)vitraTx.waitMax[i]);
  }

  Serial.println("---------------------------");
}

static void slTerminal() {

  while (Serial.available()) {

    char c = (char)Serial.read();

    switch (c) {
      case 'e': slSimLevel = 2; slSimNext = 0; Serial.println("[SIM] Pembacaan KRITIS (nilai simulasi)"); break;
      case 'w': slSimLevel = 1; slSimNext = 0; Serial.println("[SIM] Pembacaan WASPADA (nilai simulasi)"); break;
      case 'n': slSimLevel = 0; slSimNext = 0; Serial.println("[SIM] Pembacaan NORMAL (nilai simulasi)"); break;
      case 'd':
        slSimLevel = 0; slSimNext = 0;
        Serial.println("[SIM] DATA DUMMY aktif (normal, acak tiap detik). e/w/n mengubah kondisinya, x mematikan.");
        if (slState != SS_SENSING) slStartSession("data dummy");
        break;
      case 'l': {
        // Uji loopback SPI: LEPAS kabel MISO dari modul, sambungkan pin MOSI langsung ke pin MISO di ESP.
        const uint8_t pat[4] = { 0xA5, 0x5A, 0xFF, 0x00 };
        int ok = 0;
        Serial.printf("[LOOPBACK] Sambungkan GPIO%d (MOSI) langsung ke GPIO%d (MISO), modul tidak tersambung ke MISO\n",
                      LORA_PIN_MOSI, LORA_PIN_MISO);
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
        for (int i = 0; i < 4; i++) {
          uint8_t r = SPI.transfer(pat[i]);
          Serial.printf("[LOOPBACK] kirim 0x%02X terima 0x%02X %s\n", pat[i], r, r == pat[i] ? "OK" : "BEDA");
          if (r == pat[i]) ok++;
        }
        SPI.endTransaction();
        if (ok == 4) Serial.println("[LOOPBACK] SISI ESP SEHAT: masalah ada di modul, kabel ke modul, atau daya modul.");
        else Serial.println("[LOOPBACK] GAGAL: kabel MOSI-MISO belum tersambung benar, atau pin SPI ESP bermasalah.");
        break;
      }
      case 'x': slSimLevel = -1; slSimFall = false; Serial.println("[SIM] Kembali ke sensor asli"); break;
      case 'f': slSimFall = !slSimFall; Serial.println(slSimFall ? "[SIM] Jatuh AKTIF (hanya sisi LoRa)" : "[SIM] Jatuh dibersihkan"); break;
      case 'r': slStartSession("perintah manual"); break;
      case 's': slPrintStats(); break;
      case '?':
        Serial.println("Perintah LoRa: d = data dummy (mulai sesi), l = uji loopback SPI, e/w/n = kritis/waspada/normal, x = sensor asli,");
        Serial.println("               f = simulasi jatuh, r = ulang sesi, s = status");
        break;
      default: break;
    }
  }
}
#else
static void slTerminal() {}
#endif

static SenseSnapshot slEffective(const SenseSnapshot &in, uint32_t now) {

  SenseSnapshot s = in;

  if (slSimLevel >= 0) {

    if ((int32_t)(now - slSimNext) >= 0) {
      float t;
      slSimNext = now + 1000;
      vitraSimValues(slSimLevel, slSimHr, slSimSpo2, t);
      slSimRr = (slSimLevel >= 2) ? 34 : (slSimLevel == 1) ? 22 : 16;
    }

    s.valid = true;
    s.hr = slSimHr;
    s.spo2 = slSimSpo2;
    s.rr = slSimRr;
    s.triLevel = (uint8_t)slSimLevel;
  }

  if (slSimFall) {
    s.fall = true;
  }

  return s;
}


// ---------------------------------------------------------------------
// API
// ---------------------------------------------------------------------
// Diagnosa saat LoRa.begin() gagal: baca RegVersion (0x42) langsung lewat SPI.
// 0x12 = SX1276/77/78/79 menjawab | 0x00 atau 0xFF = chip tidak menjawab.
static void slDiagnose() {

  pinMode(LORA_SS, OUTPUT);
  digitalWrite(LORA_SS, HIGH);

  if (LORA_PIN_RST >= 0) {
    pinMode(LORA_PIN_RST, OUTPUT);
    digitalWrite(LORA_PIN_RST, LOW);
    delay(10);
    digitalWrite(LORA_PIN_RST, HIGH);
    delay(20);
  }

  uint8_t v = vitraReadReg(0x42);

  Serial.printf("[LORA] DIAG RegVersion = 0x%02X  (pin SCK=%d MOSI=%d MISO=%d NSS=%d RST=%d)\n",
                v, LORA_PIN_SCK, LORA_PIN_MOSI, LORA_PIN_MISO, LORA_SS, LORA_PIN_RST);

  if (v == 0x12) {
    Serial.println("[LORA] Chip menjawab (0x12) tapi begin() gagal: cek SF/frekuensi atau reset ulang daya.");
  } else if (v == 0x00) {
    Serial.println("[LORA] 0x00: MISO tertarik ke GND / tidak tersambung, atau modul tidak diberi daya 3V3.");
  } else if (v == 0xFF) {
    Serial.println("[LORA] 0xFF: MISO mengambang/ke 3V3 atau NSS salah: cek NSS, MISO, SCK, dan GND.");
  } else {
    Serial.println("[LORA] Nilai tak terduga: kemungkinan MOSI/SCK tertukar atau modul bukan SX127x.");
  }
}

static bool slRadioInit() {

  SPI.begin(LORA_PIN_SCK, LORA_PIN_MISO, LORA_PIN_MOSI, LORA_SS);

  LoRa.setPins(LORA_SS, LORA_PIN_RST, LORA_PIN_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    slDiagnose();
    return false;
  }

  vitraRadioConfig();
  vitraRxHandler = slOnPacket;
  LoRa.receive();

  return true;
}

static void senseLoraBegin(uint16_t nodeId) {

  snprintf(slId, sizeof(slId), "VITRA-%03u", (unsigned)nodeId);

  slRadioOk = slRadioInit();

  Serial.println();
  Serial.println("=== LoRa VITRA-SENSE ===");
  Serial.printf("Node ID : %s (harus sama dengan QR di badan perangkat)\n", slId);
  Serial.printf("Radio   : %s | 433 MHz | SF%d\n", slRadioOk ? "OK" : "GAGAL (dicoba ulang tiap 10 dtk)", LORA_SF);
  Serial.printf("Jadwal  : %lu kali kirim, tiap %lu dtk, sesi %lu dtk\n",
                (unsigned long)VITRA_SESSION_SENDS,
                (unsigned long)(VITRA_SEND_INTERVAL_MS / 1000UL),
                (unsigned long)(VITRA_SESSION_MS / 1000UL));
  Serial.println("Sesi mulai saat triase disimpan di Sense atau RESTART dari Link.");
#if SENSE_LORA_TERMINAL
  Serial.println("Serial: ketik ? untuk perintah uji LoRa (d = data dummy)");
#endif
#if SENSE_DUMMY_DEFAULT
  slSimLevel = 0;
  slSimNext = 0;
  Serial.println("[SIM] SENSE_DUMMY_DEFAULT aktif: data dummy dipakai sejak boot");
  slStartSession("data dummy");
#endif
}

// Panggil saat data pasien di Sense berubah (triase asli disimpan)
static void senseLoraPatientChanged() {
  slStartSession("data pasien berubah");
}

static void senseLoraTick(const SenseSnapshot &in) {

  uint32_t now = millis();

  slTerminal();

  SenseSnapshot s = slEffective(in, now);

  if (!slRadioOk) {

    if ((int32_t)(now - slRadioRetryAt) >= 0) {
      slRadioRetryAt = now + SENSE_RADIO_RETRY_MS;
      slRadioOk = slRadioInit();
      if (slRadioOk) Serial.println("[LORA] Radio terhubung kembali");
    }

    return;
  }

  slTxService();

  if (slRestartReq) {
    slRestartReq = false;
    slStartSession("RESTART dari Link");
  }

  // ---- jatuh / tombol emergency: aktif di semua state ----
  bool evNow = s.fall || s.sos;
  bool evPrev = slPrevFall || slPrevSos;
  bool evChanged = (s.fall != slPrevFall) || (s.sos != slPrevSos);

  if (evChanged) {

    if (evNow) {
      slSendPacket(s, false, false, s.fall ? "jatuh terdeteksi" : "tombol emergency");
    } else if (evPrev) {
      slSendPacket(s, false, false, "emergency dibatalkan / di-reset");
    }

    slLastUrgent = now;
  }

  slPrevFall = s.fall;
  slPrevSos = s.sos;

  // ---- sesi vital ----
  if (slState == SS_SENSING) {

    uint8_t lvl = slLevel(s);

    // Status menurun (atau pulih) -> kirim segera setelah bertahan beberapa detik
    if (!s.valid || (int)lvl == (int)slLastLevel) {

      slCandLevel = -1;

    } else if (slCandLevel != (int8_t)lvl) {

      slCandLevel = (int8_t)lvl;
      slCandSince = now;

    } else if ((uint32_t)(now - slCandSince) >= SENSE_URGENT_CONFIRM_MS) {

      bool worse = (int)lvl > (int)slLastLevel;
      bool gapOk = (uint32_t)(now - slLastUrgent) >= SENSE_URGENT_MIN_GAP_MS;

      if ((worse && lvl >= 2) || gapOk) {
        slSendPacket(s, false, false, worse ? "status menurun" : "status membaik");
        slLastUrgent = now;
        slCandLevel = -1;
      }
    }

    // Pengiriman terjadwal (jadwal absolut dari awal sesi, tanpa drift)
    if (!slDueActive) {

      uint32_t due = slStart + (slSent + 1) * VITRA_SEND_INTERVAL_MS;

      if ((int32_t)(now - due) >= 0) {
        slDueActive = true;
        slDueSince = now;
      }
    }

    if (slDueActive && (s.valid || (uint32_t)(now - slDueSince) >= SENSE_SCHED_GRACE_MS)) {

      bool last = (slSent + 1 >= (uint32_t)VITRA_SESSION_SENDS);

      slSendPacket(s, true, last, s.valid ? "data rutin" : "data rutin (tanpa sinyal)");

      slSent++;
      slDueActive = false;

      if (last) {
        slState = SS_DONE;
        Serial.printf("[SESI] %s selesai. Sense berhenti bekerja sampai data pasien berubah.\n", slId);
      }
    }
  }

  // ---- ulangi warning yang belum dikonfirmasi Link ----
  if (slAckPending && slAckArmed && (int32_t)(now - slAckDeadline) >= 0) {

    if (slAckTries >= SENSE_ACK_MAX_TRIES) {

      slAckPending = false;
      slAckArmed = false;
      Serial.println("[LORA] Link tidak membalas WACK, berhenti mengulang");

    } else {

      slAckTries++;
      slAckArmed = false;

      Serial.printf("[LORA] Warning dikirim ulang (%d/%d)\n", (int)slAckTries, SENSE_ACK_MAX_TRIES);

      slEnqueue(slAckMsg, slAckPrio, true);
    }
  }
}
