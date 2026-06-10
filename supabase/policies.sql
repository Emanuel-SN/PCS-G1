-- ============================================================
-- RLS Policies for FreshTrack
--
-- Run this in the Supabase SQL Editor AFTER setup.sql.
--
-- Why this is needed:
--   Supabase enables Row Level Security (RLS) on all new tables
--   by default. Without explicit policies, every query made with
--   the anon key (including from the dashboard) returns 0 rows,
--   even if the data exists.
--
-- This file grants full read/write access to the anon role for
-- every table the dashboard and pipelines use. This is appropriate
-- for an internal MVP with no public-facing auth.
--
-- ============================================================

-- ── Helper: enable RLS on every table (idempotent) ───────────
ALTER TABLE users            ENABLE ROW LEVEL SECURITY;
ALTER TABLE stores           ENABLE ROW LEVEL SECURITY;
ALTER TABLE product_info     ENABLE ROW LEVEL SECURITY;
ALTER TABLE sensor_devices   ENABLE ROW LEVEL SECURITY;
ALTER TABLE cam_devices      ENABLE ROW LEVEL SECURITY;
ALTER TABLE groups           ENABLE ROW LEVEL SECURITY;
ALTER TABLE sensor_readings  ENABLE ROW LEVEL SECURITY;
ALTER TABLE image_captures   ENABLE ROW LEVEL SECURITY;
ALTER TABLE cv_analysis      ENABLE ROW LEVEL SECURITY;
ALTER TABLE optimized_prices ENABLE ROW LEVEL SECURITY;

-- ── Drop existing anon policies (safe to re-run) ─────────────
DO $$ DECLARE
  r RECORD;
BEGIN
  FOR r IN
    SELECT policyname, tablename
    FROM pg_policies
    WHERE schemaname = 'public' AND roles::text LIKE '%anon%'
  LOOP
    EXECUTE format('DROP POLICY IF EXISTS %I ON %I', r.policyname, r.tablename);
  END LOOP;
END $$;

-- ============================================================
-- users
-- ============================================================
CREATE POLICY "anon: select users"
  ON users FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert users"
  ON users FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update users"
  ON users FOR UPDATE TO anon USING (true);

-- ============================================================
-- stores
-- ============================================================
CREATE POLICY "anon: select stores"
  ON stores FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert stores"
  ON stores FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update stores"
  ON stores FOR UPDATE TO anon USING (true);

CREATE POLICY "anon: delete stores"
  ON stores FOR DELETE TO anon USING (true);

-- ============================================================
-- product_info
-- ============================================================
CREATE POLICY "anon: select product_info"
  ON product_info FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert product_info"
  ON product_info FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update product_info"
  ON product_info FOR UPDATE TO anon USING (true);

-- ============================================================
-- sensor_devices
-- ============================================================
CREATE POLICY "anon: select sensor_devices"
  ON sensor_devices FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert sensor_devices"
  ON sensor_devices FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update sensor_devices"
  ON sensor_devices FOR UPDATE TO anon USING (true);

CREATE POLICY "anon: delete sensor_devices"
  ON sensor_devices FOR DELETE TO anon USING (true);

-- ============================================================
-- cam_devices
-- ============================================================
CREATE POLICY "anon: select cam_devices"
  ON cam_devices FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert cam_devices"
  ON cam_devices FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update cam_devices"
  ON cam_devices FOR UPDATE TO anon USING (true);

CREATE POLICY "anon: delete cam_devices"
  ON cam_devices FOR DELETE TO anon USING (true);

-- ============================================================
-- groups
-- ============================================================
CREATE POLICY "anon: select groups"
  ON groups FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert groups"
  ON groups FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update groups"
  ON groups FOR UPDATE TO anon USING (true);

CREATE POLICY "anon: delete groups"
  ON groups FOR DELETE TO anon USING (true);

-- ============================================================
-- sensor_readings
-- ============================================================
CREATE POLICY "anon: select sensor_readings"
  ON sensor_readings FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert sensor_readings"
  ON sensor_readings FOR INSERT TO anon WITH CHECK (true);

-- ============================================================
-- image_captures
-- ============================================================
CREATE POLICY "anon: select image_captures"
  ON image_captures FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert image_captures"
  ON image_captures FOR INSERT TO anon WITH CHECK (true);

-- ============================================================
-- cv_analysis
-- ============================================================
CREATE POLICY "anon: select cv_analysis"
  ON cv_analysis FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert cv_analysis"
  ON cv_analysis FOR INSERT TO anon WITH CHECK (true);

-- ============================================================
-- optimized_prices
-- ============================================================
CREATE POLICY "anon: select optimized_prices"
  ON optimized_prices FOR SELECT TO anon USING (true);

CREATE POLICY "anon: insert optimized_prices"
  ON optimized_prices FOR INSERT TO anon WITH CHECK (true);

CREATE POLICY "anon: update optimized_prices"
  ON optimized_prices FOR UPDATE TO anon USING (true);