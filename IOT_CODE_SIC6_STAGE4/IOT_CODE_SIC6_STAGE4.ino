// ===== LIBRARY =====
#include <WiFi.h>            // Untuk koneksi WiFi
#include <HTTPClient.h>      // Untuk kirim data ke server lewat HTTP
#include <ArduinoJson.h>     // Untuk format data JSON
#include <OneWire.h>         // Untuk komunikasi 1 kabel dengan DS18B20
#include <DallasTemperature.h> // Untuk membaca suhu dari sensor DS18B20
#include <NTPClient.h>       // Untuk mendapatkan waktu dari internet
#include <WiFiUdp.h>         // Untuk membantu NTP
#include <ESP32Servo.h>      // Untuk mengendalikan motor servo

// ===== KONFIGURASI =====
const char* ssid = "OOO";             // Nama WiFi
const char* password = "12345678";      // Password WiFi

const char* API_SENSOR_URL = "http://192.168.18.4:5000/sensor";        // URL API untuk mengirim data sensor
const char* API_JADWAL_URL = "http://192.168.18.4:5000/jadwal_pakan";   // URL API untuk mengambil jadwal pakan

// Pin yang digunakan
const int pinRelay = 26;   // Relay untuk pompa
const int pinServo = 18;   // Servo untuk buka tutup pakan
const int pinTrig = 12;    // Ultrasonik TRIG
const int pinEcho = 14;    // Ultrasonik ECHO
const int pinOneWire = 4;  // Pin sensor suhu DS18B20

// ===== OBJEK SENSOR =====
OneWire oneWire(pinOneWire);           // Buat jalur OneWire
DallasTemperature sensors(&oneWire);   // Buat objek sensor suhu
Servo servo;                           // Buat objek motor servo

// Objek untuk ambil waktu internet
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000); // UTC +7 WIB

// ===== STRUKTUR DATA UNTUK JADWAL =====
struct Jadwal {
  int jam;
  int menit;
};

Jadwal jadwalList[10];     // List untuk menyimpan maksimal 10 jadwal pakan
int jumlahJadwal = 0;      // Banyaknya jadwal aktif

// ===== TIMER =====
unsigned long lastSuhuCheck = 0;
unsigned long lastPakanCheck = 0;
unsigned long lastTarikJadwal = 0;
int lastJamEksekusi = -1;
int lastMenitEksekusi = -1;

// ===== FUNGSI =====

// 1. Fungsi untuk koneksi ke WiFi
void konekWifi() {
  WiFi.begin(ssid, password);
  Serial.print("Menyambung ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
}

// 2. Fungsi untuk baca suhu dari DS18B20
float bacaSuhu() {
  sensors.requestTemperatures();
  return sensors.getTempCByIndex(0); // Ambil suhu pertama
}

// 3. Fungsi untuk menggerakkan motor servo
void gerakServo(int angle) {
  int duty = map(angle, 0, 180, 544, 2400); // Konversi sudut ke pulsa servo
  servo.writeMicroseconds(duty);
}

// 4. Fungsi untuk buka tutup pakan beberapa kali
void bukaTutupPakan(int repetisi = 3) {
  for (int i = 0; i < repetisi; i++) {
    gerakServo(90);    // Buka pakan
    delay(2000);       // Tunggu 2 detik
    gerakServo(0);     // Tutup pakan
    delay(2000);       // Tunggu 2 detik
  }
}

// 5. Fungsi untuk baca jarak isi pakan
long jarakSensor() {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);

  long duration = pulseIn(pinEcho, HIGH, 10000); // Baca lama pantulan
  long distance = duration * 0.034 / 2; // Konversi ke cm
  return distance;
}

// 6. Fungsi untuk hitung persen isi pakan
int hitungPersenIsi(long jarakCm) {
  const int TINGGI_WADAH_CM = 10; // Tinggi wadah pakan
  const int JARAK_MIN_CM = 2;     // Batas jarak minimum jika penuh

  if (jarakCm > TINGGI_WADAH_CM) jarakCm = TINGGI_WADAH_CM;
  if (jarakCm < JARAK_MIN_CM) jarakCm = JARAK_MIN_CM;

  int tinggiIsi = TINGGI_WADAH_CM - jarakCm;
  int kapasitasMax = TINGGI_WADAH_CM - JARAK_MIN_CM;
  float persen = (tinggiIsi * 100.0) / kapasitasMax;

  return round(persen);
}

