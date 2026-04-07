# 🌿 Irrigazione Orto — Smart Irrigation System

Sistema di irrigazione automatica per orto domestico basato su **ESP32** + **Raspberry Pi**.

```
[ESP32-WROOM-32U + L9110S + Elettrovalvola bistabile]
        ↕ MQTT (Wi-Fi LAN)
[Raspberry Pi — Mosquitto broker]
        ↕
[Raspberry Pi — Flask web app :5000]
        ↕ Browser / Cellulare
[Interfaccia web responsive]
```

---

## ✨ Funzionalità

- **Fasce orarie** mattina + sera configurabili dall'interfaccia web
- **Controllo manuale** apri/chiudi valvola dal browser
- **Stato in tempo reale**: valvola, RSSI Wi-Fi, uptime, versione firmware
- **OTA automatico**: l'ESP32 si aggiorna da solo ad ogni nuova release su GitHub
- **Responsive**: interfaccia ottimizzata per cellulare
- **Architettura ampliabile**: nuove zone, sensori, automazioni

---

## 📁 Struttura del repository

```
├── irrigazione_esp32/
│   ├── irrigazione_esp32.ino   # Firmware ESP32
│   └── config.example.h        # Template credenziali (copia in config.h)
│
├── irrigazione_raspberry/
│   ├── app.py                  # Backend Flask
│   ├── templates/
│   │   └── index.html          # Interfaccia web
│   ├── .env.example            # Template variabili d'ambiente
│   └── requirements.txt        # Dipendenze Python
│
├── firmware/
│   └── version.json            # Versione corrente — aggiornato da GitHub Actions
│
├── docs/
│   └── schema_collegamento.html  # Schema visivo ESP32 + L9110S
│
├── .github/
│   └── workflows/
│       └── build-firmware.yml  # CI/CD: compila e pubblica il .bin automaticamente
│
├── .gitignore
└── README.md
```

---

## 🚀 Setup — Raspberry Pi

### 1. Mosquitto (broker MQTT)

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients

# Configurazione minima
sudo nano /etc/mosquitto/mosquitto.conf
```
Aggiungi:
```
listener 1883
allow_anonymous true
```
```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
```

### 2. App web (Flask)

```bash
# Clona il repository
git clone https://github.com/TUO_USERNAME/NOME_REPO.git
cd NOME_REPO

# Installa dipendenze
pip3 install -r irrigazione_raspberry/requirements.txt

# Copia e configura le variabili d'ambiente
cp irrigazione_raspberry/.env.example irrigazione_raspberry/.env
nano irrigazione_raspberry/.env   # modifica se necessario

# Avvia
cd irrigazione_raspberry
python3 app.py
```

L'interfaccia sarà disponibile su **http://IP_RASPBERRY:5000**

### 3. Avvio automatico al boot

Crea `/etc/systemd/system/irrigazione.service`:
```ini
[Unit]
Description=Irrigazione Orto Web App
After=network.target mosquitto.service

[Service]
User=pi
WorkingDirectory=/home/pi/NOME_REPO/irrigazione_raspberry
ExecStart=/usr/bin/python3 app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```
```bash
sudo systemctl daemon-reload
sudo systemctl enable irrigazione
sudo systemctl start irrigazione
```

---

## 🔧 Setup — ESP32

### Librerie richieste (Arduino Library Manager)
- `PubSubClient` by Nick O'Leary
- `ArduinoJson` by Benoit Blanchon

### Configurazione credenziali

```bash
cp irrigazione_esp32/config.example.h irrigazione_esp32/config.h
```

Modifica `config.h` con i tuoi dati:
```cpp
#define WIFI_SSID      "il_tuo_ssid"
#define WIFI_PASSWORD  "la_tua_password"
#define MQTT_BROKER    "192.168.1.X"   // IP del Raspberry
#define GITHUB_USER    "tuo_username"
#define GITHUB_REPO    "nome_repository"
```

> ⚠️ `config.h` è nel `.gitignore` — non viene mai committato.

Carica lo sketch tramite USB. Dopo il primo caricamento tutti gli aggiornamenti avvengono via OTA.

---

## 📡 Topic MQTT

| Topic                    | Direzione     | Payload esempio |
|--------------------------|---------------|-----------------|
| `irrigazione/cmd`        | Web → ESP32   | `{"cmd":"open"}` / `{"cmd":"close"}` / `{"cmd":"ota"}` |
| `irrigazione/schedule`   | Web → ESP32   | vedi sotto |
| `irrigazione/status`     | ESP32 → Web   | `{"valve":"open","wifi_rssi":-55,"firmware_version":"1.0.0"}` |
| `irrigazione/heartbeat`  | ESP32 → Web   | `{"online":true,"uptime_s":3600,"firmware_version":"1.0.0"}` |
| `irrigazione/ota`        | ESP32 → Web   | `{"stato":"updating","version":"1.1.0"}` |

**Payload schedule:**
```json
{
  "mattina": {"ora_inizio":6,"min_inizio":0,"ora_fine":6,"min_fine":30,"abilitata":true},
  "sera":    {"ora_inizio":19,"min_inizio":0,"ora_fine":19,"min_fine":30,"abilitata":false}
}
```

---

## 🔄 OTA — Aggiornamenti firmware

Il sistema usa **OTA via HTTP pull da GitHub Releases**. Il flusso è completamente automatico:

```
git tag v1.1.0 && git push origin v1.1.0
        ↓
