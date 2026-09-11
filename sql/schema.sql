-- Visora schema.
--
-- Column names and types are the predecessor's, unchanged: an existing
-- deployment's data must keep working after the cutover. Everything here is
-- idempotent, so it can be applied to a fresh database or to one that already
-- holds recordings.

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE IF NOT EXISTS cameras (
  id                   UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name                 VARCHAR(128) NOT NULL,
  rtsp                 VARCHAR(512) NOT NULL,
  state                VARCHAR(32)  NOT NULL DEFAULT 'offline',
  input_rtsp           VARCHAR(512) NOT NULL DEFAULT '',
  output_rtsp          VARCHAR(512) NOT NULL DEFAULT '',
  codec                VARCHAR(16)  NOT NULL DEFAULT 'unknown',
  hardware             VARCHAR(32)  NOT NULL DEFAULT 'auto',
  recording_enabled    BOOLEAN      NOT NULL DEFAULT false,
  recording_mode       VARCHAR(16)  NOT NULL DEFAULT 'off',
  motion_enabled       BOOLEAN      NOT NULL DEFAULT false,
  motion_sensitivity   DOUBLE PRECISION NOT NULL DEFAULT 0.5,
  motion_threshold     DOUBLE PRECISION NOT NULL DEFAULT 0.01,
  pre_motion_seconds   INTEGER      NOT NULL DEFAULT 10,
  post_motion_seconds  INTEGER      NOT NULL DEFAULT 20,
  segment_seconds      INTEGER      NOT NULL DEFAULT 10,
  motion_keyframe_only BOOLEAN      NOT NULL DEFAULT false,
  motion_grid_x        INTEGER      NOT NULL DEFAULT 32,
  motion_grid_y        INTEGER      NOT NULL DEFAULT 32,
  motion_cell_levels   TEXT         NOT NULL DEFAULT '',
  motion_zones         TEXT         NOT NULL DEFAULT '',
  motion_save_events   BOOLEAN      NOT NULL DEFAULT true,
  retention_days       INTEGER      NOT NULL DEFAULT 0,
  retry_count          INTEGER      NOT NULL DEFAULT 0,
  last_error           TEXT         NOT NULL DEFAULT '',
  last_changed_at      VARCHAR(32)  NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS motion_events (
  id          UUID             PRIMARY KEY DEFAULT gen_random_uuid(),
  camera_id   UUID             NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  start_at    TIMESTAMPTZ      NOT NULL,
  end_at      TIMESTAMPTZ,
  max_score   DOUBLE PRECISION NOT NULL DEFAULT 0,
  cells       TEXT             NOT NULL DEFAULT '',
  grid_x      INTEGER          NOT NULL DEFAULT 0,
  grid_y      INTEGER          NOT NULL DEFAULT 0,
  image_path  TEXT             NOT NULL DEFAULT '',
  created_at  TIMESTAMPTZ      NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_motion_events_camera_time
  ON motion_events(camera_id, start_at, end_at);

CREATE TABLE IF NOT EXISTS recording_segments (
  id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
  camera_id       UUID        NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  path            TEXT        NOT NULL,
  start_at        TIMESTAMPTZ NOT NULL,
  end_at          TIMESTAMPTZ NOT NULL,
  duration_ms     INTEGER     NOT NULL,
  codec           VARCHAR(16) NOT NULL,
  container       VARCHAR(16) NOT NULL,
  recording_mode  VARCHAR(16) NOT NULL,
  has_motion      BOOLEAN     NOT NULL DEFAULT false,
  motion_event_id UUID        REFERENCES motion_events(id) ON DELETE SET NULL,
  status          VARCHAR(16) NOT NULL DEFAULT 'complete',
  -- When the recording SESSION that produced this segment started, epoch ms.
  -- A new session is a new muxer whose clock restarts, so two segments that are
  -- adjacent in wall-clock time can still have a PTS jump between them. The
  -- playlist builder inserts EXT-X-DISCONTINUITY when this changes; wall-clock
  -- comparison alone cannot detect it, because the gap can be milliseconds.
  session_start   BIGINT      NOT NULL DEFAULT 0,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_time
  ON recording_segments(camera_id, start_at, end_at);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_motion_time
  ON recording_segments(camera_id, has_motion, start_at);
-- UNIQUE on path is what makes the writer's upsert possible: splitmuxsink
-- reports a file opening and later closing, and the two messages share only the
-- name. Without this, a restart produces a second row for one file.
CREATE UNIQUE INDEX IF NOT EXISTS idx_recording_segments_path
  ON recording_segments(path);

-- AI jobs. One row is one whole stage TREE.
--
-- `stages` is jsonb rather than a table of stages because a stage's `parent` is
-- an index into the array — it means nothing outside it. A row is read and
-- written whole, which is also how the engine loads it.
--
-- The stored shape is documented in src/store/AiJobJson.hpp. Note that
-- `inputClasses` and `classFilter` inside it are comma-separated STRINGS, not
-- arrays: the column is shared with the predecessor, which reads it that way.
--
-- MUST come after `cameras`: the foreign key needs it.
--
-- A database still carrying the pre-2026 columns (model_path, model_type_2 and
-- the rest) needs the predecessor's sql/init.sql run once to fold them into
-- `stages`. That migration is not repeated here, because a migration nobody can
-- test against a real old database is worse than a pointer to the one that was.
CREATE TABLE IF NOT EXISTS ai_jobs (
  id          UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name        VARCHAR(128) NOT NULL,
  camera_id   UUID         NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  enabled     BOOLEAN      NOT NULL DEFAULT true,
  max_fps     INTEGER      NOT NULL DEFAULT 0,
  stages      JSONB        NOT NULL DEFAULT '[]'::jsonb,
  created_at  TIMESTAMPTZ  NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_ai_jobs_camera ON ai_jobs(camera_id);
CREATE INDEX IF NOT EXISTS idx_ai_jobs_enabled ON ai_jobs(enabled);

-- Per-camera transcode bitrate for browser viewers, in kbit/s. 0 follows what
-- the camera itself sends, which is right unless the VIEWER's link is the
-- constraint rather than the camera's. Added after the first deployments, so
-- idempotently.
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS stream_bitrate_kbps INTEGER NOT NULL DEFAULT 0;
