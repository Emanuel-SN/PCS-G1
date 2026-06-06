import json
import logging
import signal
import sys
import threading
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from supabase import create_client, Client
from dotenv import load_dotenv
from flask import Flask
import os

# ----------------------------------------------------------------
load_dotenv()
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)
log = logging.getLogger(__name__)

# ----------------------------------------------------------------
MQTT_BROKER   = os.getenv("MQTT_BROKER")
MQTT_PORT     = int(os.getenv("MQTT_PORT", 8883))
MQTT_USERNAME = os.getenv("MQTT_USERNAME")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")

SUPABASE_URL  = os.getenv("SUPABASE_URL")
SUPABASE_KEY  = os.getenv("SUPABASE_KEY")

# ----------------------------------------------------------------
supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

# ----------------------------------------------------------------
app = Flask(__name__)

@app.get("/health")
def health():
    return {"status": "ok"}

def run_web():
    port = int(os.getenv("PORT", 8080))
    app.run(host="0.0.0.0", port=port)

# ----------------------------------------------------------------
def get_device_context(device_id: str, device_table: str, device_id_col: str) -> dict | None:
    """Returns group_id and store_id for a given device."""
    try:
        res = supabase.table(device_table).select("group_id, store_id").eq(device_id_col, device_id).single().execute()
        return res.data
    except Exception as e:
        log.warning(f"Could not find context for {device_id}: {e}")
        return None

# ----------------------------------------------------------------
def handle_sensordata(device_id: str, payload: dict):
    log.info(f"[sensordata] {device_id}: {payload}")

    ctx = get_device_context(device_id, "sensor_devices", "sensor_device_id")
    if not ctx:
        log.warning(f"[sensordata] No DB entry for sensor device {device_id}, skipping")
        return

    temperature = payload.get("temperature")
    humidity    = payload.get("humidity")
    captured_at = payload.get("time", datetime.now(timezone.utc).isoformat())

    # Insert sensor reading
    supabase.table("sensor_readings").insert({
        "sensor_device_id": device_id,
        "group_id":         ctx["group_id"],
        "store_id":         ctx["store_id"],
        "temperature":      temperature,
        "humidity":         humidity,
        "captured_at":      captured_at,
    }).execute()

    # Update latest values on group
    if ctx["group_id"]:
        supabase.table("groups").update({
            "latest_temperature": temperature,
            "latest_humidity":    humidity,
        }).eq("group_id", ctx["group_id"]).execute()

    log.info(f"[sensordata] Inserted reading for group {ctx['group_id']}")


def handle_captured_images(device_id: str, payload: dict):
    log.info(f"[captured_images] {device_id}: {payload}")

    ctx = get_device_context(device_id, "cam_devices", "cam_device_id")
    if not ctx:
        log.warning(f"[captured_images] No DB entry for cam device {device_id}, skipping")
        return

    storage_bucket = payload.get("storage_bucket")
    storage_path   = payload.get("storage_path")
    captured_at    = payload.get("time", datetime.now(timezone.utc).isoformat())

    # Insert image capture
    supabase.table("image_captures").insert({
        "cam_device_id":  device_id,
        "group_id":       ctx["group_id"],
        "store_id":       ctx["store_id"],
        "storage_bucket": storage_bucket,
        "storage_path":   storage_path,
        "captured_at":    captured_at,
    }).execute()

    log.info(f"[captured_images] Inserted capture for group {ctx['group_id']}")
    # TODO: enqueue CV analysis job here once model is ready


# ----------------------------------------------------------------
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info("Connected to MQTT broker")
        client.subscribe("devices/+/sensordata")
        client.subscribe("devices/+/captured_images")
        log.info("Subscribed to devices/+/sensordata and devices/+/captured_images")
    else:
        log.error(f"MQTT connection failed, rc={rc}")


def on_disconnect(client, userdata, rc, properties=None, reasonCode=None):
    log.warning(f"Disconnected from MQTT broker (rc={rc}), will auto-reconnect")


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except json.JSONDecodeError as e:
        log.error(f"Failed to parse message on {topic}: {e}")
        return

    # Extract device_id from topic: "devices/<device_id>/<type>"
    parts = topic.split("/")
    if len(parts) != 3:
        log.warning(f"Unexpected topic format: {topic}")
        return

    device_id    = parts[1]
    message_type = parts[2]

    if message_type == "sensordata":
        handle_sensordata(device_id, payload)
    elif message_type == "captured_images":
        handle_captured_images(device_id, payload)
    else:
        log.warning(f"Unhandled message type: {message_type}")


# ----------------------------------------------------------------
def main():
    # Start web server in background thread
    threading.Thread(target=run_web, daemon=True).start()
    log.info("Web server started")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.tls_set()

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)

    def shutdown(sig, frame):
        log.info("Shutting down...")
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    log.info("MQTT pipeline running")
    client.loop_forever()


if __name__ == "__main__":
    main()