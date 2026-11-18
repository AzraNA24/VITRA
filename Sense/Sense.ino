#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

// =====================================================
// KONFIGURASI
// =====================================================
#define NODE_ID_START 2

#define SIM_NODES     1
#define MAX_SIM_NODES 32

#define ACK_WAIT_MS   2500
#define ACK_MAX_TRIES 5

#define SENSE_VERBOSE 1

// Pin SX1278
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

// SPI ESP32
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

#define LORA_FREQUENCY 433E6

#define LORA_SF 9

#include "vitra_radio.h"


#define S_SENSING 0
#define S_DONE    1

struct Node {
  char id[12];
  uint8_t mode;        // kondisi simulasi: 0 normal, 1 waspada, 2 kritis
  uint8_t seq;         
  uint8_t state;       
  uint8_t sent;       
  bool urgent;        
  uint32_t nextMs;     

  bool ackWait;
  uint8_t ackTries;
  uint8_t pendPrio;
  uint32_t ackAt;
  String pend;
};

Node nodes[MAX_SIM_NODES];
int nodeCount = 0;

String cmdBuf = "";
uint32_t lastStats = 0;
bool allDoneShown = false;


uint32_t firstDelay() {

  uint32_t d = VITRA_SEND_INTERVAL_MS;

  if (nodeCount > 1) {
    d += random(VITRA_SEND_INTERVAL_MS / 2);
  }

  return d;
}

void startSession(int i, const char *why) {

  Node &n = nodes[i];

  n.state = S_SENSING;
  n.sent = 0;
  n.urgent = false;
  n.nextMs = millis() + firstDelay();

  allDoneShown = false;

  Serial.printf("[SESI] %s mulai dari awal (%s): %d pengiriman, tiap %lu dtk\n",
                n.id, why, (int)VITRA_SESSION_SENDS,
                (unsigned long)(VITRA_SEND_INTERVAL_MS / 1000));
}

void onLoRaPacket(const String &pkt, int rssi) {

  int bar = pkt.indexOf('|');

  if (bar < 0) {
    return;
  }

  String type = pkt.substring(0, bar);

  if (type != "WACK" && type != "RESTART") {
    return;
  }

  String id = pkt.substring(bar + 1);

  int b2 = id.indexOf('|');

  if (b2 >= 0) {
    id = id.substring(0, b2);
  }

  for (int i = 0; i < nodeCount; i++) {

    if (id != nodes[i].id) {
      continue;
    }

    if (type == "WACK") {

      if (nodes[i].ackWait) {
        nodes[i].ackWait = false;
        Serial.printf("[WARN] %s dikonfirmasi Link\n", nodes[i].id);
      }

    } else {

      startSession(i, "data pasien berubah");

    }
  }
}


void setup() {

  Serial.begin(115200);
  delay(1000);

  randomSeed(analogRead(0) + micros());

  Serial.println();
  Serial.println("================================");
  Serial.println("       VITRA-SENSE");
  Serial.println("================================");

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, LORA_SS);

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] GAGAL!");
    while (true) {
      delay(1000);
    }
  }

  vitraRadioConfig();

  vitraRxHandler = onLoRaPacket;

  Serial.println("[LoRa] OK");
  Serial.println("[LoRa] Frequency : 433 MHz");
  Serial.println("[LoRa] SF        : " + String(LORA_SF));

  nodeCount = SIM_NODES;
  if (nodeCount < 1) nodeCount = 1;
  if (nodeCount > MAX_SIM_NODES) nodeCount = MAX_SIM_NODES;

  for (int i = 0; i < nodeCount; i++) {
    snprintf(nodes[i].id, sizeof(nodes[i].id), "VITRA-%03d", NODE_ID_START + i);
    nodes[i].mode = 0;
    nodes[i].seq = (uint8_t)random(256);
    nodes[i].ackWait = false;
    startSession(i, "boot");
  }

  Serial.println("[SIM] Jumlah node: " + String(nodeCount) +
                 " (" + String(nodes[0].id) + " .. " + String(nodes[nodeCount - 1].id) + ")");
  Serial.println();
  Serial.println("Perintah (akhiri Enter):");
  Serial.println("  e / w / n      -> SEMUA node kritis / waspada / normal");
  Serial.println("  e3 / w3 / n3   -> hanya node nomor 3");
  Serial.println("  e / w          -> status menurun: kirim WARNING sekarang juga");
  Serial.println("  r / r3         -> ulangi sesi dari awal (semua / node nomor 3)");
  Serial.println("  s              -> statistik pengiriman");
}


void printTxStats() {

  const char *nm[4] = { "CRIT", "WARN", "REG", "NORMAL" };

  Serial.println();
  Serial.println("---- STATISTIK TX (sejak boot) ----");
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

  float duty = 100.0f * vitraTx.airMs / (float)millis();
  Serial.printf("Duty cycle pemancar ini: %.1f %% (waktu di udara %lu ms)\n",
                duty, (unsigned long)vitraTx.airMs);
  Serial.println("-----------------------------------");
}


