#define MAX_LOG      20
#define LOG_DOC_SIZE 6144


// -----------------------------------------------------
// Data form
// -----------------------------------------------------

String fieldName[] = {
  "AGE", "GENDER", "STATUS", "TRIAGE"
};

String options[][4] = {
  { "Anak-anak", "Remaja", "Dewasa", "Lansia" },
  { "Pria", "Wanita", "Unidentified" },
  { "Sadar", "Bingung", "Tidak respon" },
  { "Hijau", "Kuning" }
};

int optionCount[] = {
  4, 3, 3, 2
};

int optionIndex[4] = { 0, 0, 0, 0 };

int currentField = 0;
int currentPatient = 0;


// Nilai yang sedang dipilih untuk field f
String formValue(int f) {
  return options[f][optionIndex[f]];
}


// Buttons
const int BTN_PINS[5] = {
  BTN_SCAN, BTN_LEFT, BTN_RIGHT, BTN_SELECT, BTN_LOG
};

bool btnLast[5] = { false, false, false, false, false };
bool btnEvt[5]  = { false, false, false, false, false };
unsigned long btnTime[5] = { 0, 0, 0, 0, 0 };


void updateButtons() {

  for (int k = 0; k < 5; k++) {

    bool down = (digitalRead(BTN_PINS[k]) == LOW);

    if (down && !btnLast[k] && millis() - btnTime[k] > 80) {
      btnEvt[k] = true;
      btnTime[k] = millis();
    }

    btnLast[k] = down;
  }
}


// True sekali per penekanan tombol
bool pressed(int pin) {

  for (int k = 0; k < 5; k++) {

    if (BTN_PINS[k] == pin && btnEvt[k]) {
      btnEvt[k] = false;
      return true;
    }
  }

  return false;
}


void clearButtonEvents() {

  for (int k = 0; k < 5; k++) {
    btnEvt[k] = false;
  }
}


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


uint16_t triageColor(String t) {

  if (t == "Hijau") {
    return SUCCESS;
  }

  if (t == "Kuning") {
    return WARNING;
  }

  return DANGER;
}

void drawButton(
  int x,
  int y,
  int w,
  int h,
  String label,
  uint16_t color
)
{
  tft.fillRoundRect(x, y, w, h, 10, color);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);

  int textX = x + (w - (int)label.length() * 12) / 2;
  int textY = y + h / 2 - 8;

  tft.setCursor(textX, textY);
  tft.print(label);
}


void drawHeader()
{
  tft.fillRect(0, 0, 320, 45, CARD);

  tft.setTextSize(2);
  tft.setTextColor(TEXT);
  tft.setCursor(20, 15);
  tft.print("VITRA LOG");

  tft.drawRect(260, 15, 30, 12, TEXT);
  tft.fillRect(291, 18, 3, 6, TEXT);

  tft.fillRect(263, 18, 20, 6, SUCCESS);
}


void drawCard(
  int x,
  int y,
  int w,
  int h,
  uint16_t color
)
{
  tft.fillRoundRect(x, y, w, h, 8, color);
}


void drawRow(int y, String label, String value, uint16_t color)
{
  tft.setTextSize(2);

  tft.setCursor(20, y);
  tft.setTextColor(SECONDARY);
  tft.print(label);

  tft.setTextColor(color);
  tft.print(value);
}

void showHome()
{
  tft.fillScreen(BG);
  drawHeader();

  drawButton(60, 80, 200, 45, "SCAN", PRIMARY);

  drawButton(60, 145, 200, 45, "VIEW LOG", CARD);
}


void showScan(String msg)
{
  tft.fillScreen(BG);
  drawHeader();

  drawCard(20, 70, 280, 130, CARD);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(35, 85);
  tft.print("SCAN QR");

  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);
  tft.setCursor(35, 120);
  tft.print("Arahkan scanner ke QR VITRA Sense");

  tft.setCursor(35, 135);
  tft.print("SCAN / SELECT / LOG = batal");

  if (msg.length() > 0) {
    tft.setTextColor(DANGER);
    tft.setCursor(35, 170);
    tft.print(msg);
  }
}


