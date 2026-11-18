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

// SF HARUS SAMA di Sense, Link, dan Log
#define LORA_SF 9

#include "vitra_radio.h"


// -----------------------------------------------------
// Konfigurasi
// -----------------------------------------------------

#define MAX_PATIENTS    60     // termasuk pasien simulasi
#define EMERG_RESEND_MS 4000   // kirim ulang EMERG ke LOG selagi belum di-EMACK
#define TX_QUEUE_SIZE   8
#define LINK_VERBOSE    1      // 1 = satu baris log per paket SENSE, 0 = hanya kejadian penting
#define LINK_TEST_TOOLS 1      // 1 = aktifkan /stats dan /api/sim (matikan untuk produksi)


// -----------------------------------------------------
// Data
// -----------------------------------------------------

WebServer server(80);

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
  int lastLevel = 0;
  bool done = false;         // sesi Sense selesai (pengiriman terjadwal terakhir bertanda LAST=1)

  int alert = 0; // 0 = tidak ada, 1 = terkirim menunggu EMACK, 2 = di-EMACK LOG
  unsigned long lastEmergMs = 0;

  int lastSeq = -1;          // nomor urut paket SENSE terakhir (untuk hitung paket hilang)

  bool sim = false;          // pasien simulasi (tidak disimpan ke NVS)
  uint8_t simClass = 0;
  uint32_t simNext = 0;
};

struct TxItem {
  String msg;
  uint8_t prio;
};

Patient patients[MAX_PATIENTS];
int patientCount = 0;

TxItem txq[TX_QUEUE_SIZE];
int txCount = 0;
uint32_t txDropped = 0;

Preferences prefs;

unsigned long bootMs = 0;
unsigned long packetCount = 0;      // semua paket SENSE yang diproses (RF + simulasi)

// Statistik untuk pengujian
uint32_t rfSense = 0, rfReg = 0, rfEmack = 0, rfOther = 0;
uint32_t rfSeqRx = 0, rfLost = 0;
uint32_t simPackets = 0;
uint32_t rfProcUsAvg = 0;
uint32_t apiCalls = 0, apiUsAvg = 0, apiUsMax = 0, apiBytesLast = 0;
uint32_t loopGapMaxUs = 0;

// Simulator
bool simOn = false;
uint32_t simMs = 1000;
bool simTx = false;


// Buang karakter yang bisa merusak format paket atau HTML
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


// =====================================================
// ANTRIAN KIRIM BERPRIORITAS
// Handler tidak pernah memancar langsung; mereka hanya mengantre,
// lalu loop() mengirim item berprioritas tertinggi lebih dulu.
// =====================================================

bool txHas(const String &prefix) {

  for (int i = 0; i < txCount; i++) {
    if (txq[i].msg.startsWith(prefix)) {
      return true;
    }
  }

  return false;
}


void txEnqueue(const String &msg, uint8_t prio) {

  if (txCount >= TX_QUEUE_SIZE) {

    // Penuh: buang item berprioritas terendah. Kalau yang baru tidak lebih penting, buang yang baru.
    int worst = 0;

    for (int i = 1; i < txCount; i++) {
      if (txq[i].prio >= txq[worst].prio) {
        worst = i;
      }
    }

    txDropped++;

    if (txq[worst].prio <= prio) {
      return;
    }

    for (int i = worst; i < txCount - 1; i++) {
      txq[i] = txq[i + 1];
    }

    txCount--;
  }

  txq[txCount].msg = msg;
  txq[txCount].prio = prio;
  txCount++;
}


void txPump() {

  if (txCount == 0) {
    return;
  }

  int best = 0;

  for (int i = 1; i < txCount; i++) {
    if (txq[i].prio < txq[best].prio) {
      best = i;
    }
  }

  TxItem item = txq[best];

  for (int i = best; i < txCount - 1; i++) {
    txq[i] = txq[i + 1];
  }

  txCount--;

  vitraSend(item.msg, item.prio);
}


// =====================================================
// DATA PASIEN (identitas registrasi -- persisten di NVS)
// =====================================================

