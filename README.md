# 🌿 OrtoCasa — Sistema Irrigazione Smart

Sistema di irrigazione automatica per orto domestico basato su ESP32 e Raspberry Pi, con interfaccia web, schedulazione avanzata, monitoraggio del flusso e sensore di umidità del suolo.

---

## Architettura

```
[ESP32-WROOM-32U + TB6612FNG + Elettrovalvola bistabile + FS400A + HD-38]
        ↕ MQTT (Wi-Fi LAN, autenticazione user/pass)
[Raspberry Pi — Mosquitto broker]
        ↕
[Raspberry Pi — Flask web app :5000 + SQLite]
        ↕ Browser / Cellulare
```

---

## Hardware

### Componenti

| Componente | Descrizione |
|-----------|-------------|
| ESP32-WROOM-32U | Microcontrollore principale con Wi-Fi |
| TB6612FNG | Driver doppio motore — pilota la valvola bistabile |
| Elettrovalvola bistabile | Valvola on/off, mantiene la posizione senza corrente |
| FS400A | Flussometro — conta i litri erogati |
| HD-38 | Sensore umidità suolo resistivo (solo per test) |
| Raspberry Pi | Server MQTT + Web App |

### Collegamento PIN ESP32

#### TB6612FNG
| TB6612FNG | ESP32 | Funzione |
|-----------|-------|----------|
| AIN1 | GPIO 26 | Apre valvola |
| AIN2 | GPIO 27 | Chiude valvola |
| PWMA | GPIO 25 | Enable canale A (fisso HIGH) |
| STBY | GPIO 14 | Standby (fisso HIGH = attivo) |
| VCC | 3.3V | Alimentazione logica |
| VMOT | 5V | Alimentazione motore |
| GND / PGND | GND | Massa |

#### FS400A (flussometro)
| FS400A | ESP32 |
|--------|-------|
| VCC | 5V |
| GND | GND |
| Signal | GPIO 34 |

#### HD-38 (sensore suolo)
| HD-38 | ESP32 |
|-------|-------|
| VCC | 3.3V |
| GND | GND |
| DO | GPIO 32 |
| AO | GPIO 33 |

> ⚠️ Il sensore HD-38 si corrode rapidamente — consigliato solo per test. Per uso permanente preferire un sensore RS485 capacitivo.

---

## Software

### Struttura repository

```
├── .github/workflows/build-firmware.yml   # CI/CD — compila e pubblica automaticamente
├── docs/schema_collegamento.html          # Schema elettrico interattivo
├── firmware/version.json                  # Versione corrente (aggiornata da Actions)
├── irrigazione_esp32/
│   ├── src/main.cpp                       # Firmware principale
│   ├── src/config.h                       # Credenziali (in .gitignore)
│   ├── config.example.h                   # Template configurazione
│   └── platformio.ini                     # Configurazione PlatformIO
├── irrigazione_raspberry/
│   ├── templates/index.html               # Interfaccia web
│   ├── app.py                             # Backend Flask
│   ├── irrigazione.db                     # Database SQLite (generato automaticamente)
│   ├── requirements.txt                   # Dipendenze Python
│   └── .env / .env.example               # Variabili d'ambiente
├── .gitignore
└── README.md
```

### Configurazione ESP32

Copia `config.example.h` in `config.h` e compila:

```cpp
#define WIFI_SSID       "tua_rete"
#define WIFI_PASSWORD   "tua_password"
#define MQTT_BROKER     "192.168.1.250"
#define MQTT_PORT       1883
#define MQTT_USER       "utente"
#define MQTT_PASS       "password"
#define GITHUB_USER     "o1gres"
#define GITHUB_REPO     "OrtoCasa"
```

### Configurazione Raspberry Pi

```bash
cd ~/OrtoCasa/irrigazione_raspberry
cp .env.example .env
# Modifica .env con le tue credenziali MQTT
```