void showForm()
{
  tft.fillScreen(BG);
  drawHeader();

  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);
  tft.setCursor(20, 52);
  tft.print("ID SENSE: " + senseId);

  int y = 68;

  for (int i = 0; i < 4; i++)
  {
    // label
    tft.setTextSize(1);
    tft.setTextColor(SECONDARY);
    tft.setCursor(25, y);
    tft.print(fieldName[i]);

    drawCard(20, y + 10, 280, 30, (i == currentField) ? PRIMARY : CARD);

    tft.setTextSize(2);
    tft.setTextColor(TEXT);
    tft.setCursor(35, y + 17);
    tft.print(formValue(i));

    y += 42;
  }
}


void showSending()
{
  tft.fillScreen(BG);
  drawHeader();

  drawCard(20, 70, 280, 130, CARD);

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(35, 85);
  tft.print("MENGIRIM KE LINK");

  tft.setCursor(35, 120);
  tft.print(senseId);

  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);
  tft.setCursor(35, 160);
  tft.print("Percobaan " + String(regTries) + "/" + String(REG_MAX_TRIES));
}


void showResult(bool ok)
{
  tft.fillScreen(BG);
  drawHeader();

  drawCard(20, 70, 280, 130, CARD);

  tft.setTextSize(3);
  tft.setTextColor(ok ? SUCCESS : DANGER);
  tft.setCursor(35, 85);
  tft.print(ok ? "TERKIRIM" : "GAGAL KIRIM");

  tft.setTextSize(2);
  tft.setTextColor(TEXT);
  tft.setCursor(35, 125);
  tft.print(senseId);

  tft.setTextSize(1);
  tft.setTextColor(SECONDARY);

  tft.setCursor(35, 160);
  tft.print(ok ? "Tersimpan di LINK dan LOG" : "Gagal terkirim ke LINK");

  if (!ok) {
    tft.setCursor(35, 175);
    tft.print("Data hanya tersimpan di LOG");
  }

  mode = M_RESULT;

  resultUntil = millis() + 2500;
}


void showLog()
{
  tft.fillScreen(BG);
  drawHeader();

  DynamicJsonDocument doc(LOG_DOC_SIZE);

  loadPatients(doc);

  JsonArray patients = doc.as<JsonArray>();

  int n = patients.size();

  if (n == 0)
  {
    tft.setTextColor(TEXT);
    tft.setTextSize(2);
    tft.setCursor(76, 110);
    tft.print("BELUM ADA DATA");

    return;
  }

  if (currentPatient >= n) {
    currentPatient = n - 1;
  }

  if (currentPatient < 0) {
    currentPatient = 0;
  }

  JsonObject p = patients[currentPatient];

  // nomor data di header
  tft.setTextSize(2);
  tft.setTextColor(SECONDARY);
  tft.setCursor(170, 15);
  tft.print(String(currentPatient + 1) + "/" + String(n));

  String triage = p["triage"] | "-";
  bool sent = p["sent"] | false;

  drawRow(58,  "ID: ",     String(p["id"] | "-"),     TEXT);
  drawRow(88,  "AGE: ",    String(p["age"] | "-"),    TEXT);
  drawRow(118, "GENDER: ", String(p["gender"] | "-"), TEXT);
  drawRow(148, "STATUS: ", String(p["status"] | "-"), TEXT);
  drawRow(178, "TRIAGE: ", triage,                    triageColor(triage));
  drawRow(208, "LINK: ",   sent ? "Terkirim" : "Belum", sent ? SUCCESS : WARNING);
}