void savePatients() {

  String s = "";

  for (int i = 0; i < patientCount; i++) {

    if (!patients[i].reg || patients[i].sim) continue;   // hanya pasien nyata yang teregistrasi

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


// Cari slot pasien, buat baru (belum teregistrasi) kalau belum ada
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
// (data vital yang sudah ada TIDAK dihapus)
bool regChanged = false;   // true kalau upsert terakhir menambah / mengubah data pasien

bool upsertPatient(const String &id, const String &age,
                   const String &gender, const String &status,
                   const String &triage) {

  int idx = findOrCreatePatient(id);

  if (idx < 0) {
    return false;
  }

  Patient &old = patients[idx];

  regChanged = !old.reg || old.age != age || old.gender != gender ||
               old.status != status || old.triage != triage;

  patients[idx].reg    = true;
  patients[idx].age    = age;
  patients[idx].gender = gender;
  patients[idx].status = status;
  patients[idx].triage = triage;

  savePatients();

  return true;
}


// =====================================================
// DARURAT -- kirim & kelola alert ke VITRA LOG
// =====================================================

void updateEmergencyState(int idx, int lv) {

  Patient &p = patients[idx];

  if (lv >= 2) {

    bool needSend = (p.alert == 0) ||
                    (p.alert == 1 && millis() - p.lastEmergMs >= EMERG_RESEND_MS);

    if (needSend) {

      VitalLevel v = vitraClassify(p.hr, p.spo2, p.temp);

      char why[48];
      vitraWhy(v, why, sizeof(why));

      for (char *c = why; *c; c++) {
        if (*c == ' ') *c = '_';
      }

      char pkt[120];
      snprintf(pkt, sizeof(pkt), "EMERG|%s|HR=%d|SPO2=%d|TEMP=%.1f|WHY=%s",
               p.id.c_str(), p.hr, p.spo2, (double)p.temp, why);

      // Jangan menumpuk EMERG yang sama di antrian
      if (!txHas("EMERG|" + p.id + "|")) {
        txEnqueue(String(pkt), PRIO_CRIT);
        Serial.println("[LINK] >>> DARURAT dijadwalkan ke LOG: " + String(pkt));
      }

      p.alert = 1;
      p.lastEmergMs = millis();
    }

  } else {
    // Pasien sudah tidak kritis lagi -> reset supaya episode berikutnya memicu alert baru
    if (p.alert != 0) {
      p.alert = 0;
    }
  }
}


void handleEmergencyAck(const String &packet) {
  // Format: EMACK|<id>
  String id = packet.substring(String("EMACK|").length());

  int idx = findPatient(id);

  if (idx < 0) {
    return;
  }

  patients[idx].alert = 2;

  Serial.println("[LINK] Darurat " + id + " sudah dikonfirmasi VITRA LOG");
}


// Data Sense kini jarang datang (tiap 15 menit), jadi pengulangan EMERG yang belum
// di-EMACK tidak boleh menunggu paket Sense berikutnya. Dipanggil dari loop().
void emergencyTick() {

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    if (p.sim || p.alert != 1 || !p.hasVitals) {
      continue;
    }

    updateEmergencyState(i, vitraClassify(p.hr, p.spo2, p.temp).lv);
  }
}


// =====================================================
// TERAPKAN DATA VITAL (dipakai paket RF maupun simulator)
// =====================================================

void applyVitals(int idx, int hr, int spo2, float temp, int rssi, int seq, bool allowTx) {

  Patient &p = patients[idx];

  // Hitung paket hilang dari celah nomor urut (modulo 256)
  if (seq >= 0) {

    if (p.lastSeq >= 0) {

      int d = (seq - p.lastSeq) & 0xFF;

      if (d > 1 && d < 128) {
        rfLost += d - 1;
      }
    }

    p.lastSeq = seq;
    rfSeqRx++;
  }

  p.hasVitals = true;
  p.hr = hr;
  p.spo2 = spo2;
  p.temp = temp;
  p.lastVitalsMs = millis();
  p.rssi = rssi;

  VitalLevel v = vitraClassify(hr, spo2, temp);

  if (v.lv != p.lastLevel) {
    Serial.printf("[LINK] %s level %d -> %d\n", p.id.c_str(), p.lastLevel, v.lv);
    p.lastLevel = v.lv;
  }

#if LINK_VERBOSE
  if (!p.sim) {
    Serial.printf("[SENSE] %-10s HR=%3d SpO2=%3d T=%4.1f L%d %4ddBm q=%d %s\n",
                  p.id.c_str(), hr, spo2, (double)temp, v.lv, rssi, seq,
                  p.reg ? "" : "(belum terdaftar)");
  }
#endif

  if (allowTx) {
    updateEmergencyState(idx, v.lv);
  }
}


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
.card.done{border-left-color:var(--pri);opacity:.85}
.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.pid{font-weight:700;font-size:18px}
.pill{font-size:11px;font-weight:700;padding:3px 9px;border-radius:99px;background:var(--ok);color:#06210f}
.pill.warn{background:var(--warn);color:#2b1d00}
.pill.crit{background:var(--crit);color:#fff}
.pill.off,.pill.wait{background:var(--muted);color:#0a121e}
.pill.done{background:var(--pri);color:#fff}
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
  <div class="chip"><b id="n_done">0</b><span>Selesai</span></div>
  <div class="chip"><b id="n_off">0</b><span>Offline / menunggu</span></div>
</div>

<div class="grid" id="grid"><div class="empty">Memuat&hellip;</div></div>

</div>

<script>
const $ = id => document.getElementById(id);
let OFFLINE_S = 40;   // diisi dari server (d.off)
const ESC = {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'};
const esc = s => String(s).replace(/[&<>"']/g, c => ESC[c]);
const ALERT_TXT = ['', 'menunggu konfirmasi LOG\u2026', 'diterima VITRA LOG \u2713'];
const RANK = {crit:0, warn:1, off:2, wait:3, ok:4, done:5};

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
  if (p.done) return ['done', 'SESI SELESAI'];
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
  const off = p.hasv && !p.done && p.ago > OFFLINE_S;

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
  OFFLINE_S = d.off;
  const ps = d.patients.slice().sort((a, b) =>
    RANK[state(a)[0]] - RANK[state(b)[0]] || a.id.localeCompare(b.id));

  const cnt = {crit:0, warn:0, ok:0, off:0, wait:0, done:0};
  ps.forEach(p => cnt[state(p)[0]]++);

  $('n_all').textContent  = ps.length;
  $('n_crit').textContent = cnt.crit;
  $('n_warn').textContent = cnt.warn;
  $('n_ok').textContent   = cnt.ok;
  $('n_off').textContent  = cnt.off + cnt.wait;
  $('n_done').textContent = cnt.done;
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

#if LINK_TEST_TOOLS
const char STATS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VITRA LINK - Stats</title>
<style>
body{margin:0 auto;background:#0a121e;color:#f5f5f5;font:14px/1.4 system-ui,sans-serif;padding:16px;max-width:1000px}
h1{font-size:20px;margin:0 0 12px}
h2{font-size:12px;color:#8a96ab;text-transform:uppercase;letter-spacing:.5px;margin:18px 0 6px}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px}
.c{background:#192332;border:1px solid #26344a;border-radius:8px;padding:8px 12px}
.c b{display:block;font-size:20px}
.c span{color:#8a96ab;font-size:11px;text-transform:uppercase}
table{width:100%;border-collapse:collapse}
td,th{padding:4px 8px;border-bottom:1px solid #26344a;text-align:right}
th:first-child,td:first-child{text-align:left}
label{margin-right:12px;color:#8a96ab}
input{width:70px;background:#101a29;color:#fff;border:1px solid #26344a;border-radius:6px;padding:4px}
button{background:#1e8cf0;color:#fff;border:0;border-radius:6px;padding:6px 14px;cursor:pointer;margin-right:6px}
button.s{background:#f04646}
a{color:#1e8cf0;font-size:13px}
</style></head><body>
<h1>VITRA LINK &middot; Stats &amp; Uji Beban <a href="/">&larr; dashboard</a></h1>

<h2>Simulasi pasien (tanpa perangkat Sense)</h2>
<div>
<label>Pasien <input id="n" type="number" value="20"></label>
<label>Interval ms <input id="ms" type="number" value="1000"></label>
<label>Kritis <input id="cr" type="number" value="2"></label>
<label>Waspada <input id="wr" type="number" value="3"></label>
<label><input id="tx" type="checkbox" style="width:auto"> kirim EMERG via LoRa</label>
<br><br>
<button onclick="sim(1)">Mulai</button><button class="s" onclick="sim(0)">Stop</button>
<label>Viewer paralel <input id="vw" type="number" value="1" onchange="setViewers()"></label>
</div>

<h2>Latensi web (diukur di browser ini)</h2><div class="g" id="lat"></div>
<h2>Kondisi Link</h2><div class="g" id="st"></div>
<h2>Akses kanal LoRa per prioritas (Link sebagai pemancar)</h2><table id="tx2"></table>

<script>
const $=id=>document.getElementById(id);
const cell=(l,v)=>'<div class="c"><b>'+v+'</b><span>'+l+'</span></div>';
let rtt=[],viewers=[],stale=0,bytes=0,errs=0;

function stat(a){
  if(!a.length)return[0,0,0,0];
  const s=a.slice().sort((x,y)=>x-y);
  return[a[a.length-1],a.reduce((x,y)=>x+y,0)/a.length,s[Math.floor(s.length*.95)],s[s.length-1]];
}

async function pollData(){
  const t=performance.now();
  try{
    const r=await fetch('/api/data',{cache:'no-store'});
    const tx=await r.text();
    const d=JSON.parse(tx);
    rtt.push(performance.now()-t);
    if(rtt.length>200)rtt.shift();
    bytes=tx.length;
    stale=Math.max(0,...d.patients.filter(p=>p.hasv).map(p=>p.ago));
  }catch(e){errs++;}
}

function setViewers(){
  viewers.forEach(clearInterval);viewers=[];
  const k=Math.max(1,Math.min(20,+$('vw').value||1));
  for(let i=0;i<k;i++)viewers.push(setInterval(pollData,1000));
}

async function sim(on){
  const q=on?('n='+$('n').value+'&ms='+$('ms').value+'&crit='+$('cr').value+'&warn='+$('wr').value+'&tx='+($('tx').checked?1:0)):'stop=1';
  await fetch('/api/sim?'+q);
}

async function pollStats(){
  try{
    const s=await(await fetch('/api/stats',{cache:'no-store'})).json();
    const [l,a,p,m]=stat(rtt);
    $('lat').innerHTML=
      cell('RTT terakhir',l.toFixed(0)+' ms')+cell('RTT rata-rata',a.toFixed(0)+' ms')+
      cell('RTT p95',p.toFixed(0)+' ms')+cell('RTT maks',m.toFixed(0)+' ms')+
      cell('Ukuran /api/data',(bytes/1024).toFixed(1)+' KB')+
      cell('Data tertua',stale+' dtk')+cell('Fetch gagal',errs)+
      cell('Bangun JSON rata2',(s.api.usAvg/1000).toFixed(1)+' ms')+
      cell('Bangun JSON maks',(s.api.usMax/1000).toFixed(1)+' ms');
    const tot=s.rf.seqRx+s.rf.lost;
    $('st').innerHTML=
      cell('Uptime',s.uptime+' dtk')+cell('Heap bebas',(s.heap/1024).toFixed(0)+' KB')+
      cell('Heap min',(s.heapMin/1024).toFixed(0)+' KB')+cell('Klien WiFi',s.clients)+
      cell('Pasien (sim)',s.patients+' ('+s.sim+')')+
      cell('Paket SENSE RF',s.rf.sense)+cell('REG',s.rf.reg)+cell('EMACK',s.rf.emack)+
      cell('Paket simulasi',s.simPackets)+
      cell('Paket hilang (RF)',tot?(100*s.rf.lost/tot).toFixed(1)+' %':'-')+
      cell('Proses paket RF',s.proc.usAvg+' us')+
      cell('Loop macet maks',(s.loopGapMaxUs/1000).toFixed(0)+' ms')+
      cell('Antrian TX',s.tx.queue+' / drop '+s.tx.dropped);
    const nm=['CRIT','WARN','REG','NORMAL'];
    let h='<tr><th>Kelas</th><th>Terkirim</th><th>Ditunda</th><th>Dipaksa</th><th>Tunggu rata2</th><th>Tunggu maks</th></tr>';
    for(let i=0;i<4;i++)h+='<tr><td>'+nm[i]+'</td><td>'+s.tx.sent[i]+'</td><td>'+s.tx.deferred[i]+'</td><td>'+s.tx.forced[i]+'</td><td>'+s.tx.waitAvg[i]+' ms</td><td>'+s.tx.waitMax[i]+' ms</td></tr>';
    $('tx2').innerHTML=h;
  }catch(e){}
}

setViewers();
setInterval(pollStats,2000);
pollStats();
</script></body></html>
)rawliteral";
#endif


void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatsPage() {
#if LINK_TEST_TOOLS
  server.send_P(200, "text/html", STATS_HTML);
#else
  server.send(404, "text/plain", "off");
#endif
}


// =====================================================
// API DATA -- dialirkan per potongan (tanpa String raksasa)
// =====================================================

static char outBuf[1400];
static size_t outLen = 0;
static uint32_t outTotal = 0;

void outFlush() {

  if (outLen == 0) {
    return;
  }

  outBuf[outLen] = 0;
  server.sendContent(outBuf);
  outTotal += outLen;
  outLen = 0;
}

void outAdd(const char *s, size_t n) {

  if (n >= sizeof(outBuf) - 1) {
    return;
  }

  if (outLen + n >= sizeof(outBuf) - 1) {
    outFlush();
  }

  memcpy(outBuf + outLen, s, n);
  outLen += n;
}

void handleApiData() {

  uint32_t t0 = micros();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  outLen = 0;
  outTotal = 0;

  char buf[512];

  int n = snprintf(buf, sizeof(buf), "{\"packets\":%lu,\"uptime\":%lu,\"off\":%lu,\"patients\":[",
                   (unsigned long)packetCount, (unsigned long)((millis() - bootMs) / 1000),
                   (unsigned long)VITRA_OFFLINE_S);
  outAdd(buf, n);

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    VitalLevel v = vitraClassify(p.hr, p.spo2, p.temp);

    if (!p.hasVitals) {
      v.lh = v.ls = v.lt = v.lv = 0;
    }

    char why[48];
    vitraWhy(v, why, sizeof(why));

    long ago = p.hasVitals ? (long)((millis() - p.lastVitalsMs) / 1000) : 0;

    n = snprintf(buf, sizeof(buf),
      "%s{\"id\":\"%s\",\"reg\":%s,\"age\":\"%s\",\"gender\":\"%s\",\"status\":\"%s\",\"triage\":\"%s\","
      "\"hasv\":%s,\"hr\":%d,\"spo2\":%d,\"temp\":%.1f,\"lh\":%d,\"ls\":%d,\"lt\":%d,\"lv\":%d,"
      "\"why\":\"%s\",\"ago\":%ld,\"rssi\":%d,\"alert\":%d,\"done\":%s}",
      i > 0 ? "," : "",
      p.id.c_str(), p.reg ? "true" : "false",
      p.age.c_str(), p.gender.c_str(), p.status.c_str(), p.triage.c_str(),
      p.hasVitals ? "true" : "false", p.hr, p.spo2, (double)p.temp,
      v.lh, v.ls, v.lt, v.lv, why, ago, p.rssi, p.alert, p.done ? "true" : "false");

    if (n > 0 && n < (int)sizeof(buf)) {
      outAdd(buf, n);
    }
  }

  outAdd("]}", 2);
  outFlush();
  server.sendContent("");      // akhiri respons chunked

  uint32_t dt = micros() - t0;

  apiCalls++;
  apiUsAvg = (apiUsAvg * 7 + dt) / 8;
  if (dt > apiUsMax) apiUsMax = dt;
  apiBytesLast = outTotal;
}


// =====================================================
// STATISTIK + SIMULATOR (alat uji)
// =====================================================

int simCount() {

  int c = 0;

  for (int i = 0; i < patientCount; i++) {
    if (patients[i].sim) c++;
  }

  return c;
}


void simStop() {

  int j = 0;

  for (int i = 0; i < patientCount; i++) {

    if (!patients[i].sim) {

      if (i != j) {
        patients[j] = patients[i];
      }

      j++;
    }
  }

  for (int k = j; k < patientCount; k++) {
    patients[k] = Patient();
  }

  patientCount = j;
  simOn = false;
}


void simStart(int n, uint32_t ms, int crit, int warn, bool tx) {

  static const char *AGES[]   = { "Anak-anak", "Remaja", "Dewasa", "Lansia" };
  static const char *GENDERS[] = { "Pria", "Wanita" };
  static const char *STATUSES[] = { "Sadar", "Bingung", "Tidak respon" };
  static const char *TRIAGES[] = { "Hijau", "Kuning" };

  simStop();

  if (n < 1) n = 1;

  int room = MAX_PATIENTS - patientCount;

  if (n > room) n = room;

  if (ms < 100) ms = 100;

  for (int i = 0; i < n; i++) {

    int idx = patientCount++;

    patients[idx] = Patient();

    char id[16];
    snprintf(id, sizeof(id), "SIM-%03d", i + 1);

    Patient &p = patients[idx];

    p.id = id;
    p.sim = true;
    p.reg = true;
    p.age = AGES[random(4)];
    p.gender = GENDERS[random(2)];
    p.status = STATUSES[random(3)];
    p.triage = TRIAGES[random(2)];
    p.simClass = (i < crit) ? 2 : (i < crit + warn) ? 1 : 0;
    p.simNext = millis() + random((long)ms);
  }

  simMs = ms;
  simTx = tx;
  simOn = true;

  Serial.println("[SIM] Mulai: " + String(n) + " pasien, tiap " + String(ms) +
                 " ms, kritis=" + String(crit) + ", waspada=" + String(warn) +
                 ", tx=" + String(tx ? "ya" : "tidak"));
}


void simTick() {

  if (!simOn) {
    return;
  }

  uint32_t now = millis();

  for (int i = 0; i < patientCount; i++) {

    Patient &p = patients[i];

    if (!p.sim || (int32_t)(now - p.simNext) < 0) {
      continue;
    }

    int hr, spo2;
    float temp;

    vitraSimValues(p.simClass, hr, spo2, temp);

    p.simNext += simMs;

    if ((int32_t)(now - p.simNext) >= 0) {
      p.simNext = now + simMs;     // ketinggalan jauh: sinkron ulang
    }

    simPackets++;
    packetCount++;

    applyVitals(i, hr, spo2, temp, -60, -1, simTx);
  }
}


void handleApiSim() {

  if (server.hasArg("stop")) {

    simStop();

  } else {

    int n    = server.hasArg("n")    ? server.arg("n").toInt()    : 20;
    int ms   = server.hasArg("ms")   ? server.arg("ms").toInt()   : 1000;
    int crit = server.hasArg("crit") ? server.arg("crit").toInt() : 0;
    int warn = server.hasArg("warn") ? server.arg("warn").toInt() : 0;
    int tx   = server.hasArg("tx")   ? server.arg("tx").toInt()   : 0;

    simStart(n, (uint32_t)(ms < 0 ? 0 : ms), crit, warn, tx != 0);
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"sim\":%d}", simCount());
  server.send(200, "application/json", buf);
}


void handleApiStats() {

  char buf[1300];

  uint32_t avg[4];

  for (int i = 0; i < 4; i++) {
    avg[i] = vitraTx.sent[i] ? vitraTx.waitSum[i] / vitraTx.sent[i] : 0;
  }

  snprintf(buf, sizeof(buf),
    "{\"uptime\":%lu,\"heap\":%lu,\"heapMin\":%lu,\"clients\":%d,\"patients\":%d,\"sim\":%d,"
    "\"simPackets\":%lu,"
    "\"rf\":{\"sense\":%lu,\"reg\":%lu,\"emack\":%lu,\"other\":%lu,\"lost\":%lu,\"seqRx\":%lu},"
    "\"api\":{\"calls\":%lu,\"usAvg\":%lu,\"usMax\":%lu,\"bytes\":%lu},"
    "\"proc\":{\"usAvg\":%lu},\"loopGapMaxUs\":%lu,"
    "\"tx\":{\"queue\":%d,\"dropped\":%lu,\"airMs\":%lu,"
    "\"sent\":[%lu,%lu,%lu,%lu],\"deferred\":[%lu,%lu,%lu,%lu],\"forced\":[%lu,%lu,%lu,%lu],"
    "\"waitAvg\":[%lu,%lu,%lu,%lu],\"waitMax\":[%lu,%lu,%lu,%lu]}}",
    (unsigned long)((millis() - bootMs) / 1000),
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
    (int)WiFi.softAPgetStationNum(), patientCount, simCount(),
    (unsigned long)simPackets,
    (unsigned long)rfSense, (unsigned long)rfReg, (unsigned long)rfEmack, (unsigned long)rfOther,
    (unsigned long)rfLost, (unsigned long)rfSeqRx,
    (unsigned long)apiCalls, (unsigned long)apiUsAvg, (unsigned long)apiUsMax, (unsigned long)apiBytesLast,
    (unsigned long)rfProcUsAvg, (unsigned long)loopGapMaxUs,
    txCount, (unsigned long)txDropped, (unsigned long)vitraTx.airMs,
    (unsigned long)vitraTx.sent[0], (unsigned long)vitraTx.sent[1], (unsigned long)vitraTx.sent[2], (unsigned long)vitraTx.sent[3],
    (unsigned long)vitraTx.deferred[0], (unsigned long)vitraTx.deferred[1], (unsigned long)vitraTx.deferred[2], (unsigned long)vitraTx.deferred[3],
    (unsigned long)vitraTx.forced[0], (unsigned long)vitraTx.forced[1], (unsigned long)vitraTx.forced[2], (unsigned long)vitraTx.forced[3],
    (unsigned long)avg[0], (unsigned long)avg[1], (unsigned long)avg[2], (unsigned long)avg[3],
    (unsigned long)vitraTx.waitMax[0], (unsigned long)vitraTx.waitMax[1], (unsigned long)vitraTx.waitMax[2], (unsigned long)vitraTx.waitMax[3]);

  loopGapMaxUs = 0;   // jendela pengukuran baru

  server.send(200, "application/json", buf);
}


// =====================================================
// REGISTRASI PASIEN DARI LOG
// Format: REG|VITRA-001|AGE=Dewasa|GENDER=Pria|STATUS=Sadar|TRIAGE=Hijau
// =====================================================

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
    txEnqueue("NACK|" + id + "|INVALID", PRIO_REG);
    Serial.println("================================");
    return;
  }

  if (!upsertPatient(id, age, gender, status, triage)) {
    Serial.println("[LINK] Penyimpanan penuh");
    txEnqueue("NACK|" + id + "|FULL", PRIO_REG);
    Serial.println("================================");
    return;
  }

  Serial.println(
    "[LINK] Tersimpan: " + id + " - " + age + ", " +
    gender + ", " + status + ", " + triage
  );

  txEnqueue("ACK|" + id, PRIO_REG);

  Serial.println("[LINK] ACK dijadwalkan ke LOG");

  // Data pasien baru/berubah -> SENSE mengulang pengukuran dari awal
  if (regChanged) {

    int pi = findPatient(id);

    if (pi >= 0) {
      patients[pi].done = false;
    }

    if (!txHas("RESTART|" + id)) {
      txEnqueue("RESTART|" + id, PRIO_REG);
    }

    Serial.println("[LINK] Data pasien berubah -> RESTART dijadwalkan ke SENSE");
  }
  Serial.println("================================");
}


// =====================================================
// DATA VITAL DARI SENSE
// Format: SENSE|VITRA-001|HR=80|SPO2=98|TEMP=36.8|Q=17
// =====================================================

void handleSenseData(const String &packet, int rssi) {

  int a = packet.indexOf('|');
  int b = packet.indexOf('|', a + 1);

  String id = (b > a) ? packet.substring(a + 1, b) : "";

  if (id.length() == 0 || id.length() > 20 || sanitizeField(id, 20) != id) {
    return;
  }

  int idx = findOrCreatePatient(id);

  if (idx < 0) {
    Serial.println("[LINK] Data SENSE ditolak, penyimpanan penuh: " + id);
    return;
  }

  String q = getField(packet, "Q");

  applyVitals(idx,
              getField(packet, "HR").toInt(),
              getField(packet, "SPO2").toInt(),
              getField(packet, "TEMP").length() ? getField(packet, "TEMP").toFloat() : 36.6f,  // Sense asli tanpa sensor suhu -> netral
              rssi,
              q.length() ? (int)q.toInt() : -1,
              true);

  // Pengiriman terjadwal terakhir sesi bertanda LAST=1 -> sesi selesai
  patients[idx].done = (getField(packet, "LAST") == "1");

  // Pembacaan waspada/kritis (A=1) minta konfirmasi; Sense mengulang sampai WACK diterima
  if (getField(packet, "A") == "1" && !txHas("WACK|" + id)) {
    txEnqueue("WACK|" + id, PRIO_CRIT);
  }
}


// =====================================================
// PAKET LoRa MASUK (dipanggil oleh vitraPoll)
// =====================================================

void onLoRaPacket(const String &received, int rssi) {

  uint32_t t0 = micros();

  if (received.startsWith("SENSE|")) {

    rfSense++;
    packetCount++;
    handleSenseData(received, rssi);

  } else if (received.startsWith("REG|")) {

    rfReg++;
    handleRegistration(received, rssi);

  } else if (received.startsWith("EMACK|")) {

    rfEmack++;
    handleEmergencyAck(received);

  } else {

    rfOther++;

    Serial.println(
      "[RX] Paket tidak dikenal: " + received +
      " | RSSI " + String(rssi) + " dBm"
    );
  }

  rfProcUsAvg = (rfProcUsAvg * 7 + (micros() - t0)) / 8;
}


// Dipanggil dari dalam vitraSend() selagi menunggu giliran, supaya web tetap responsif
void linkIdle() {
  server.handleClient();
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

  vitraRadioConfig();

  vitraRxHandler = onLoRaPacket;
  vitraIdleHook = linkIdle;

  Serial.println("[LoRa] OK (SF" + String(LORA_SF) + ")");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.println("[WiFi] SSID: " + String(AP_SSID));
  Serial.println("[WiFi] IP: " + WiFi.softAPIP().toString());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);

#if LINK_TEST_TOOLS
  server.on("/stats", HTTP_GET, handleStatsPage);
  server.on("/api/stats", HTTP_GET, handleApiStats);
  server.on("/api/sim", HTTP_GET, handleApiSim);
#endif

  server.begin();

  Serial.println();
  Serial.println("================================");
  Serial.println("VITRA-LINK SIAP");
  Serial.println("Dashboard : http://" + WiFi.softAPIP().toString());
#if LINK_TEST_TOOLS
  Serial.println("Uji beban : http://" + WiFi.softAPIP().toString() + "/stats");
#endif
  Serial.println("================================");
}


void loop() {

  static uint32_t lastLoopUs = 0;

  uint32_t nowUs = micros();

  if (lastLoopUs != 0) {
    uint32_t gap = nowUs - lastLoopUs;
    if (gap > loopGapMaxUs) loopGapMaxUs = gap;
  }

  lastLoopUs = nowUs;

  vitraPoll();      // baca paket RF yang sudah masuk
  emergencyTick();  // ulangi EMERG yang belum di-EMACK
  txPump();         // kirim item antrian berprioritas tertinggi
  simTick();        // pasien simulasi (kalau aktif)
  server.handleClient();

  delay(1);
}
