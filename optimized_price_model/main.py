import logging
import signal
import sys
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from supabase import create_client, Client
from dotenv import load_dotenv
from flask import Flask
import json
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

RUN_INTERVAL  = int(os.getenv("RUN_INTERVAL_SECONDS", 300))  # default: every 5 minutes

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
mqtt_client = None  # set in main()

def compute_condition_factor(temperature, humidity, t_min, t_max, h_min, h_max) -> float:
    """
    Returns a factor between 0.5 and 1.0 based on how far
    temperature and humidity are from ideal thresholds.
    """
    t_ok = t_min <= temperature <= t_max if temperature is not None else False
    h_ok = h_min <= humidity    <= h_max if humidity    is not None else False

    if t_ok and h_ok:
        return 1.0
    elif t_ok or h_ok:
        return 0.75  # one condition out of range
    else:
        return 0.5   # both out of range


def compute_price(base_price, freshness, condition_factor) -> float:
    """
    Price range: base_price (worst) to base_price + max_bonus (best).
    max_bonus is set so that ideal conditions yield ~13.00.
    """
    max_bonus = 9.0  # 4.00 + 9.00 = 13.00 at perfect conditions
    recommended = base_price + (max_bonus * freshness * condition_factor)
    return round(recommended, 2)


def publish_flag(sensor_device_id: str, flag: bool):
    """Push LED flag to the sensor device via MQTT."""
    if mqtt_client and mqtt_client.is_connected():
        topic = f"devices/{sensor_device_id}/commands"
        payload = json.dumps({"flag": flag})
        mqtt_client.publish(topic, payload)
        log.info(f"Published flag={flag} to {topic}")
    else:
        log.warning("MQTT client not connected, skipping flag publish")


def run_price_optimization():
    log.info("Running price optimization cycle...")

    # Fetch all groups that have a product and at least some data
    try:
        res = supabase.table("groups").select(
            "group_id, store_id, product_id, sensor_device_id, "
            "latest_temperature, latest_humidity, latest_freshness"
        ).execute()
        groups = res.data
    except Exception as e:
        log.error(f"Failed to fetch groups: {e}")
        return

    for group in groups:
        group_id         = group.get("group_id")
        store_id         = group.get("store_id")
        product_id       = group.get("product_id")
        sensor_device_id = group.get("sensor_device_id")
        temperature      = group.get("latest_temperature")
        humidity         = group.get("latest_humidity")
        freshness        = group.get("latest_freshness")

        if not product_id:
            log.info(f"Group {group_id} has no product assigned, skipping")
            continue

        if freshness is None:
            log.info(f"Group {group_id} has no freshness data yet, skipping")
            continue

        # Fetch product info
        try:
            prod_res = supabase.table("product_info").select(
                "base_price, T_min, T_max, H_min, H_max"
            ).eq("product_id", product_id).single().execute()
            product = prod_res.data
        except Exception as e:
            log.warning(f"Could not fetch product info for group {group_id}: {e}")
            continue

        base_price = product.get("base_price", 4.0)
        t_min      = product.get("T_min")
        t_max      = product.get("T_max")
        h_min      = product.get("H_min")
        h_max      = product.get("H_max")

        # Check if conditions are out of range
        t_out = temperature is not None and t_min is not None and t_max is not None and not (t_min <= temperature <= t_max)
        h_out = humidity    is not None and h_min is not None and h_max is not None and not (h_min <= humidity    <= h_max)
        conditions_violated = t_out or h_out

        condition_factor    = compute_condition_factor(temperature, humidity, t_min, t_max, h_min, h_max)
        recommended_price   = compute_price(base_price, freshness, condition_factor)

        log.info(
            f"Group {group_id}: freshness={freshness:.3f}, "
            f"condition_factor={condition_factor}, "
            f"recommended_price={recommended_price}"
        )

        # Insert into optimized_prices
        try:
            supabase.table("optimized_prices").insert({
                "recommended_price": recommended_price,
                "group_id":          group_id,
                "store_id":          store_id,
                "product_id":        product_id,
                "generated_at":      datetime.now(timezone.utc).isoformat(),
            }).execute()
        except Exception as e:
            log.error(f"Failed to insert optimized price for group {group_id}: {e}")
            continue

        # Update groups.recommended_price
        try:
            supabase.table("groups").update({
                "recommended_price": recommended_price,
            }).eq("group_id", group_id).execute()
        except Exception as e:
            log.error(f"Failed to update group recommended_price for {group_id}: {e}")

        # Publish LED flag to sensor device if one is assigned
        if sensor_device_id:
            publish_flag(sensor_device_id, conditions_violated)

    log.info("Price optimization cycle complete")


def optimization_loop():
    while True:
        try:
            run_price_optimization()
        except Exception as e:
            log.error(f"Optimization cycle error: {e}")
        time.sleep(RUN_INTERVAL)


# ----------------------------------------------------------------
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info("Connected to MQTT broker")
    else:
        log.error(f"MQTT connection failed, rc={rc}")


def on_disconnect(client, userdata, rc, properties=None, reasonCode=None):
    log.warning(f"Disconnected from MQTT broker (rc={rc}), will auto-reconnect")


# ----------------------------------------------------------------
def main():
    global mqtt_client

    # Start web server
    threading.Thread(target=run_web, daemon=True).start()
    log.info("Web server started")

    # Start optimization loop
    threading.Thread(target=optimization_loop, daemon=True).start()
    log.info(f"Price optimization loop started (interval: {RUN_INTERVAL}s)")

    # MQTT client for publishing flag commands
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    mqtt_client.tls_set()
    mqtt_client.on_connect    = on_connect
    mqtt_client.on_disconnect = on_disconnect
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)

    def shutdown(sig, frame):
        log.info("Shutting down...")
        mqtt_client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    log.info("Price optimizer running")
    mqtt_client.loop_forever()


if __name__ == "__main__":
    main()