void initColors() {
  BG        = tft.color565(10, 18, 30);
  CARD      = tft.color565(25, 35, 50);
  PRIMARY   = tft.color565(30, 140, 240);
  TEXT      = tft.color565(245, 245, 245);
  SUCCESS   = tft.color565(50, 210, 130);
  WARNING   = tft.color565(255, 190, 30);
  DANGER    = tft.color565(240, 70, 70);
  SECONDARY = tft.color565(100, 110, 130);
}

String shortAge(String age) {
  if (age == "Anak-anak") return "Ank";
  if (age == "Remaja") return "Rem";
  if (age == "Dewasa") return "Dew";
  if (age == "Lansia") return "Lan";
  return "-";
}

String shortGender(String gender) {
  if (gender == "Pria") return "P";
  if (gender == "Wanita") return "W";
  return "-";
}

String shortStatus(String status) {
  if (status == "Sadar") return "Sad";
  if (status == "Bingung") return "Bng";
  if (status == "Tidak respon") return "NR";
  return "-";
}

String shortTriage(String triage) {
  if (triage == "Hijau") return "Hij";
  if (triage == "Kuning") return "Kun";
  if (triage == "Merah") return "Mer";
  return "-";
}

void drawButton(int x, int y, int w, int h, String label, uint16_t color) {

  tft.fillRoundRect(x, y, w, h, 10, color);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);

  int textX = x + (w - label.length() * 12) / 2;
  int textY = y + h / 2 - 8;

  tft.setCursor(textX, textY);
  tft.print(label);
}

void drawHeader() {
  tft.fillRect(0, 0, 320, 45, CARD);

  tft.setTextSize(2);
  tft.setTextColor(TEXT);
  tft.setCursor(20, 15);
  tft.print("VITRA LOG");

  // battery icon (dummy)
  tft.drawRect(260, 15, 30, 12, TEXT);
  tft.fillRect(291, 18, 3, 6, TEXT);
  tft.fillRect(263, 18, 20, 6, SUCCESS);
}

void drawCard(int x, int y, int w, int h, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 8, color);
}

void drawSelection(String title, String value, int y, bool active) {

  tft.setTextSize(1);
  tft.setTextColor(active ? PRIMARY : TEXT);

  int titleX = (320 - title.length() * 6) / 2;
  tft.setCursor(titleX, y);
  tft.print(title);

  tft.setTextSize(2);
  tft.setTextColor(active ? PRIMARY : TEXT);

  String valueDisplay = "< " + value + " >";
  int valueX = (320 - valueDisplay.length() * 12) / 2;

  tft.setCursor(valueX, y + 15);
  tft.print(valueDisplay);
}

void showHome() {
  tft.fillScreen(BG);
  drawHeader();

  drawButton(60, 80, 200, 45, "SCAN", PRIMARY);
  drawButton(60, 145, 200, 45, "VIEW LOG", CARD);
}

void showScan(String message) {
  tft.fillScreen(BG);
  drawHeader();

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(60, 100);
  tft.println("SCANNING...");

  drawCard(20, 185, 280, 35, CARD);

  tft.setCursor(30, 195);
  tft.setTextSize(1);
  tft.println(message);
}

void showForm() {
  tft.fillScreen(BG);
  drawHeader();

  drawCard(20, 55, 280, 30, CARD);

  tft.setTextSize(1);
  tft.setTextColor(TEXT);
  tft.setCursor(35, 65);
  tft.print("ID: ");
  tft.print(senseId);

  drawSelection("AGE", selectedValue[0], 90, currentField == 0);
  drawSelection("GENDER", selectedValue[1], 125, currentField == 1);
  drawSelection("STATUS", selectedValue[2], 160, currentField == 2);
  drawSelection("TRIAGE", selectedValue[3], 195, currentField == 3);

  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);
  tft.setCursor(60, 225);
  tft.print(currentField < 3 ? "SELECT: field berikutnya" : "SELECT: kirim ke LINK");
}