void runCommand(const String &cmd) {

  char k = cmd[0];

  if (k == 's') {
    printTxStats();
    return;
  }

  int which = (cmd.length() > 1) ? cmd.substring(1).toInt() : 0;   // 0 = semua

  if (k == 'r') {

    for (int i = 0; i < nodeCount; i++) {
      if (which == 0 || which == i + 1) {
        startSession(i, "perintah manual");
      }
    }

    return;
  }

  int mode = (k == 'e') ? 2 : (k == 'w') ? 1 : (k == 'n') ? 0 : -1;

  if (mode < 0) {
    Serial.println("Perintah tidak dikenal. Pakai: e, w, n, r (+nomor), s");
    return;
  }

  for (int i = 0; i < nodeCount; i++) {

    if (which != 0 && which != i + 1) {
      continue;
    }

    if (mode > 0 && nodes[i].state == S_DONE) {
      Serial.printf("[SIM] %s sesi selesai, sensing tidak aktif (ketik r%d untuk mengulang)\n",
                    nodes[i].id, i + 1);
      continue;
    }

    nodes[i].mode = mode;

    if (mode > 0) {
      nodes[i].urgent = true;   
    }
  }

  const char *label = (mode == 2) ? "KRITIS" : (mode == 1) ? "WASPADA" : "normal";

  if (which == 0) Serial.println(String("[SIM] Semua node -> ") + label);
  else Serial.println("[SIM] Node " + String(which) + " -> " + label);
}


void handleSerial() {

  while (Serial.available()) {

    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {

      if (cmdBuf.length() > 0) {
        runCommand(cmdBuf);
        cmdBuf = "";
      }

    } else if (cmdBuf.length() < 16) {

      cmdBuf += c;
    }
  }
}


void sendNode(int i) {

  Node &n = nodes[i];

  bool scheduled = !n.urgent;
  n.urgent = false;

  int hr, spo2;
  float temp;

  vitraSimValues(n.mode, hr, spo2, temp);   // ganti dengan pembacaan sensor asli nanti

  int level = vitraClassify(hr, spo2, temp).lv;

  uint8_t prio = (level >= 2) ? PRIO_CRIT : (level == 1) ? PRIO_WARN : PRIO_NORMAL;

  bool last = scheduled && ((int)n.sent + 1 >= (int)VITRA_SESSION_SENDS);

  String packet =
    "SENSE|" + String(n.id) +
    "|HR=" + String(hr) +
    "|SPO2=" + String(spo2) +
    "|TEMP=" + String(temp, 1) +
    "|Q=" + String(n.seq);

  if (level >= 1) packet += "|A=1";
  if (last)       packet += "|LAST=1";

  vitraSend(packet, prio);

  n.seq++;

  if (level >= 1) {
    n.pend = packet;
    n.pendPrio = prio;
    n.ackTries = 1;
    n.ackWait = true;
    n.ackAt = millis() + ACK_WAIT_MS;
  } else {
    n.ackWait = false; 
  }

  if (scheduled) {
    n.sent++;
    n.nextMs += VITRA_SEND_INTERVAL_MS;  
  }

#if SENSE_VERBOSE
  char tag[24];

  if (scheduled) snprintf(tag, sizeof(tag), "terjadwal %d/%d", (int)n.sent, (int)VITRA_SESSION_SENDS);
  else snprintf(tag, sizeof(tag), "SEGERA");

  Serial.printf("[TX] %s HR=%d SpO2=%d T=%.1f L%d prio=%d tunggu=%lums (%s)\n",
                n.id, hr, spo2, temp, level, prio, (unsigned long)vitraLastWaitMs, tag);
#endif

  if (last) {

    n.state = S_DONE;

    Serial.printf("[SESI] %s selesai. Sense berhenti bekerja sampai data pasien berubah.\n", n.id);
  }
}


void ackTick() {

  uint32_t now = millis();

  for (int i = 0; i < nodeCount; i++) {

    Node &n = nodes[i];

    if (!n.ackWait || (int32_t)(now - n.ackAt) < 0) {
      continue;
    }

    if (n.ackTries >= ACK_MAX_TRIES) {
      n.ackWait = false;
      Serial.printf("[WARN] %s: Link tidak membalas, berhenti mengulang\n", n.id);
      continue;
    }

    n.ackTries++;

    Serial.printf("[WARN] %s kirim ulang (%d/%d)\n", n.id, n.ackTries, ACK_MAX_TRIES);

    vitraSend(n.pend, n.pendPrio);

    n.ackAt = millis() + ACK_WAIT_MS;

    return;  
  }
}


void loop() {

  handleSerial();

  vitraPoll();

  ackTick();

  int pick = -1;
  uint32_t now = millis();

  for (int i = 0; i < nodeCount; i++) {

    Node &n = nodes[i];

    bool due = (n.state == S_SENSING) && ((int32_t)(now - n.nextMs) >= 0);

    if (!n.urgent && !due) continue;

    if (pick < 0) {
      pick = i;
      continue;
    }

    Node &p = nodes[pick];

    if (n.urgent != p.urgent) {
      if (n.urgent) pick = i;
      continue;
    }

    if (n.mode != p.mode) {
      if (n.mode > p.mode) pick = i;
      continue;
    }

    if ((int32_t)(n.nextMs - p.nextMs) < 0) {
      pick = i;
    }
  }

  if (pick >= 0) {
    sendNode(pick);
  }

  if (!allDoneShown) {

    bool all = true;

    for (int i = 0; i < nodeCount; i++) {
      if (nodes[i].state != S_DONE) all = false;
    }

    if (all) {
      allDoneShown = true;
      Serial.println("[SESI] Semua sesi selesai. Ubah data pasien di VITRA Log (atau ketik r) untuk mengulang.");
    }
  }

  if (millis() - lastStats >= 30000) {
    lastStats = millis();
    printTxStats();
  }

  delay(2);
}
