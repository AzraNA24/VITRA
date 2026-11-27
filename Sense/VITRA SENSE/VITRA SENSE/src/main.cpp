#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <U8g2lib.h>
#include <Adafruit_MCP23X17.h>
#include "ppg_view.h"
#include "vitra_lora.h"
// =====================================================
// VITRA-SENSE
// ESP32-C3 SuperMini + MCP23017 + OLED SH1106
// =====================================================
// -------------------- PIN ESP32 --------------------
constexpr uint8_t PIN_LED_EMERGENCY = 1;
constexpr uint8_t PIN_LED_RED       = 3;
constexpr uint8_t PIN_LED_GREEN     = 4;
constexpr uint8_t PIN_EMERGENCY     = 5;
constexpr uint8_t PIN_BUZZER        = 10;
constexpr uint8_t PIN_SDA           = 20;
constexpr uint8_t PIN_SCL           = 21;
// -------------------- PIN MCP23017 --------------------
constexpr uint8_t MCP_BUTTON_1 = 5; // GPA5
constexpr uint8_t MCP_BUTTON_2 = 4; // GPA4
constexpr uint8_t MCP_ADDRESS  = 0x20;
constexpr uint8_t OLED_ADDRESS = 0x3C;
// Node ID tidak ditampilkan di OLED.
// Nilai ini tetap dikirim bersama paket data.
constexpr uint16_t NODE_ID = 1;
// -------------------- KONFIGURASI WAKTU --------------------
constexpr unsigned long DEBOUNCE_MS        = 45;
constexpr unsigned long BUTTON1_HOLD_MS    = 2500;
constexpr unsigned long BUTTON2_HOLD_MS    = 800;
constexpr unsigned long EMERGENCY_HOLD_MS  = 2000;
constexpr unsigned long SCREEN_TIMEOUT_MS  = 120000;
constexpr unsigned long DEMO_INTERVAL_MS   = 8000;
constexpr unsigned long BUZZER_INTERVAL_MS = 300;
constexpr unsigned long LED_BLINK_MS       = 300;
// Beban bus I2C dikurangi karena OLED, MCP23017, dan MPU6050
// menggunakan jalur SDA/SCL yang sama.
constexpr unsigned long OLED_REFRESH_MS    = 200;
constexpr unsigned long MCP_POLL_MS        = 20;
constexpr unsigned long MPU_STATUS_INTERVAL_MS = 1000;
// HR dan SpO2 ditampilkan setelah fase stabilisasi awal 10 detik.
constexpr unsigned long PPG_STABILIZATION_MS = 10000;
// RR baru boleh ditampilkan paling cepat 90 detik sejak jari terdeteksi.
// Waktu tunggu ini bukan jaminan hasil RR akurat: sensor harus memberi nilai valid.
constexpr unsigned long RR_STABILIZATION_MS  = 90000;
constexpr unsigned long ANIMATION_MS       = 120;
constexpr uint8_t BUZZER_ON  = HIGH;
constexpr uint8_t BUZZER_OFF = LOW;
// =====================================================
// PERANGKAT
// =====================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);
Adafruit_MCP23X17 mcp;
bool oledTerdeteksi = false;
bool mcpTerdeteksi  = false;
bool mpuTerdeteksi  = false;
bool ppgTerdeteksi  = false;
// =====================================================
// STATUS TRIASE
// Nilai semakin besar berarti kondisi semakin buruk.
// =====================================================
enum Triase : uint8_t {
  HIJAU  = 0,
  KUNING = 1,
  MERAH  = 2,
  BELUM  = 3
};
Triase triaseAsli = BELUM;
Triase triaseRe   = BELUM;
Triase pilihan           = MERAH;
Triase pilihanSebelumnya = MERAH;
bool sedangMemilih = true;
bool animasiAktif  = false;
unsigned long mulaiAnimasi = 0;
// =====================================================
// DATA SENSOR DUMMY
// =====================================================
int spo2 = 0;
int hr   = 0;
int rr   = 0;
// dataSensorValid menyatakan HR dan SpO2 sudah terbaca.
// rrTersedia terpisah karena RR membutuhkan waktu tunggu lebih panjang.
bool dataSensorValid = false;
bool rrTersedia = false;
bool jariPPGSebelumnya = false;
bool ppgSedangStabilisasi = false;
// Hasil PPG baru yang sudah diambil oleh layaniSensorSaatLoRa() selagi radio
// menunggu kanal kosong; dibaca oleh perbaruiDataPPG().
bool ppgHasilTertunda = false;
unsigned long mulaiStabilisasiPPG = 0;
bool gerakanDarurat  = false;
bool emergencyAktif  = false;
// =====================================================
// STATUS OUTPUT
// =====================================================
bool layarAktif        = true;
bool buzzerMenyala     = false;
bool ledEmergencyNyala = true;
// =====================================================
// TIMER
// =====================================================
unsigned long terakhirInteraksi      = 0;
unsigned long terakhirDemo           = 0;
unsigned long terakhirBunyi          = 0;
unsigned long terakhirKedipEmergency = 0;
unsigned long terakhirGambar         = 0;
unsigned long terakhirBacaMCP        = 0;
unsigned long terakhirStatusMPU      = 0;
unsigned long terakhirDataPPG        = 0;
unsigned long terakhirBacaMPUBerhasil = 0;
unsigned long terakhirCobaMPU         = 0;
float magnitudeMPUTerakhir = 0.0f;
bool dataMPUTerakhirValid = false;
uint32_t jumlahBacaMPUBerhasil = 0;
uint32_t jumlahBacaMPUGagal = 0;
uint16_t gagalMPUBerturut = 0;
uint32_t nomorPaket = 0;
// Hasil pembacaan seluruh GPIO MCP23017.
uint16_t keadaanMCP = 0xFFFF;
// =====================================================
// SISTEM TOMBOL
// =====================================================
enum AksiTombol : uint8_t {
  TIDAK_ADA,
  DITEKAN,
  KLIK,
  TAHAN
};
struct StatusTombol {
  bool bacaanTerakhir;
  bool keadaanStabil;
  bool tahanSudahDikirim;
  bool izinMatikanOLED;
  unsigned long waktuPerubahan;
  unsigned long mulaiDitekan;
};
StatusTombol tombol1 = {
  HIGH,
  HIGH,
  false,
  false,
  0,
  0
};
StatusTombol tombol2 = {
  HIGH,
  HIGH,
  false,
  false,
  0,
  0
};
StatusTombol tombolEmergency = {
  HIGH,
  HIGH,
  false,
  false,
  0,
  0
};
// =====================================================
// PROTOTIPE FUNGSI
// =====================================================
bool perangkatI2CAda(uint8_t alamat);
AksiTombol perbaruiTombol(
  StatusTombol &tombol,
  bool bacaan,
  unsigned long sekarang,
  unsigned long batasTahan
);
const char *namaTriase(Triase nilai);
Triase pilihanBerikutnya(Triase nilai);
void nyalakanLayar();
void matikanLayar();
void perbaruiRGB();
void perbaruiBuzzer(unsigned long sekarang);
void perbaruiLedEmergency(unsigned long sekarang);
void perbaruiDataPPG(unsigned long sekarang);
Triase hitungTriasePPG(int nilaiSpO2, int nilaiHR, int nilaiRR);
void kirimPaketUji(const char *event);
void laporkanStatusMPU(unsigned long sekarang);
void gambarHeader(const char *judul);
void gambarAlarm();
void gambarKotakTriase(const char *teks);
void gambarLabelTombol(int x, int y, const char *label, bool terisi);
void gambarPilihan(unsigned long sekarang);
void gambarLayarPilih(unsigned long sekarang);
void gambarTeksTengah(const char *teks, int y);
void gambarLayarPantau();
void gambarOLED(unsigned long sekarang);
void tampilkanStatusI2C();
void sampelMPU();
void layaniSensorSaatLoRa();
// =====================================================
// MPU6050 FALL DETECTION INTEGRATION
// =====================================================
/*
===========================================================
 VITRA-Sense FALL DETECTION
 ESP32-C3 + MPU6050
===========================================================
 MAIN REFERENCE:
 Khojasteh et al., 2018
 "Improving Fall Detection Using an On-Wrist
 Wearable Accelerometer"
 CURRENT LOGIC:
 1. Peak candidate:
      ACC > 3.0929 g
 2. Peak isolation:
      Dalam 2500 ms setelah candidate,
      tidak boleh ada lagi ACC > 3.0929 g
 3. SCI:
      Mengukur aktivitas berulang sebelum peak.
 4. Temporal fall sequence:
      ACC <= 0.8 g
              ↓
      ACC >= 1.5 g
              ↓
      PEAK >= 3.0929 g
 5. Decision:
      PEAK besar
      + tidak sedang aktivitas berulang
      + sequence fall ditemukan
      = FALL
===========================================================
*/
// =========================================================
// MPU6050
// =========================================================
#define MPU_ADDR 0x68
#define SDA_PIN 20
#define SCL_PIN 21
#define REG_WHO_AM_I      0x75
#define REG_PWR_MGMT_1    0x6B
#define REG_CONFIG        0x1A
#define REG_SMPLRT_DIV    0x19
#define REG_ACCEL_CONFIG  0x1C
#define REG_GYRO_CONFIG   0x1B
#define REG_ACCEL_XOUT_H  0x3B
// =========================================================
// ACCELEROMETER
// =========================================================
// MPU6050 ±8g
#define ACCEL_SCALE 4096.0f
// =========================================================
// SAMPLING
// =========================================================
#define SAMPLE_RATE_HZ 100
#define SAMPLE_INTERVAL_US (1000000UL / SAMPLE_RATE_HZ)
// =========================================================
// FALL DETECTION THRESHOLDS
// =========================================================
// Optimized peak threshold from Khojasteh et al.
#define PEAK_THRESHOLD_G 2.0f
// Impact threshold
#define IMPACT_THRESHOLD_G 0.1f
// Low acceleration / free fall threshold
#define FREEFALL_THRESHOLD_G 0.80f
// Peak duration threshold
#define PEAK_DURATION_G 1.80f
// ARI
#define ARI_LOW_G 0.85f
#define ARI_HIGH_G 1.30f
// =========================================================
// TIME WINDOWS
// =========================================================
#define PEAK_LOOKBACK_MS 2500
#define IMPACT_START_WINDOW_MS 1200
#define IMPACT_END_FALLBACK_MS 1000
#define MVI_LOOKBACK_MS 500
#define FFI_LOOKBACK_MS 200
#define ARI_WINDOW_MS 700
#define SCI_WINDOW_MS 2200
// =========================================================
// SCI IMPLEMENTATION
//
// Based on the related 2018 discussion paper:
//
// valley:
//   ACC < 1.0g
//   for at least 80ms
//
// followed by:
//   ACC > 1.6g
//   within next 200ms
// =========================================================
#define SCI_LOW_G 1.0f
#define SCI_HIGH_G 1.6f
#define SCI_LOW_DURATION_MS 80
#define SCI_HIGH_FOLLOW_MS 200
// =========================================================
// SCI DECISION THRESHOLD
//
// IMPORTANT:
// This is OUR experimental decision rule,
// NOT an explicit threshold from the paper.
//
// SCI >= 3:
// considered repeated activity / exercise.
//
// This value can later be tuned from experiments.
// =========================================================
#define SCI_EXERCISE_THRESHOLD 3
// =========================================================
// BUFFER
//
// 100 Hz:
//
// 600 samples = 6000 ms = 6 seconds
//
// Needed because:
// 2500 ms waiting
// +
// 2200 ms SCI history
// +
// safety margin
// =========================================================
#define BUFFER_SIZE 600
#define MIN_SAMPLES_FOR_ANALYSIS 480
// =========================================================
// DATA STRUCTURE
// =========================================================
struct Sample
{
  uint32_t timeMs;
  float acc;
};
Sample buffer[BUFFER_SIZE];
int bufferHead = 0;
int sampleCount = 0;
// =========================================================
// FEATURE STRUCTURE
// =========================================================
struct FallFeatures
{
  float AAMV;
  float IDI;
  float MPI;
  float MVI;
  float PDI;
  float ARI;
  float FFI;
  float SCI;
  uint32_t peakTime;
  uint32_t impactStartTime;
  uint32_t impactEndTime;
  bool impactSequenceFound;
};
// =========================================================
// GLOBAL
// =========================================================
uint32_t lastSampleUs = 0;
uint32_t lastProcessedCandidateTime = 0;
uint32_t lastDetectedPeakTime = 0;
// =========================================================
// MPU6050 LOW LEVEL
// =========================================================
bool writeRegister(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return (Wire.endTransmission() == 0);
}
bool readRegisters(
  uint8_t reg,
  uint8_t *data,
  uint8_t length
)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }
  uint8_t received =
    Wire.requestFrom(
      (int)MPU_ADDR,
      (int)length,
      (int)true
    );
  if (received != length)
  {
    return false;
  }
  for (int i = 0; i < length; i++)
  {
    data[i] = Wire.read();
  }
  return true;
}
// =========================================================
// MPU6050 INIT
// =========================================================
bool initMPU6050()
{
  delay(100);
  // Wake up
  if (!writeRegister(REG_PWR_MGMT_1, 0x00))
  {
    Serial.println("Gagal menulis PWR_MGMT_1");
    return false;
  }
  delay(100);
  // Digital Low Pass Filter
  writeRegister(REG_CONFIG, 0x03);
  // 1kHz / (1 + 9) = 100Hz
  writeRegister(REG_SMPLRT_DIV, 9);
  // ±8g
  writeRegister(REG_ACCEL_CONFIG, 0x10);
  // ±250 deg/s
  // Gyro belum digunakan
  writeRegister(REG_GYRO_CONFIG, 0x00);
  // Pastikan perangkat yang menjawab benar-benar MPU6050.
  uint8_t who = 0;
  if (!readRegisters(REG_WHO_AM_I, &who, 1))
  {
    Serial.println("Tidak bisa membaca WHO_AM_I");
    return false;
  }
  Serial.print("WHO_AM_I = 0x");
  if (who < 16)
    Serial.print("0");
  Serial.println(who, HEX);
  if (who != 0x68)
  {
    Serial.println("WHO_AM_I bukan 0x68");
    return false;
  }
  return true;
}
// =========================================================
// READ ACCELERATION MAGNITUDE
// =========================================================
bool readAccelerationMagnitude(float &magnitude)
{
  uint8_t data[6];
  if (!readRegisters(
        REG_ACCEL_XOUT_H,
        data,
        6
      ))
  {
    return false;
  }
  int16_t rawX =
    ((int16_t)data[0] << 8) |
    data[1];
  int16_t rawY =
    ((int16_t)data[2] << 8) |
    data[3];
  int16_t rawZ =
    ((int16_t)data[4] << 8) |
    data[5];
  float ax =
    (float)rawX / ACCEL_SCALE;
  float ay =
    (float)rawY / ACCEL_SCALE;
  float az =
    (float)rawZ / ACCEL_SCALE;
  magnitude =
    sqrtf(
      ax * ax +
      ay * ay +
      az * az
    );
  return true;
}
// =========================================================
// BUFFER
// =========================================================
void addSample(
  uint32_t timeMs,
  float acceleration
)
{
  buffer[bufferHead].timeMs =
    timeMs;
  buffer[bufferHead].acc =
    acceleration;
  bufferHead++;
  if (bufferHead >= BUFFER_SIZE)
  {
    bufferHead = 0;
  }
  if (sampleCount < BUFFER_SIZE)
  {
    sampleCount++;
  }
}
// =========================================================
// CHRONOLOGICAL BUFFER INDEX
// =========================================================
int chronologicalIndex(int position)
{
  int oldest =
    (
      bufferHead -
      sampleCount +
      BUFFER_SIZE
    ) % BUFFER_SIZE;
  return (
    oldest +
    position
  ) % BUFFER_SIZE;
}
// =========================================================
// GET SAMPLE
// =========================================================
Sample getSample(int position)
{
  return buffer[
    chronologicalIndex(position)
  ];
}
// =========================================================
// FIND SAMPLE NEAREST TO TIME
// =========================================================
int findNearestSamplePosition(
  uint32_t targetTime
)
{
  if (sampleCount == 0)
  {
    return -1;
  }
  int bestPosition = 0;
  uint32_t bestDifference =
    0xFFFFFFFF;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    uint32_t difference;
    if (s.timeMs > targetTime)
    {
      difference =
        s.timeMs - targetTime;
    }
    else
    {
      difference =
        targetTime - s.timeMs;
    }
    if (difference < bestDifference)
    {
      bestDifference = difference;
      bestPosition = i;
    }
  }
  return bestPosition;
}
// =========================================================
// LOCAL PEAK
// =========================================================
bool isLocalPeak(int position)
{
  if (position <= 0)
  {
    return false;
  }
  if (position >= sampleCount - 1)
  {
    return false;
  }
  Sample previous =
    getSample(position - 1);
  Sample current =
    getSample(position);
  Sample next =
    getSample(position + 1);
  return (
    current.acc >= previous.acc &&
    current.acc > next.acc
  );
}
// =========================================================
// PEAK ISOLATION
//
// IMPORTANT CORRECTION
//
// Candidate peak = pt
//
// Paper:
//
// At analysis time t,
// pt = t - 2500ms
//
// No other value above threshold
// may appear in:
//
//     (pt, t)
//
// So we MUST inspect the samples
// AFTER the candidate.
//
// =========================================================
bool isIsolatedPeak(int position)
{
  Sample candidate =
    getSample(position);
  uint32_t candidateTime =
    candidate.timeMs;
  uint32_t isolationEnd =
    candidateTime +
    PEAK_LOOKBACK_MS;
  for (
    int i = position + 1;
    i < sampleCount;
    i++
  )
  {
    Sample s =
      getSample(i);
    if (s.timeMs > isolationEnd)
    {
      break;
    }
    // Another acceleration value
    // above peak threshold exists.
    //
    // Therefore candidate is NOT isolated.
    if (s.acc > PEAK_THRESHOLD_G)
    {
      return false;
    }
  }
  return true;
}
// =========================================================
// FIND IMPACT END
// =========================================================
bool findImpactEnd(
  uint32_t peakTime,
  uint32_t &impactEnd
)
{
  uint32_t searchEnd =
    peakTime +
    IMPACT_END_FALLBACK_MS;
  bool found = false;
  impactEnd = peakTime;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < peakTime)
    {
      continue;
    }
    if (s.timeMs > searchEnd)
    {
      break;
    }
    if (s.acc > IMPACT_THRESHOLD_G)
    {
      found = true;
      impactEnd =
        s.timeMs;
    }
  }
  if (!found)
  {
    impactEnd =
      peakTime +
      IMPACT_END_FALLBACK_MS;
  }
  return found;
}
// =========================================================
// FIND IMPACT START
//
// Required temporal pattern:
//
// ACC <= 0.8g
//        ↓
// ACC >= 1.5g
//
// Search interval:
//
// [impactEnd - 1200ms, peakTime]
// =========================================================
bool findImpactStart(
  uint32_t peakTime,
  uint32_t impactEnd,
  uint32_t &impactStart
)
{
  uint32_t searchStart;
  if (impactEnd > IMPACT_START_WINDOW_MS)
  {
    searchStart =
      impactEnd -
      IMPACT_START_WINDOW_MS;
  }
  else
  {
    searchStart = 0;
  }
  bool lowAccelerationFound =
    false;
  uint32_t lowAccelerationTime = 0;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < searchStart)
    {
      continue;
    }
    if (s.timeMs > peakTime)
    {
      break;
    }
    // -----------------------------------------
    // STEP 1:
    // Find <=0.8g
    // -----------------------------------------
    if (
      !lowAccelerationFound &&
      s.acc <= FREEFALL_THRESHOLD_G
    )
    {
      lowAccelerationFound = true;
      lowAccelerationTime =
        s.timeMs;
    }
    // -----------------------------------------
    // STEP 2:
    // After low acceleration,
    // find >=1.5g
    // -----------------------------------------
    if (
      lowAccelerationFound &&
      s.timeMs > lowAccelerationTime &&
      s.acc >= IMPACT_THRESHOLD_G
    )
    {
      impactStart =
        lowAccelerationTime;
      return true;
    }
  }
  return false;
}
// =========================================================
// AAMV
// =========================================================
float calculateAAMV(
  uint32_t startTime,
  uint32_t endTime
)
{
  float totalVariation =
    0.0f;
  int numberOfDifferences =
    0;
  bool previousExists =
    false;
  float previousValue =
    0.0f;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startTime)
    {
      continue;
    }
    if (s.timeMs > endTime)
    {
      break;
    }
    if (previousExists)
    {
      totalVariation +=
        fabsf(
          s.acc -
          previousValue
        );
      numberOfDifferences++;
    }
    previousValue =
      s.acc;
    previousExists =
      true;
  }
  if (numberOfDifferences == 0)
  {
    return 0.0f;
  }
  return
    totalVariation /
    numberOfDifferences;
}
// =========================================================
// MAXIMUM
// =========================================================
float calculateMaximum(
  uint32_t startTime,
  uint32_t endTime
)
{
  float maximumValue =
    -100.0f;
  bool found = false;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startTime)
    {
      continue;
    }
    if (s.timeMs > endTime)
    {
      break;
    }
    if (
      !found ||
      s.acc > maximumValue
    )
    {
      maximumValue =
        s.acc;
      found = true;
    }
  }
  if (!found)
  {
    return 0.0f;
  }
  return maximumValue;
}
// =========================================================
// MINIMUM
// =========================================================
float calculateMinimum(
  uint32_t startTime,
  uint32_t endTime
)
{
  float minimumValue =
    100.0f;
  bool found = false;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startTime)
    {
      continue;
    }
    if (s.timeMs > endTime)
    {
      break;
    }
    if (
      !found ||
      s.acc < minimumValue
    )
    {
      minimumValue =
        s.acc;
      found = true;
    }
  }
  if (!found)
  {
    return 0.0f;
  }
  return minimumValue;
}
// =========================================================
// PDI
// =========================================================
float calculatePDI(
  uint32_t startTime,
  uint32_t endTime
)
{
  bool aboveThreshold =
    false;
  uint32_t firstTime = 0;
  uint32_t lastTime = 0;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startTime)
    {
      continue;
    }
    if (s.timeMs > endTime)
    {
      break;
    }
    if (s.acc >= PEAK_DURATION_G)
    {
      if (!aboveThreshold)
      {
        firstTime =
          s.timeMs;
        aboveThreshold =
          true;
      }
      lastTime =
        s.timeMs;
    }
  }
  if (!aboveThreshold)
  {
    return 0.0f;
  }
  return
    (float)(
      lastTime -
      firstTime
    ) / 1000.0f;
}
// =========================================================
// ARI
// =========================================================
float calculateARI(
  uint32_t impactStart,
  uint32_t impactEnd
)
{
  uint32_t center =
    impactStart +
    (
      impactEnd -
      impactStart
    ) / 2;
  uint32_t halfWindow =
    ARI_WINDOW_MS / 2;
  uint32_t startTime;
  if (center > halfWindow)
  {
    startTime =
      center -
      halfWindow;
  }
  else
  {
    startTime = 0;
  }
  uint32_t endTime =
    center +
    halfWindow;
  int totalSamples =
    0;
  int outsideSamples =
    0;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startTime)
    {
      continue;
    }
    if (s.timeMs > endTime)
    {
      break;
    }
    totalSamples++;
    if (
      s.acc < ARI_LOW_G ||
      s.acc > ARI_HIGH_G
    )
    {
      outsideSamples++;
    }
  }
  if (totalSamples == 0)
  {
    return 0.0f;
  }
  return
    (float)outsideSamples /
    (float)totalSamples;
}
// =========================================================
// FFI
// =========================================================
float calculateFFI(
  uint32_t peakTime
)
{
  uint32_t startLimit;
  if (peakTime > FFI_LOOKBACK_MS)
  {
    startLimit =
      peakTime -
      FFI_LOOKBACK_MS;
  }
  else
  {
    startLimit = 0;
  }
  bool freeFallFound =
    false;
  uint32_t freeFallStart =
    startLimit;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < startLimit)
    {
      continue;
    }
    if (s.timeMs > peakTime)
    {
      break;
    }
    if (
      s.acc <= FREEFALL_THRESHOLD_G
    )
    {
      freeFallStart =
        s.timeMs;
      freeFallFound =
        true;
      break;
    }
  }
  if (!freeFallFound)
  {
    freeFallStart =
      startLimit;
  }
  float sum = 0.0f;
  int count = 0;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample s =
      getSample(i);
    if (s.timeMs < freeFallStart)
    {
      continue;
    }
    if (s.timeMs > peakTime)
    {
      break;
    }
    sum += s.acc;
    count++;
  }
  if (count == 0)
  {
    return 0.0f;
  }
  return
    sum /
    count;
}
// =========================================================
// SCI
//
// SCI = repeated activity / step count.
//
// We search in:
//
// [pt - 2200ms, pt]
//
// One activity event:
//
// ACC < 1.0g
// continuously >= 80ms
//
// followed by:
//
// ACC > 1.6g
// within next 200ms
//
// =========================================================
float calculateSCI(
  uint32_t peakTime
)
{
  uint32_t startTime;
  if (peakTime > SCI_WINDOW_MS)
  {
    startTime =
      peakTime -
      SCI_WINDOW_MS;
  }
  else
  {
    startTime = 0;
  }
  uint32_t endTime =
    peakTime;
  int activityCount = 0;
  for (int i = 0; i < sampleCount; i++)
  {
    Sample first =
      getSample(i);
    if (first.timeMs < startTime)
    {
      continue;
    }
    if (first.timeMs > endTime)
    {
      break;
    }
    // ---------------------------------------------------
    // Start of possible valley
    // ---------------------------------------------------
    if (first.acc >= SCI_LOW_G)
    {
      continue;
    }
    uint32_t valleyStart =
      first.timeMs;
    int j = i;
    // ---------------------------------------------------
    // Continue through low-acceleration region
    // ---------------------------------------------------
    while (
      j + 1 < sampleCount
    )
    {
      Sample next =
        getSample(j + 1);
      if (next.timeMs > endTime)
      {
        break;
      }
      if (next.acc >= SCI_LOW_G)
      {
        break;
      }
      j++;
    }
    Sample valleyEnd =
      getSample(j);
    uint32_t valleyDuration =
      valleyEnd.timeMs -
      valleyStart;
    // ---------------------------------------------------
    // Valley must last at least 80ms
    // ---------------------------------------------------
    if (
      valleyDuration >=
      SCI_LOW_DURATION_MS
    )
    {
      uint32_t followEnd =
        valleyEnd.timeMs +
        SCI_HIGH_FOLLOW_MS;
      // -------------------------------------------------
      // Search for high acceleration after valley
      // -------------------------------------------------
      for (
        int k = j + 1;
        k < sampleCount;
        k++
      )
      {
        Sample high =
          getSample(k);
        if (high.timeMs > endTime)
        {
          break;
        }
        if (high.timeMs > followEnd)
        {
          break;
        }
        if (
          high.acc >
          SCI_HIGH_G
        )
        {
          activityCount++;
          // Skip to this event.
          i = k;
          break;
        }
      }
    }
    else
    {
      // Skip this short valley.
      i = j;
    }
  }
  return
    (float)activityCount;
}
// =========================================================
// EXTRACT FEATURES
// =========================================================
FallFeatures extractFeatures(
  uint32_t peakTime,
  float candidateAcceleration,
  bool impactSequenceFound,
  uint32_t impactStart,
  uint32_t impactEnd
)
{
  FallFeatures f;
  f.peakTime =
    peakTime;
  f.impactSequenceFound =
    impactSequenceFound;
  f.impactStartTime =
    impactStart;
  f.impactEndTime =
    impactEnd;
  // Always calculate SCI
  f.SCI =
    calculateSCI(
      peakTime
    );
  // -------------------------------------------------------
  // If sequence not found,
  // some fall-event features are not meaningful.
  //
  // Keep MPI equal to candidate acceleration
  // only for debugging / display.
  // -------------------------------------------------------
  if (!impactSequenceFound)
  {
    f.AAMV = 0.0f;
    f.IDI = 0.0f;
    f.MPI =
      candidateAcceleration;
    f.MVI = 0.0f;
    f.PDI = 0.0f;
    f.ARI = 0.0f;
    f.FFI =
      calculateFFI(
        peakTime
      );
    return f;
  }
  // -------------------------------------------------------
  // AAMV
  // -------------------------------------------------------
  f.AAMV =
    calculateAAMV(
      impactStart,
      impactEnd
    );
  // -------------------------------------------------------
  // IDI
  // -------------------------------------------------------
  f.IDI =
    (float)(
      impactEnd -
      impactStart
    ) / 1000.0f;
  // -------------------------------------------------------
  // MPI
  // -------------------------------------------------------
  f.MPI =
    calculateMaximum(
      impactStart,
      impactEnd
    );
  // -------------------------------------------------------
  // MVI
  // -------------------------------------------------------
  uint32_t mviStart;
  if (impactStart > MVI_LOOKBACK_MS)
  {
    mviStart =
      impactStart -
      MVI_LOOKBACK_MS;
  }
  else
  {
    mviStart = 0;
  }
  f.MVI =
    calculateMinimum(
      mviStart,
      impactEnd
    );
  // -------------------------------------------------------
  // PDI
  // -------------------------------------------------------
  f.PDI =
    calculatePDI(
      impactStart,
      impactEnd
    );
  // -------------------------------------------------------
  // ARI
  // -------------------------------------------------------
  f.ARI =
    calculateARI(
      impactStart,
      impactEnd
    );
  // -------------------------------------------------------
  // FFI
  // -------------------------------------------------------
  f.FFI =
    calculateFFI(
      peakTime
    );
  return f;
}
// =========================================================
// MANUAL DECISION TREE
//
// This is NOT the original C5.0 tree from the paper.
//
// It is our manual rule-based implementation
// derived from the paper's features / thresholds,
// with an additional SCI rule.
//
// ---------------------------------------------------------
//
// NODE 1:
// Peak >= 3.0929g?
//
// NODE 2:
// SCI >= 3?
//     YES -> repeated activity / exercise
//     NO  -> continue
//
// NODE 3:
// Sequence:
//
//     <=0.8g
//         ↓
//     >=1.5g
//
//     YES -> continue
//     NO  -> NOT FALL
//
// NODE 4:
// Impact start before peak
//
// =========================================================
bool decisionTree(
  const FallFeatures &f
)
{
  // -------------------------------------------------------
  // NODE 1
  // Peak threshold
  // -------------------------------------------------------
  if (
    f.MPI <
    PEAK_THRESHOLD_G
  )
  {
    return false;
  }
  // -------------------------------------------------------
  // NODE 2
  // SCI
  //
  // Repeated activity / exercise
  // -------------------------------------------------------
  if (
    f.SCI >=
    SCI_EXERCISE_THRESHOLD
  )
  {
    return false;
  }
  // -------------------------------------------------------
  // NODE 3
  //
  // Actual temporal sequence:
  //
  // <=0.8g
  //    ↓
  // >=1.5g
  // -------------------------------------------------------
  if (!f.impactSequenceFound)
  {
    return false;
  }
  // -------------------------------------------------------
  // NODE 4
  //
  // Impact start must occur before peak
  // -------------------------------------------------------
  if (
    f.impactStartTime >=
    f.peakTime
  )
  {
    return false;
  }
  // -------------------------------------------------------
  // NODE 5
  //
  // Must be within 1200ms
  // -------------------------------------------------------
  if (
    (
      f.peakTime -
      f.impactStartTime
    )
    >
    IMPACT_START_WINDOW_MS
  )
  {
    return false;
  }
  return true;
}
// =========================================================
// PRINT FEATURES
// =========================================================
void printFeatures(
  const FallFeatures &f
)
{
  Serial.println();
  Serial.println(
    "--------------------------------------"
  );
  Serial.println(
    "FALL EVENT ANALYSIS"
  );
  Serial.println(
    "--------------------------------------"
  );
  Serial.print("Peak Time       : ");
  Serial.print(f.peakTime);
  Serial.println(" ms");
  Serial.print("Impact Start    : ");
  Serial.print(f.impactStartTime);
  Serial.println(" ms");
  Serial.print("Impact End      : ");
  Serial.print(f.impactEndTime);
  Serial.println(" ms");
  Serial.print("Impact Sequence : ");
  if (f.impactSequenceFound)
  {
    Serial.println("YES");
  }
  else
  {
    Serial.println("NO");
  }
  Serial.println();
  Serial.print("AAMV = ");
  Serial.println(
    f.AAMV,
    4
  );
  Serial.print("IDI  = ");
  Serial.print(
    f.IDI,
    3
  );
  Serial.println(" s");
  Serial.print("MPI  = ");
  Serial.print(
    f.MPI,
    4
  );
  Serial.println(" g");
  Serial.print("MVI  = ");
  Serial.print(
    f.MVI,
    4
  );
  Serial.println(" g");
  Serial.print("PDI  = ");
  Serial.print(
    f.PDI,
    3
  );
  Serial.println(" s");
  Serial.print("ARI  = ");
  Serial.println(
    f.ARI,
    4
  );
  Serial.print("FFI  = ");
  Serial.println(
    f.FFI,
    4
  );
  Serial.print("SCI  = ");
  Serial.println(
    f.SCI,
    0
  );
  Serial.println(
    "--------------------------------------"
  );
}
// =========================================================
// PROCESS FALL DETECTION
// =========================================================
void processFallDetection(
  uint32_t currentTime
)
{
  // Need enough history:
  //
  // SCI history 2200ms
  // +
  // peak waiting 2500ms
  //
  if (
    sampleCount <
    MIN_SAMPLES_FOR_ANALYSIS
  )
  {
    return;
  }
  // -------------------------------------------------------
  // Candidate peak:
  //
  // pt = t - 2500ms
  // -------------------------------------------------------
  uint32_t targetTime;
  if (
    currentTime >
    PEAK_LOOKBACK_MS
  )
  {
    targetTime =
      currentTime -
      PEAK_LOOKBACK_MS;
  }
  else
  {
    return;
  }
  int candidatePosition =
    findNearestSamplePosition(
      targetTime
    );
  if (candidatePosition < 0)
  {
    return;
  }
  Sample candidate =
    getSample(
      candidatePosition
    );
  // -------------------------------------------------------
  // Do not process same candidate twice
  // -------------------------------------------------------
  if (
    candidate.timeMs ==
    lastProcessedCandidateTime
  )
  {
    return;
  }
  lastProcessedCandidateTime =
    candidate.timeMs;
  // -------------------------------------------------------
  // Peak threshold
  // -------------------------------------------------------
  if (
    candidate.acc <=
    PEAK_THRESHOLD_G
  )
  {
    return;
  }
  // -------------------------------------------------------
  // Local peak
  // -------------------------------------------------------
  if (
    !isLocalPeak(
      candidatePosition
    )
  )
  {
    return;
  }
  // -------------------------------------------------------
  // PEAK ISOLATION
  //
  // No other >3.0929g
  // in the following 2500ms.
  // -------------------------------------------------------
  if (
    !isIsolatedPeak(
      candidatePosition
    )
  )
  {
    return;
  }
  // -------------------------------------------------------
  // Prevent duplicate detections
  // -------------------------------------------------------
  if (
    lastDetectedPeakTime != 0 &&
    (
      candidate.timeMs -
      lastDetectedPeakTime
    ) < 1500
  )
  {
    return;
  }
  // -------------------------------------------------------
  // Candidate found
  // -------------------------------------------------------
  Serial.println();
  Serial.println();
  Serial.println(
    "======================================"
  );
  Serial.println(
    "POTENTIAL FALL PEAK DETECTED"
  );
  Serial.println(
    "======================================"
  );
  Serial.print(
    "Peak acceleration = "
  );
  Serial.print(
    candidate.acc,
    4
  );
  Serial.println(
    " g"
  );
  // -------------------------------------------------------
  // Impact end
  // -------------------------------------------------------
  uint32_t impactEnd =
    candidate.timeMs;
  findImpactEnd(
    candidate.timeMs,
    impactEnd
  );
  // -------------------------------------------------------
  // Impact start
  //
  // Search actual:
  //
  // <=0.8g
  //    ↓
  // >=1.5g
  // -------------------------------------------------------
  uint32_t impactStart =
    candidate.timeMs;
  bool sequenceFound =
    findImpactStart(
      candidate.timeMs,
      impactEnd,
      impactStart
    );
  // -------------------------------------------------------
  // Extract features
  // -------------------------------------------------------
  FallFeatures features =
    extractFeatures(
      candidate.timeMs,
      candidate.acc,
      sequenceFound,
      impactStart,
      impactEnd
    );
  // -------------------------------------------------------
  // Decision
  // -------------------------------------------------------
  bool fallDetected =
    decisionTree(
      features
    );
  // -------------------------------------------------------
  // PRINT FEATURES
  // -------------------------------------------------------
  printFeatures(
    features
  );
  // -------------------------------------------------------
  // FINAL RESULT
  // -------------------------------------------------------
  Serial.println();
  Serial.println(
    "======================================"
  );
  if (fallDetected)
  {
    Serial.println(
      ">>> FALL DETECTED <<<"
    );
    lastDetectedPeakTime =
      candidate.timeMs;
    // =====================================================
    // INTEGRASI KE VITRA-SENSE
    // =====================================================
    emergencyAktif = true;
    gerakanDarurat = true;
    vitraLoraSetEmergency(1);   // LoRa: SOS jatuh -> LINK
    Serial.println(
      "VITRA-SENSE: FALL -> EMERGENCY AKTIF"
    );
    kirimPaketUji(
      "FALL_DETECTED"
    );
  }
  else
  {
    Serial.println(
      ">>> NOT FALL <<<"
    );
  }
  Serial.println(
    "======================================"
  );
  // -------------------------------------------------------
  // DECISION EXPLANATION
  // -------------------------------------------------------
  Serial.println();
  Serial.println(
    "Decision explanation:"
  );
  // Peak
  if (
    features.MPI <
    PEAK_THRESHOLD_G
  )
  {
    Serial.println(
      "- Peak < 3.0929g"
    );
  }
  else
  {
    Serial.println(
      "- Peak >= 3.0929g"
    );
  }
  // SCI
  if (
    features.SCI >=
    SCI_EXERCISE_THRESHOLD
  )
  {
    Serial.print(
      "- SCI >= "
    );
    Serial.print(
      SCI_EXERCISE_THRESHOLD
    );
    Serial.println(
      " -> repeated activity/exercise"
    );
  }
  else
  {
    Serial.print(
      "- SCI < "
    );
    Serial.print(
      SCI_EXERCISE_THRESHOLD
    );
    Serial.println(
      " -> no strong repeated activity"
    );
  }
  // Sequence
  if (
    features.impactSequenceFound
  )
  {
    Serial.println(
      "- Sequence <=0.8g -> >=1.5g FOUND"
    );
  }
  else
  {
    Serial.println(
      "- Sequence <=0.8g -> >=1.5g NOT FOUND"
    );
  }
  Serial.println();
}
// =====================================================
// PEMERIKSAAN I2C
// =====================================================
bool perangkatI2CAda(uint8_t alamat) {
  Wire.beginTransmission(alamat);
  return Wire.endTransmission() == 0;
}
// =====================================================
// PEMBACAAN TOMBOL
// =====================================================
AksiTombol perbaruiTombol(
  StatusTombol &tombol,
  bool bacaan,
  unsigned long sekarang,
  unsigned long batasTahan
) {
  if (bacaan != tombol.bacaanTerakhir) {
    tombol.bacaanTerakhir = bacaan;
    tombol.waktuPerubahan = sekarang;
  }
  if (
    sekarang - tombol.waktuPerubahan >= DEBOUNCE_MS &&
    bacaan != tombol.keadaanStabil
  ) {
    tombol.keadaanStabil = bacaan;
    // Tombol baru ditekan.
    if (tombol.keadaanStabil == LOW) {
      tombol.mulaiDitekan = sekarang;
      tombol.tahanSudahDikirim = false;
      return DITEKAN;
    }
    // Tombol dilepas sebelum mencapai durasi tahan.
    if (!tombol.tahanSudahDikirim) {
      tombol.mulaiDitekan = 0;
      return KLIK;
    }
    // Tombol dilepas setelah aksi tahan dikirim.
    tombol.mulaiDitekan = 0;
    tombol.tahanSudahDikirim = false;
  }
  // Deteksi long-press.
  if (
    tombol.keadaanStabil == LOW &&
    !tombol.tahanSudahDikirim &&
    sekarang - tombol.mulaiDitekan >= batasTahan
  ) {
    tombol.tahanSudahDikirim = true;
    return TAHAN;
  }
  return TIDAK_ADA;
}
// =====================================================
// FUNGSI TRIASE
// =====================================================
const char *namaTriase(Triase nilai) {
  switch (nilai) {
    case HIJAU:
      return "HIJAU";
    case KUNING:
      return "KUNING";
    case MERAH:
      return "MERAH";
    default:
      return "BELUM";
  }
}
Triase pilihanBerikutnya(Triase nilai) {
  // MERAH -> HIJAU -> KUNING -> MERAH
  switch (nilai) {
    case MERAH:
      return HIJAU;
    case HIJAU:
      return KUNING;
    case KUNING:
    default:
      return MERAH;
  }
}
// =====================================================
// KONTROL OLED
// =====================================================
void nyalakanLayar() {
  if (!oledTerdeteksi) {
    return;
  }
  oled.setPowerSave(0);
  layarAktif = true;
  terakhirInteraksi = millis();
}
void matikanLayar() {
  if (!oledTerdeteksi) {
    return;
  }
  if (layarAktif) {
    layarAktif = false;
    oled.setPowerSave(1);
  }
}
// =====================================================
// LED TRIASE
// =====================================================
void perbaruiRGB() {
  bool merah = false;
  bool hijau = false;
  if (
    triaseAsli != BELUM &&
    triaseRe != BELUM &&
    dataSensorValid
  ) {
    if (triaseRe > triaseAsli) {
      // Re-triage lebih buruk.
      merah = true;
    } else {
      // Re-triage sama atau lebih baik.
      hijau = true;
    }
  }
  digitalWrite(
    PIN_LED_RED,
    merah ? HIGH : LOW
  );
  digitalWrite(
    PIN_LED_GREEN,
    hijau ? HIGH : LOW
  );
}
// =====================================================
// BUZZER
// =====================================================
void perbaruiBuzzer(unsigned long sekarang) {
  bool alarmAktif =
    emergencyAktif ||
    gerakanDarurat;
  if (!alarmAktif) {
    buzzerMenyala = false;
    digitalWrite(PIN_BUZZER, BUZZER_OFF);
    return;
  }
  if (
    sekarang - terakhirBunyi >=
    BUZZER_INTERVAL_MS
  ) {
    terakhirBunyi = sekarang;
    buzzerMenyala = !buzzerMenyala;
    digitalWrite(
      PIN_BUZZER,
      buzzerMenyala
        ? BUZZER_ON
        : BUZZER_OFF
    );
  }
}
// =====================================================
// LED EMERGENCY
// =====================================================
void perbaruiLedEmergency(unsigned long sekarang) {
  bool alarmAktif =
    emergencyAktif ||
    gerakanDarurat;
  if (!alarmAktif) {
    ledEmergencyNyala = true;
    digitalWrite(PIN_LED_EMERGENCY, HIGH);
    terakhirKedipEmergency = sekarang;
    return;
  }
  if (
    sekarang - terakhirKedipEmergency >=
    LED_BLINK_MS
  ) {
    terakhirKedipEmergency = sekarang;
    ledEmergencyNyala = !ledEmergencyNyala;
    digitalWrite(
      PIN_LED_EMERGENCY,
      ledEmergencyNyala ? HIGH : LOW
    );
  }
}