void showSending(int tries) {
  tft.fillScreen(BG);
  drawHeader();

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(60, 100);
  tft.println("MENGIRIM...");

  tft.setTextSize(1);
  tft.setCursor(90, 140);
  tft.print("Percobaan ");
  tft.print(tries);
  tft.print("/");
  tft.print(REG_MAX_TRIES);

  tft.setCursor(60, 165);
  tft.print("Menunggu balasan VITRA LINK");
}

void showResult(bool ok) {
  tft.fillScreen(BG);
  drawHeader();

  drawCard(40, 80, 240, 90, CARD);

  tft.setTextSize(2);
  tft.setTextColor(ok ? SUCCESS : DANGER);
  tft.setCursor(ok ? 90 : 70, 100);
  tft.print(ok ? "TERSIMPAN" : "GAGAL KIRIM");

  tft.setTextSize(1);
  tft.setTextColor(TEXT);
  tft.setCursor(60, 135);
  tft.print(senseId);

  if (!ok) {
    tft.setCursor(55, 150);
    tft.setTextColor(SECONDARY);
    tft.print("Data tetap disimpan lokal");
  }
}

void showEmergency() {
  tft.fillScreen(DANGER);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(20, 15);
  tft.print("!! EMERGENCY !!");

  drawCard(20, 55, 280, 140, BG);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(35, 70);
  tft.print("ID: ");
  tft.print(emergId);

  tft.setTextSize(1);
  tft.setCursor(35, 100);
  tft.print("HR   : "); tft.print(emergHr);   tft.print(" bpm");
  tft.setCursor(35, 118);
  tft.print("SpO2 : "); tft.print(emergSpo2); tft.print(" %");
  tft.setCursor(35, 136);
  tft.print("Suhu : "); tft.print(emergTemp);  tft.print(" C");

  tft.setCursor(35, 160);
  tft.setTextColor(WARNING);
  tft.print(emergWhy);

  tft.setTextColor(SECONDARY);
  tft.setCursor(60, 210);
  tft.print("Tekan SELECT untuk tutup");
}


void showLog() {

  tft.fillScreen(BG);
  drawHeader();

  DynamicJsonDocument doc = logLoadPatients();
  JsonArray patients = doc.as<JsonArray>();

  int total = patients.size();
  const int pageSize = 6;

  if (total == 0) {
    tft.setTextColor(TEXT);
    tft.setTextSize(2);
    tft.setCursor(90, 120);
    tft.println("NO DATA");
    return;
  }

  int totalPages = (total + pageSize - 1) / pageSize;

  // currentPatient sekarang menyimpan index awal halaman
  currentPatient = (currentPatient / pageSize) * pageSize;

  if (currentPatient >= total) {
    currentPatient = (totalPages - 1) * pageSize;
  }

  int currentPage = currentPatient / pageSize;
  int startIndex = currentPage * pageSize;
  int endIndex = min(startIndex + pageSize, total);

  // Table header
  drawCard(10, 55, 300, 30, CARD);

  tft.setTextColor(TEXT);
  tft.setTextSize(1);

  tft.setCursor(20, 65);  tft.print("ID");
  tft.setCursor(75, 65);  tft.print("AGE");
  tft.setCursor(120, 65); tft.print("GEN");
  tft.setCursor(160, 65); tft.print("STAT");
  tft.setCursor(210, 65); tft.print("TRI");
  tft.setCursor(255, 65); tft.print("SENT");

  int y = 100;

  // Hanya tampilkan 6 pasien per halaman
  for (int i = startIndex; i < endIndex; i++) {

    JsonObject patient = patients[i];

    tft.setTextSize(1);
    tft.setTextColor(TEXT);

    String id = patient["id"].as<String>();
    String shortID =
      id.startsWith("VITRA-") ? id.substring(6) : id;

    tft.setCursor(20, y);
    tft.print(shortID);

    tft.setCursor(75, y);
    tft.print(shortAge(patient["age"].as<String>()));

    tft.setCursor(120, y);
    tft.print(shortGender(patient["gender"].as<String>()));

    tft.setCursor(160, y);
    tft.print(shortStatus(patient["status"].as<String>()));

    tft.setCursor(210, y);
    tft.print(shortTriage(patient["triage"].as<String>()));

    tft.setCursor(255, y);
    tft.print(patient["ok"].as<bool>() ? "OK" : "X");

    y += 20;
  }

  // Footer
  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);

  tft.setCursor(20, 225);
  tft.print("TOTAL: ");
  tft.print(total);

  tft.setCursor(235, 225);
  tft.print("PAGE ");
  tft.print(currentPage + 1);
  tft.print("/");
  tft.print(totalPages);
}


