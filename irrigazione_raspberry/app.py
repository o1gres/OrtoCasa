#!/usr/bin/env python3
"""
Backend Flask — Sistema Irrigazione
Gira sul Raspberry Pi, si connette al broker Mosquitto locale.
"""

from flask import Flask, render_template, jsonify, request
import paho.mqtt.client as mqtt
import json
import threading
import time
from datetime import datetime
from dotenv import load_dotenv
import os
load_dotenv()

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
# ─────────────────────────────────────────────────────────────────────────────

# Stato condiviso (thread-safe con lock)
_lock = threading.Lock()
state = {
    "valve": "unknown",
    "online": False,
    "last_seen": None,
    "uptime_s": 0,
    "wifi_rssi": None,
    "esp_time": None,
    "schedule": {
        "mattina": {"ora_inizio": 6,  "min_inizio": 0,
                    "ora_fine": 6,   "min_fine": 30,  "abilitata": False},
        "sera":    {"ora_inizio": 19, "min_inizio": 0,
                    "ora_fine": 19,  "min_fine": 30,  "abilitata": False},
    }
}

# ── MQTT client ───────────────────────────────────────────────────────────────
def on_connect(client, userdata, flags, rc):
    print(f"[MQTT] Connesso al broker (rc={rc})")
    client.subscribe(TOPIC_STATUS)
    client.subscribe(TOPIC_HEARTBEAT)

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
    except Exception:
        return

    with _lock:
        if msg.topic == TOPIC_HEARTBEAT:
            state["online"]   = payload.get("online", False)
            state["uptime_s"] = payload.get("uptime_s", 0)
            state["last_seen"] = datetime.now().isoformat(timespec="seconds")

        elif msg.topic == TOPIC_STATUS:
            state["valve"]     = payload.get("valve", "unknown")
            state["wifi_rssi"] = payload.get("wifi_rssi")
            state["esp_time"]  = payload.get("time")
            state["last_seen"] = datetime.now().isoformat(timespec="seconds")

def on_disconnect(client, userdata, rc):
    print(f"[MQTT] Disconnesso (rc={rc}), riconnessione...")

def mqtt_thread():
    client = mqtt.Client(client_id="raspberry-web")
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    # Will message: segna ESP come offline se il broker perde la connessione
    client.will_set(TOPIC_HEARTBEAT, json.dumps({"online": False}), retain=True)

    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
            app.mqtt_client = client
            client.loop_forever()
        except Exception as e:
            print(f"[MQTT] Errore connessione: {e}, riprovo tra 5s")
            time.sleep(5)

# Avvia MQTT in background
mqtt_thread_obj = threading.Thread(target=mqtt_thread, daemon=True)
mqtt_thread_obj.start()

# Timeout: se non riceviamo heartbeat da 90s, segna come offline
def watchdog_thread():
    while True:
        time.sleep(15)
        with _lock:
            if state["last_seen"]:
                delta = (datetime.now() -
                         datetime.fromisoformat(state["last_seen"])).total_seconds()
                if delta > 90:
                    state["online"] = False

watchdog = threading.Thread(target=watchdog_thread, daemon=True)
watchdog.start()

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
    if cmd not in ("open", "close", "status"):
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
        app.mqtt_client.publish(TOPIC_SCHEDULE,
                                json.dumps(schedule_payload),
                                retain=True)  # retained: ESP la riceve al boot
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == "__main__":
    # host="0.0.0.0" → accessibile da tutti i dispositivi nella rete locale
    app.run(host="0.0.0.0", port=5000, debug=False)
