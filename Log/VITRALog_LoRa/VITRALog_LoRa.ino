#include <Arduino.h>
#include <WiFi.h>
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
#define BTN_HOME   35

// UART dari modul ESP32-CAM (QR reader)
#define QR_RX   16
#define QR_TX   17
#define QR_BAUD 115200

#define REG_TIMEOUT_MS 4000
#define REG_MAX_TRIES  3

#define LORA_SF 11

#include "vitra_radio.h"

// =====================================================
// UI GLOBALS (dipakai bersama oleh VITRALog_Setting.ino)
// =====================================================

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
enum LogMode {
  M_HOME,
  M_SCAN,
  M_FORM,
  M_SEND,
  M_RESULT,
  M_LOG,
  M_EMERGENCY
};

LogMode mode = M_HOME;
LogMode modeBeforeEmergency = M_HOME;

bool loraReady = false;

String senseId = "";
String qrBuf = "";
String serBuf = "";

int regTries = 0;
unsigned long lastRegSend = 0;
unsigned long resultUntil = 0;
bool lastResultOk = false;

// form data (dipakai oleh VITRALog_Setting.ino)
String fieldName[] = {
  "AGE", "GENDER", "STATUS", "TRIAGE"
};

String selectedValue[] = {
  "Anak-anak", "Pria", "Sadar", "Hijau"
};

String options[][4] = {
  { "Anak-anak", "Remaja", "Dewasa", "Lansia" },
  { "Pria", "Wanita", "Unidentified" },
  { "Sadar", "Bingung", "Tidak respon" },
  { "Hijau", "Kuning" }
};

int optionCount[] = { 4, 3, 3, 2 };

int currentField = 0;
int optionIndex[4] = { 0, 0, 0, 0 };
int currentPatient = 0;

// Info darurat terakhir yang diterima dari VITRA LINK
String emergId = "";
String emergWhy = "";
String emergHr = "";
String emergSpo2 = "";
String emergRr = "";


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


void resetForm() {
  currentField = 0;

  for (int i = 0; i < 4; i++) {
    optionIndex[i] = 0;
    selectedValue[i] = options[i][0];
  }
}

void startForm() {
  mode = M_FORM;
  resetForm();
  showForm();
}

void goHome() {
  mode = M_HOME;
  showHome();
}


// =====================================================
// QR
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

  // Serial2 = kamera QR (GPIO16). Serial (USB) = jalur uji coba manual;
  // ketik ID (mis. "VITRA-001") di Serial Monitor untuk mensimulasikan
  // hasil scan selama kamera belum dipasang. Aman dibiarkan aktif terus,
  // tidak mengganggu jalur kamera saat sudah terpasang.
  if (!pollLine(Serial2, qrBuf, line) &&
      !pollLine(Serial, serBuf, line)) {
    return;
  }

  line.trim();

  if (!validSenseId(line)) {

    Serial.println("[QR] Tidak dikenali: " + line);
    return;
  }

  senseId = line;

  Serial.println("[QR] ID Sense: " + senseId);

  startForm();
}


// =====================================================
// REGISTRASI KE VITRA LINK
// =====================================================

