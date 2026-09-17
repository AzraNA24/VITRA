#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>

// -----------------------------------------------------
// WiFi
// -----------------------------------------------------

const char* AP_SSID = "Link";
const char* AP_PASSWORD = "12345678";

// IP yang diminta
IPAddress local_IP(192, 168, 4, 2);
IPAddress gateway(192, 168, 4, 2);
IPAddress subnet(255, 255, 255, 0);


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


// -----------------------------------------------------
// Web Server
// -----------------------------------------------------

WebServer server(80);


// -----------------------------------------------------
// Data
// -----------------------------------------------------

String latestData = "Belum ada data";

String chatHistory = "";

unsigned long packetCount = 0;


// =====================================================
// WEB PAGE
// =====================================================

void handleRoot() {

  String html = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>VITRA-LINK</title>

<style>

body {
  font-family: Arial;
  background: #111;
  color: white;
  margin: 0;
  padding: 20px;
}

.container {
  max-width: 800px;
  margin: auto;
}

h1 {
  color: #00ffff;
}

.status {
  background: #222;
  padding: 15px;
  border-radius: 10px;
  margin-bottom: 15px;
}

.data {
  background: #181818;
  border: 1px solid #333;
  border-radius: 10px;
  padding: 20px;
  font-family: monospace;
  word-break: break-all;
}

.history {
  margin-top: 20px;
  background: #181818;
  border: 1px solid #333;
  border-radius: 10px;
  padding: 15px;
  max-height: 400px;
  overflow-y: auto;
}

.packet {
  padding: 10px;
  border-bottom: 1px solid #333;
}

.title {
  font-size: 24px;
  font-weight: bold;
}

.small {
  color: #aaa;
  font-size: 13px;
}

</style>

</head>


<body>

<div class="container">

<h1>VITRA-LINK</h1>

<div class="status">

<div class="title">
Gateway Status
</div>

<p>
WiFi:
<b>Link</b>
</p>

<p>
IP:
<b>192.168.4.2</b>
</p>

<p>
Packets:
<b id="count">0</b>
</p>

</div>


<div class="data">

<div class="small">
DATA TERAKHIR
</div>

<br>

<div id="latest">
Menunggu data...
</div>

</div>


<div class="history">

<div class="title">
Riwayat Data
</div>

<div id="history">
Menunggu data...
</div>

</div>

</div>


<script>

function updateData() {

  fetch('/api/data')

    .then(response => response.json())

    .then(data => {

      document.getElementById("latest")
        .innerText = data.latest;

      document.getElementById("count")
        .innerText = data.count;

      document.getElementById("history")
        .innerHTML = data.history;

    })

    .catch(error => {

      console.log(error);

    });

}


// update setiap 1 detik

setInterval(updateData, 1000);

updateData();

</script>


</body>

</html>

)rawliteral";

  server.send(200, "text/html", html);
}


// =====================================================
// API DATA
// =====================================================

void handleApiData() {

  String json = "{";

  json += "\"latest\":\"";

  // Escape tanda kutip
  String safeLatest = latestData;
  safeLatest.replace("\\", "\\\\");
  safeLatest.replace("\"", "\\\"");

  json += safeLatest;

  json += "\",";

  json += "\"count\":";
  json += String(packetCount);
  json += ",";

  json += "\"history\":\"";

  String safeHistory = chatHistory;
  safeHistory.replace("\\", "\\\\");
  safeHistory.replace("\"", "\\\"");

  json += safeHistory;

  json += "\"";

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}


// =====================================================
// TERIMA DATA LoRa
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

  packetCount++;

  int rssi = LoRa.packetRssi();


  // ---------------------------------------------------
  // SIMPAN DATA
  // ---------------------------------------------------

  latestData = received;


  // ---------------------------------------------------
  // TAMPILKAN DI SERIAL
  // ---------------------------------------------------

  Serial.println();
  Serial.println("================================");
  Serial.println("[LINK] DATA DITERIMA");
  Serial.println("================================");

  Serial.println("DATA : " + received);

  Serial.println(
    "RSSI : " +
    String(rssi) +
    " dBm"
  );

  Serial.println(
    "COUNT: " +
    String(packetCount)
  );


  // ---------------------------------------------------
  // SIMPAN HISTORY WEB
  // ---------------------------------------------------

  chatHistory +=
    "<div class='packet'>" +
    received +
    "<br><span class='small'>RSSI: " +
    String(rssi) +
    " dBm</span></div>";


  // Jangan terlalu besar
  if (chatHistory.length() > 8000) {

    chatHistory =
      chatHistory.substring(
        chatHistory.length() - 8000
      );

  }


  // ---------------------------------------------------
  // FORWARD KE VITRA-LOG
  // ---------------------------------------------------

  Serial.println("[LINK] Forwarding ke LOG...");

  LoRa.beginPacket();

  // Tambahkan prefix supaya Log tahu
  // bahwa paket berasal dari Link
  LoRa.print("LINK|" + received);

  LoRa.endPacket();

  Serial.println("[LINK] Forward selesai");

  Serial.println("================================");
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);


  Serial.println();
  Serial.println("================================");
  Serial.println("        VITRA-LINK");
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

  LoRa.setSpreadingFactor(9);

  LoRa.setSignalBandwidth(125E3);

  LoRa.setCodingRate4(5);

  LoRa.enableCrc();


  Serial.println("[LoRa] OK");


  // ---------------------------------------------------
  // WIFI AP
  // ---------------------------------------------------

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAPConfig(
        local_IP,
        gateway,
        subnet
      )) {

    Serial.println(
      "[WiFi] Gagal konfigurasi IP"
    );

  }


  WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );


  Serial.println(
    "[WiFi] SSID: " +
    String(AP_SSID)
  );

  Serial.println(
    "[WiFi] IP: " +
    WiFi.softAPIP().toString()
  );


  // ---------------------------------------------------
  // WEB SERVER
  // ---------------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  server.on(
    "/api/data",
    HTTP_GET,
    handleApiData
  );

  server.begin();


  Serial.println();
  Serial.println("================================");
  Serial.println("VITRA-LINK SIAP");
  Serial.println("Buka:");
  Serial.println("http://192.168.4.2");
  Serial.println("================================");
}

void loop() {
  server.handleClient();
  receiveLoRa();


  delay(5);
}