Installa e avvia il servizio:

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
sudo systemctl enable irrigazione
sudo systemctl start irrigazione
```

---

## MQTT Topics

| Topic | Direzione | Descrizione |
|-------|-----------|-------------|
| `irrigazione/cmd` | Web → ESP | Comandi: `open`, `close`, `status`, `ota`, `reset_flow` |
| `irrigazione/schedule` | Web → ESP | Programmazione (retained) |
| `irrigazione/status` | ESP → Web | Stato valvola, flusso, suolo (retained) |
| `irrigazione/heartbeat` | ESP → Web | Online/offline, uptime, ora ESP (retained) |
| `irrigazione/flow` | ESP → Web | Dati flusso ogni 5 secondi |
| `irrigazione/soil` | ESP → Web | Umidità suolo ogni 10 secondi |
| `irrigazione/ota` | ESP → Web | Stato aggiornamento firmware |
| `irrigazione/schedule_debug` | ESP → Web | Debug schedulazione ogni 60 secondi |

---

## Funzionalità

### Controllo valvola
- Apertura/chiusura manuale da interfaccia web
- Impulso bistabile da 300ms (mantiene posizione senza corrente)
- Chiusura forzata automatica alle 22:00 ogni giorno

### Schedulazione
- **Giorni fissi** — configura fasce mattina/sera per ogni giorno della settimana
- **Giorno sì/no** — irrigazione a giorni alterni con fasce comuni
- Ogni giorno può avere mattina e sera abilitati indipendentemente

### Monitoraggio acqua
- Conteggio litri per sessione
- Storico giornaliero su SQLite
- Totale stagionale con reset manuale
- Rilevamento perdite (flusso con valvola chiusa)

### Sensore suolo
- Lettura percentuale umidità (0-100%)
- Stato digitale bagnato/secco
- Barra colorata nell'interfaccia (ambra → verde)
- Calibrazione con valori `SOIL_DRY` e `SOIL_WET`

### OTA (Over The Air)
- Check automatico ogni ora
- Aggiornamento forzato via comando MQTT
- Pipeline CI/CD con GitHub Actions
- Versioning semantico (es. v2.1.9)

---

## Rilascio nuova versione firmware

```bash
# 1. Modifica FIRMWARE_VERSION in main.cpp
# 2. Committa e pusha
git add .
git commit -m "feat: descrizione modifica, vX.Y.Z"
git push origin master

# 3. Crea il tag — GitHub Actions compila automaticamente
git tag vX.Y.Z
git push origin vX.Y.Z

# 4. Forza OTA sull'ESP32
mosquitto_pub -h localhost -u utente -P password \
  -t "irrigazione/cmd" -m '{"cmd":"ota"}'
```

### Flash manuale via USB (emergenza)

```bash
# Scarica il binario
wget https://github.com/o1gres/OrtoCasa/releases/latest/download/irrigazione_esp32.bin

# Flash con esptool (Windows: COM5, Linux: /dev/ttyUSB0)
python -m esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 \
  write_flash -z 0x10000 irrigazione_esp32.bin
```

---

## Calibrazione flussometro FS400A

Il valore `PULSES_PER_LITER` va calibrato con il tuo impianto:

```cpp
const float PULSES_PER_LITER = 100.0;  // da calibrare
```

Procedura: lascia scorrere 10 litri misurati con un secchio e conta i pulse. Dividi pulse per litri.

---

## Calibrazione sensore suolo HD-38

```cpp
const int SOIL_DRY = 2688;  // valore ADC con sensore asciutto
const int SOIL_WET = 1100;  // valore ADC con sensore in acqua
```

Procedura:
1. Sensore asciutto in aria → annota il valore `Raw` da MQTT
2. Sensore immerso in acqua → annota il valore `Raw`
3. Aggiorna le costanti e fai OTA

---

## Versioni firmware

| Versione | Descrizione |
|----------|-------------|
| v1.0.1 | Versione base |
| v1.2.0 | Flussometro FS400A, storico SQLite, rilevamento perdite |
| v2.0.0 | Scheduler avanzato giorni fissi + giorno sì/no |
| v2.1.0 | Sensore suolo HD-38 |
| v2.1.2 | Debug schedule su MQTT |
| v2.1.5 | Fix buffer MQTT 2048 per payload schedule |
| v2.1.7 | Ora ESP in heartbeat, debug mqtt_callback |
| v2.1.8 | Chiusura forzata valvola alle 22:00 |
| v2.1.9 | Supporto TB6612FNG |

---

## Dipendenze

### ESP32 (PlatformIO)
```ini
lib_deps =
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7.0
```

### Raspberry Pi (Python)
```
flask
paho-mqtt
python-dotenv
```

---

## Note

- Il Raspberry Pi ha IP fisso `192.168.1.250`
- Il broker Mosquitto richiede autenticazione
- Il servizio Flask gira su porta `5000`
- I file `config.h` e `.env` sono in `.gitignore` — non vengono mai pushati
- GitHub Actions richiede i secret: `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER`, `MQTT_USER`, `MQTT_PASS`
- In Settings → Actions → General → abilitare "Read and write permissions"