Triase hitungTriasePPG(int nilaiSpO2, int nilaiHR, int nilaiRR) {
  return (Triase)vitraLoraLevel(nilaiHR, nilaiSpO2, nilaiRR);
}

void perbaruiDataPPG(unsigned long sekarang) {
  if (!ppgTerdeteksi) {
    dataSensorValid = false;
    rrTersedia = false;
    triaseRe = BELUM;
    jariPPGSebelumnya = false;
    ppgSedangStabilisasi = false;
    mulaiStabilisasiPPG = 0;
    hr = 0;
    spo2 = 0;
    rr = 0;
    return;
  }

  // Selalu perbarui pengolahan PPG agar data RR terus dikumpulkan
  // sekalipun hasilnya belum boleh ditampilkan pada OLED.
  bool hasilBaru = ppgUpdate();
  if (ppgHasilTertunda) {
    hasilBaru = true;
    ppgHasilTertunda = false;
  }
  bool jariSekarang = ppgFingerDetected();

  if (!jariSekarang) {
    bool baruDilepas = jariPPGSebelumnya;
    jariPPGSebelumnya = false;
    ppgSedangStabilisasi = false;
    mulaiStabilisasiPPG = 0;
    dataSensorValid = false;
    rrTersedia = false;
    hr = 0;
    spo2 = 0;
    rr = 0;
    triaseRe = BELUM;
    if (baruDilepas) {
      kirimPaketUji("JARI_DILEPAS");
    }
    return;
  }

  if (!jariPPGSebelumnya) {
    jariPPGSebelumnya = true;
    ppgSedangStabilisasi = true;
    mulaiStabilisasiPPG = sekarang;
    dataSensorValid = false;
    rrTersedia = false;
    hr = 0;
    spo2 = 0;
    rr = 0;
    triaseRe = BELUM;
    Serial.println("PPG: JARI TERDETEKSI, STABILISASI HR/SpO2 10 DETIK");
  }

  if (ppgSedangStabilisasi) {
    if (sekarang - mulaiStabilisasiPPG < PPG_STABILIZATION_MS) {
      return;
    }
    ppgSedangStabilisasi = false;
    Serial.println("PPG: HR/SpO2 SELESAI STABILISASI, RR MENUNGGU 90 DETIK");
  }

  if (!hasilBaru) {
    if (dataSensorValid && sekarang - terakhirDataPPG > 5000UL) {
      dataSensorValid = false;
      rrTersedia = false;
      rr = 0;
      triaseRe = BELUM;
      kirimPaketUji("PPG_TIMEOUT");
    }
    return;
  }

  hr = (int)roundf(ppgGetBPM());
  spo2 = (int)roundf(ppgGetSpO2());
  dataSensorValid = hr > 0 && spo2 > 0;

  if (!dataSensorValid) {
    rrTersedia = false;
    rr = 0;
    triaseRe = BELUM;
    return;
  }

  // RR tetap dihitung oleh modul PPG, tetapi hasilnya disembunyikan
  // sampai 90 detik dari awal deteksi jari serta tersedia nilai > 0.
  bool waktuRRCukup =
    sekarang - mulaiStabilisasiPPG >= RR_STABILIZATION_MS;
  int nilaiRR = waktuRRCukup ? (int)roundf(ppgGetRR()) : 0;
  rrTersedia = waktuRRCukup && nilaiRR > 0;
  rr = rrTersedia ? nilaiRR : 0;

  terakhirDataPPG = sekarang;
  // Hasil triase lengkap belum dibuat sebelum ketiga parameter tersedia.
  triaseRe = rrTersedia ? hitungTriasePPG(spo2, hr, rr) : BELUM;

  Serial.print("OLED PPG -> SpO2: ");
  Serial.print(spo2);
  Serial.print("% | HR: ");
  Serial.print(hr);
  Serial.print(" BPM | RR: ");
  if (rrTersedia) {
    Serial.print(rr);
    Serial.println(" RPM");
  } else {
    Serial.println("MENUNGGU");
  }

  kirimPaketUji("UPDATE_PPG");
}
// =====================================================
// PAKET DATA
// =====================================================
void kirimPaketUji(const char *event) {
  Serial.print("node_id=");
  Serial.print(NODE_ID);
  Serial.print(", seq=");
  Serial.print(nomorPaket++);
  Serial.print(", uptime_ms=");
  Serial.print(millis());
  Serial.print(", event=");
  Serial.print(event);
  Serial.print(", spo2=");
  Serial.print(spo2);
  Serial.print(", hr=");
  Serial.print(hr);
  Serial.print(", rr=");
  Serial.print(rr); // 0 jika RR belum siap
  Serial.print(", sensor_valid=");
  Serial.print(dataSensorValid ? 1 : 0); // HR/SpO2
  Serial.print(", rr_valid=");
  Serial.print(rrTersedia ? 1 : 0);
  Serial.print(", triase_asli=");
  Serial.print(namaTriase(triaseAsli));
  Serial.print(", triase_re=");
  Serial.print(namaTriase(triaseRe));
  Serial.print(", gerakan_darurat=");
  Serial.print(gerakanDarurat ? 1 : 0);
  Serial.print(", emergency=");
  Serial.print(emergencyAktif ? 1 : 0);
  Serial.print(", mpu6050=");
  Serial.println(mpuTerdeteksi ? "TERDETEKSI" : "TIDAK_TERDETEKSI");
}
// =====================================================
// HEADER OLED
// =====================================================
void gambarHeader(const char *judul) {
  // Header dibuat pendek agar area konten lebih luas.
  oled.drawRBox(6, 1, 116, 10, 2);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tr);
  int lebarJudul =
    oled.getStrWidth(judul);
  oled.drawStr(
    64 - lebarJudul / 2,
    8,
    judul
  );
  oled.setDrawColor(1);
}
// =====================================================
// IKON ALARM
// =====================================================
void gambarAlarm() {
  if (
    !emergencyAktif &&
    !gerakanDarurat
  ) {
    return;
  }
  // Ikon kecil di sebelah kanan header.
  oled.drawDisc(114, 6, 3);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_4x6_tr);
  oled.drawStr(113, 8, "!");
  oled.setDrawColor(1);
}
// =====================================================
// KOTAK PILIHAN TRIASE
// =====================================================
void gambarKotakTriase(const char *teks) {
  oled.setFont(u8g2_font_7x13B_tr);
  int lebarTeks =
    oled.getStrWidth(teks);
  int lebarKotak =
    lebarTeks + 22;
  int x =
    (128 - lebarKotak) / 2;
  if (x < 8) {
    x = 8;
  }
  oled.drawRBox(
    x,
    18,
    lebarKotak,
    20,
    4
  );
  oled.setDrawColor(0);
  oled.drawStr(
    64 - lebarTeks / 2,
    33,
    teks
  );
  oled.setDrawColor(1);
}

