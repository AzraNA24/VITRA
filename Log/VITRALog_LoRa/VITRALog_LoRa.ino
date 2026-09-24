#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

#define LORA_FREQUENCY 433E6

#define TFT_CS  25
#define TFT_DC  27
#define TFT_RST 33

#define BTN_SCAN   32
#define BTN_LEFT   13
#define BTN_RIGHT  21
#define BTN_SELECT 22
#define BTN_LOG    12

#define QR_RX   16
#define QR_TX   17
#define QR_BAUD 9600

#define REG_TIMEOUT_MS 4000
#define REG_MAX_TRIES  3

uint16_t BG;
uint16_t CARD;
uint16_t PRIMARY;
uint16_t TEXT;
uint16_t SUCCESS;
uint16_t WARNING;
uint16_t DANGER;
uint16_t SECONDARY;

Adafruit_ILI9341 tft(
  TFT_CS,
  TFT_DC,
  TFT_RST
);

// System State
enum Mode {
  M_HOME,
  M_SCAN,
  M_FORM,
  M_SEND,
  M_RESULT,
  M_LOG,
  M_ALERT
};

Mode mode = M_HOME;

bool loraReady = false;

String senseId = "";
String qrBuf = "";
String serBuf = "";

int regTries = 0;
unsigned long lastRegSend = 0;
unsigned long resultUntil = 0;

// Alert emergency dari LINK
#define MAX_ALERTS 5

struct AlertInfo {
  String id;
  String hr;
  String spo2;
  String temp;
  String why;      // alasan, dipisah '+'  (mis. "SpO2 rendah+HR tinggi")
  String age;
  String gender;
  String status;
  String triage;
};

AlertInfo alerts[MAX_ALERTS];

int alertCount = 0;            // jumlah alert aktif
int alertIdx = 0;              // alert yang sedang ditampilkan
bool alertNew = false;         // ada alert baru yang belum ditampilkan
Mode modeBeforeAlert = M_HOME; // layar yang dikembalikan setelah alert ditutup

// Baca satu baris dari stream tanpa blocking
bool pollLine(Stream &s, String &buf, String &out) {

  while (s.available()) {

    char c = (char)s.read();

    if (c == '\n' || c == '\r') {

      if (buf.length() > 0) {
        out = buf;
        buf = "";
        return true;
      }

    } else if (buf.length() < 64) {

      buf += c;

    }
  }

  return false;
}


// Format ID Sense: VITRA-xxx
bool validSenseId(const String &s) {

  if (!s.startsWith("VITRA-") || s.length() > 20) {
    return false;
  }

  for (size_t i = 0; i < s.length(); i++) {

    char c = s[i];

    if (!isalnum((unsigned char)c) && c != '-') {
      return false;
    }
  }

  return true;
}


// =====================================================
// QR BUAT NANTI
// =====================================================

void handleQR() {

  // Di luar mode SCAN, buang semua input scanner
  if (mode != M_SCAN) {

    while (Serial2.available()) {
      Serial2.read();
    }

    while (Serial.available()) {
      Serial.read();
    }

    return;
  }

  String line = "";

  if (!pollLine(Serial2, qrBuf, line) &&
      !pollLine(Serial, serBuf, line)) {
    return;
  }

  line.trim();

  if (!validSenseId(line)) {

    Serial.println("[QR] Tidak dikenali: " + line);

    showScan("QR bukan VITRA Sense");

    return;
  }

  senseId = line;

  Serial.println("[QR] ID Sense: " + senseId);

  startForm();
}

void finishSend(bool ok) {
  savePatient(ok);

  showResult(ok);
}


void sendRegistration() {

  if (!loraReady) {

    Serial.println("[LOG] LoRa tidak aktif, data hanya disimpan lokal");

    finishSend(false);

    return;
  }

  String packet =
    "REG|" +
    senseId +
    "|AGE=" + formValue(0) +
    "|GENDER=" + formValue(1) +
    "|STATUS=" + formValue(2) +
    "|TRIAGE=" + formValue(3);

  regTries++;

  Serial.println();
  Serial.println(
    "[TX] Kirim ke LINK (" +
    String(regTries) + "/" + String(REG_MAX_TRIES) + ")"
  );
  Serial.println(packet);

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();

  lastRegSend = millis();

  showSending();
}


void checkAckTimeout() {

  if (mode != M_SEND) {
    return;
  }

  if (millis() - lastRegSend < REG_TIMEOUT_MS) {
    return;
  }

  if (regTries < REG_MAX_TRIES) {

    Serial.println("[LOG] ACK belum diterima, kirim ulang...");

    sendRegistration();

  } else {

    Serial.println("[LOG] GAGAL: LINK tidak merespons");

    finishSend(false);

  }
}

// Ambil nilai dari "|KEY=value" di dalam paket
String getField(const String &packet, const String &key) {

  String needle = "|" + key + "=";

  int s = packet.indexOf(needle);

  if (s < 0) {
    return "";
  }

  s += needle.length();

  int e = packet.indexOf('|', s);

  if (e < 0) {
    e = packet.length();
  }

  return packet.substring(s, e);
}