void finishSend(bool ok) {
  lastResultOk = ok;
  logSavePatient(ok);
  showResult(ok);

  mode = M_RESULT;
  resultUntil = millis() + 1800;
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
    "|AGE=" + selectedValue[0] +
    "|GENDER=" + selectedValue[1] +
    "|STATUS=" + selectedValue[2] +
    "|TRIAGE=" + selectedValue[3];

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

  mode = M_SEND;
  showSending(regTries);
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


// =====================================================
// DARURAT DARI VITRA LINK
// =====================================================

void handleEmergencyPacket(const String &received) {

  // Format: EMERG|<id>|HR=..|SPO2=..|RR=..|WHY=...
  int a = received.indexOf('|');
  int b = received.indexOf('|', a + 1);

  String id = (b > a) ? received.substring(a + 1, b) : "";

  if (id.length() == 0) {
    return;
  }

  emergId = id;
  emergHr = getField(received, "HR");
  emergSpo2 = getField(received, "SPO2");
  emergRr = getField(received, "RR");
  emergWhy = getField(received, "WHY");
  emergWhy.replace("_", " ");

  Serial.println("[LOG] !!! DARURAT: " + emergId + " (" + emergWhy + ")");

  // Balas konfirmasi supaya LINK berhenti mengirim ulang
  LoRa.beginPacket();
  LoRa.print("EMACK|" + emergId);
  LoRa.endPacket();

  if (mode != M_EMERGENCY) {
    modeBeforeEmergency = mode;
  }

  mode = M_EMERGENCY;
  showEmergency();
}


// Tutup layar darurat dan kembali ke layar sebelumnya
void closeEmergency() {

  mode = modeBeforeEmergency;

  if (mode == M_HOME) showHome();
  else if (mode == M_LOG) showLog();
  else if (mode == M_FORM) showForm();
  else if (mode == M_SCAN) showScan("WAITING FOR QR...");
  else goHome();
}


// Format: EMEND|<id> -- LINK: darurat pasien ini sudah selesai
void handleEmergencyEnd(const String &received) {

  String id = received.substring(String("EMEND|").length());
  id.trim();

  if (id.length() == 0) {
    return;
  }

  Serial.println("[LOG] Darurat selesai: " + id);

  // Hanya tutup kalau layar yang terbuka memang untuk pasien ini
  if (mode == M_EMERGENCY && id == emergId) {
    closeEmergency();
  }
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

  if (received.startsWith("EMERG|")) {
    handleEmergencyPacket(received);
    return;
  }

  if (received.startsWith("EMEND|")) {
    handleEmergencyEnd(received);
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

  Serial.println(
    "[RX] " + received +
    " | RSSI " + String(LoRa.packetRssi()) + " dBm"
  );
}


// =====================================================
// TOMBOL
// =====================================================

bool pressed(int pin) {

  if (digitalRead(pin) == LOW) {
    delay(150);
    return true;
  }

  return false;
}

bool homePressed() {

  static unsigned long lowSince = 0;
  static bool triggered = false;

  if (digitalRead(BTN_HOME) == HIGH) {
    lowSince = 0;
    triggered = false;
    return false;
  }

  if (lowSince == 0) {
    lowSince = millis();
  }

  if (!triggered && millis() - lowSince >= 500) {
    triggered = true;
    return true;
  }

  return false;
}

void handleButtons() {
    // Tombol HOME
    if (homePressed()) {

      if (mode != M_SEND && mode != M_EMERGENCY) {

        senseId = "";
        qrBuf = "";
        serBuf = "";

        goHome();
      }

      return;
  }

  // Layar darurat bisa ditutup dari mode APAPUN dengan tombol SELECT
  if (mode == M_EMERGENCY) {
    if (pressed(BTN_SELECT) || pressed(BTN_LOG)) {
      closeEmergency();
    }
    return;
  }

  if (mode == M_HOME) {

    if (pressed(BTN_SCAN)) {
      senseId = "";
      qrBuf = "";
      mode = M_SCAN;
      showScan("WAITING FOR QR...");
    }

    if (pressed(BTN_LOG)) {
      mode = M_LOG;
      currentPatient = 0;
      showLog();
    }

  } else if (mode == M_LOG) {

    // Halaman berikutnya
    if (pressed(BTN_RIGHT)) {

      DynamicJsonDocument doc = logLoadPatients();
      JsonArray patients = doc.as<JsonArray>();

      if (currentPatient + 6 < (int)patients.size()) {
        currentPatient += 6;
      }

      showLog();
      return;
    }

    // Halaman sebelumnya
    if (pressed(BTN_LEFT)) {

      if (currentPatient >= 6) {
        currentPatient -= 6;
      }

      showLog();
      return;
    }


  } else if (mode == M_FORM) {

    if (pressed(BTN_LEFT)) {
      optionIndex[currentField]--;

      if (optionIndex[currentField] < 0) {
        optionIndex[currentField] = optionCount[currentField] - 1;
      }
      selectedValue[currentField] = options[currentField][optionIndex[currentField]];

      showForm();
    }

    if (pressed(BTN_RIGHT)) {
      optionIndex[currentField]++;

      if (optionIndex[currentField] >= optionCount[currentField]) {
        optionIndex[currentField] = 0;
      }
      selectedValue[currentField] = options[currentField][optionIndex[currentField]];

      showForm();
    }

    if (pressed(BTN_SELECT)) {
      if (currentField < 3) {
        currentField++;
        showForm();
      } else {
        regTries = 0;
        sendRegistration();
      }
    }

  } else if (mode == M_RESULT) {

    // dibiarkan; auto-kembali ke HOME lewat timer di loop()

  } else if (mode == M_SCAN) {
    // Tombol SELECT untuk simulasi QR
    if (pressed(BTN_SELECT)) {
      senseId = "VITRA-002";
      qrBuf = "";
      serBuf = "";

      Serial.println("[TEST] ID simulasi: " + senseId);

      startForm();
      return;
    }

    // Tombol SELECT untuk simulasi QR
    if (pressed(BTN_SELECT)) {
      senseId = "VITRA-002";
      qrBuf = "";
      serBuf = "";

      Serial.println("[TEST] ID simulasi: " + senseId);

      startForm();
      return;
    }
  } else if (mode == M_SEND) {
  // Menunggu ACK dari VITRA LINK
  }
}

void ujiDaya() {
  int levels[] = {2, 5, 10, 14, 17, 20};
  for (int i = 0; i < 6; i++) {
    LoRa.setTxPower(levels[i], PA_OUTPUT_PA_BOOST_PIN);
    for (int k = 0; k < 3; k++) {
      LoRa.beginPacket();
      LoRa.print("TEST|" + String(levels[i]));
      LoRa.endPacket();
      delay(1000);
    }
  }
  LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);
}

// =====================================================
// SETUP / LOOP
// =====================================================

void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_OFF);
  btStop();

  setCpuFrequencyMhz(80);
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
  pinMode(BTN_HOME,   INPUT);

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
    vitraRadioConfig();

    loraReady = true;

    Serial.println("[LoRa] OK");
  }

  ujiDaya();
  showHome();
}


void loop() {

  if (loraReady) {
    receiveLoRa();
  }

  checkAckTimeout();

  handleQR();

  handleButtons();

  if (mode == M_RESULT && millis() >= resultUntil) {
    goHome();
  }

  delay(2);
}