void gambarLabelTombol(int x, int y, const char *label, bool terisi) {
  constexpr int lebar = 18;
  constexpr int tinggi = 10;
  oled.setFont(u8g2_font_5x8_tr);

  if (terisi) {
    // Pada OLED monokrom biru, piksel aktif pada kotak ini terlihat biru.
    oled.drawRBox(x, y, lebar, tinggi, 2);
    oled.setDrawColor(0);
    oled.drawStr(x + 4, y + 8, label);
    oled.setDrawColor(1);
  } else {
    // Tombol outline: bagian dalam tetap mati/hitam.
    oled.drawRFrame(x, y, lebar, tinggi, 2);
    oled.drawStr(x + 4, y + 8, label);
  }
}
// =====================================================
// ANIMASI PILIHAN TRIASE
// =====================================================
void gambarPilihan(unsigned long sekarang) {
  oled.setFont(u8g2_font_7x13B_tr);
  // Kondisi biasa: tampilkan pilihan tanpa animasi.
  if (!animasiAktif) {
    gambarKotakTriase(
      namaTriase(pilihan)
    );
    return;
  }
  unsigned long berjalan =
    sekarang - mulaiAnimasi;
  // Animasi selesai.
  if (berjalan >= ANIMATION_MS) {
    animasiAktif = false;
    gambarKotakTriase(
      namaTriase(pilihan)
    );
    return;
  }
  // Kotak tetap ditampilkan selama animasi.
  oled.drawRBox(
    25,
    18,
    78,
    20,
    4
  );
  const char *teksLama =
    namaTriase(pilihanSebelumnya);
  const char *teksBaru =
    namaTriase(pilihan);
  int lebarLama =
    oled.getStrWidth(teksLama);
  int lebarBaru =
    oled.getStrWidth(teksBaru);
  int perpindahan =
    (berjalan * 50) /
    ANIMATION_MS;
  // Tulisan di dalam kotak dibuat hitam.
  oled.setDrawColor(0);
  // Pilihan lama bergerak ke kiri.
  oled.drawStr(
    64 - lebarLama / 2 - perpindahan,
    33,
    teksLama
  );
  // Pilihan baru masuk dari kanan.
  oled.drawStr(
    64 - lebarBaru / 2 + 50 - perpindahan,
    33,
    teksBaru
  );
  // Kembalikan warna gambar menjadi putih.
  oled.setDrawColor(1);
}
// =====================================================
// HALAMAN PEMILIHAN TRIASE
// =====================================================
void gambarLayarPilih(unsigned long sekarang) {
  gambarHeader("PILIH TRIASE");
  gambarAlarm();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(10, 31, "<");
  oled.drawStr(112, 31, ">");
  gambarPilihan(sekarang);
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(
    7,
    47,
    "Tersimpan:"
  );
  oled.drawStr(
    63,
    47,
    namaTriase(triaseAsli)
  );
  oled.drawHLine(
    6,
    51,
    116
  );
  if (triaseAsli == BELUM) {
    gambarLabelTombol(5, 53, "B2", true);
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(26, 62, "Klik:Ganti Tahan:OK");
  } else {
    gambarLabelTombol(5, 53, "B1", false);
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(25, 62, "Back");
    gambarLabelTombol(51, 53, "B2", true);
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(72, 62, "Simpan");
  }
}
// =====================================================
// HALAMAN PEMANTAUAN
// =====================================================
// Center alignment otomatis berdasarkan lebar teks dan font aktif.
void gambarTeksTengah(const char *teks, int y) {
  int lebarTeks = oled.getStrWidth(teks);
  oled.drawStr((128 - lebarTeks) / 2, y, teks);
}

