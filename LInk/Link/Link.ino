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

const int   SPO2_CRIT_LT = 90;
const int   SPO2_WARN_LT = 95;

const int   HR_CRIT_LT   = 40;
const int   HR_WARN_LT   = 50;
const int   HR_WARN_GT   = 110;
const int   HR_CRIT_GT   = 130;

const float TEMP_CRIT_LT = 35.0;
const float TEMP_WARN_LT = 36.0;
const float TEMP_WARN_GT = 38.0;
const float TEMP_CRIT_GT = 39.5;


// Sense dianggap offline kalau tidak ada paket selama ini
const unsigned long SENSE_TIMEOUT_MS = 15000;
const uint8_t ALERT_CONFIRM = 2;

const unsigned long ALERT_RETRY_FAST_MS = 4000;
const unsigned long ALERT_RETRY_SLOW_MS = 30000;
const uint8_t       ALERT_FAST_TRIES    = 5;


// Data pasien = ifo + vital
#define MAX_PATIENTS 20

struct Patient {
  String id;
  String age;
  String gender;
  String status;
  String triage;
  bool   registered;

  bool          hasVital;
  int           hr;
  int           spo2;
  float         temp;
  int           rssi;
  uint32_t      packets;
  unsigned long lastSeen;

  uint8_t lvlHr;
  uint8_t lvlSpo2;
  uint8_t lvlTemp;
  uint8_t level;
  String  why;
  uint8_t critStreak;

  uint8_t       alertState;
  uint8_t       alertTries;
  unsigned long lastAlertTx;
};

Patient patients[MAX_PATIENTS];
int patientCount = 0;

unsigned long packetCount = 0;

WebServer server(80);

Preferences prefs;

