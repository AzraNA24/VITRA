#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

// -----------------------------------------------------
// LoRa
// -----------------------------------------------------

#define LORA_SS   5
#define LORA_RST  14
#define LORA_DIO0 26

#define SPI_SCK  18
#define SPI_MISO 19
#define SPI_MOSI 23

#define LORA_FREQUENCY 433E6


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("          VITRA-LOG");
  Serial.println("================================");


  // ---------------------------------------------------
  // SPI
  // ---------------------------------------------------

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    LORA_SS
  );


  // ---------------------------------------------------
  // LoRa
  // ---------------------------------------------------

  LoRa.setPins(
    LORA_SS,
    LORA_RST,
    LORA_DIO0
  );


  if (!LoRa.begin(LORA_FREQUENCY)) {

    Serial.println("[LoRa] GAGAL!");

    while (true) {

      delay(1000);

    }

  }


  // Harus sama dengan Sense dan Link

  LoRa.setSpreadingFactor(9);

  LoRa.setSignalBandwidth(125E3);

  LoRa.setCodingRate4(5);

  LoRa.enableCrc();


  Serial.println("[LoRa] OK");

  Serial.println(
    "[LoRa] Menunggu data dari LINK..."
  );

  Serial.println();
}


// =====================================================
// RECEIVE
// =====================================================

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


  int rssi = LoRa.packetRssi();

  Serial.println();
  Serial.println("================================");
  Serial.println("       DATA PASIEN");
  Serial.println("================================");

  Serial.println(
    "DATA : " +
    received
  );

  Serial.println(
    "RSSI : " +
    String(rssi) +
    " dBm"
  );

  Serial.println("================================");
}


void loop() {

  receiveLoRa();

  delay(5);
}