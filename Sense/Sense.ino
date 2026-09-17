#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

#define NODE_ID "VITRA-001"

// Pin SX1278
#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

// SPI ESP32
#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

#define LORA_FREQUENCY 433E6

unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 3000;


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("       VITRA-SENSE");
  Serial.println("================================");

  // SPI
  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    LORA_SS
  );

  // LoRa pin
  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0
  );

  // LoRa initialization
  if (!LoRa.begin(LORA_FREQUENCY)) {

    Serial.println("[LoRa] GAGAL!");

    while (true) {
      delay(1000);
    }
  }

 // Ini pastiin sama, yess
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);

  // CRC
  LoRa.enableCrc();

  Serial.println("[LoRa] OK");
  Serial.println("[LoRa] Frequency : 433 MHz");
  Serial.println("[LoRa] SF        : 9");
  Serial.println("[LoRa] BW        : 125 kHz");

  Serial.println();
  Serial.println("Sense siap mengirim data...");
}

void sendPatientData() {
  int heartRate = random(70, 100);
  int spo2 = random(95, 100);
  float temperature = random(360, 380) / 10.0;

  String packet =
    "SENSE|" +
    String(NODE_ID) +
    "|HR=" + String(heartRate) +
    "|SPO2=" + String(spo2) +
    "|TEMP=" + String(temperature, 1);

  Serial.println();
  Serial.println("[TX] Mengirim:");
  Serial.println(packet);

  LoRa.beginPacket();
  LoRa.print(packet);
  LoRa.endPacket();

  Serial.println("[TX] Berhasil");
}

void loop() {

  if (millis() - lastSend >= SEND_INTERVAL) {

    lastSend = millis();

    sendPatientData();
  }
}