// Layar emergency: latar merah, vital pasien, alasan alert
void showAlert()
{
  if (alertCount <= 0) {
    return;
  }

  if (alertIdx >= alertCount) {
    alertIdx = alertCount - 1;
  }

  if (alertIdx < 0) {
    alertIdx = 0;
  }

  AlertInfo &a = alerts[alertIdx];

  tft.fillScreen(DANGER);

  // header
  tft.fillRect(0, 0, 320, 30, BG);

  tft.setTextSize(2);
  tft.setTextColor(DANGER);
  tft.setCursor(10, 8);
  tft.print("! EMERGENCY !");

  if (alertCount > 1) {
    tft.setTextColor(TEXT);
    tft.setCursor(250, 8);
    tft.print(String(alertIdx + 1) + "/" + String(alertCount));
  }

  // ID Sense
  tft.setTextSize(a.id.length() <= 17 ? 3 : 2);
  tft.setTextColor(TEXT);
  tft.setCursor(10, 38);
  tft.print(a.id);

  // Info pasien (dari registrasi di LOG)
  tft.setTextSize(2);

  if (a.age == "-") {

    tft.setCursor(10, 68);
    tft.print("Belum terdaftar");

  } else {

    tft.setCursor(10, 68);
    tft.print(a.age + ", " + a.gender);

    tft.setCursor(10, 86);
    tft.print(a.status + ", " + a.triage);
  }

  // Vital: yang bermasalah dikuningkan
  const char* labels[3] = { "HR (bpm)", "SpO2 (%)", "SUHU (C)" };
  String vals[3] = { a.hr, a.spo2, a.temp };
  bool bad[3] = {
    a.why.indexOf("HR")   >= 0,
    a.why.indexOf("SpO2") >= 0,
    a.why.indexOf("Suhu") >= 0
  };

  for (int i = 0; i < 3; i++)
  {
    int x = 10 + i * 102;

    tft.fillRoundRect(x, 110, 96, 52, 6, BG);

    tft.setTextSize(1);
    tft.setTextColor(SECONDARY);
    tft.setCursor(x + 8, 116);
    tft.print(labels[i]);

    tft.setTextSize(3);
    tft.setTextColor(bad[i] ? WARNING : TEXT);
    tft.setCursor(x + 8, 130);
    tft.print(vals[i]);
  }

  // Alasan (maks. 3 baris)
  tft.setTextSize(2);
  tft.setTextColor(TEXT);

  int y = 172;
  String w = a.why;

  while (w.length() > 0 && y <= 208)
  {
    int p = w.indexOf('+');

    String part = (p < 0) ? w : w.substring(0, p);

    w = (p < 0) ? String("") : w.substring(p + 1);

    tft.setCursor(10, y);
    tft.print("- " + part);

    y += 18;
  }

  tft.setTextSize(1);
  tft.setCursor(10, 228);
  tft.print(alertCount > 1 ? "< > lainnya   SELECT = tutup" : "SELECT = tutup");
}


// Kembali ke layar sebelum alert muncul
void restoreScreen()
{
  mode = modeBeforeAlert;

  switch (mode)
  {
    case M_SCAN:
      showScan("");
      break;

    case M_FORM:
      showForm();
      break;

    case M_LOG:
      showLog();
      break;

    default:
      goHome();   // M_HOME atau M_RESULT
      break;
  }
}


// Tutup alert yang sedang tampil; kalau masih ada alert lain, tampilkan berikutnya
void dismissAlert()
{
  if (alertCount > 0)
  {
    for (int i = alertIdx + 1; i < alertCount; i++) {
      alerts[i - 1] = alerts[i];
    }

    alertCount--;
  }

  if (alertCount == 0)
  {
    alertIdx = 0;

    restoreScreen();

    return;
  }

  if (alertIdx >= alertCount) {
    alertIdx = alertCount - 1;
  }

  showAlert();
}


void goHome()
{
  mode = M_HOME;

  showHome();
}


void startScan()
{
  qrBuf = "";
  serBuf = "";

  while (Serial2.available()) {
    Serial2.read();
  }

  mode = M_SCAN;

  showScan("");
}


// Dipanggil setelah QR valid terbaca
void startForm()
{
  for (int k = 0; k < 4; k++) {
    optionIndex[k] = 0;
  }

  currentField = 0;

  mode = M_FORM;

  showForm();
}