// Dicache a bit
DynamicJsonDocument logLoadPatients() {
  File file = LittleFS.open("/patients.json", "r");

  DynamicJsonDocument doc(4096);

  if (file) {
    deserializeJson(doc, file);
    file.close();
  }

  if (!doc.is<JsonArray>()) {
    doc.to<JsonArray>();
  }

  return doc;
}

void logSavePatient(bool ok) {

  String newID = senseId;
  newID.trim();

  if (newID.isEmpty()) {
    Serial.println("[FS] ID kosong");
    return;
  }

  DynamicJsonDocument doc(4096);
  File file = LittleFS.open("/patients.json", "r");

  if (file) {
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
      Serial.println("[FS] JSON ERROR");
      return;
    }
  }

  if (doc.isNull()) {
    doc.to<JsonArray>();
  }

  if (!doc.is<JsonArray>()) {
    Serial.println("[FS] Format JSON tidak valid");
    return;
  }

  JsonArray patients = doc.as<JsonArray>();

  int foundIndex = -1;

  // Cari ID yang sudah ada
  for (int i = 0; i < (int)patients.size(); i++) {
    String oldID = patients[i]["id"].as<String>();
    oldID.trim();

    if (oldID == newID) {
      foundIndex = i;
      break;
    }
  }

  if (foundIndex == -1) {
    // Pasien baru
    foundIndex = patients.size();
    patients.createNestedObject();

    Serial.println("[FS] NEW PATIENT: " + newID);

  } else {
    // Hapus duplikat ID dari belakang
    for (int i = (int)patients.size() - 1;
         i > foundIndex; i--) {

      String oldID = patients[i]["id"].as<String>();
      oldID.trim();

      if (oldID == newID) {
        patients.remove(i);
      }
    }

    Serial.println("[FS] PATIENT UPDATED: " + newID);
  }

  // Overwrite informasi pasien
  JsonObject patient = patients[foundIndex];

  patient["id"] = newID;
  patient["age"] = selectedValue[0];
  patient["gender"] = selectedValue[1];
  patient["status"] = selectedValue[2];
  patient["triage"] = selectedValue[3];
  patient["ok"] = ok;

  if (doc.overflowed()) {
    Serial.println("[FS] JSON capacity exceeded");
    return;
  }

  // Simpan kembali ke LittleFS
  file = LittleFS.open("/patients.json", "w");

  if (!file) {
    Serial.println("[FS] WRITE ERROR");
    return;
  }

  size_t written = serializeJson(doc, file);
  file.close();

  if (written == 0) {
    Serial.println("[FS] SAVE ERROR");
    return;
  }

  Serial.println("[FS] SAVE SUCCESS: " + newID);
}


void resetPatientLog() {

  if (LittleFS.exists("/patients.json")) {

    if (LittleFS.remove("/patients.json")) {
      Serial.println("[FS] ALL PATIENT DATA DELETED");
    } else {
      Serial.println("[FS] DELETE FAILED");
    }

  } else {
    Serial.println("[FS] NO PATIENT DATA");
  }
}
