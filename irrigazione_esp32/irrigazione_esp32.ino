/*
 * Sistema Irrigazione ESP32
 * Hardware: ESP32-WROOM-32U + L9110S + Elettrovalvola bistabile
 * Protocollo: MQTT su rete locale
 * OTA: HTTP pull da GitHub Releases
 *
 * PIN L9110S:
 *   IA -> GPIO 26  (fase A — apre)
 *   IB -> GPIO 27  (fase B — chiude)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "config.h"   // credenziali WiFi, MQTT, GitHub — NON committare

// ── Versione firmware (aggiorna ad ogni release) ──────────────────────────────
#define FIRMWARE_VERSION "1.0.0"

// ── URL OTA (costruiti da GITHUB_USER e GITHUB_REPO definiti in config.h) ─────
#define OTA_VERSION_URL  "https://raw.githubusercontent.com/" GITHUB_USER "/" GITHUB_REPO "/main/firmware/version.json"
#define OTA_BIN_URL      "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/irrigazione_esp32.bin"

// Controlla aggiornamenti ogni ora
#define OTA_CHECK_INTERVAL  3600000UL

// ── PIN ponte H L9110S ────────────────────────────────────────────────────────
const int PIN_IA   = 26;
const int PIN_IB   = 27;
const int PULSE_MS = 300;

// ── MQTT Topics ───────────────────────────────────────────────────────────────
const char* TOPIC_CMD       = "irrigazione/cmd";
const char* TOPIC_SCHEDULE  = "irrigazione/schedule";
const char* TOPIC_STATUS    = "irrigazione/status";
const char* TOPIC_HEARTBEAT = "irrigazione/heartbeat";
const char* TOPIC_OTA       = "irrigazione/ota";

// ─────────────────────────────────────────────────────────────────────────────

WiFiClient       wifiClient;
WiFiClientSecure wifiClientSecure;
PubSubClient     mqtt(wifiClient);

bool valveOpen    = false;
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

const unsigned long HEARTBEAT_INTERVAL      = 30000UL;
const unsigned long SCHEDULE_CHECK_INTERVAL = 60000UL;

// ── Forward declarations ──────────────────────────────────────────────────────
void publishStatus();
void closeValve();

// ── Valvola ───────────────────────────────────────────────────────────────────
void valveImpulse(bool apri) {
  Serial.printf("[VALVOLA] Impulso %s\n", apri ? "APERTURA" : "CHIUSURA");
  digitalWrite(PIN_IA, apri  ? HIGH : LOW);
  digitalWrite(PIN_IB, !apri ? HIGH : LOW);
  delay(PULSE_MS);
  digitalWrite(PIN_IA, LOW);
  digitalWrite(PIN_IB, LOW);
  valveOpen = apri;
  publishStatus();
}

void openValve()  { if (!valveOpen)  valveImpulse(true);  }
void closeValve() { if (valveOpen)   valveImpulse(false); }

// ── OTA ───────────────────────────────────────────────────────────────────────
void publishOtaStatus(const char* stato, const char* msg = "") {
  StaticJsonDocument<200> doc;
  doc["stato"]   = stato;
  doc["version"] = FIRMWARE_VERSION;
  if (strlen(msg) > 0) doc["msg"] = msg;
  char payload[200];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_OTA, payload, false);
  Serial.printf("[OTA] stato=%s %s\n", stato, msg);
}

// Confronta versioni "MAJOR.MINOR.PATCH" — true se remota > locale
bool isNewerVersion(const char* remote, const char* local) {
  int rMaj=0, rMin=0, rPat=0;
  int lMaj=0, lMin=0, lPat=0;
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

  wifiClientSecure.setInsecure();   // accetta qualsiasi cert (LAN domestica)

  HTTPClient http;
  http.begin(wifiClientSecure, OTA_VERSION_URL);
  http.setTimeout(10000);
  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Errore HTTP %d sul version.json\n", code);
    publishOtaStatus("error", "impossibile leggere version.json");
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<128> doc;
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

  // Nuova versione trovata
  Serial.printf("[OTA] Nuova versione %s — avvio download...\n", remoteVer);
  publishOtaStatus("updating", remoteVer);
  mqtt.loop();   // assicura che il messaggio venga inviato prima dell'update

  closeValve();  // sicurezza: chiudi prima di riavviarti
  otaInProgress = true;

  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[OTA] %d / %d bytes (%.0f%%)\n",
                  cur, total, total > 0 ? (float)cur/total*100.0f : 0.0f);
  });

  wifiClientSecure.setInsecure();
  t_httpUpdate_return ret = httpUpdate.update(wifiClientSecure, OTA_BIN_URL);

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
      // L'ESP32 si riavvia automaticamente — questo punto non viene raggiunto
      break;
  }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
void publishStatus() {
  StaticJsonDocument<256> doc;
  doc["valve"]            = valveOpen ? "open" : "closed";
  doc["wifi_rssi"]        = WiFi.RSSI();
  doc["firmware_version"] = FIRMWARE_VERSION;

  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    doc["time"] = buf;
  }
  char payload[256];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_STATUS, payload, true);
}

void publishHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["online"]           = true;
  doc["uptime_s"]         = millis() / 1000;
  doc["firmware_version"] = FIRMWARE_VERSION;
  char payload[128];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_HEARTBEAT, payload, true);
}

void handleCommand(const char* payload) {
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  const char* cmd = doc["cmd"];
  if (!cmd) return;
  if (strcmp(cmd, "open")   == 0) openValve();
  if (strcmp(cmd, "close")  == 0) closeValve();
  if (strcmp(cmd, "status") == 0) publishStatus();
  if (strcmp(cmd, "ota")    == 0) checkAndUpdate();  // trigger manuale da MQTT
}

void handleSchedule(const char* payload) {
  StaticJsonDocument<300> doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;

  if (doc.containsKey("mattina")) {
    fasciaMattina.oraInizio = doc["mattina"]["ora_inizio"] | fasciaMattina.oraInizio;
    fasciaMattina.minInizio = doc["mattina"]["min_inizio"] | fasciaMattina.minInizio;
    fasciaMattina.oraFine   = doc["mattina"]["ora_fine"]   | fasciaMattina.oraFine;
    fasciaMattina.minFine   = doc["mattina"]["min_fine"]   | fasciaMattina.minFine;
    fasciaMattina.abilitata = doc["mattina"]["abilitata"]  | fasciaMattina.abilitata;
  }
  if (doc.containsKey("sera")) {
    fasciasSera.oraInizio = doc["sera"]["ora_inizio"] | fasciasSera.oraInizio;
    fasciasSera.minInizio = doc["sera"]["min_inizio"] | fasciasSera.minInizio;
    fasciasSera.oraFine   = doc["sera"]["ora_fine"]   | fasciasSera.oraFine;
    fasciasSera.minFine   = doc["sera"]["min_fine"]   | fasciasSera.minFine;
    fasciasSera.abilitata = doc["sera"]["abilitata"]  | fasciasSera.abilitata;
  }

  Serial.printf("[SCHEDULE] Mattina %02d:%02d-%02d:%02d (%s)\n",
    fasciaMattina.oraInizio, fasciaMattina.minInizio,
    fasciaMattina.oraFine,   fasciaMattina.minFine,
    fasciaMattina.abilitata ? "ON" : "OFF");
  Serial.printf("[SCHEDULE] Sera    %02d:%02d-%02d:%02d (%s)\n",
    fasciasSera.oraInizio, fasciasSera.minInizio,
    fasciasSera.oraFine,   fasciasSera.minFine,
    fasciasSera.abilitata ? "ON" : "OFF");
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

void mqttConnect() {
  while (!mqtt.connected()) {
    Serial.print("[MQTT] Connessione...");
    String clientId = "ESP32-Irrigazione-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = mqtt.connect(clientId.c_str(), nullptr, nullptr,
                           TOPIC_HEARTBEAT, 1, true, "{\"online\":false}");
    if (ok) {
      Serial.println(" OK");
      mqtt.subscribe(TOPIC_CMD);
      mqtt.subscribe(TOPIC_SCHEDULE);
      publishStatus();
      publishHeartbeat();
    } else {
      Serial.printf(" Errore %d, riprovo tra 5s\n", mqtt.state());
      delay(5000);
    }
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

  wifiConnect();
  configTime(3600, 3600, "pool.ntp.org");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqttConnect();

  // Primo OTA check 30s dopo il boot (aspetta rete stabile)
  lastOtaCheck = millis() - OTA_CHECK_INTERVAL + 30000UL;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnesso, riconnessione...");
    wifiConnect();
  }
  if (!mqtt.connected()) mqttConnect();
  mqtt.loop();

  unsigned long now = millis();

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    publishHeartbeat();
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