GitHub Actions compila il firmware
        ↓
Crea una Release con il .bin allegato
        ↓
Aggiorna firmware/version.json su main
        ↓
ESP32 (entro 1 ora) rileva la nuova versione
        ↓
Scarica il .bin, si flasha e si riavvia ✓
```

### Come rilasciare una nuova versione

1. Aggiorna `#define FIRMWARE_VERSION` nel `.ino` (es. `"1.1.0"`)
2. Fai commit e push
3. Crea il tag:
```bash
git tag v1.1.0
git push origin v1.1.0
```
4. GitHub Actions fa tutto il resto automaticamente.

### Trigger manuale OTA (senza aspettare 1 ora)
```bash
mosquitto_pub -t "irrigazione/cmd" -m '{"cmd":"ota"}'
```

### Configurazione GitHub Secrets richiesti

Vai su **Settings → Secrets and variables → Actions** nel tuo repo e aggiungi:

| Secret | Valore |
|--------|--------|
| `WIFI_SSID` | Nome della tua rete Wi-Fi |
| `WIFI_PASSWORD` | Password Wi-Fi |
| `MQTT_BROKER` | IP del Raspberry Pi |

---

## 🔌 Schema collegamento

| Da (ESP32) | A (L9110S) | Colore | Note |
|------------|------------|--------|------|
| VIN / 5V   | VCC        | Blu    | O alimentazione esterna per valvole 12V |
| GND        | GND        | Nero   | Massa comune obbligatoria |
| GPIO 26    | IA         | Verde  | Impulso apertura (300ms) |
| GPIO 27    | IB         | Giallo | Impulso chiusura (300ms) |
| —          | OA → OB    | Viola  | Fili elettrovalvola bistabile |

Vedi `docs/schema_collegamento.html` per lo schema visivo completo.

---

## 🛠️ Debug

```bash
# Ascolta tutti i topic MQTT
mosquitto_sub -t "irrigazione/#" -v

# Apri manualmente la valvola
mosquitto_pub -t "irrigazione/cmd" -m '{"cmd":"open"}'

# Forza controllo OTA
mosquitto_pub -t "irrigazione/cmd" -m '{"cmd":"ota"}'

# Log app Flask
sudo journalctl -u irrigazione -f
```

---

## 📈 Come aggiungere funzionalità

### Nuova fascia oraria
1. `irrigazione_esp32.ino`: aggiungi `Fascia fasciaMezzogiorno` e includila in `checkSchedule()`
2. `app.py`: aggiungi `"mezzogiorno"` nel dict `state["schedule"]`
3. `index.html`: duplica il blocco card mattina/sera

### Nuova zona di irrigazione
1. Aggiungi nuovi PIN e funzioni valvola nel `.ino`
2. Estendi i topic: `irrigazione/zona2/cmd`
3. Aggiungi sezione nella web app

### Sensore umidità del suolo
1. Leggi ADC sul `.ino` e aggiungilo al payload `publishStatus()`
2. Mostralo nell'interfaccia come nuova pill informativa
3. Opzionale: logica per irrigare solo sotto soglia di umidità

---

## 📄 Licenza

MIT — libero uso personale e modifiche.