// Beri tahu LINK bahwa ALERT sudah diterima (LINK berhenti mengulang)
void sendAlertAck(const String &id) {

  if (!loraReady) {
    return;
  }

  delay(50);   // beri waktu LINK kembali ke mode terima

  LoRa.beginPacket();
  LoRa.print("AACK|" + id);
  LoRa.endPacket();
}


// Format: ALERT|VITRA-001|HR=..|SPO2=..|TEMP=..|WHY=..|AGE=..|GENDER=..|STATUS=..|TRIAGE=..
void handleAlert(const String &packet) {

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  if (!validSenseId(id)) {
    return;
  }

  sendAlertAck(id);

  // Alert untuk pasien yang sama diperbarui, bukan ditambah
  int idx = -1;

  for (int i = 0; i < alertCount; i++) {

    if (alerts[i].id == id) {
      idx = i;
      break;
    }
  }

  bool isNew = (idx < 0);

  if (isNew) {

    if (alertCount >= MAX_ALERTS) {

      // Penuh: buang yang paling lama
      for (int i = 1; i < alertCount; i++) {
        alerts[i - 1] = alerts[i];
      }

      alertCount--;
    }

    idx = alertCount++;
  }

  alerts[idx].id     = id;
  alerts[idx].hr     = getField(packet, "HR");
  alerts[idx].spo2   = getField(packet, "SPO2");
  alerts[idx].temp   = getField(packet, "TEMP");
  alerts[idx].why    = getField(packet, "WHY");
  alerts[idx].age    = getField(packet, "AGE");
  alerts[idx].gender = getField(packet, "GENDER");
  alerts[idx].status = getField(packet, "STATUS");
  alerts[idx].triage = getField(packet, "TRIAGE");

  Serial.println("[LOG] EMERGENCY " + id + " : " + alerts[idx].why);

  if (isNew) {

    alertIdx = idx;
    alertNew = true;

  } else if (mode == M_ALERT && alertIdx == idx) {

    showAlert();   // perbarui angka di layar
  }
}


// Tampilkan alert. Layar sebelumnya diingat untuk dikembalikan nanti.
void enterAlert() {

  if (mode != M_ALERT) {
    modeBeforeAlert = mode;
    mode = M_ALERT;
  }

  clearButtonEvents();   // cegah tombol yang sedang ditekan langsung menutup alert

  showAlert();
}


void receiveLoRa() {

  int packetSize = LoRa.parsePacket();

  if (packetSize <= 0) {
    return;
  }

  String received = "";

  while (LoRa.available()) {

    received += (char)LoRa.read();

  }

  received.trim();

  if (received.length() == 0) {
    return;
  }

  if (received.startsWith("ACK|")) {

    if (mode == M_SEND && received.substring(4) == senseId) {

      Serial.println("[LOG] ACK dari LINK");

      finishSend(true);
    }

    return;
  }

  if (received.startsWith("NACK|")) {

    if (mode == M_SEND && received.startsWith("NACK|" + senseId)) {

      Serial.println("[LOG] Ditolak LINK: " + received);

      finishSend(false);
    }

    return;
  }

  if (received.startsWith("ALERT|")) {

    Serial.println(
      "[RX] " + received +
      " | RSSI " + String(LoRa.packetRssi()) + " dBm"
    );

    handleAlert(received);

    return;
  }

  // Data vital SENSE ikut terdengar langsung oleh LOG; tidak dipakai di sini
  if (received.startsWith("SENSE|")) {
    return;
  }

  Serial.println(
    "[RX] " + received +
    " | RSSI " + String(LoRa.packetRssi()) + " dBm"
  );
}

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("          VITRA-LOG");
  Serial.println("================================");

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS ERROR");
  } else {
    Serial.println("[FS] LittleFS READY");
  }

  pinMode(BTN_SCAN,   INPUT_PULLUP);
  pinMode(BTN_LEFT,   INPUT_PULLUP);
  pinMode(BTN_RIGHT,  INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_LOG,    INPUT_PULLUP);

  Serial2.begin(QR_BAUD, SERIAL_8N1, QR_RX, QR_TX);

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    LORA_SS
  );

  initColors();

  tft.begin();

  tft.setRotation(1);

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0
  );

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LoRa] GAGAL!");

  } else {
    LoRa.setSpreadingFactor(9);

    LoRa.setSignalBandwidth(125E3);

    LoRa.setCodingRate4(5);

    LoRa.enableCrc();

    loraReady = true;

    Serial.println("[LoRa] OK");
  }


  showHome();
}


void loop() {

  if (loraReady) {
    receiveLoRa();
  }

  checkAckTimeout();

  handleQR();

  handleButtons();

  // Alert baru menyela layar apa pun, kecuali saat registrasi sedang dikirim
  // (supaya ACK dari LINK tidak terlewat). Ditampilkan begitu pengiriman selesai.
  if (alertNew && mode != M_SEND) {
    alertNew = false;
    enterAlert();
  }

  if (mode == M_RESULT && millis() >= resultUntil) {
    goHome();
  }

  delay(2);
}
