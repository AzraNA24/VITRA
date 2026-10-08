#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <LoRa.h>
#include <Preferences.h>

const char* AP_SSID = "Link";
const char* AP_PASSWORD = "12345678";


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


// normal HR 70-100 / SpO2 95-100 / Temp 36.0-38.0
// darurat HR 140-160 / SpO2 82-88 / Temp 39.6-40.5)
// Ionno nanti gimana. Ganti aja. :v

#define HR_WARN_HI 100
#define HR_WARN_LO 60
#define HR_CRIT_HI 130
#define HR_CRIT_LO 50

#define SPO2_WARN 95
#define SPO2_CRIT 90

#define TEMP_WARN 37.8
#define TEMP_CRIT 39.0
#define TEMP_CRIT_LO 35.0

#define OFFLINE_S 15

#define EMERG_RESEND_MS 4000


// -----------------------------------------------------
// Web Server
// -----------------------------------------------------

WebServer server(80);

unsigned long packetCount = 0;
unsigned long bootMs = 0;

#define MAX_PATIENTS 20

struct Patient {
  String id;
  bool reg = false;
  String age, gender, status, triage;

  bool hasVitals = false;
  int hr = 0;
  int spo2 = 0;
  float temp = 0;
  unsigned long lastVitalsMs = 0;
  int rssi = 0;

  // 0 = tidak ada, 1 = terkirim menunggu ACK, 2 = di-ACK LOG
  int alert = 0; 
  unsigned long lastEmergMs = 0;
};

Patient patients[MAX_PATIENTS];
int patientCount = 0;

Preferences prefs;

// Besih-bersih
String sanitizeField(String s, size_t maxLen) {

  String out = "";

  for (size_t i = 0; i < s.length(); i++) {

    char c = s[i];

    if (c == '|' || c == '=' || c == '<' || c == '>' ||
        c == '&' || c == '"' || c == '\'' || c == '\\' ||
        (uint8_t)c < 32) {
      c = ' ';
    }

    out += c;
  }

  out.trim();

  if (out.length() > maxLen) {
    out = out.substring(0, maxLen);
  }

  return out;
}

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


void sendLoRa(const String &msg) {
  delay(50);

  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
}

void savePatients() {

  String s = "";

  for (int i = 0; i < patientCount; i++) {

    if (!patients[i].reg) continue;
    s += patients[i].id + "|" +
         patients[i].age + "|" +
         patients[i].gender + "|" +
         patients[i].status + "|" +
         patients[i].triage + "\n";

  }

  prefs.putString("reg", s);
}


void loadPatients() {

  patientCount = 0;

  String s = prefs.getString("reg", "");

  int pos = 0;

  while (pos < (int)s.length() && patientCount < MAX_PATIENTS) {

    int nl = s.indexOf('\n', pos);

    if (nl < 0) {
      nl = s.length();
    }

    String line = s.substring(pos, nl);

    pos = nl + 1;

    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    int p4 = line.indexOf('|', p3 + 1);

    if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) {
      continue;
    }

    patients[patientCount].id     = line.substring(0, p1);
    patients[patientCount].reg    = true;
    patients[patientCount].age    = line.substring(p1 + 1, p2);
    patients[patientCount].gender = line.substring(p2 + 1, p3);
    patients[patientCount].status = line.substring(p3 + 1, p4);
    patients[patientCount].triage = line.substring(p4 + 1);

    patientCount++;
  }
}


int findPatient(const String &id) {

  for (int i = 0; i < patientCount; i++) {
    if (patients[i].id == id) {
      return i;
    }
  }

  return -1;
}

int findOrCreatePatient(const String &id) {

  int idx = findPatient(id);

  if (idx >= 0) {
    return idx;
  }

  if (patientCount >= MAX_PATIENTS) {
    return -1;
  }

  idx = patientCount++;
  patients[idx] = Patient();
  patients[idx].id = id;

  return idx;
}


// Tambah pasien baru atau timpa data registrasi lama dengan ID Sense yang sama
bool upsertPatient(const String &id, const String &age,
                   const String &gender, const String &status,
                   const String &triage) {

  int idx = findOrCreatePatient(id);

  if (idx < 0) {
    return false;
  }

  patients[idx].reg    = true;
  patients[idx].age    = age;
  patients[idx].gender = gender;
  patients[idx].status = status;
  patients[idx].triage = triage;

  savePatients();

  return true;
}

struct VitalLevel {
  int lh, ls, lt, lv;
  String why;
};

