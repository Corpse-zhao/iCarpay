-- One row per Showcase install. The id is a random UUID made on the device.
CREATE TABLE IF NOT EXISTS installs (
  id TEXT PRIMARY KEY,
  first_seen TEXT NOT NULL,
  last_seen TEXT NOT NULL,
  version TEXT,
  package TEXT,
  model TEXT,
  ios TEXT,
  jailbreak TEXT,
  language TEXT,
  country TEXT
);
CREATE INDEX IF NOT EXISTS installs_last_seen ON installs(last_seen);
