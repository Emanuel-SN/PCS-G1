import json
import logging
import signal
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from io import BytesIO

import paho.mqtt.client as mqtt
import requests
from supabase import create_client, Client
from dotenv import load_dotenv
from flask import Flask
from PIL import Image
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

HF_API_TOKEN  = os.getenv("HF_API_TOKEN")
HF_API_URL    = "https://router.huggingface.co/hf-inference/models/TCleo/banana-maturity-mobile-vit-small"

# ----------------------------------------------------------------
supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

LABEL_UNRIPE   = "underripe"
LABEL_RIPE     = "ripe"
LABEL_OVERRIPE = "overripe"

FRESHNESS_WEIGHTS = {
    LABEL_UNRIPE:   0.5,
    LABEL_RIPE:     1.0,
    LABEL_OVERRIPE: 0.1,
}

# Thread pool — keeps MQTT loop thread free for keepalive pings.
# max_workers=1 processes images in order; raise if you need parallelism.
executor = ThreadPoolExecutor(max_workers=1)

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
    return sum(FRESHNESS_WEIGHTS.get(label, 0) * prob for label, prob in scores.items())


def download_image(storage_bucket: str, storage_path: str) -> Image.Image:
    url = f"{SUPABASE_URL}/storage/v1/object/public/{storage_bucket}/{storage_path}"
    log.info(f"Downloading image from {url}")
    t = time.time()
    response = requests.get(url, timeout=15)
    response.raise_for_status()
    log.info(f"Download took {time.time()-t:.1f}s")
    return Image.open(BytesIO(response.content)).convert("RGB")


def run_inference(image: Image.Image) -> dict:
    """
    Sends the image to the Hugging Face Inference API and returns
    a dict of {label: score}. Runs on HF's GPU-accelerated servers
    instead of local CPU — typically 2-5s vs 80s locally.

    On the free tier the model may have a cold start (~20s) if it
    hasn't been called recently. Set HF_API_TOKEN to a paid account
    to keep it warm, or accept the occasional cold start.
    """
    buffer = BytesIO()
    image.save(buffer, format="JPEG")

    t = time.time()
    response = requests.post(
        HF_API_URL,
        headers={"Authorization": f"Bearer {HF_API_TOKEN}"},
        data=buffer.getvalue(),
        timeout=60,  # generous timeout to handle cold starts
    )

    # If the model is loading (cold start), HF returns 503 with an
    # estimated_time field. We wait and retry once.
    if response.status_code == 503:
        body = response.json()
        wait  = min(float(body.get("estimated_time", 20)), 30)
        log.info(f"Model loading on HF, waiting {wait:.0f}s...")
        time.sleep(wait)
        response = requests.post(
            HF_API_URL,
            headers={"Authorization": f"Bearer {HF_API_TOKEN}"},
            data=buffer.getvalue(),
            timeout=60,
        )

    response.raise_for_status()

    # HF returns: [{"label": "ripe", "score": 0.93}, ...]
    results = response.json()
    log.info(f"Inference took {time.time()-t:.1f}s")
    scores = {item["label"].lower(): item["score"] for item in results}
    log.info(f"Inference scores: {scores}")
    return scores


def handle_captured_image(payload: dict):
    """
    Runs in a thread-pool worker, NOT on the MQTT thread, so the
    MQTT loop stays free to send keepalive pings.
    """
    cam_device_id  = payload.get("device_id")
    storage_bucket = payload.get("storage_bucket")
    storage_path   = payload.get("storage_path")
    captured_at    = payload.get("time")
    group_id       = None
    store_id       = None

    try:
        res = supabase.table("cam_devices").select("group_id, store_id").eq("cam_device_id", cam_device_id).single().execute()
        group_id = res.data.get("group_id")
        store_id = res.data.get("store_id")
    except Exception as e:
        log.warning(f"Could not find context for cam {cam_device_id}: {e}")

    try:
        image  = download_image(storage_bucket, storage_path)
        scores = run_inference(image)
    except Exception as e:
        log.error(f"Inference failed for {storage_path}: {e}")
        return

    freshness = compute_freshness(scores)

    supabase.table("cv_analysis").insert({
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

    if group_id:
        supabase.table("groups").update({
            "latest_freshness": freshness,
        }).eq("group_id", group_id).execute()
        log.info(f"Updated latest_freshness to {freshness:.3f} for group {group_id}")

    log.info(f"CV analysis inserted — freshness: {freshness:.3f} for group {group_id}")


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

    # Submit to thread pool immediately — do NOT block the MQTT thread
    executor.submit(handle_captured_image, payload)


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
        executor.shutdown(wait=False)
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    log.info("CV worker running")
    client.loop_forever()


if __name__ == "__main__":
    main()