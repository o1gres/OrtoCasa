#!/usr/bin/env python3
"""
Backend Flask — Sistema Irrigazione
Gira sul Raspberry Pi, si connette al broker Mosquitto locale.
Salva storico litri su SQLite.
"""

from flask import Flask, render_template, jsonify, request
import paho.mqtt.client as mqtt
import json, threading, time, sqlite3, os
from datetime import datetime, date
from dotenv import load_dotenv

# Carica .env dal percorso assoluto
load_dotenv(os.path.join(os.path.dirname(__file__), '.env'))

app = Flask(__name__)

# ── Configurazione ────────────────────────────────────────────────────────────
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT   = int(os.getenv("MQTT_PORT", 1883))
MQTT_USER   = os.getenv("MQTT_USER", "")
MQTT_PASS   = os.getenv("MQTT_PASS", "")

TOPIC_CMD       = "irrigazione/cmd"
TOPIC_SCHEDULE  = "irrigazione/schedule"
TOPIC_STATUS    = "irrigazione/status"
TOPIC_HEARTBEAT = "irrigazione/heartbeat"
TOPIC_FLOW      = "irrigazione/flow"

DB_PATH = os.path.join(os.path.dirname(__file__), "irrigazione.db")
# ─────────────────────────────────────────────────────────────────────────────