VitalLevel classifyVitals(int hr, int spo2, float temp) {

  VitalLevel v;
  v.lh = 0; v.ls = 0; v.lt = 0;

  String why = "";

  if (hr >= HR_CRIT_HI || hr <= HR_CRIT_LO) {
    v.lh = 2;
    why += "HR Kritis+";
  } else if (hr >= HR_WARN_HI || hr <= HR_WARN_LO) {
    v.lh = 1;
    why += "HR Waspada+";
  }

  if (spo2 <= SPO2_CRIT) {
    v.ls = 2;
    why += "SpO2 Kritis+";
  } else if (spo2 <= SPO2_WARN) {
    v.ls = 1;
    why += "SpO2 Waspada+";
  }

  if (temp >= TEMP_CRIT || temp <= TEMP_CRIT_LO) {
    v.lt = 2;
    why += "Suhu Kritis+";
  } else if (temp >= TEMP_WARN) {
    v.lt = 1;
    why += "Suhu Waspada+";
  }

  if (why.endsWith("+")) {
    why.remove(why.length() - 1);
  }

  v.why = why;
  v.lv = max(v.lh, max(v.ls, v.lt));

  return v;
}

void updateEmergencyState(int idx, int lv) {

  Patient &p = patients[idx];

  if (lv >= 2) {

    bool needSend = (p.alert == 0) ||
                    (p.alert == 1 && millis() - p.lastEmergMs >= EMERG_RESEND_MS);

    if (needSend) {

      VitalLevel v = classifyVitals(p.hr, p.spo2, p.temp);
      String whyEsc = v.why;
      whyEsc.replace(" ", "_");

      String packet =
        "EMERG|" + p.id +
        "|HR=" + String(p.hr) +
        "|SPO2=" + String(p.spo2) +
        "|TEMP=" + String(p.temp, 1) +
        "|WHY=" + whyEsc;

      Serial.println("[LINK] >>> KIRIM DARURAT ke LOG: " + packet);

      sendLoRa(packet);

      p.alert = 1;
      p.lastEmergMs = millis();
    }

  } else {
    if (p.alert != 0) {
      p.alert = 0;
    }
  }
}


void handleEmergencyAck(const String &packet) {
  String id = packet.substring(String("EMACK|").length());

  int idx = findPatient(id);

  if (idx < 0) {
    return;
  }

  patients[idx].alert = 2;

  Serial.println("[LINK] Darurat " + id + " sudah dikonfirmasi VITRA LOG");
}


