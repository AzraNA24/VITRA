#pragma once
#include <Arduino.h>

struct VitraLoraStatus {
  bool     ready;          // modul LoRa terdeteksi dan terkonfigurasi
  uint32_t sendsDone;      // jumlah pengiriman terjadwal pada sesi ini
  uint32_t sendsTotal;     // jumlah pengiriman terjadwal per sesi
  bool     sessionDone;    // pengiriman terjadwal terakhir (LAST=1) sudah dikirim
  bool     alertActive;    // ada pembacaan waspada/kritis yang sedang dilaporkan
  bool     alertAcked;     // alert sudah dikonfirmasi LINK (WACK)
  uint32_t txCount;        // total paket SENSE yang dikirim
  uint32_t lastTxMs;       // millis() pengiriman terakhir (0 = belum pernah)
  uint8_t  emergency;      // darurat yang dilaporkan: 0 tidak ada, 1 jatuh, 2 tombol manual
  bool     emergencyAcked; // status darurat terakhir sudah dikonfirmasi LINK (SACK)
  int      dummyMode;      // -1 sensor asli | 0 normal | 1 waspada | 2 kritis
};


bool vitraLoraBegin(uint16_t nodeId);


void vitraLoraLoop(uint32_t now, bool valid, int hr, int spo2, int rr);

void vitraLoraSetServiceHook(void (*hook)());


void vitraLoraSetRestartHandler(void (*handler)());

// Ulangi sesi dari awal (dijalankan pada iterasi vitraLoraLoop berikutnya).
void vitraLoraRestartSession();

void vitraLoraSetEmergency(uint8_t type);

void vitraLoraSetDummy(int mode);

int vitraLoraLevel(int hr, int spo2, int rr);

bool            vitraLoraReady();
const char     *vitraLoraId();     
VitraLoraStatus vitraLoraStatus();