// 7. Fungsi untuk kirim data sensor ke server
void kirimData(float suhu, int persenPakan, int statusPompa) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(API_SENSOR_URL);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["suhu"] = suhu;
    doc["pakan(%)"] = persenPakan;
    doc["pompa"] = statusPompa;

    String body;
    serializeJson(doc, body);
    int httpResponseCode = http.POST(body);

    Serial.println("Kirim ke API, Response: " + String(httpResponseCode));
    http.end();
  }
}

// 8. Fungsi untuk ambil jadwal pakan dari server
void tarikJadwalPakan() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(API_JADWAL_URL);

    int httpResponseCode = http.GET();
    if (httpResponseCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        JsonArray arr = doc.as<JsonArray>();
        jumlahJadwal = 0;
        for (JsonObject obj : arr) {
          if (jumlahJadwal < 10) {
            jadwalList[jumlahJadwal].jam = obj["jam"];
            jadwalList[jumlahJadwal].menit = obj["menit"];

             // === Tambahkan print di sini ===
            Serial.print("Jadwal ke-");
            Serial.print(jumlahJadwal);
            Serial.print(": ");
            Serial.print(jadwalList[jumlahJadwal].jam);
            Serial.print(":");
            Serial.println(jadwalList[jumlahJadwal].menit);
            // ==============================

            jumlahJadwal++;
          }
        }
        Serial.println("Jadwal diperbarui.");
      }
    }
    http.end();
  }
}

// 9. Fungsi untuk cek apakah sekarang waktunya memberi makan
bool cekJadwal(int jam, int menit) {
  for (int i = 0; i < jumlahJadwal; i++) {
    if (jadwalList[i].jam == jam && jadwalList[i].menit == menit) {
      return true;
    }
  }
  return false;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  pinMode(pinRelay, OUTPUT);
  pinMode(pinTrig, OUTPUT);
  pinMode(pinEcho, INPUT);

  servo.attach(pinServo);
  servo.write(0); // Servo di posisi tutup

  konekWifi();      // Sambung WiFi
  sensors.begin();  // Mulai sensor suhu
  timeClient.begin(); // Mulai NTP Client
  timeClient.update(); // Sinkronisasi jam pertama kali

  tarikJadwalPakan(); // Tarik jadwal awal
}

// ===== LOOP =====
void loop() {
  timeClient.update(); // Update jam internet
  int jam = timeClient.getHours();
  int menit = timeClient.getMinutes();

  unsigned long now = millis();

  // 1. Cek suhu tiap 5 detik
  if (now - lastSuhuCheck > 5000) {
    float suhu = bacaSuhu();
    Serial.println("Suhu: " + String(suhu) + " °C");

    // Kalau suhu > 32 derajat, nyalakan pompa (relay)
    if (suhu > 30) {
      digitalWrite(pinRelay, HIGH);
    } else {
      digitalWrite(pinRelay, LOW);
    }
    lastSuhuCheck = now;
  }

  // 2. Cek isi pakan tiap 20 detik
  if (now - lastPakanCheck > 20000) {
    long jarak = jarakSensor();
    int persenPakan = hitungPersenIsi(jarak);
    Serial.println("Pakan tersisa: " + String(persenPakan) + "%");

    kirimData(bacaSuhu(), persenPakan, digitalRead(pinRelay));

    lastPakanCheck = now;
  }

  // 3. Tarik ulang jadwal pakan tiap 1 menit
  if (now - lastTarikJadwal > 10000) {
    tarikJadwalPakan();
    lastTarikJadwal = now;
  }

  // 4. Cek apakah sekarang waktunya memberi makan
  if (cekJadwal(jam, menit) && (jam != lastJamEksekusi || menit != lastMenitEksekusi)) {
    bukaTutupPakan();
    lastJamEksekusi = jam;
    lastMenitEksekusi = menit;
  }

  delay(1000); // Delay kecil supaya tidak terlalu berat kerja CPU
}