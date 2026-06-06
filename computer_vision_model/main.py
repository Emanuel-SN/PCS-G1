import json
import logging
import signal
import sys
import threading
from io import BytesIO

import paho.mqtt.client as mqtt
import requests
from supabase import create_client, Client
from dotenv import load_dotenv
from flask import Flask
from transformers import AutoImageProcessor, AutoModelForImageClassification
from PIL import Image
import torch
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

MODEL_ID = "ITCleo/banana-maturity-mobile-vit-small"

# ----------------------------------------------------------------
supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

# ----------------------------------------------------------------
log.info(f"Loading model {MODEL_ID}...")
processor = AutoImageProcessor.from_pretrained(MODEL_ID)
model     = AutoModelForImageClassification.from_pretrained(MODEL_ID)
model.eval()
log.info("Model loaded")

# Expected class labels — verify these match the model's config.id2label
LABEL_UNRIPE   = "unripe"
LABEL_RIPE     = "ripe"
LABEL_OVERRIPE = "overripe"

FRESHNESS_WEIGHTS = {
    LABEL_UNRIPE:   0.5,
    LABEL_RIPE:     1.0,
    LABEL_OVERRIPE: 0.1,
}

# ----------------------------------------------------------------
app = Flask(__name__)

@app.get("/health")
def health():
    return {"status": "ok"}

def run_web():
    port = int(os.getenv("PORT", 8080))
    app.run(host="0.0.0.0", port=port)

# ----------------------------------------------------------------
def compute_freshness(scores: dict) -> float:
    """Weighted freshness score from class probabilities."""
    return sum(FRESHNESS_WEIGHTS.get(label, 0) * prob for label, prob in scores.items())


def download_image(storage_bucket: str, storage_path: str) -> Image.Image:
    """Download image from Supabase public storage."""
    url = f"{SUPABASE_URL}/storage/v1/object/public/{storage_bucket}/{storage_path}"
    log.info(f"Downloading image from {url}")
    response = requests.get(url, timeout=15)
    response.raise_for_status()
    return Image.open(BytesIO(response.content)).convert("RGB")


def run_inference(image: Image.Image) -> dict:
    """Run the banana maturity model and return per-class probabilities."""
    inputs  = processor(images=image, return_tensors="pt")
    with torch.no_grad():
        logits = model(**inputs).logits
    probs = torch.softmax(logits, dim=-1)[0]

    id2label = model.config.id2label
    scores = {id2label[i].lower(): float(probs[i]) for i in range(len(probs))}
    log.info(f"Inference scores: {scores}")
    return scores


def handle_captured_image(payload: dict):
    cam_device_id  = payload.get("device_id")
    storage_bucket = payload.get("storage_bucket")
    storage_path   = payload.get("storage_path")
    captured_at    = payload.get("time")
    group_id       = None
    store_id       = None

    # Look up group and store from cam_devices
    try:
        res = supabase.table("cam_devices").select("group_id, store_id").eq("cam_device_id", cam_device_id).single().execute()
        group_id = res.data.get("group_id")
        store_id = res.data.get("store_id")
    except Exception as e:
        log.warning(f"Could not find context for cam {cam_device_id}: {e}")

    # Download and run inference
    try:
        image  = download_image(storage_bucket, storage_path)
        scores = run_inference(image)
    except Exception as e:
        log.error(f"Inference failed for {storage_path}: {e}")
        return

    freshness = compute_freshness(scores)

    # Insert into CV_analysis
    supabase.table("CV_analysis").insert({
        "cam_device_id":  cam_device_id,
        "group_id":       group_id,
        "store_id":       store_id,
        "freshness":      freshness,
        "unripe":         scores.get(LABEL_UNRIPE,   0),
        "ripe":           scores.get(LABEL_RIPE,     0),
        "overripe":       scores.get(LABEL_OVERRIPE, 0),
        "storage_bucket": storage_bucket,
        "storage_path":   storage_path,
        "captured_at":    captured_at,
    }).execute()

    log.info(f"CV analysis inserted — freshness: {freshness:.3f} for group {group_id}")
    # TODO: dispatch price optimization job via Procrastinate once queue is ready


# ----------------------------------------------------------------
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info("Connected to MQTT broker")
        client.subscribe("devices/+/captured_images")
        log.info("Subscribed to devices/+/captured_images")
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

    parts = topic.split("/")
    if len(parts) != 3 or parts[2] != "captured_images":
        return

    handle_captured_image(payload)


# ----------------------------------------------------------------
def main():
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

    log.info("CV worker running")
    client.loop_forever()


if __name__ == "__main__":
    main()
