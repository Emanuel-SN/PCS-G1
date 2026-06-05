-- ============================================================
-- Full schema setup
-- ============================================================

-- Users
CREATE TABLE users (
  user_id    UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  username   TEXT NOT NULL UNIQUE,
  password   TEXT NOT NULL
);

-- Stores
CREATE TABLE stores (
  store_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id    UUID NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
  store_name TEXT NOT NULL
);

-- Product info
CREATE TABLE product_info (
  product_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  product_name TEXT NOT NULL,
  T_min        FLOAT,
  T_max        FLOAT,
  H_min        FLOAT,
  H_max        FLOAT,
  base_price   FLOAT,
  unit         TEXT  -- kg, g, un, etc
);

-- Sensor devices (no group_id yet, groups references devices)
CREATE TABLE sensor_devices (
  sensor_device_id TEXT PRIMARY KEY,  -- MAC address string
  store_id         UUID REFERENCES stores(store_id) ON DELETE SET NULL,
  led_flag         BOOLEAN NOT NULL DEFAULT FALSE
);

-- Cam devices
CREATE TABLE cam_devices (
  cam_device_id TEXT PRIMARY KEY,  -- MAC address string
  store_id      UUID REFERENCES stores(store_id) ON DELETE SET NULL
);

-- Groups (links everything together)
CREATE TABLE groups (
  group_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  store_id            UUID NOT NULL REFERENCES stores(store_id) ON DELETE CASCADE,
  group_name          TEXT NOT NULL,
  product_id          UUID REFERENCES product_info(product_id) ON DELETE SET NULL,
  sensor_device_id    TEXT REFERENCES sensor_devices(sensor_device_id) ON DELETE SET NULL,
  cam_device_id       TEXT REFERENCES cam_devices(cam_device_id) ON DELETE SET NULL,
  latest_temperature  FLOAT,
  latest_humidity     FLOAT,
  latest_freshness    FLOAT,
  display_price       FLOAT,
  recommended_price   FLOAT
);

-- Back-fill group_id into devices now that groups exists
ALTER TABLE sensor_devices ADD COLUMN group_id UUID REFERENCES groups(group_id) ON DELETE SET NULL;
ALTER TABLE cam_devices     ADD COLUMN group_id UUID REFERENCES groups(group_id) ON DELETE SET NULL;

-- Sensor readings
CREATE TABLE sensor_readings (
  reading_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  sensor_device_id TEXT  NOT NULL REFERENCES sensor_devices(sensor_device_id) ON DELETE CASCADE,
  group_id         UUID  REFERENCES groups(group_id) ON DELETE SET NULL,
  store_id         UUID  REFERENCES stores(store_id) ON DELETE SET NULL,
  temperature      FLOAT NOT NULL,
  humidity         FLOAT NOT NULL,
  captured_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Image captures
CREATE TABLE image_captures (
  capture_id    UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  cam_device_id TEXT NOT NULL REFERENCES cam_devices(cam_device_id) ON DELETE CASCADE,
  group_id      UUID REFERENCES groups(group_id) ON DELETE SET NULL,
  store_id      UUID REFERENCES stores(store_id) ON DELETE SET NULL,
  storage_bucket TEXT NOT NULL,
  storage_path   TEXT NOT NULL,
  captured_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- CV analysis
CREATE TABLE CV_analysis (
  analysis_id   UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  cam_device_id TEXT  NOT NULL REFERENCES cam_devices(cam_device_id) ON DELETE CASCADE,
  group_id      UUID  REFERENCES groups(group_id) ON DELETE SET NULL,
  store_id      UUID  REFERENCES stores(store_id) ON DELETE SET NULL,
  freshness     FLOAT,
  unripe        FLOAT,
  ripe          FLOAT,
  overripe      FLOAT,
  storage_bucket TEXT,
  storage_path   TEXT,
  captured_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Optimized prices
CREATE TABLE optimized_prices (
  price_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  recommended_price FLOAT NOT NULL,
  group_id          UUID REFERENCES groups(group_id) ON DELETE SET NULL,
  store_id          UUID REFERENCES stores(store_id) ON DELETE SET NULL,
  product_id        UUID REFERENCES product_info(product_id) ON DELETE SET NULL,
  generated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ============================================================
-- Indexes for common lookups
-- ============================================================
CREATE INDEX ON sensor_readings (sensor_device_id);
CREATE INDEX ON sensor_readings (group_id);
CREATE INDEX ON image_captures  (cam_device_id);
CREATE INDEX ON image_captures  (group_id);
CREATE INDEX ON CV_analysis     (cam_device_id);
CREATE INDEX ON CV_analysis     (group_id);
CREATE INDEX ON optimized_prices(group_id);

-- ============================================================
-- Storage bucket + policy
-- ============================================================
INSERT INTO storage.buckets (id, name, public)
VALUES ('captured_images', 'captured_images', true);

CREATE POLICY "allow anon uploads"
ON storage.objects
FOR INSERT
TO anon
WITH CHECK (bucket_id = 'captured_images');