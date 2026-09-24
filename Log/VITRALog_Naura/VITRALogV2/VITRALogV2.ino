#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

HardwareSerial QRSerial(2);

String scannedQR="";
String patientID="";
bool qrCaptured = false;

#define TFT_CS 25
#define TFT_DC 27
#define TFT_RST 33
#define BTN_SCAN 32
#define BTN_LEFT 13
#define BTN_RIGHT 26
#define BTN_SELECT 14
#define BTN_LOG 12

// colors ui ux
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

// system state
enum Mode {
  HOME,
  SCAN,
  FORM,
  LOG
};

Mode mode = HOME;

// form data
String fieldName[] = {
  "AGE",  "GENDER",  "STATUS",  "TRIAGE"
};

String selectedValue[] = {
  "Anak-anak", "Pria", "Sadar", "Hijau"
};

String options[][4] = {
  {
    "Anak-anak", "Remaja", "Dewasa", "Lansia"
  },
  {
    "Pria", "Wanita", "Unidentified"
  },
  {
    "Sadar", "Bingung", "Tidak respon"
  },
  {
    "Hijau", "Kuning"
  }
};

String shortAge(String age)
{
  if(age=="Anak-anak") return "Ank";
  if(age=="Remaja") return "Rem";
  if(age=="Dewasa") return "Dew";
  if(age=="Lansia") return "Lan";

  return "-";
}

String shortGender(String gender)
{
  if(gender=="Pria") return "P";
  if(gender=="Wanita") return "W";

  return "-";
}

String shortStatus(String status)
{
  if(status=="Sadar") return "Sad";
  if(status=="Bingung") return "Bng";
  if(status=="Tidak respon") return "NR";

  return "-";
}

String shortTriage(String triage)
{
  if(triage=="Hijau") return "Hij";
  if(triage=="Kuning") return "Kun";
  if(triage=="Merah") return "Mer";

  return "-";
}

int optionCount[] = {
  4,  3,  3,  2
};

int currentField = 0;
int optionIndex[4] = {0,0,0,0};
int currentPatient = 0;

int getNextPatientID()
{
  File file = LittleFS.open("/patients.json","r");

  if(!file)
  {
    return 1;
  }

  DynamicJsonDocument doc(4096);

  deserializeJson(doc,file);
  file.close();

  JsonArray patients = doc.as<JsonArray>();

  return patients.size()+1;
}

void clearScreen()
{
  tft.fillScreen(BG);
}

// ui ux button
void drawButton(
  int x,
  int y,
  int w,
  int h,
  String label,
  uint16_t color
)
{
  tft.fillRoundRect(
    x,
    y,
    w,
    h,
    10,
    color
  );

  tft.setTextColor(TEXT);
  tft.setTextSize(2);

  int textX =
  x + (w - label.length()*12)/2;

  int textY =
  y + h/2 - 8;

  tft.setCursor(textX,textY);
  tft.print(label);
}

// ui ux header
void drawHeader()
{
  tft.fillRect(
    0,
    0,
    320,
    45,
    CARD
  );

  tft.setTextSize(2);
  tft.setTextColor(TEXT);
  tft.setCursor(20,15);
  tft.print("VITRA LOG");
  // battery icon
  tft.drawRect(
    260,
    15,
    30,
    12,
    TEXT
  );
  tft.fillRect(
    291,
    18,
    3,
    6,
    TEXT
  );

  // dummy battery
  tft.fillRect(
    263,
    18,
    20,
    6,
    SUCCESS
  );
}

// ui ux card
void drawCard(
  int x,
  int y,
  int w,
  int h,
  uint16_t color
)
{
  tft.fillRoundRect(
    x,
    y,
    w,
    h,
    8,
    color
  );
}

// ui ux selection
void drawSelection(
  String title,
  String value,
  int y,
  bool active
)
{
  // LABEL
  tft.setTextSize(1);

  if(active)
    tft.setTextColor(PRIMARY);
  else
    tft.setTextColor(TEXT);

  String titleDisplay;

  int titleX = (320 - titleDisplay.length()*6)/2;

  tft.setCursor(titleX, y);
  tft.print(titleDisplay);

  // VALUE
  tft.setTextSize(2);

  if(active)
    tft.setTextColor(PRIMARY);
  else
    tft.setTextColor(TEXT);

  String valueDisplay = "< " + value + " >";

  int valueX = (320 - valueDisplay.length()*12)/2;

  tft.setCursor(valueX, y+15);

  tft.print(valueDisplay);
}