# ── Database SQLite ───────────────────────────────────────────────────────────
def db_connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def db_init():
    with db_connect() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS daily_liters (
                data        TEXT PRIMARY KEY,   -- YYYY-MM-DD
                liters      REAL DEFAULT 0,
                sessions    INTEGER DEFAULT 0
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS season (
                id          INTEGER PRIMARY KEY CHECK (id = 1),
                start_date  TEXT,
                total_liters REAL DEFAULT 0
            )
        """)
        # Riga stagione unica
        conn.execute("""
            INSERT OR IGNORE INTO season (id, start_date, total_liters)
            VALUES (1, date('now'), 0)
        """)
        conn.commit()

db_init()

def db_add_liters(liters: float):
    """Aggiunge litri al giorno corrente e alla stagione."""
    if liters <= 0:
        return
    today = date.today().isoformat()
    with db_connect() as conn:
        conn.execute("""
            INSERT INTO daily_liters (data, liters, sessions)
            VALUES (?, ?, 1)
            ON CONFLICT(data) DO UPDATE SET
                liters   = liters + excluded.liters,
                sessions = sessions + 1
        """, (today, liters))
        conn.execute("""
            UPDATE season SET total_liters = total_liters + ? WHERE id = 1
        """, (liters,))
        conn.commit()

def db_get_daily_history(days: int = 30):
    with db_connect() as conn:
        rows = conn.execute("""
            SELECT data, liters, sessions
            FROM daily_liters
            ORDER BY data DESC
            LIMIT ?
        """, (days,)).fetchall()
    return [dict(r) for r in rows]

def db_get_season():
    with db_connect() as conn:
        row = conn.execute("SELECT * FROM season WHERE id = 1").fetchone()
    return dict(row) if row else {}

def db_reset_season():
    today = date.today().isoformat()
    with db_connect() as conn:
        conn.execute("""
            UPDATE season SET total_liters = 0, start_date = ? WHERE id = 1
        """, (today,))
        conn.commit()

# ── Stato condiviso ───────────────────────────────────────────────────────────
_lock = threading.Lock()
state = {
    "valve":            "unknown",
    "online":           False,
    "last_seen":        None,
    "uptime_s":         0,
    "wifi_rssi":        None,
    "esp_time":         None,
    "firmware_version": None,
    "flow_lpm":         0.0,
    "session_liters":   0.0,
    "leak_alert":       False,
    "schedule": {
        "mattina": {"ora_inizio":6,  "min_inizio":0, "ora_fine":6,  "min_fine":30, "abilitata":False},
        "sera":    {"ora_inizio":19, "min_inizio":0, "ora_fine":19, "min_fine":30, "abilitata":False},
    }
}

# Traccia sessione per salvare i litri a fine irrigazione
_session_start_liters = 0.0
_valve_was_open       = False

# ── MQTT ──────────────────────────────────────────────────────────────────────
def on_connect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Connesso al broker (rc={reason_code})")
    client.subscribe(TOPIC_STATUS)
    client.subscribe(TOPIC_HEARTBEAT)
    client.subscribe(TOPIC_FLOW)

def on_message(client, userdata, msg):
    global _session_start_liters, _valve_was_open
    try:
        payload = json.loads(msg.payload.decode())
    except Exception:
        return

    with _lock:
        if msg.topic == TOPIC_HEARTBEAT:
            state["online"]           = payload.get("online", False)
            state["uptime_s"]         = payload.get("uptime_s", 0)
            state["firmware_version"] = payload.get("firmware_version")
            state["last_seen"]        = datetime.now().isoformat(timespec="seconds")

        elif msg.topic == TOPIC_STATUS:
            state["valve"]            = payload.get("valve", "unknown")
            state["wifi_rssi"]        = payload.get("wifi_rssi")
            state["esp_time"]         = payload.get("time")
            state["firmware_version"] = payload.get("firmware_version")
            state["flow_lpm"]         = payload.get("flow_lpm", 0.0)
            state["session_liters"]   = payload.get("session_liters", 0.0)
            state["leak_alert"]       = payload.get("leak_alert", False)
            state["last_seen"]        = datetime.now().isoformat(timespec="seconds")

        elif msg.topic == TOPIC_FLOW:
            new_session = payload.get("session_liters", 0.0)
            valve_open  = payload.get("valve_open", False)

            state["flow_lpm"]       = payload.get("flow_lpm", 0.0)
            state["session_liters"] = new_session
            state["leak_alert"]     = payload.get("leak_alert", False)

            # Salva i litri quando la valvola si chiude
            if _valve_was_open and not valve_open:
                liters_this_session = new_session - _session_start_liters
                if liters_this_session > 0:
                    db_add_liters(liters_this_session)
                    print(f"[DB] Sessione completata: {liters_this_session:.2f} L salvati")
                _session_start_liters = 0.0

            # Registra inizio sessione
            if valve_open and not _valve_was_open:
                _session_start_liters = new_session

            _valve_was_open = valve_open

def on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[MQTT] Disconnesso (rc={reason_code}), riconnessione...")

def mqtt_thread():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="raspberry-web")
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect
    client.will_set(TOPIC_HEARTBEAT, json.dumps({"online": False}), retain=True)

    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            app.mqtt_client = client
            client.loop_forever()
        except Exception as e:
            print(f"[MQTT] Errore: {e}, riprovo tra 5s")
            time.sleep(5)

mqtt_thread_obj = threading.Thread(target=mqtt_thread, daemon=True)
mqtt_thread_obj.start()

# Watchdog: segna offline se heartbeat manca da 90s
def watchdog_thread():
    while True:
        time.sleep(15)
        with _lock:
            if state["last_seen"]:
                delta = (datetime.now() -
                         datetime.fromisoformat(state["last_seen"])).total_seconds()
                if delta > 90:
                    state["online"] = False

threading.Thread(target=watchdog_thread, daemon=True).start()

# ── Routes API ────────────────────────────────────────────────────────────────
@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/status")
def api_status():
    with _lock:
        return jsonify(state)

@app.route("/api/cmd", methods=["POST"])
def api_cmd():
    data = request.get_json()
    cmd  = data.get("cmd")
    if cmd not in ("open", "close", "status", "ota", "reset_flow"):
        return jsonify({"error": "Comando non valido"}), 400
    try:
        app.mqtt_client.publish(TOPIC_CMD, json.dumps({"cmd": cmd}))
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route("/api/schedule", methods=["GET"])
def api_schedule_get():
    with _lock:
        return jsonify(state["schedule"])

@app.route("/api/schedule", methods=["POST"])
def api_schedule_set():
    data = request.get_json()
    with _lock:
        for fascia in ("mattina", "sera"):
            if fascia in data:
                state["schedule"][fascia].update(data[fascia])
        schedule_payload = state["schedule"].copy()
    try:
        app.mqtt_client.publish(TOPIC_SCHEDULE, json.dumps(schedule_payload), retain=True)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route("/api/flow/history")
def api_flow_history():
    days = request.args.get("days", 30, type=int)
    return jsonify(db_get_daily_history(days))

@app.route("/api/flow/season")
def api_flow_season():
    return jsonify(db_get_season())

@app.route("/api/flow/season/reset", methods=["POST"])
def api_flow_season_reset():
    db_reset_season()
    return jsonify({"ok": True})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
