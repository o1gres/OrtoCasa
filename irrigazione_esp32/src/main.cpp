/*
 * Sistema Irrigazione ESP32 v2.1.0
 * Hardware: ESP32-WROOM-32U + L9110S + Elettrovalvola bistabile + FS400A + HD-38
 *
 * PIN L9110S:    IA -> GPIO 26, IB -> GPIO 27
 * PIN FS400A:    Segnale -> GPIO 34
 * PIN HD-38:     DO -> GPIO 32, AO -> GPIO 33
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"

#define FIRMWARE_VERSION "2.1.5"
#define OTA_CHECK_INTERVAL 3600000UL

// ── PIN ───────────────────────────────────────────────────────────────────────
const int PIN_IA       = 26;
const int PIN_IB       = 27;
const int PIN_FLOW     = 34;
const int PIN_SOIL_DO  = 32;   // HD-38 digitale
const int PIN_SOIL_AO  = 33;   // HD-38 analogico
const int PULSE_MS     = 300;

// ── Calibrazione sensore suolo ────────────────────────────────────────────────
// Valori da calibrare: misura raw con sensore asciutto e bagnato
// e aggiorna questi valori
const int SOIL_DRY = 2688;   // valore ADC con sensore asciutto
const int SOIL_WET = 1100;    // valore ADC con sensore in acqua

// ── Flussometro ───────────────────────────────────────────────────────────────
const float PULSES_PER_LITER   = 100.0;
const float LEAK_THRESHOLD_LPM = 0.5;

volatile long pulseCount     = 0;
long          lastPulseCount = 0;
float         sessionLiters  = 0.0;
float         flowLPM        = 0.0;
bool          leakAlert      = false;

// ── Sensore suolo ─────────────────────────────────────────────────────────────
int  soilMoisturePct = 0;
int  soilRaw         = 0;
bool soilWet         = false;

// ── MQTT Topics ───────────────────────────────────────────────────────────────
const char* TOPIC_CMD       = "irrigazione/cmd";
const char* TOPIC_SCHEDULE  = "irrigazione/schedule";
const char* TOPIC_STATUS    = "irrigazione/status";
const char* TOPIC_HEARTBEAT = "irrigazione/heartbeat";
const char* TOPIC_OTA       = "irrigazione/ota";
const char* TOPIC_FLOW      = "irrigazione/flow";
const char* TOPIC_SOIL      = "irrigazione/soil";

// ─────────────────────────────────────────────────────────────────────────────

WiFiClient       wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient     mqtt(wifiClient);

bool valveOpen     = false;
bool otaInProgress = false;

// ── Scheduler ─────────────────────────────────────────────────────────────────
char scheduleMode[12] = "fixed";

struct Fascia {
  int  oraInizio, minInizio;
  int  oraFine,   minFine;
  bool abilitata;
};

struct GiornoFisso {
  bool   abilitato;
  Fascia mattina;
  Fascia sera;
};
GiornoFisso giorniFissi[7] = {
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Dom
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Lun
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Mar
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Mer
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Gio
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Ven
  {false, {6,0,6,30,false}, {19,0,19,30,false}},  // Sab
};

struct AlternateSchedule {
  Fascia mattina;
  Fascia sera;
  int startDay, startMonth, startYear;
};
AlternateSchedule altSchedule = {
  {6,0,6,30,false}, {19,0,19,30,false}, 1, 1, 2024
};

// ── Timer ─────────────────────────────────────────────────────────────────────
unsigned long lastHeartbeat     = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastOtaCheck      = 0;
unsigned long lastFlowPublish   = 0;
unsigned long lastFlowCalc      = 0;
unsigned long lastMqttRetry     = 0;
unsigned long lastSoilRead      = 0;
unsigned long lastDebug         = 0;

const unsigned long HEARTBEAT_INTERVAL      = 30000UL;
const unsigned long SCHEDULE_CHECK_INTERVAL = 60000UL;
const unsigned long FLOW_PUBLISH_INTERVAL   = 5000UL;
const unsigned long FLOW_CALC_INTERVAL      = 60000UL;
const unsigned long MQTT_RETRY_INTERVAL     = 10000UL;
const unsigned long SOIL_READ_INTERVAL      = 10000UL;
const unsigned long DEBUG_INTERVAL          = 60000UL;

// ── Forward declarations ──────────────────────────────────────────────────────
void publishStatus();
void closeValve();

// ── Interrupt flussometro ─────────────────────────────────────────────────────
void IRAM_ATTR onFlowPulse() { pulseCount++; }

// ── Valvola ───────────────────────────────────────────────────────────────────
void valveImpulse(bool apri) {
  Serial.printf("[VALVOLA] Impulso %s\n", apri ? "APERTURA" : "CHIUSURA");
  digitalWrite(PIN_IA, apri  ? HIGH : LOW);
  digitalWrite(PIN_IB, !apri ? HIGH : LOW);
  delay(PULSE_MS);
  digitalWrite(PIN_IA, LOW);
  digitalWrite(PIN_IB, LOW);
  valveOpen = apri;
  if (apri) { pulseCount = 0; sessionLiters = 0.0; leakAlert = false; }
  publishStatus();
}

void openValve()  { if (!valveOpen)  valveImpulse(true);  }
void closeValve() { if (valveOpen)   valveImpulse(false); }

// ── Flusso ────────────────────────────────────────────────────────────────────
void calcFlowRate() {
  long cur = pulseCount;
  flowLPM       = (float)(cur - lastPulseCount) / PULSES_PER_LITER;
  sessionLiters = (float)cur / PULSES_PER_LITER;
  lastPulseCount = cur;
  if (!valveOpen && flowLPM > LEAK_THRESHOLD_LPM) {
    leakAlert = true;
    Serial.printf("[PERDITA] %.2f L/min con valvola chiusa!\n", flowLPM);
  } else if (valveOpen) leakAlert = false;
}

void publishFlow() {
  JsonDocument doc;
  doc["flow_lpm"]       = round(flowLPM * 100.0) / 100.0;
  doc["session_liters"] = round(sessionLiters * 100.0) / 100.0;
  doc["valve_open"]     = valveOpen;
  doc["leak_alert"]     = leakAlert;
  char payload[200];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_FLOW, payload, false);
}

// ── Sensore suolo HD-38 ───────────────────────────────────────────────────────
void readSoilSensor() {
  soilRaw = analogRead(PIN_SOIL_AO);
  // map: SOIL_DRY=0%, SOIL_WET=100%
  soilMoisturePct = map(soilRaw, SOIL_DRY, SOIL_WET, 0, 100);
  soilMoisturePct = constrain(soilMoisturePct, 0, 100);
  soilWet         = (digitalRead(PIN_SOIL_DO) == LOW);

  Serial.printf("[SUOLO] Raw: %d  Umidità: %d%%  Stato: %s\n",
    soilRaw, soilMoisturePct, soilWet ? "BAGNATO" : "SECCO");

  JsonDocument doc;
  doc["moisture_pct"] = soilMoisturePct;
  doc["moisture_raw"] = soilRaw;
  doc["wet"]          = soilWet;
  char payload[128];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_SOIL, payload, false);
}

// ── OTA ───────────────────────────────────────────────────────────────────────
String getOtaVersionUrl() {
  return String("https://raw.githubusercontent.com/")
       + GITHUB_USER + "/" + GITHUB_REPO + "/master/firmware/version.json";
}
String getOtaBinUrl() {
  return String("https://github.com/")
       + GITHUB_USER + "/" + GITHUB_REPO
       + "/releases/latest/download/irrigazione_esp32.bin";
}

void publishOtaStatus(const char* stato, const char* msg = "") {
  JsonDocument doc;
  doc["stato"]   = stato;
  doc["version"] = FIRMWARE_VERSION;
  if (strlen(msg) > 0) doc["msg"] = msg;
  char payload[200];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_OTA, payload, false);
}

bool isNewerVersion(const char* remote, const char* local) {
  int rMaj=0,rMin=0,rPat=0,lMaj=0,lMin=0,lPat=0;
  sscanf(remote, "%d.%d.%d", &rMaj, &rMin, &rPat);
  sscanf(local,  "%d.%d.%d", &lMaj, &lMin, &lPat);
  if (rMaj != lMaj) return rMaj > lMaj;
  if (rMin != lMin) return rMin > lMin;
  return rPat > lPat;
}

void checkAndUpdate() {
  if (otaInProgress) return;
  publishOtaStatus("checking");
  wifiClientSecure.setInsecure();
  HTTPClient http;
  http.begin(wifiClientSecure, getOtaVersionUrl());
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { publishOtaStatus("error", "HTTP error"); http.end(); return; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) { publishOtaStatus("error", "json error"); return; }
  const char* remoteVer = doc["version"];
  if (!isNewerVersion(remoteVer, FIRMWARE_VERSION)) { publishOtaStatus("up_to_date"); return; }
  publishOtaStatus("updating", remoteVer);
  mqtt.loop();
  closeValve();
  otaInProgress = true;
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[OTA] %d/%d (%.0f%%)\n", cur, total, total>0?(float)cur/total*100:0);
  });
  wifiClientSecure.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(wifiClientSecure, getOtaBinUrl());
  if (ret == HTTP_UPDATE_FAILED) {
    publishOtaStatus("failed", httpUpdate.getLastErrorString().c_str());
    otaInProgress = false;
  }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
void publishStatus() {
  JsonDocument doc;
  doc["valve"]            = valveOpen ? "open" : "closed";
  doc["wifi_rssi"]        = WiFi.RSSI();
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["session_liters"]   = round(sessionLiters * 100.0) / 100.0;
  doc["flow_lpm"]         = round(flowLPM * 100.0) / 100.0;
  doc["leak_alert"]       = leakAlert;
  doc["schedule_mode"]    = scheduleMode;
  doc["soil_moisture"]    = soilMoisturePct;
  doc["soil_raw"]         = soilRaw;
  doc["soil_wet"]         = soilWet;
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    doc["time"] = buf;
  }
  char payload[400];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_STATUS, payload, true);
}

void publishHeartbeat() {
  JsonDocument doc;
  doc["online"]           = true;
  doc["uptime_s"]         = millis() / 1000;
  doc["firmware_version"] = FIRMWARE_VERSION;

  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    doc["time"] = buf;
  }

  char payload[128];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_HEARTBEAT, payload, true);
}

void parseFascia(Fascia& f, JsonObject obj) {
  f.oraInizio = obj["ora_inizio"] | f.oraInizio;
  f.minInizio = obj["min_inizio"] | f.minInizio;
  f.oraFine   = obj["ora_fine"]   | f.oraFine;
  f.minFine   = obj["min_fine"]   | f.minFine;
  f.abilitata = obj["abilitata"]  | f.abilitata;
}

void handleSchedule(const char* payload) {
  Serial.printf("[SCHEDULE] Ricevuto messaggio, lunghezza: %d\n", strlen(payload));
  Serial.printf("[SCHEDULE] Payload: %s\n", payload);
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err != DeserializationError::Ok) {
    Serial.printf("[SCHEDULE] JSON parse error: %s\n", err.c_str());
    return;
  }
  
  Serial.println("[SCHEDULE] JSON parsing OK");
  
  if (doc["mode"].is<const char*>()) {
    strncpy(scheduleMode, doc["mode"], sizeof(scheduleMode) - 1);
    Serial.printf("[SCHEDULE] Mode: %s\n", scheduleMode);
  }
  
  if (doc["days"].is<JsonArray>()) {
    JsonArray days = doc["days"].as<JsonArray>();
    Serial.printf("[SCHEDULE] Days array size: %d\n", days.size());
    for (JsonObject d : days) {
      int idx = d["day"] | -1;
      if (idx < 0 || idx > 6) continue;
      Serial.printf("[SCHEDULE] Day %d: abilitato=%d\n", idx, (bool)d["abilitato"]);
      giorniFissi[idx].abilitato = d["abilitato"] | giorniFissi[idx].abilitato;
      if (d["mattina"].is<JsonObject>()) parseFascia(giorniFissi[idx].mattina, d["mattina"]);
      if (d["sera"].is<JsonObject>()) {
        parseFascia(giorniFissi[idx].sera, d["sera"]);
        Serial.printf("[SCHEDULE]   Sera: %02d:%02d-%02d:%02d (abilitata=%d)\n",
          giorniFissi[idx].sera.oraInizio, giorniFissi[idx].sera.minInizio,
          giorniFissi[idx].sera.oraFine,   giorniFissi[idx].sera.minFine,
          (bool)giorniFissi[idx].sera.abilitata);
      }
    }
  }
  
  if (doc["alternate"].is<JsonObject>()) {
    JsonObject alt = doc["alternate"];
    if (alt["mattina"].is<JsonObject>()) parseFascia(altSchedule.mattina, alt["mattina"]);
    if (alt["sera"].is<JsonObject>())    parseFascia(altSchedule.sera,    alt["sera"]);
    struct tm ti;
    if (getLocalTime(&ti)) {
      altSchedule.startDay   = ti.tm_mday;
      altSchedule.startMonth = ti.tm_mon + 1;
      altSchedule.startYear  = ti.tm_year + 1900;
    }
  }
  
  Serial.println("[SCHEDULE] Parsing completato");
}

void handleCommand(const char* payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  const char* cmd = doc["cmd"];
  if (!cmd) return;
  if (strcmp(cmd, "open")       == 0) openValve();
  if (strcmp(cmd, "close")      == 0) closeValve();
  if (strcmp(cmd, "status")     == 0) publishStatus();
  if (strcmp(cmd, "ota")        == 0) checkAndUpdate();
  if (strcmp(cmd, "reset_flow") == 0) {
    pulseCount = 0; sessionLiters = 0.0; flowLPM = 0.0; leakAlert = false;
    publishFlow();
  }
}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  char payload[2048];
  length = min(length, (unsigned int)2047);
  memcpy(payload, message, length);
  payload[length] = '\0';
  Serial.printf("[MQTT] %s\n", topic);
  if (strcmp(topic, TOPIC_CMD)      == 0) handleCommand(payload);
  if (strcmp(topic, TOPIC_SCHEDULE) == 0) handleSchedule(payload);
}

void mqttConnect() {
  if (mqtt.connected()) return;
  Serial.print("[MQTT] Connessione...");
  String clientId = "ESP32-Irrigazione-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                         TOPIC_HEARTBEAT, 1, true, "{\"online\":false}");
  if (ok) {
    Serial.println(" OK");
    mqtt.subscribe(TOPIC_CMD);
    mqtt.subscribe(TOPIC_SCHEDULE);
    publishStatus();
    publishHeartbeat();
  } else {
    Serial.printf(" Errore %d\n", mqtt.state());
  }
}

// ── Scheduler ─────────────────────────────────────────────────────────────────
bool inFascia(const Fascia& f, int ora, int min) {
  if (!f.abilitata) return false;
  int now  = ora * 60 + min;
  int iniz = f.oraInizio * 60 + f.minInizio;
  int fine = f.oraFine   * 60 + f.minFine;
  return (now >= iniz && now < fine);
}

bool isAlternateDay(const struct tm& ti) {
  struct tm start = {};
  start.tm_year = altSchedule.startYear - 1900;
  start.tm_mon  = altSchedule.startMonth - 1;
  start.tm_mday = altSchedule.startDay;
  time_t t_start = mktime(&start);
  struct tm today = ti;
  today.tm_hour = 0; today.tm_min = 0; today.tm_sec = 0;
  time_t t_today = mktime(&today);
  long diffDays = (long)((t_today - t_start) / 86400);
  return (diffDays % 2 == 0);
}

void publishScheduleDebug() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  
  bool deveEssereAperta = false;
  int wday = ti.tm_wday;
  
  if (strcmp(scheduleMode, "fixed") == 0) {
    if (giorniFissi[wday].abilitato) {
      deveEssereAperta =
        inFascia(giorniFissi[wday].mattina, ti.tm_hour, ti.tm_min) ||
        inFascia(giorniFissi[wday].sera,    ti.tm_hour, ti.tm_min);
    }
  }

  JsonDocument doc;
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d", ti.tm_hour, ti.tm_min);
  doc["ora"]       = timeStr;
  doc["giorno"]    = wday;
  doc["abilitato"] = giorniFissi[wday].abilitato;
  doc["deve_aprire"] = deveEssereAperta;
  doc["sera_ora_inizio"]  = giorniFissi[wday].sera.oraInizio;
  doc["sera_ora_fine"]    = giorniFissi[wday].sera.oraFine;
  doc["sera_min_inizio"]  = giorniFissi[wday].sera.minInizio;
  doc["sera_min_fine"]    = giorniFissi[wday].sera.minFine;
  doc["sera_abilitata"]   = giorniFissi[wday].sera.abilitata;
  doc["mattina_abilitata"] = giorniFissi[wday].mattina.abilitata;

  char payload[400];
  serializeJson(doc, payload);
  mqtt.publish("irrigazione/schedule_debug", payload, false);
}

void checkSchedule() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  bool deveEssereAperta = false;
  if (strcmp(scheduleMode, "fixed") == 0) {
    int wday = ti.tm_wday;
    if (giorniFissi[wday].abilitato) {
      deveEssereAperta =
        inFascia(giorniFissi[wday].mattina, ti.tm_hour, ti.tm_min) ||
        inFascia(giorniFissi[wday].sera,    ti.tm_hour, ti.tm_min);
    }
  } else if (strcmp(scheduleMode, "alternate") == 0) {
    if (isAlternateDay(ti)) {
      deveEssereAperta =
        inFascia(altSchedule.mattina, ti.tm_hour, ti.tm_min) ||
        inFascia(altSchedule.sera,    ti.tm_hour, ti.tm_min);
    }
  }
  if (deveEssereAperta && !valveOpen)  openValve();
  if (!deveEssereAperta && valveOpen)  closeValve();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
void wifiConnect() {
  Serial.printf("[WiFi] Connessione a %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
              IPAddress(8,8,8,8), IPAddress(8,8,4,4));
  Serial.printf("\n[WiFi] Connesso! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ── Setup & Loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.printf("\n=== Sistema Irrigazione ESP32 v%s ===\n", FIRMWARE_VERSION);

  pinMode(PIN_IA, OUTPUT); pinMode(PIN_IB, OUTPUT);
  digitalWrite(PIN_IA, LOW); digitalWrite(PIN_IB, LOW);

  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, FALLING);

  pinMode(PIN_SOIL_DO, INPUT);
  // PIN_SOIL_AO è analogico, non serve pinMode

  wifiConnect();
  configTime(3600, 3600, "pool.ntp.org");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(2048);

  lastOtaCheck = millis() - OTA_CHECK_INTERVAL + 30000UL;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) wifiConnect();

  unsigned long now = millis();

  if (!mqtt.connected() && now - lastMqttRetry >= MQTT_RETRY_INTERVAL) {
    lastMqttRetry = now; mqttConnect();
  }
  if (mqtt.connected()) mqtt.loop();

  if (now - lastSoilRead >= SOIL_READ_INTERVAL) {
    lastSoilRead = now;
    readSoilSensor();
  }
  if (now - lastFlowCalc >= FLOW_CALC_INTERVAL) {
    lastFlowCalc = now; calcFlowRate();
  }
  if (now - lastFlowPublish >= FLOW_PUBLISH_INTERVAL) {
    lastFlowPublish = now;
    if (mqtt.connected()) publishFlow();
  }
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    if (mqtt.connected()) publishHeartbeat();
  }
  if (now - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
    lastScheduleCheck = now; checkSchedule();
  }
  if (now - lastDebug >= DEBUG_INTERVAL) {
    lastDebug = now;
    if (mqtt.connected()) publishScheduleDebug();
  }
  if (now - lastOtaCheck >= OTA_CHECK_INTERVAL) {
    lastOtaCheck = now; checkAndUpdate();
  }
}