// display home
void showHome()
{
  tft.fillScreen(BG);
  drawHeader();

  // Button SCAN
  drawButton(
    60,
    80,
    200,
    45,
    "SCAN",
    PRIMARY
  );

  // Button LOG
  drawButton(
    60,
    145,
    200,
    45,
    "VIEW LOG",
    CARD
  );
}

// display scan page
void showScanPage()
{
  tft.fillScreen(BG);
  drawHeader();

  tft.setTextColor(TEXT);
  tft.setTextSize(2);
  tft.setCursor(100,100);
  tft.println("SCANNING...");

  drawCard(
    30,
    185,
    260,
    35,
    CARD
  );

  tft.setCursor(55,195);
  tft.setTextSize(1);
  tft.println("WAITING FOR QR...");
}

// display form
void showForm()
{
  tft.fillScreen(BG);
  drawHeader();

  // ID pasien
  drawCard(20, 55, 280, 30, CARD);

  tft.setTextSize(1);
  tft.setTextColor(TEXT);
  tft.setCursor(35,65);
  tft.print("ID: ");
  tft.print(patientID);

  // selection fields
  drawSelection("AGE", selectedValue[0], 90, currentField==0);
  drawSelection("GENDER", selectedValue[1], 125, currentField==1);
  drawSelection("STATUS", selectedValue[2], 160, currentField==2);
  drawSelection("TRIAGE", selectedValue[3], 195, currentField==3);
}

// display log
void showLog()
{
  tft.fillScreen(BG);

  drawHeader();

  DynamicJsonDocument doc = loadPatients();
  JsonArray patients = doc.as<JsonArray>();

  if(patients.size()==0)
  {
    tft.setTextColor(TEXT);
    tft.setTextSize(2);

    tft.setCursor(90,120);
    tft.println("NO DATA");

    return;
  }

  // TABLE HEADER
  drawCard(10, 55, 300, 30, CARD);

  tft.setTextColor(TEXT);
  tft.setTextSize(1);

  tft.setCursor(25,65);
  tft.print("ID");
  tft.setCursor(75,65);
  tft.print("AGE");
  tft.setCursor(130,65);
  tft.print("GEN");
  tft.setCursor(185,65);
  tft.print("STAT");
  tft.setCursor(250,65);
  tft.print("TRI");

  // DATA ROW
  int y = 100;

  for(int i=0; i<patients.size(); i++)
  {
    JsonObject patient = patients[i];
    tft.setTextSize(1);
    tft.setTextColor(TEXT);

    // ID (ambil 3 digit terakhir)
    String id = patient["id"].as<String>();
    String shortID = id;
    if(id.startsWith("VITRA-"))
    {
      shortID = id.substring(6);
    }

    tft.setCursor(25,y);
    tft.print(shortID);

    tft.setCursor(75,y);
    tft.print(
      shortAge(patient["age"].as<String>())
    );

    tft.setCursor(130,y);
    tft.print(
      shortGender(patient["gender"].as<String>())
    );

    tft.setCursor(185,y);
    tft.print(
      shortStatus(patient["status"].as<String>())
    );

    tft.setCursor(250,y);
    tft.print(
      shortTriage(patient["triage"].as<String>())
    );

    y += 25;

    // stop kalau layar penuh
    if(y > 220)
    {
      break;
    }
  }

  // jumlah data
  tft.setCursor(120,225);
  tft.print("TOTAL: ");
  tft.print(patients.size());
}

void readPatientFile()
{
  File file = LittleFS.open("/patients.json","r");

  if(!file)
  {
    Serial.println("FILE NOT FOUND");
    return;
  }

  while(file.available())
  {
    Serial.write(file.read());
  }

  file.close();
}

// button check
bool pressed(int pin)
{

  if(digitalRead(pin)==LOW)
  {
    delay(150);
    return true;
  }

  return false;

}

// setup
void setup()
{
  Serial.begin(115200);
  QRSerial.begin(
  115200,
  SERIAL_8N1,
  16,
  17
  );
  delay(1000);

  if(!LittleFS.begin(true))
  {
  Serial.println("LittleFS ERROR");
  }
  else
  {
  Serial.println("LittleFS READY");
  }

  pinMode(BTN_SCAN,INPUT_PULLUP);
  pinMode(BTN_LEFT,INPUT_PULLUP);
  pinMode(BTN_RIGHT,INPUT_PULLUP);
  pinMode(BTN_SELECT,INPUT_PULLUP);
  pinMode(BTN_LOG,INPUT_PULLUP);

  BG       = tft.color565(10,18,30);
  CARD     = tft.color565(25,35,50);
  PRIMARY  = tft.color565(30,140,240);
  TEXT     = tft.color565(245,245,245);
  SUCCESS  = tft.color565(50,210,130);
  WARNING  = tft.color565(255,190,30);
  DANGER   = tft.color565(240,70,70);
  SECONDARY = tft.color565(100,110,130);

  tft.begin();
  tft.setRotation(1);

  showHome();
}