void submitForm()
{
  regTries = 0;

  mode = M_SEND;

  sendRegistration();
}


// =====================================================
// Simpan data
// =====================================================

void loadPatients(DynamicJsonDocument &doc)
{
  File file = LittleFS.open("/patients.json", "r");

  if (file)
  {
    deserializeJson(doc, file);
    file.close();
  }

  if (!doc.is<JsonArray>())
  {
    doc.to<JsonArray>();
  }
}


// sent = true kalau LINK sudah mengirim ACK
void savePatient(bool sent)
{
  DynamicJsonDocument doc(LOG_DOC_SIZE);

  loadPatients(doc);

  JsonArray patients = doc.as<JsonArray>();

  // Buang data terlama kalau sudah penuh
  while (patients.size() >= MAX_LOG)
  {
    patients.remove(0);
  }

  JsonObject patient = patients.createNestedObject();

  patient["id"]     = senseId;
  patient["age"]    = formValue(0);
  patient["gender"] = formValue(1);
  patient["status"] = formValue(2);
  patient["triage"] = formValue(3);
  patient["sent"]   = sent;

  File file = LittleFS.open("/patients.json", "w");

  if (!file)
  {
    Serial.println("[FS] Gagal membuka /patients.json");
    return;
  }

  serializeJson(doc, file);

  file.close();

  Serial.println(
    "[FS] Pasien tersimpan: " + senseId +
    (sent ? " (terkirim)" : " (belum terkirim)")
  );
}


// Buttons-buttons
void handleButtons()
{
  updateButtons();

  switch (mode)
  {
    case M_HOME:

      if (pressed(BTN_SCAN))
      {
        startScan();
      }
      else if (pressed(BTN_LOG))
      {
        currentPatient = 0;
        mode = M_LOG;
        showLog();
      }

      break;


    case M_SCAN:

      if (pressed(BTN_SCAN) || pressed(BTN_SELECT) || pressed(BTN_LOG))
      {
        goHome();
      }

      break;


    case M_FORM:

      if (pressed(BTN_LEFT))
      {
        optionIndex[currentField]--;

        if (optionIndex[currentField] < 0)
        {
          optionIndex[currentField] = optionCount[currentField] - 1;
        }

        showForm();
      }
      else if (pressed(BTN_RIGHT))
      {
        optionIndex[currentField]++;

        if (optionIndex[currentField] >= optionCount[currentField])
        {
          optionIndex[currentField] = 0;
        }

        showForm();
      }
      else if (pressed(BTN_SELECT))
      {
        currentField++;

        if (currentField >= 4)
        {
          submitForm();
        }
        else
        {
          showForm();
        }
      }
      else if (pressed(BTN_LOG))
      {
        // batal
        goHome();
      }

      break;


    case M_LOG:

      if (pressed(BTN_LOG) || pressed(BTN_SELECT))
      {
        goHome();
      }
      else if (pressed(BTN_RIGHT))
      {
        currentPatient++;
        showLog();
      }
      else if (pressed(BTN_LEFT))
      {
        if (currentPatient > 0)
        {
          currentPatient--;
        }

        showLog();
      }

      break;


    case M_RESULT:

      if (pressed(BTN_SCAN))
      {
        startScan();
      }
      else if (pressed(BTN_SELECT) || pressed(BTN_LOG))
      {
        goHome();
      }

      break;


    case M_ALERT:

      if (pressed(BTN_RIGHT))
      {
        alertIdx = (alertIdx + 1) % alertCount;
        showAlert();
      }
      else if (pressed(BTN_LEFT))
      {
        alertIdx = (alertIdx + alertCount - 1) % alertCount;
        showAlert();
      }
      else if (pressed(BTN_SELECT) || pressed(BTN_LOG))
      {
        dismissAlert();
      }

      break;


    default:
      // M_SEND: tidak ada input selama pengiriman
      break;
  }

  clearButtonEvents();
}
