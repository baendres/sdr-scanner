-- sdr-scanner native (C++) config schema
-- Replaces the YAML config file: this is the single source of truth for scanner
-- configuration, and is read/written live by the control API (no restart needed
-- for changes to take effect).

CREATE TABLE IF NOT EXISTS scanner_settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);

CREATE TABLE IF NOT EXISTS receivers (
    id          TEXT PRIMARY KEY,
    type        TEXT NOT NULL,        -- 'RTL-SDR' | 'SOAPY'
    device_arg  TEXT,
    driver      TEXT,                 -- required for type = 'SOAPY'
    gain        REAL,
    gains_json  TEXT,                 -- optional per-stage gains, e.g. {"LNA":10,"MIX":10,"VGA":10}
    enabled     INTEGER NOT NULL DEFAULT 1,
    sort_order  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS channels (
    id                TEXT PRIMARY KEY,
    freq_hz           INTEGER NOT NULL,
    label             TEXT,
    mode              TEXT NOT NULL DEFAULT 'FM',   -- FM | NFM | AM
    audio_gain_db     REAL NOT NULL DEFAULT 0,
    dwell_time_s      REAL NOT NULL DEFAULT 3.0,
    squelch_threshold REAL NOT NULL DEFAULT -55,
    ctcss_tone_hz     REAL,                          -- NULL = disabled (power-squelch only)
    squelch_noise_margin_db REAL,                    -- NULL = fixed squelch_threshold; set = adaptive (noise floor + margin)
    noise_squelch_threshold_db REAL,                 -- NULL = disabled; set = FM-only reference-band ("hiss") gate
    dmr_slot          INTEGER,                       -- DMR only: 1 or 2 (dual-timeslot repeaters - see native/README.md)
    dmr_talkgroup_filter INTEGER,                    -- DMR only: NULL = unmute for any talkgroup on this slot
    enabled           INTEGER NOT NULL DEFAULT 1,
    disable_until     REAL,
    mute              INTEGER NOT NULL DEFAULT 0,
    solo              INTEGER,                       -- tri-state: NULL = inactive, 0 = muted-by-solo, 1 = soloed
    hold              INTEGER NOT NULL DEFAULT 0,
    sort_order        INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS outputs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    type        TEXT NOT NULL,        -- local | udp | websocket | icecast
    config_json TEXT NOT NULL DEFAULT '{}',
    enabled     INTEGER NOT NULL DEFAULT 1
);