// loop
void loop()
{
  if(QRSerial.available())
  {
    scannedQR = QRSerial.readStringUntil('\n');
    scannedQR.trim();

    if(scannedQR.startsWith("VITRA") && !qrCaptured)
    {
      patientID = scannedQR;
      qrCaptured = true;
      Serial.print("VALID QR: ");
      Serial.println(patientID);

      mode = FORM;
      currentField = 0;
      showForm();
    }
  }

  if(mode == SCAN && patientID!="")
  {
    mode = FORM;
    currentField=0;
    showForm();
  }

  if(mode==HOME)
  {
    if(pressed(BTN_SCAN))
    {
      qrCaptured = false;
      patientID = "";
      mode = SCAN;
      showScanPage();
    }
    if(pressed(BTN_LOG))
    {
      mode = LOG;
      currentPatient=0;
      showLog();
    }
  }

  else if(mode==LOG)
  {
    if(pressed(BTN_LOG))
    {
      mode = HOME;
      showHome();
      return;
    }
    if(pressed(BTN_RIGHT))
    {
      DynamicJsonDocument doc = loadPatients();
      JsonArray patients = doc.as<JsonArray>();

      if(currentPatient < patients.size()-1)
      {
        currentPatient++;
      }

      showLog();
    }
    if(pressed(BTN_LEFT))
    {
      if(currentPatient>0)
      {
        currentPatient--;
      }
      showLog();
    }
    if(pressed(BTN_SELECT))
    {
      mode=HOME;
      showHome();
    }
  }

  else if(mode==FORM)
  {
    if(pressed(BTN_LEFT))
    {
      optionIndex[currentField]--;

      if(optionIndex[currentField]<0)
      {
        optionIndex[currentField]=
        optionCount[currentField]-1;
      }
      selectedValue[currentField] =
      options[currentField][optionIndex[currentField]];

      showForm();
    }

    if(pressed(BTN_RIGHT))
    {
      optionIndex[currentField]++;

      if(optionIndex[currentField]>=optionCount[currentField])
      {
        optionIndex[currentField]=0;
      }

      selectedValue[currentField] =
      options[currentField][optionIndex[currentField]];

      showForm();
    }

    if(pressed(BTN_SELECT))
    {
      if(currentField < 3)
      {
        currentField++;
        showForm();
      }
      else
      {
        savePatient();
        delay(500);

        mode = HOME;
        showHome();
      }
    }
  }
}

void savePatient()
{
  Serial.print("SAVING ID: ");
  Serial.println(patientID);
  
  File file = LittleFS.open("/patients.json", "r");

  DynamicJsonDocument doc(4096);

  if(file)
  {
    deserializeJson(doc,file);
    file.close();
  }
  else
  {
    doc.to<JsonArray>();
  }

  JsonArray patients = doc.as<JsonArray>();
  bool found = false;
  for(JsonObject patient : patients)
  {
    if(patient["id"] == patientID)
    {
      // UPDATE pasien lama
      patient["age"] = selectedValue[0];
      patient["gender"] = selectedValue[1];
      patient["status"] = selectedValue[2];
      patient["triage"] = selectedValue[3];

      found = true;

      Serial.println("PATIENT UPDATED");

      break;
    }
  }

  // kalau belum ada ID, buat baru
  if(!found)
  {
    JsonObject patient = patients.createNestedObject();

    patient["id"] = patientID;
    patient["age"] = selectedValue[0];
    patient["gender"] = selectedValue[1];
    patient["status"] = selectedValue[2];
    patient["triage"] = selectedValue[3];

    Serial.println("NEW PATIENT CREATED");
  }

 file = LittleFS.open("/patients.json","w");

  if(!file)
  {
    Serial.println("FAILED TO OPEN FILE");
    return;
  }

  serializeJson(doc,file);

  file.close();

  Serial.println("WRITE SUCCESS");
  Serial.println("PATIENT SAVED");
  File check = LittleFS.open("/patients.json", "r");

  if(check)
  {
    Serial.println("FILE CONTENT:");

    while(check.available())
    {
      Serial.write(check.read());
    }
    Serial.println();
    check.close();
  }
  else
  {
    Serial.println("FILE NOT FOUND");
  }
}

DynamicJsonDocument loadPatients()
{
  File file = LittleFS.open("/patients.json","r");

  DynamicJsonDocument doc(4096);

  if(file)
  {
    deserializeJson(doc,file);
    file.close();
  }

  return doc;
}