// Rapih-rapih input
String sanitizeField(String s, size_t maxLen) {

  String out = "";

  for (size_t i = 0; i < s.length(); i++) {

    char c = s[i];

    if (c == '|' || c == '=' || c == '<' || c == '>' ||
        c == '&' || c == '\"' || c == '\'' || c == '\\' ||
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


void sendLoRa(const String &msg) {
  delay(50);

  LoRa.beginPacket();
  LoRa.print(msg);
  LoRa.endPacket();
}

// OLAHAN DATA PASIEN
void resetPatient(int idx, const String &id) {

  Patient &p = patients[idx];

  p.id         = id;
  p.age        = "";
  p.gender     = "";
  p.status     = "";
  p.triage     = "";
  p.registered = false;

  p.hasVital = false;
  p.hr       = 0;
  p.spo2     = 0;
  p.temp     = 0;
  p.rssi     = 0;
  p.packets  = 0;
  p.lastSeen = 0;

  p.lvlHr   = 0;
  p.lvlSpo2 = 0;
  p.lvlTemp = 0;
  p.level   = 0;
  p.why     = "";
  p.critStreak = 0;

  p.alertState  = 0;
  p.alertTries  = 0;
  p.lastAlertTx = 0;
}


// Hanya pasien yang sudah diregistrasi dari LOG yang disimpan
void savePatients() {

  String s = "";

  for (int i = 0; i < patientCount; i++) {

    if (!patients[i].registered) {
      continue;
    }

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

    int idx = patientCount++;

    resetPatient(idx, line.substring(0, p1));

    patients[idx].age        = line.substring(p1 + 1, p2);
    patients[idx].gender     = line.substring(p2 + 1, p3);
    patients[idx].status     = line.substring(p3 + 1, p4);
    patients[idx].triage     = line.substring(p4 + 1);
    patients[idx].registered = true;
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

int addPatient(const String &id) {

  if (patientCount < MAX_PATIENTS) {

    int idx = patientCount++;

    resetPatient(idx, id);

    return idx;
  }

  int victim = -1;

  for (int i = 0; i < patientCount; i++) {

    if (patients[i].registered) {
      continue;
    }

    if (victim < 0 || patients[i].lastSeen < patients[victim].lastSeen) {
      victim = i;
    }
  }

  if (victim < 0) {
    return -1;
  }

  resetPatient(victim, id);

  return victim;
}


// UPDATE update Patient
bool upsertPatient(const String &id, const String &age,
                   const String &gender, const String &status,
                   const String &triage) {

  int idx = findPatient(id);

  if (idx < 0) {
    idx = addPatient(id);
  }

  if (idx < 0) {
    return false;
  }

  patients[idx].registered = true;
  patients[idx].age        = age;
  patients[idx].gender     = gender;
  patients[idx].status     = status;
  patients[idx].triage     = triage;

  savePatients();

  return true;
}


void evaluateVitals(int idx) {

  Patient &p = patients[idx];

  String crit = "";
  String warn = "";

  auto add = [](String &list, const char *msg) {
    if (list.length() > 0) {
      list += "+";
    }
    list += msg;
  };

  p.lvlHr   = 0;
  p.lvlSpo2 = 0;
  p.lvlTemp = 0;

  if (p.hr <= 0) {
    p.lvlHr = 1;
    add(warn, "HR tidak terbaca");
  } else if (p.hr < HR_CRIT_LT) {
    p.lvlHr = 2;
    add(crit, "HR rendah");
  } else if (p.hr > HR_CRIT_GT) {
    p.lvlHr = 2;
    add(crit, "HR tinggi");
  } else if (p.hr < HR_WARN_LT) {
    p.lvlHr = 1;
    add(warn, "HR rendah");
  } else if (p.hr > HR_WARN_GT) {
    p.lvlHr = 1;
    add(warn, "HR tinggi");
  }

  if (p.spo2 <= 0) {
    p.lvlSpo2 = 1;
    add(warn, "SpO2 tidak terbaca");
  } else if (p.spo2 < SPO2_CRIT_LT) {
    p.lvlSpo2 = 2;
    add(crit, "SpO2 rendah");
  } else if (p.spo2 < SPO2_WARN_LT) {
    p.lvlSpo2 = 1;
    add(warn, "SpO2 rendah");
  }

  if (p.temp <= 0) {
    p.lvlTemp = 1;
    add(warn, "Suhu tidak terbaca");
  } else if (p.temp < TEMP_CRIT_LT) {
    p.lvlTemp = 2;
    add(crit, "Suhu rendah");
  } else if (p.temp > TEMP_CRIT_GT) {
    p.lvlTemp = 2;
    add(crit, "Suhu tinggi");
  } else if (p.temp < TEMP_WARN_LT) {
    p.lvlTemp = 1;
    add(warn, "Suhu rendah");
  } else if (p.temp > TEMP_WARN_GT) {
    p.lvlTemp = 1;
    add(warn, "Suhu tinggi");
  }

  uint8_t lv = p.lvlHr;

  if (p.lvlSpo2 > lv) {
    lv = p.lvlSpo2;
  }

  if (p.lvlTemp > lv) {
    lv = p.lvlTemp;
  }

  p.level = lv;
  p.why   = (lv == 2) ? crit : warn;
}

void sendAlert(int idx) {

  Patient &p = patients[idx];

  String packet =
    "ALERT|" + p.id +
    "|HR=" + String(p.hr) +
    "|SPO2=" + String(p.spo2) +
    "|TEMP=" + String(p.temp, 1) +
    "|WHY=" + p.why +
    "|AGE=" + (p.registered ? p.age : String("-")) +
    "|GENDER=" + (p.registered ? p.gender : String("-")) +
    "|STATUS=" + (p.registered ? p.status : String("-")) +
    "|TRIAGE=" + (p.registered ? p.triage : String("-"));

  p.alertTries++;
  p.lastAlertTx = millis();

  Serial.println();
  Serial.println("[LINK] EMERGENCY -> LOG (kirim ke-" + String(p.alertTries) + ")");
  Serial.println(packet);

  sendLoRa(packet);
}


// Ulangi ALERT yang belum dikonfirmasi LOG
void serviceAlerts() {

  unsigned long now = millis();

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    if (p.alertState != 1) {
      continue;
    }

    unsigned long wait =
      (p.alertTries < ALERT_FAST_TRIES) ? ALERT_RETRY_FAST_MS
                                        : ALERT_RETRY_SLOW_MS;

    if (now - p.lastAlertTx >= wait) {

      sendAlert(i);

      return;   // satu ALERT per putaran loop
    }
  }
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

void handleApiData() {

  unsigned long now = millis();

  String json;
  json.reserve(4096);

  json += "{\"uptime\":" + String(now / 1000);
  json += ",\"packets\":" + String(packetCount);
  json += ",\"patients\":[";

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    if (i > 0) {
      json += ",";
    }

    json += "{\"id\":\"" + p.id + "\"";
    json += ",\"reg\":" + String(p.registered ? "true" : "false");
    json += ",\"age\":\"" + p.age + "\"";
    json += ",\"gender\":\"" + p.gender + "\"";
    json += ",\"status\":\"" + p.status + "\"";
    json += ",\"triage\":\"" + p.triage + "\"";
    json += ",\"hasv\":" + String(p.hasVital ? "true" : "false");

    if (p.hasVital) {
      json += ",\"hr\":" + String(p.hr);
      json += ",\"spo2\":" + String(p.spo2);
      json += ",\"temp\":" + String(p.temp, 1);
      json += ",\"rssi\":" + String(p.rssi);
      json += ",\"ago\":" + String((now - p.lastSeen) / 1000);
      json += ",\"pkts\":" + String(p.packets);
    }

    json += ",\"lh\":" + String(p.lvlHr);
    json += ",\"ls\":" + String(p.lvlSpo2);
    json += ",\"lt\":" + String(p.lvlTemp);
    json += ",\"lv\":" + String(p.level);
    json += ",\"why\":\"" + p.why + "\"";
    json += ",\"alert\":" + String(p.alertState);
    json += "}";
  }

  json += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleRegistration(const String &packet, int rssi) {

  Serial.println();
  Serial.println("================================");
  Serial.println("[LINK] REGISTRASI PASIEN");
  Serial.println("================================");

  Serial.println("DATA : " + packet);

  Serial.println(
    "RSSI : " +
    String(rssi) +
    " dBm"
  );

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  String age    = sanitizeField(getField(packet, "AGE"), 16);
  String gender = sanitizeField(getField(packet, "GENDER"), 16);
  String status = sanitizeField(getField(packet, "STATUS"), 16);
  String triage = sanitizeField(getField(packet, "TRIAGE"), 16);

  bool valid =
    validSenseId(id) &&
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


// =====================================================
// DATA DARI SENSE
// =====================================================

void handleSense(const String &packet, int rssi) {

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  String sHr   = getField(packet, "HR");
  String sSpo2 = getField(packet, "SPO2");
  String sTemp = getField(packet, "TEMP");

  if (!validSenseId(id) ||
      sHr.length() == 0 || sSpo2.length() == 0 || sTemp.length() == 0) {

    Serial.println("[LINK] Paket SENSE tidak valid: " + packet);

    return;
  }

  int idx = findPatient(id);

  if (idx < 0) {
    idx = addPatient(id);
  }

  if (idx < 0) {

    Serial.println("[LINK] Daftar penuh, SENSE diabaikan: " + id);

    return;
  }

  Patient &p = patients[idx];

  p.hr       = sHr.toInt();
  p.spo2     = sSpo2.toInt();
  p.temp     = sTemp.toFloat();
  p.rssi     = rssi;
  p.lastSeen = millis();
  p.hasVital = true;
  p.packets++;

  packetCount++;

  evaluateVitals(idx);

  Serial.println(
    "[LINK] " + id +
    " HR=" + String(p.hr) +
    " SpO2=" + String(p.spo2) +
    " T=" + String(p.temp, 1) +
    " | " + (p.registered ? p.age + ", " + p.gender : String("belum terdaftar")) +
    " | level " + String(p.level) +
    " | RSSI " + String(rssi) + " dBm"
  );


  // ---------------------------------------------------
  // EMERGENCY KE LOG
  // ---------------------------------------------------
  if (p.level == 2) {

    if (p.critStreak < 255) {
      p.critStreak++;
    }

    if (p.critStreak >= ALERT_CONFIRM && p.alertState == 0) {

      p.alertState = 1;
      p.alertTries = 0;

      sendAlert(idx);
    }

  } else {
    p.critStreak = 0;
    p.alertState = 0;
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

  int rssi = LoRa.packetRssi();


  if (received.startsWith("REG|")) {

    handleRegistration(received, rssi);

    return;
  }

  if (received.startsWith("AACK|")) {

    int idx = findPatient(received.substring(5));

    if (idx >= 0 && patients[idx].alertState == 1) {

      patients[idx].alertState = 2;

      Serial.println("[LINK] LOG menerima ALERT " + patients[idx].id);
    }

    return;
  }

  if (received.startsWith("SENSE|")) {

    handleSense(received, rssi);

    return;
  }

  Serial.println("[LINK] Paket tidak dikenal: " + received);
}

void setup() {

  Serial.begin(115200);

  delay(1000);


  Serial.println();
  Serial.println("================================");
  Serial.println("        VITRA-LINK");
  Serial.println("================================");

  prefs.begin("vitra", false);

  loadPatients();

  Serial.println(
    "[NVS] Pasien tersimpan: " +
    String(patientCount)
  );

  SPI.begin(
    SPI_SCK,
    SPI_MISO,
    SPI_MOSI,
    LORA_SS
  );

  // LoRa Hell

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


  Serial.println(
    "[WiFi] SSID: " +
    String(AP_SSID)
  );

  Serial.println(
    "[WiFi] IP: " +
    WiFi.softAPIP().toString()
  );

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
  Serial.println("http://" + WiFi.softAPIP().toString());
  Serial.println("================================");
}

void loop() {
  server.handleClient();
  receiveLoRa();
  serviceAlerts();


  delay(5);
}
