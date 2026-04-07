/*
 * Sistema Irrigazione ESP32
 * Hardware: ESP32-WROOM-32U + L9110S + Elettrovalvola bistabile + FS400A
 * Protocollo: MQTT su rete locale
 * OTA: HTTP pull da GitHub Releases
 *
 * PIN L9110S:
 *   IA -> GPIO 26  (fase A — apre)
 *   IB -> GPIO 27  (fase B — chiude)
 *
 * PIN FS400A:
 *   Segnale -> GPIO 34
 *   VCC     -> 5V
 *   GND     -> GND
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"

// ── Versione firmware ─────────────────────────────────────────────────────────
#define FIRMWARE_VERSION "1.2.0"

// ── OTA ───────────────────────────────────────────────────────────────────────
#define OTA_CHECK_INTERVAL 3600000UL

// ── PIN ───────────────────────────────────────────────────────────────────────
const int PIN_IA   = 26;
const int PIN_IB   = 27;
const int PIN_FLOW = 34;
const int PULSE_MS = 300;

// ── Flussometro ───────────────────────────────────────────────────────────────
// FS400A: ~5.5 impulsi per litro — calibra con un contenitore da 1L se necessario
const float PULSES_PER_LITER    = 5.5;
const float LEAK_THRESHOLD_LPM  = 0.5;  // L/min sotto cui ignoriamo il flusso

volatile long pulseCount     = 0;
long          lastPulseCount = 0;
float         sessionLiters  = 0.0;
float         flowLPM        = 0.0;
bool          leakAlert      = false;

// ── MQTT Topics ───────────────────────────────────────────────────────────────
const char* TOPIC_CMD       = "irrigazione/cmd";
const char* TOPIC_SCHEDULE  = "irrigazione/schedule";
const char* TOPIC_STATUS    = "irrigazione/status";
const char* TOPIC_HEARTBEAT = "irrigazione/heartbeat";
const char* TOPIC_OTA       = "irrigazione/ota";
const char* TOPIC_FLOW      = "irrigazione/flow";

// ─────────────────────────────────────────────────────────────────────────────

WiFiClient       wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient     mqtt(wifiClient);

bool valveOpen     = false;
bool otaInProgress = false;

struct Fascia {
  int  oraInizio, minInizio;
  int  oraFine,   minFine;
  bool abilitata;
};
Fascia fasciaMattina = {6,  0, 6,  30, false};
Fascia fasciasSera   = {19, 0, 19, 30, false};

unsigned long lastHeartbeat     = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastOtaCheck      = 0;
unsigned long lastFlowPublish   = 0;
unsigned long lastFlowCalc      = 0;
unsigned long lastMqttRetry     = 0;

const unsigned long HEARTBEAT_INTERVAL      = 30000UL;
const unsigned long SCHEDULE_CHECK_INTERVAL = 60000UL;
const unsigned long FLOW_PUBLISH_INTERVAL   = 5000UL;
const unsigned long FLOW_CALC_INTERVAL      = 60000UL;
const unsigned long MQTT_RETRY_INTERVAL     = 10000UL;

// ── Forward declarations ──────────────────────────────────────────────────────
void publishStatus();
void closeValve();

// ── Interrupt flussometro ─────────────────────────────────────────────────────
void IRAM_ATTR onFlowPulse() {
  pulseCount++;
}

// ── Valvola ───────────────────────────────────────────────────────────────────
void valveImpulse(bool apri) {
  Serial.printf("[VALVOLA] Impulso %s\n", apri ? "APERTURA" : "CHIUSURA");
  digitalWrite(PIN_IA, apri  ? HIGH : LOW);
  digitalWrite(PIN_IB, !apri ? HIGH : LOW);
  delay(PULSE_MS);
  digitalWrite(PIN_IA, LOW);
  digitalWrite(PIN_IB, LOW);
  valveOpen = apri;

  // Reset contatore sessione ad ogni apertura
  if (apri) {
    pulseCount    = 0;
    sessionLiters = 0.0;
    leakAlert     = false;
  }

  publishStatus();
}

void openValve()  { if (!valveOpen)  valveImpulse(true);  }
void closeValve() { if (valveOpen)   valveImpulse(false); }

// ── Flusso ────────────────────────────────────────────────────────────────────
void calcFlowRate() {
  long currentPulses = pulseCount;
  long deltaPulses   = currentPulses - lastPulseCount;
  lastPulseCount     = currentPulses;

  flowLPM       = (float)deltaPulses / PULSES_PER_LITER;
  sessionLiters = (float)currentPulses / PULSES_PER_LITER;

  // Rilevamento perdite
  if (!valveOpen && flowLPM > LEAK_THRESHOLD_LPM) {
    leakAlert = true;
    Serial.printf("[PERDITA] Flusso con valvola chiusa: %.2f L/min!\n", flowLPM);
  } else if (valveOpen) {
    leakAlert = false;
  }
}

void publishFlow() {
  JsonDocument doc;
  doc["flow_lpm"]       = round(flowLPM * 100.0) / 100.0;
  doc["session_liters"] = round(sessionLiters * 100.0) / 100.0;
  doc["total_pulses"]   = pulseCount;
  doc["valve_open"]     = valveOpen;
  doc["leak_alert"]     = leakAlert;
  char payload[200];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_FLOW, payload, false);
}

// ── OTA ───────────────────────────────────────────────────────────────────────
String getOtaVersionUrl() {
  return String("https://raw.githubusercontent.com/")
       + GITHUB_USER + "/" + GITHUB_REPO
       + "/master/firmware/version.json";
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
  Serial.printf("[OTA] stato=%s %s\n", stato, msg);
}

bool isNewerVersion(const char* remote, const char* local) {
  int rMaj=0, rMin=0, rPat=0, lMaj=0, lMin=0, lPat=0;
  sscanf(remote, "%d.%d.%d", &rMaj, &rMin, &rPat);
  sscanf(local,  "%d.%d.%d", &lMaj, &lMin, &lPat);
  if (rMaj != lMaj) return rMaj > lMaj;
  if (rMin != lMin) return rMin > lMin;
  return rPat > lPat;
}

void checkAndUpdate() {
  if (otaInProgress) return;
  Serial.println("[OTA] Controllo aggiornamenti...");
  publishOtaStatus("checking");

  wifiClientSecure.setInsecure();
  HTTPClient http;
  http.begin(wifiClientSecure, getOtaVersionUrl());
  http.setTimeout(10000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Errore HTTP %d\n", code);
    publishOtaStatus("error", "impossibile leggere version.json");
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    publishOtaStatus("error", "version.json malformato");
    return;
  }

  const char* remoteVer = doc["version"];
  Serial.printf("[OTA] Remota: %s  Locale: %s\n", remoteVer, FIRMWARE_VERSION);

  if (!isNewerVersion(remoteVer, FIRMWARE_VERSION)) {
    Serial.println("[OTA] Firmware già aggiornato.");
    publishOtaStatus("up_to_date");
    return;
  }

  Serial.printf("[OTA] Nuova versione %s — avvio download...\n", remoteVer);
  publishOtaStatus("updating", remoteVer);
  mqtt.loop();

  closeValve();
  otaInProgress = true;

  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[OTA] %d / %d bytes (%.0f%%)\n",
                  cur, total, total > 0 ? (float)cur/total*100.0f : 0.0f);
  });

  wifiClientSecure.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(wifiClientSecure, getOtaBinUrl());

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA] FALLITO: (%d) %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      publishOtaStatus("failed", httpUpdate.getLastErrorString().c_str());
      otaInProgress = false;
      break;
    case HTTP_UPDATE_NO_UPDATES:
      publishOtaStatus("up_to_date");
      otaInProgress = false;
      break;
    case HTTP_UPDATE_OK:
      break;
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

  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    doc["time"] = buf;
  }
  char payload[300];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_STATUS, payload, true);
}

void publishHeartbeat() {
  JsonDocument doc;
  doc["online"]           = true;
  doc["uptime_s"]         = millis() / 1000;
  doc["firmware_version"] = FIRMWARE_VERSION;
  char payload[128];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_HEARTBEAT, payload, true);
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
    pulseCount = 0; sessionLiters = 0.0;
    flowLPM = 0.0;  leakAlert = false;
    Serial.println("[FLOW] Contatore resettato");
    publishFlow();
  }
}

void handleSchedule(const char* payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  if (doc["mattina"].is<JsonObject>()) {
    fasciaMattina.oraInizio = doc["mattina"]["ora_inizio"] | fasciaMattina.oraInizio;
    fasciaMattina.minInizio = doc["mattina"]["min_inizio"] | fasciaMattina.minInizio;
    fasciaMattina.oraFine   = doc["mattina"]["ora_fine"]   | fasciaMattina.oraFine;
    fasciaMattina.minFine   = doc["mattina"]["min_fine"]   | fasciaMattina.minFine;
    fasciaMattina.abilitata = doc["mattina"]["abilitata"]  | fasciaMattina.abilitata;
  }
  if (doc["sera"].is<JsonObject>()) {
    fasciasSera.oraInizio = doc["sera"]["ora_inizio"] | fasciasSera.oraInizio;
    fasciasSera.minInizio = doc["sera"]["min_inizio"] | fasciasSera.minInizio;
    fasciasSera.oraFine   = doc["sera"]["ora_fine"]   | fasciasSera.oraFine;
    fasciasSera.minFine   = doc["sera"]["min_fine"]   | fasciasSera.minFine;
    fasciasSera.abilitata = doc["sera"]["abilitata"]  | fasciasSera.abilitata;
  }
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
    Serial.printf(" Errore %d, riprovo al prossimo ciclo\n", mqtt.state());
  }
}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  char payload[512];
  length = min(length, (unsigned int)511);
  memcpy(payload, message, length);
  payload[length] = '\0';
  Serial.printf("[MQTT] %s -> %s\n", topic, payload);
  if (strcmp(topic, TOPIC_CMD)      == 0) handleCommand(payload);
  if (strcmp(topic, TOPIC_SCHEDULE) == 0) handleSchedule(payload);
}

// ── Scheduler ─────────────────────────────────────────────────────────────────
bool inFascia(const Fascia& f, int ora, int min) {
  if (!f.abilitata) return false;
  int now  = ora * 60 + min;
  int iniz = f.oraInizio * 60 + f.minInizio;
  int fine = f.oraFine   * 60 + f.minFine;
  return (now >= iniz && now < fine);
}

void checkSchedule() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  bool deveEssereAperta = inFascia(fasciaMattina, ti.tm_hour, ti.tm_min) ||
                          inFascia(fasciasSera,   ti.tm_hour, ti.tm_min);
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

  pinMode(PIN_IA, OUTPUT);
  pinMode(PIN_IB, OUTPUT);
  digitalWrite(PIN_IA, LOW);
  digitalWrite(PIN_IB, LOW);

  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, FALLING);

  wifiConnect();
  configTime(3600, 3600, "pool.ntp.org");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);

  lastOtaCheck = millis() - OTA_CHECK_INTERVAL + 30000UL;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnesso, riconnessione...");
    wifiConnect();
  }

  unsigned long now = millis();

  if (!mqtt.connected() && now - lastMqttRetry >= MQTT_RETRY_INTERVAL) {
    lastMqttRetry = now;
    mqttConnect();
  }
  if (mqtt.connected()) mqtt.loop();

  if (now - lastFlowCalc >= FLOW_CALC_INTERVAL) {
    lastFlowCalc = now;
    calcFlowRate();
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
    lastScheduleCheck = now;
    checkSchedule();
  }
  if (now - lastOtaCheck >= OTA_CHECK_INTERVAL) {
    lastOtaCheck = now;
    checkAndUpdate();
  }
}