// =====================================================
// WEB PAGE
// =====================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>VITRA LINK</title>
<style>
:root{--bg:#0a121e;--card:#192332;--card2:#101a29;--line:#26344a;--text:#f5f5f5;--muted:#8a96ab;--ok:#32d282;--warn:#ffbe1e;--crit:#f04646;--pri:#1e8cf0}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif}
.wrap{max-width:1100px;margin:0 auto;padding:16px}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px}
h1{margin:0;font-size:22px;letter-spacing:.5px}
h1 span{color:var(--pri)}
.meta{color:var(--muted);font-size:13px;display:flex;gap:14px;align-items:center;flex-wrap:wrap}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--ok);margin-right:6px}
.dot.bad{background:var(--crit)}
#banner{display:none;background:var(--crit);color:#fff;border-radius:10px;padding:12px 16px;margin-bottom:14px;font-weight:700;animation:blink 1s infinite}
#banner small{display:block;font-weight:400;opacity:.9}
@keyframes blink{50%{opacity:.7}}
@media (prefers-reduced-motion:reduce){#banner{animation:none}}
.sum{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:10px;margin-bottom:14px}
.chip{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 14px}
.chip b{display:block;font-size:26px}
.chip span{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.5px}
.chip.c b{color:var(--crit)}
.chip.w b{color:var(--warn)}
.chip.o b{color:var(--ok)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(290px,1fr));gap:14px}
.card{background:var(--card);border:1px solid var(--line);border-left:5px solid var(--ok);border-radius:12px;padding:14px}
.card.warn{border-left-color:var(--warn)}
.card.crit{border-color:var(--crit);border-left-color:var(--crit);box-shadow:0 0 0 1px var(--crit) inset}
.card.off,.card.wait{border-left-color:var(--muted);opacity:.75}
.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.pid{font-weight:700;font-size:18px}
.pill{font-size:11px;font-weight:700;padding:3px 9px;border-radius:99px;background:var(--ok);color:#06210f}
.pill.warn{background:var(--warn);color:#2b1d00}
.pill.crit{background:var(--crit);color:#fff}
.pill.off,.pill.wait{background:var(--muted);color:#0a121e}
.info{color:var(--muted);font-size:13px;margin-bottom:10px}
.tri{display:inline-block;padding:1px 8px;border-radius:99px;font-size:12px;font-weight:600;margin-left:6px;color:#111}
.tri.Hijau{background:var(--ok)}
.tri.Kuning{background:var(--warn)}
.vitals{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.v{background:var(--card2);border-radius:8px;padding:8px 6px;text-align:center;border-bottom:3px solid var(--ok)}
.v.l1{border-bottom-color:var(--warn)}
.v.l2{border-bottom-color:var(--crit)}
.vl{color:var(--muted);font-size:11px;text-transform:uppercase}
.vv{font-size:26px;font-weight:700}
.vv small{font-size:11px;font-weight:400;color:var(--muted);margin-left:2px}
.v.l1 .vv{color:var(--warn)}
.v.l2 .vv{color:var(--crit)}
.why{margin-top:10px;font-weight:600;font-size:13px;color:var(--crit)}
.why.w{color:var(--warn)}
.foot{margin-top:8px;color:var(--muted);font-size:12px;display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
.empty{grid-column:1/-1;color:var(--muted);text-align:center;padding:36px 0}
</style>
</head>
<body>
<div class="wrap">

<header>
  <h1>VITRA <span>LINK</span></h1>
  <div class="meta">
    <span><i class="dot" id="dot"></i><span id="conn">Menghubungkan&hellip;</span></span>
    <span>Paket SENSE: <b id="pk">0</b></span>
    <span>Uptime: <b id="up">-</b></span>
  </div>
</header>

<div id="banner"></div>

<div class="sum">
  <div class="chip"><b id="n_all">0</b><span>Pasien</span></div>
  <div class="chip c"><b id="n_crit">0</b><span>Kritis</span></div>
  <div class="chip w"><b id="n_warn">0</b><span>Waspada</span></div>
  <div class="chip o"><b id="n_ok">0</b><span>Normal</span></div>
  <div class="chip"><b id="n_off">0</b><span>Offline / menunggu</span></div>
</div>

<div class="grid" id="grid"><div class="empty">Memuat&hellip;</div></div>

</div>

<script>
const $ = id => document.getElementById(id);
const OFFLINE_S = 15;
const ESC = {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'};
const esc = s => String(s).replace(/[&<>"']/g, c => ESC[c]);
const ALERT_TXT = ['', 'menunggu konfirmasi LOG\u2026', 'diterima VITRA LOG \u2713'];
const RANK = {crit:0, warn:1, off:2, wait:3, ok:4};

function fmtAgo(s){
  if (s < 60) return s + ' dtk lalu';
  if (s < 3600) return Math.floor(s / 60) + ' mnt lalu';
  return Math.floor(s / 3600) + ' jam lalu';
}

function fmtUp(s){
  const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h > 0 ? h + ' j ' + m + ' m' : m + ' m ' + (s % 60) + ' d';
}

// [kelas css, label]
function state(p){
  if (p.lv === 2) return ['crit', 'KRITIS'];
  if (!p.hasv) return ['wait', 'MENUNGGU SENSE'];
  if (p.ago > OFFLINE_S) return ['off', 'SENSE OFFLINE'];
  if (p.lv === 1) return ['warn', 'WASPADA'];
  return ['ok', 'NORMAL'];
}

function tile(label, val, unit, lv){
  return '<div class="v l' + lv + '"><div class="vl">' + label +
         '</div><div class="vv">' + val + '<small>' + unit + '</small></div></div>';
}

function card(p){
  const st = state(p);
  const off = p.hasv && p.ago > OFFLINE_S;

  const info = p.reg
    ? esc(p.age) + ' &middot; ' + esc(p.gender) + ' &middot; ' + esc(p.status) +
      '<span class="tri ' + esc(p.triage) + '">' + esc(p.triage) + '</span>'
    : 'Belum terdaftar di VITRA LOG';

  let body;

  if (p.hasv) {
    body = '<div class="vitals">' +
      tile('HR',   p.hr   > 0 ? p.hr : '--',                 'bpm', p.lh) +
      tile('SpO2', p.spo2 > 0 ? p.spo2 : '--',               '%',   p.ls) +
      tile('Suhu', p.temp > 0 ? p.temp.toFixed(1) : '--',    '&deg;C', p.lt) +
      '</div>';
  } else {
    body = '<div class="empty" style="padding:16px 0">Menunggu data VITRA SENSE&hellip;</div>';
  }

  let why = '';
  if (p.lv > 0 && p.why) {
    why = '<div class="why' + (p.lv === 1 ? ' w' : '') + '">' +
          esc(p.why.split('+').join(', ')) + '</div>';
  }

  let foot = '';
  if (p.hasv) {
    foot += '<div class="foot"><span>' + fmtAgo(p.ago) + (off ? ' &middot; SENSE OFFLINE' : '') +
            '</span><span>RSSI ' + p.rssi + ' dBm</span></div>';
  }
  if (p.alert > 0) {
    foot += '<div class="foot"><span>Alert: ' + ALERT_TXT[p.alert] + '</span></div>';
  }

  return '<div class="card ' + st[0] + '">' +
    '<div class="top"><span class="pid">' + esc(p.id) + '</span>' +
    '<span class="pill ' + st[0] + '">' + st[1] + '</span></div>' +
    '<div class="info">' + info + '</div>' + body + why + foot + '</div>';
}

function render(d){
  const ps = d.patients.slice().sort((a, b) =>
    RANK[state(a)[0]] - RANK[state(b)[0]] || a.id.localeCompare(b.id));

  const cnt = {crit:0, warn:0, ok:0, off:0, wait:0};
  ps.forEach(p => cnt[state(p)[0]]++);

  $('n_all').textContent  = ps.length;
  $('n_crit').textContent = cnt.crit;
  $('n_warn').textContent = cnt.warn;
  $('n_ok').textContent   = cnt.ok;
  $('n_off').textContent  = cnt.off + cnt.wait;
  $('pk').textContent     = d.packets;
  $('up').textContent     = fmtUp(d.uptime);

  const cr = ps.filter(p => state(p)[0] === 'crit');
  const b = $('banner');

  if (cr.length > 0) {
    let sub;
    if (cr.some(p => p.alert === 1)) sub = 'Mengirim alert ke VITRA LOG\u2026';
    else if (cr.every(p => p.alert === 2)) sub = 'Alert sudah diterima VITRA LOG';
    else sub = 'Memverifikasi pembacaan kritis\u2026';

    b.style.display = 'block';
    b.innerHTML = '&#9888; EMERGENCY: ' + cr.map(p => esc(p.id)).join(', ') +
                  '<small>' + sub + '</small>';
    document.title = '(!) VITRA LINK';
  } else {
    b.style.display = 'none';
    document.title = 'VITRA LINK';
  }

  $('grid').innerHTML = ps.length > 0
    ? ps.map(card).join('')
    : '<div class="empty">Belum ada pasien atau data SENSE.</div>';
}

let busy = false, fails = 0;

async function poll(){
  if (busy) return;
  busy = true;

  try {
    const r = await fetch('/api/data', {cache: 'no-store'});
    render(await r.json());
    fails = 0;
  } catch (e) {
    fails++;
  }

  busy = false;

  $('dot').className = 'dot' + (fails >= 3 ? ' bad' : '');
  $('conn').textContent = fails >= 3 ? 'Terputus dari LINK' : 'Terhubung';
}

setInterval(poll, 1000);
poll();
</script>
</body>
</html>
)rawliteral";


void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

String jsonEscape(const String &s) {
  String out = s;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  return out;
}

void handleApiData() {

  String json = "{";

  json += "\"packets\":" + String(packetCount) + ",";
  json += "\"uptime\":" + String((millis() - bootMs) / 1000) + ",";

  json += "\"patients\":[";

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    if (i > 0) json += ",";

    VitalLevel v = classifyVitals(p.hr, p.spo2, p.temp);
    long ago = p.hasVitals ? (long)((millis() - p.lastVitalsMs) / 1000) : 0;

    json += "{";
    json += "\"id\":\"" + jsonEscape(p.id) + "\",";
    json += "\"reg\":" + String(p.reg ? "true" : "false") + ",";
    json += "\"age\":\"" + jsonEscape(p.age) + "\",";
    json += "\"gender\":\"" + jsonEscape(p.gender) + "\",";
    json += "\"status\":\"" + jsonEscape(p.status) + "\",";
    json += "\"triage\":\"" + jsonEscape(p.triage) + "\",";
    json += "\"hasv\":" + String(p.hasVitals ? "true" : "false") + ",";
    json += "\"hr\":" + String(p.hr) + ",";
    json += "\"spo2\":" + String(p.spo2) + ",";
    json += "\"temp\":" + String(p.temp, 1) + ",";
    json += "\"lh\":" + String(p.hasVitals ? v.lh : 0) + ",";
    json += "\"ls\":" + String(p.hasVitals ? v.ls : 0) + ",";
    json += "\"lt\":" + String(p.hasVitals ? v.lt : 0) + ",";
    json += "\"lv\":" + String(p.hasVitals ? v.lv : 0) + ",";
    json += "\"why\":\"" + jsonEscape(p.hasVitals ? v.why : "") + "\",";
    json += "\"ago\":" + String(ago) + ",";
    json += "\"rssi\":" + String(p.rssi) + ",";
    json += "\"alert\":" + String(p.alert);
    json += "}";
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void handleRegistration(const String &packet, int rssi) {

  Serial.println();
  Serial.println("================================");
  Serial.println("[LINK] REGISTRASI PASIEN");
  Serial.println("================================");

  Serial.println("DATA : " + packet);
  Serial.println("RSSI : " + String(rssi) + " dBm");

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  String age    = sanitizeField(getField(packet, "AGE"), 16);
  String gender = sanitizeField(getField(packet, "GENDER"), 16);
  String status = sanitizeField(getField(packet, "STATUS"), 16);
  String triage = sanitizeField(getField(packet, "TRIAGE"), 16);

  bool valid =
    id.length() > 0 &&
    id.length() <= 20 &&
    sanitizeField(id, 20) == id &&
    age.length() > 0 &&
    gender.length() > 0 &&
    status.length() > 0 &&
    triage.length() > 0;

  if (!valid) {
    Serial.println("[LINK] Registrasi tidak valid");
    sendLoRa("NACK|" + id + "|INVALID");
    Serial.println("================================");
    return;
  }

  if (!upsertPatient(id, age, gender, status, triage)) {
    Serial.println("[LINK] Penyimpanan penuh");
    sendLoRa("NACK|" + id + "|FULL");
    Serial.println("================================");
    return;
  }

  Serial.println(
    "[LINK] Tersimpan: " + id + " - " + age + ", " +
    gender + ", " + status + ", " + triage
  );

  sendLoRa("ACK|" + id);

  Serial.println("[LINK] ACK dikirim ke LOG");
  Serial.println("================================");
}

// Dari Sense
void handleSenseData(const String &packet, int rssi) {

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  if (id.length() == 0) {
    return;
  }

  int idx = findOrCreatePatient(id);

  if (idx < 0) {
    Serial.println("[LINK] Data SENSE ditolak, penyimpanan penuh: " + id);
    return;
  }

  Patient &p = patients[idx];

  p.hasVitals = true;
  p.hr = getField(packet, "HR").toInt();
  p.spo2 = getField(packet, "SPO2").toInt();
  p.temp = getField(packet, "TEMP").toFloat();
  p.lastVitalsMs = millis();
  p.rssi = rssi;

  VitalLevel v = classifyVitals(p.hr, p.spo2, p.temp);

  Serial.println();
  Serial.println("================================");
  Serial.println("[LINK] DATA VITAL DITERIMA");
  Serial.println("================================");
  Serial.println("ID   : " + id + (p.reg ? " (terdaftar)" : " (belum terdaftar)"));
  Serial.println("HR   : " + String(p.hr) + " bpm");
  Serial.println("SpO2 : " + String(p.spo2) + " %");
  Serial.println("Suhu : " + String(p.temp, 1) + " C");
  Serial.println("RSSI : " + String(rssi) + " dBm");

  if (v.lv > 0) {
    Serial.println("STATUS: " + v.why);
  }

  Serial.println("================================");

  updateEmergencyState(idx, v.lv);
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

  int rssi = LoRa.packetRssi();

  if (received.startsWith("REG|")) {
    handleRegistration(received, rssi);
    return;
  }

  if (received.startsWith("EMACK|")) {
    handleEmergencyAck(received);
    return;
  }

  if (received.startsWith("SENSE|")) {
    packetCount++;
    handleSenseData(received, rssi);
    return;
  }

  Serial.println(
    "[RX] Paket tidak dikenal: " + received +
    " | RSSI " + String(rssi) + " dBm"
  );
}

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("        VITRA-LINK");
  Serial.println("================================");

  bootMs = millis();

  prefs.begin("vitra", false);

  loadPatients();

  Serial.println("[NVS] Pasien terdaftar: " + String(patientCount));

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    LORA_SS
  );

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

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.println("[WiFi] SSID: " + String(AP_SSID));
  Serial.println("[WiFi] IP: " + WiFi.softAPIP().toString());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);

  server.begin();

  Serial.println();
  Serial.println("================================");
  Serial.println("VITRA-LINK SIAP");
  Serial.println("Buka:");
  Serial.println("http://" + WiFi.softAPIP().toString());
  Serial.println("================================");
}

void loop() {
  server.handleClient();
  receiveLoRa();

  delay(5);
}