void gambarLayarPantau() {
  gambarHeader("VITRA-SENSE");
  gambarAlarm();
  if (!ppgTerdeteksi) {
    oled.setFont(u8g2_font_6x12_tr);
    gambarTeksTengah("MAX30102 GAGAL", 34);
    oled.setFont(u8g2_font_5x8_tr);
    gambarTeksTengah("Periksa koneksi", 50);
    return;
  }
  if (!ppgFingerDetected()) {
    oled.setFont(u8g2_font_6x12_tr);
    gambarTeksTengah("TEMPELKAN JARI", 34);
    oled.setFont(u8g2_font_5x8_tr);
    gambarTeksTengah("pada Oximeter", 50);
    return;
  }
  if (ppgSedangStabilisasi) {
    unsigned long sudahBerjalan = millis() - mulaiStabilisasiPPG;
    unsigned long sisaMs =
      sudahBerjalan < PPG_STABILIZATION_MS
        ? PPG_STABILIZATION_MS - sudahBerjalan
        : 0;
    unsigned int sisaDetik = (sisaMs + 999UL) / 1000UL;
    // Dua durasi stabilisasi berjalan bersamaan sejak jari terdeteksi:
    // HR dan SpO2: 10 detik; RR: 90 detik (bukan tambahan 90 detik).
    // Susun empat baris agar seluruh teks terbaca pada OLED 128x64.
    oled.setFont(u8g2_font_6x12_tr);
    gambarTeksTengah("TAHAN JARI", 22);
    oled.setFont(u8g2_font_7x13B_tr);
    char teksHitungMundur[8];
    snprintf(teksHitungMundur, sizeof(teksHitungMundur), "%u", sisaDetik);
    gambarTeksTengah(teksHitungMundur, 39);
    oled.setFont(u8g2_font_5x8_tr);
    gambarTeksTengah("Stabilisasi HR/SpO2", 50);
    gambarTeksTengah("RR: tunggu 90 dtk", 61);
    return;
  }
  // ---------------- Label ----------------
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(6, 19, "SpO2%");
  oled.drawStr(56, 19, "HR");
  oled.drawStr(99, 19, "RR");
  // Pemisah SpO2, HR, dan RR.
  oled.drawVLine(43, 13, 24);
  oled.drawVLine(86, 13, 24);

  oled.setFont(u8g2_font_7x13B_tr);
  // Nilai HR/SpO2 hanya muncul jika keduanya memiliki nilai valid.
  if (dataSensorValid) {
    oled.setCursor(8, 35);
    oled.print(spo2);
    oled.setCursor(51, 35);
    oled.print(hr);
  } else {
    oled.drawStr(8, 35, "--");
    oled.drawStr(51, 35, "--");
  }
  // ---------------- Nilai RR ----------------
  // 90 detik dari jari terdeteksi dan RR > 0 diperlukan.
  // Kolom RR berada di area x=87..127, jadi teks dirata-tengah di kolom.
  if (dataSensorValid && rrTersedia) {
    char teksRR[10];
    snprintf(teksRR, sizeof(teksRR), "%d", rr);
    oled.drawStr(87 + (41 - oled.getStrWidth(teksRR)) / 2, 35, teksRR);
  } else {
    const char *simbolMenunggu = "...";
    oled.drawStr(87 + (41 - oled.getStrWidth(simbolMenunggu)) / 2,
                 35, simbolMenunggu);
  }
  // Pembatas data utama dan triase.
  oled.drawHLine(6, 39, 116);
  // ---------------- Alur triase ----------------
  // TriaseRe = BELUM hingga RR tersedia.
  oled.setFont(u8g2_font_6x10_tr);
  char teksTriase[30];
  snprintf(
    teksTriase,
    sizeof(teksTriase),
    "%s -> %s",
    namaTriase(triaseAsli),
    namaTriase(triaseRe)
  );
  int lebarTriase = oled.getStrWidth(teksTriase);
  oled.drawStr(64 - lebarTriase / 2, 51, teksTriase);
  // ---------------- Petunjuk Button 2 ----------------
  gambarLabelTombol(23, 53, "B2", true);
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(45, 62, "Ubah triase");
}
// =====================================================
// STATUS KONEKSI PERANGKAT I2C SAAT STARTUP
// =====================================================
void laporkanStatusMPU(unsigned long sekarang) {
  if (
    sekarang - terakhirStatusMPU <
    MPU_STATUS_INTERVAL_MS
  ) {
    return;
  }
  terakhirStatusMPU = sekarang;
  // Jangan melakukan scan alamat atau membaca WHO_AM_I lagi di sini.
  // Status diambil dari transaksi data akselerasi yang memang dijalankan
  // oleh sampling 100 Hz, sehingga laporan serial tidak menambah beban I2C.
  bool koneksiSekarang =
    mpuTerdeteksi &&
    dataMPUTerakhirValid &&
    (sekarang - terakhirBacaMPUBerhasil <= 1000UL);
  Serial.print("[MPU6050] waktu=");
  Serial.print(sekarang);
  Serial.print(" ms");
  Serial.print(" | alamat=0x68");
  Serial.print(" | status=");
  Serial.print(
    koneksiSekarang
      ? "CONNECTED"
      : "NOT CONNECTED"
  );
  Serial.print(" | data=");
  Serial.print(
    dataMPUTerakhirValid
      ? "VALID"
      : "TIDAK_VALID"
  );
  if (dataMPUTerakhirValid) {
    Serial.print(" | magnitude=");
    Serial.print(
      magnitudeMPUTerakhir,
      3
    );
    Serial.print(" g");
  }
  Serial.print(" | berhasil=");
  Serial.print(jumlahBacaMPUBerhasil);
  Serial.print(" | gagal=");
  Serial.print(jumlahBacaMPUGagal);
  Serial.print(" | gagal_berturut=");
  Serial.println(gagalMPUBerturut);
}
void tampilkanStatusI2C() {
  Serial.println();
  Serial.println("=== STATUS AKHIR I2C ===");
  Serial.print("OLED 0x3C     : ");
  Serial.println(
    oledTerdeteksi ? "CONNECTED" : "NOT CONNECTED"
  );
  Serial.print("MCP23017 0x20 : ");
  Serial.println(
    mcpTerdeteksi ? "CONNECTED" : "NOT CONNECTED"
  );
  Serial.print("MPU6050 0x68  : ");
  Serial.println(
    mpuTerdeteksi ? "CONNECTED" : "NOT CONNECTED"
  );
  Serial.print("MAX30102 0x57 : ");
  Serial.println(
    ppgTerdeteksi ? "CONNECTED" : "NOT CONNECTED"
  );
  // Jika OLED tidak terhubung, hasil pemeriksaan tetap tersedia
  // melalui Serial Monitor pada baud rate 115200.
  if (!oledTerdeteksi) {
    return;
  }
  oled.clearBuffer();
  oled.drawRBox(5, 1, 118, 11, 2);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(28, 10, "I2C STATUS");
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(8, 22, "OLED 0x3C");
  oled.drawStr(
    75,
    22,
    oledTerdeteksi ? "CONNECTED" : "FAILED"
  );
  oled.drawStr(8, 34, "MCP  0x20");
  oled.drawStr(
    75,
    34,
    mcpTerdeteksi ? "CONNECTED" : "FAILED"
  );
  oled.drawStr(8, 46, "MPU  0x68");
  oled.drawStr(
    75,
    46,
    mpuTerdeteksi ? "CONNECTED" : "FAILED"
  );
  oled.drawStr(8, 58, "MAX  0x57");
  oled.drawStr(
    75,
    58,
    ppgTerdeteksi ? "CONNECTED" : "FAILED"
  );
  oled.sendBuffer();
}
// =====================================================
// RENDER OLED
// =====================================================
void gambarOLED(unsigned long sekarang) {
  if (
    !oledTerdeteksi ||
    !layarAktif
  ) {
    return;
  }
  oled.clearBuffer();
  if (sedangMemilih) {
    gambarLayarPilih(sekarang);
  } else {
    gambarLayarPantau();
  }
  oled.sendBuffer();
}
// =====================================================
// MPU6050: SAMPLING 100 Hz + FALL DETECTION
// Dipanggil dari loop(), dan dari layaniSensorSaatLoRa() agar sampling
// tidak terhenti selagi LoRa menunggu kanal kosong.
// =====================================================
void sampelMPU() {
  if (mpuTerdeteksi) {
    uint32_t currentMicros =
      micros();
    if (
      (uint32_t)(
        currentMicros -
        lastSampleUs
      ) >=
      SAMPLE_INTERVAL_US
    ) {
      lastSampleUs +=
        SAMPLE_INTERVAL_US;
      float accelerationMagnitude;
      if (
        readAccelerationMagnitude(
          accelerationMagnitude
        )
      ) {
        magnitudeMPUTerakhir =
          accelerationMagnitude;
        dataMPUTerakhirValid = true;
        terakhirBacaMPUBerhasil = millis();
        gagalMPUBerturut = 0;
        jumlahBacaMPUBerhasil++;
        uint32_t currentMillis =
          millis();
        addSample(
          currentMillis,
          accelerationMagnitude
        );
        if (!emergencyAktif) {
          processFallDetection(
            currentMillis
          );
        }
      } else {
        dataMPUTerakhirValid = false;
        jumlahBacaMPUGagal++;
        if (gagalMPUBerturut < 65535) {
          gagalMPUBerturut++;
        }
        // Jangan memutus status hanya karena satu pembacaan gagal.
        // Sepuluh kegagalan berturut-turut pada 100 Hz berarti sekitar 100 ms.
        if (gagalMPUBerturut >= 10) {
          mpuTerdeteksi = false;
        }
      }
    }
  }
}
// =====================================================
// PELAYANAN SENSOR SAAT LoRa MENUNGGU KANAL
// vitraSend() bisa menunggu sampai beberapa ratus ms. Selama itu FIFO
// MAX30102 (32 sampel = 80 ms pada 400 Hz) harus tetap dikosongkan dan
// MPU6050 tetap disampel.
// =====================================================
void layaniSensorSaatLoRa() {
  if (ppgTerdeteksi && ppgUpdate()) {
    ppgHasilTertunda = true;
  }
  sampelMPU();
}
// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(
    PIN_EMERGENCY,
    INPUT_PULLUP
  );
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_EMERGENCY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_EMERGENCY, HIGH);
  digitalWrite(PIN_BUZZER, BUZZER_OFF);
  Wire.begin(
    PIN_SDA,
    PIN_SCL
  );
  // I2C 400 kHz dipakai bersama OLED, MCP23017, dan MPU6050.
  // Ini membantu mempertahankan sampling MPU6050 100 Hz.
  Wire.setClock(400000);
  Serial.println();
  Serial.println("=== VITRA-SENSE ===");
  oledTerdeteksi =
    perangkatI2CAda(OLED_ADDRESS);
  mcpTerdeteksi =
    perangkatI2CAda(MCP_ADDRESS);
  Serial.print("OLED 0x3C: ");
  Serial.println(
    oledTerdeteksi
      ? "TERDETEKSI"
      : "TIDAK TERDETEKSI"
  );
  Serial.print("MCP23017 0x20: ");
  Serial.println(
    mcpTerdeteksi
      ? "TERDETEKSI"
      : "TIDAK TERDETEKSI"
  );
  if (mcpTerdeteksi) {
    mcpTerdeteksi =
      mcp.begin_I2C(
        MCP_ADDRESS,
        &Wire
      );
    if (mcpTerdeteksi) {
      mcp.pinMode(
        MCP_BUTTON_1,
        INPUT_PULLUP
      );
      mcp.pinMode(
        MCP_BUTTON_2,
        INPUT_PULLUP
      );
      keadaanMCP =
        mcp.readGPIOAB();
    }
  }
  if (oledTerdeteksi) {
    oled.setI2CAddress(
      OLED_ADDRESS * 2
    );
    oled.begin();
    oled.setContrast(220);
    oled.setPowerSave(0);
    layarAktif = true;
  } else {
    layarAktif = false;
  }
  // ---------------------------------------------------
  // MPU6050
  // ---------------------------------------------------
  mpuTerdeteksi = initMPU6050();
  Serial.print("MPU6050: ");
  Serial.println(
    mpuTerdeteksi
      ? "TERDETEKSI"
      : "TIDAK TERDETEKSI"
  );
  if (mpuTerdeteksi) {
    Serial.println("Fall detection aktif.");
    Serial.println("Sampling: 100 Hz");
  } else {
    Serial.println(
      "Fall detection nonaktif, VITRA-Sense tetap berjalan."
    );
  }
  // ---------------------------------------------------
  // MAX30102 / PPG
  // ---------------------------------------------------
  ppgTerdeteksi = ppgBegin();
  Serial.print("MAX30102: ");
  Serial.println(ppgTerdeteksi ? "TERDETEKSI" : "TIDAK TERDETEKSI");

  // ---------------------------------------------------
  // LoRa (VITRA-Sense -> VITRA-Link)
  // Kalau modul tidak terdeteksi, Sense tetap berjalan dan mencoba lagi.
  // ---------------------------------------------------
  vitraLoraSetServiceHook(layaniSensorSaatLoRa);
  vitraLoraBegin(NODE_ID);

  // Tampilkan hasil pemeriksaan semua perangkat I2C selama 3 detik.
  // Setelah itu sistem otomatis masuk ke tampilan utama VITRA-Sense.
  tampilkanStatusI2C();
  delay(3000);
  unsigned long sekarang =
    millis();
  terakhirInteraksi = sekarang;
  terakhirDemo = sekarang;
  terakhirBunyi = sekarang;
  terakhirKedipEmergency = sekarang;
  terakhirGambar = sekarang;
  terakhirBacaMCP = sekarang;
  terakhirStatusMPU = sekarang;
  terakhirCobaMPU = sekarang;
  lastSampleUs = micros();
  lastProcessedCandidateTime = 0;
  lastDetectedPeakTime = 0;
  kirimPaketUji("BOOT");
}
// =====================================================
// LOOP
// =====================================================
void loop() {
  unsigned long sekarang =
    millis();
  // ===================================================
  // MPU6050 SAMPLING + FALL DETECTION
  // ===================================================
  sampelMPU();
  // Jika MPU terlepas atau gagal saat startup, coba inisialisasi ulang
  // maksimal sekali setiap 2 detik. VITRA-Sense lainnya tetap berjalan.
  if (
    !mpuTerdeteksi &&
    sekarang - terakhirCobaMPU >= 2000UL
  ) {
    terakhirCobaMPU = sekarang;
    Serial.println("[MPU6050] Mencoba menghubungkan ulang...");
    mpuTerdeteksi = initMPU6050();
    if (mpuTerdeteksi) {
      dataMPUTerakhirValid = false;
      gagalMPUBerturut = 0;
      lastSampleUs = micros();
      Serial.println("[MPU6050] Terhubung kembali.");
    }
  }
  // Cetak koneksi MPU6050 satu kali setiap detik.
  // Sampling dan algoritma deteksi jatuh tetap berjalan 100 Hz.
  laporkanStatusMPU(sekarang);
  // ---------------------------------------------------
  // Baca seluruh GPIO MCP23017 dalam satu transaksi.
  // ---------------------------------------------------
  if (
    mcpTerdeteksi &&
    sekarang - terakhirBacaMCP >= MCP_POLL_MS
  ) {
    terakhirBacaMCP = sekarang;
    keadaanMCP =
      mcp.readGPIOAB();
  }
  bool bacaanButton1 =
    mcpTerdeteksi
      ? bitRead(
          keadaanMCP,
          MCP_BUTTON_1
        )
      : HIGH;
  bool bacaanButton2 =
    mcpTerdeteksi
      ? bitRead(
          keadaanMCP,
          MCP_BUTTON_2
        )
      : HIGH;
  bool bacaanEmergency =
    digitalRead(PIN_EMERGENCY);
  // Button 2 secara langsung membatalkan izin
  // long-press Button 1.
  if (bacaanButton2 == LOW) {
    tombol1.izinMatikanOLED = false;
  }
  AksiTombol aksi1 =
    perbaruiTombol(
      tombol1,
      bacaanButton1,
      sekarang,
      BUTTON1_HOLD_MS
    );
  AksiTombol aksi2 =
    perbaruiTombol(
      tombol2,
      bacaanButton2,
      sekarang,
      BUTTON2_HOLD_MS
    );
  AksiTombol aksiEmergency =
    perbaruiTombol(
      tombolEmergency,
      bacaanEmergency,
      sekarang,
      EMERGENCY_HOLD_MS
    );
  // ===================================================
  // BUTTON 1
  // ===================================================
  if (aksi1 == DITEKAN) {
    nyalakanLayar();
    tombol1.izinMatikanOLED =
      !sedangMemilih &&
      bacaanButton2 == HIGH;
  }
  if (aksi1 == KLIK) {
    nyalakanLayar();
    if (
      sedangMemilih &&
      triaseAsli != BELUM
    ) {
      // Kembalikan pilihan sementara ke nilai tersimpan.
      pilihan = triaseAsli;
      pilihanSebelumnya = triaseAsli;
      sedangMemilih = false;
      animasiAktif = false;
      Serial.println(
        "BACK: perubahan triase dibatalkan"
      );
    } else {
      Serial.println("OLED aktif");
    }
    tombol1.izinMatikanOLED = false;
  }
  if (aksi1 == TAHAN) {
    if (
      sedangMemilih &&
      triaseAsli != BELUM
    ) {
      // Di halaman pemilihan, tahan Button 1 juga Back.
      pilihan = triaseAsli;
      pilihanSebelumnya = triaseAsli;
      sedangMemilih = false;
      animasiAktif = false;
      nyalakanLayar();
      Serial.println(
        "BACK: perubahan triase dibatalkan"
      );
    } else if (
      tombol1.izinMatikanOLED &&
      !sedangMemilih &&
      bacaanButton2 == HIGH
    ) {
      matikanLayar();
      Serial.println(
        "OLED padam karena Button 1 ditahan"
      );
    }
    tombol1.izinMatikanOLED = false;
  }
  // ===================================================
  // BUTTON 2
  // ===================================================
  if (aksi2 == DITEKAN) {
    nyalakanLayar();
    tombol1.izinMatikanOLED = false;
  }
  if (aksi2 == KLIK) {
    nyalakanLayar();
    tombol1.izinMatikanOLED = false;
    if (!sedangMemilih) {
      sedangMemilih = true;
      pilihan =
        triaseAsli == BELUM
          ? MERAH
          : triaseAsli;
      pilihanSebelumnya = pilihan;
      animasiAktif = false;
      Serial.println(
        "Menu perubahan triase dibuka"
      );
    } else {
      pilihanSebelumnya = pilihan;
      pilihan =
        pilihanBerikutnya(pilihan);
      mulaiAnimasi = sekarang;
      animasiAktif = true;
      Serial.print(
        "Pilihan sementara: "
      );
      Serial.println(
        namaTriase(pilihan)
      );
    }
  }
  if (aksi2 == TAHAN) {
    nyalakanLayar();
    tombol1.izinMatikanOLED = false;
    if (sedangMemilih) {
      triaseAsli = pilihan;
      sedangMemilih = false;
      animasiAktif = false;
      perbaruiRGB();
      Serial.print(
        "Triase disimpan: "
      );
      Serial.println(
        namaTriase(triaseAsli)
      );
      kirimPaketUji(
        "TRIASE_DIUBAH"
      );
    }
  }
  // ===================================================
  // TOMBOL EMERGENCY
  // ===================================================
  if (
    aksiEmergency == KLIK ||
    aksiEmergency == TAHAN
  ) {
    if (!emergencyAktif) {
      // B3: aktifkan emergency manual.
      emergencyAktif = true;
      gerakanDarurat = false;
      vitraLoraSetEmergency(2);   // LoRa: SOS tombol -> LINK
      Serial.println("Emergency AKTIF (MANUAL)");
      kirimPaketUji(
        "EMERGENCY_AKTIF"
      );
    } else {
      // B3: ACK/RESET alarm. Berlaku untuk emergency manual
      // maupun emergency yang berasal dari fall detection.
      emergencyAktif = false;
      gerakanDarurat = false;
      vitraLoraSetEmergency(0);   // LoRa: darurat selesai -> LINK
      Serial.println("Emergency SELESAI / ALARM DI-RESET");
      kirimPaketUji(
        "EMERGENCY_SELESAI"
      );
    }
    nyalakanLayar();
  }
  // ===================================================
  // TIMEOUT OLED
  // ===================================================
  // Ambil waktu terbaru setelah seluruh pemrosesan tombol selesai.
  unsigned long waktuCekTimeout = millis();
  if (
    layarAktif &&
    waktuCekTimeout - terakhirInteraksi >=
    SCREEN_TIMEOUT_MS
  ) {
    matikanLayar();
    Serial.println(
      "OLED otomatis padam setelah 2 menit"
    );
  }
  // ===================================================
  // PROSES SISTEM
  // ===================================================
  perbaruiDataPPG(sekarang);
  // LoRa: terima WACK/RESTART dari Link, kirim data terjadwal dan alert.
  // rr = 0 selama RR belum tersedia.
  vitraLoraLoop(millis(), dataSensorValid, hr, spo2, rrTersedia ? rr : 0);
  perbaruiRGB();
  perbaruiLedEmergency(sekarang);
  perbaruiBuzzer(sekarang);
  // ===================================================
  // REFRESH OLED
  // ===================================================
  // Saat animasi, OLED diperbarui lebih cepat.
  // Di luar animasi, tetap menggunakan refresh normal
  // untuk mengurangi beban bus I2C.
  unsigned long intervalGambar =
    animasiAktif
      ? 30
      : OLED_REFRESH_MS;
  if (
    layarAktif &&
    sekarang - terakhirGambar >=
    intervalGambar
  ) {
    terakhirGambar = sekarang;
    gambarOLED(sekarang);
  }
}
