-- warc-studio SQLite schema
-- This file is a human-readable reference only.
-- The application manages schema creation and migrations automatically at startup via Database.cpp.
-- To add a new migration: append to kMigrations[] in Database.cpp and bump kCurrentSchemaVersion.

PRAGMA foreign_keys = ON;

-- Tracks the current schema version.
-- Always contains exactly one row.
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

-- Schema version history:
--   1 — initial schema: collection + entry (with warc_path and browsertrix_id)
--   2 — extended schema: new columns on collection/entry, plus archive_file, crawl_run,
--         capture_metadata, tag, entry_tag, entry_note; data migration from warc_path/browsertrix_id

-- ---------------------------------------------------------------------------
-- Core tables
-- ---------------------------------------------------------------------------

CREATE TABLE collection (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL UNIQUE,
    description TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE entry (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    collection_id  INTEGER NOT NULL,
    url            TEXT NOT NULL,
    normalized_url TEXT,
    title          TEXT,
    status         TEXT NOT NULL DEFAULT 'new',  -- new | recording | archived | failed
    note           TEXT,
    created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    archived_at    DATETIME,
    last_error     TEXT,

    -- Legacy columns kept for backward compatibility; prefer archive_file and crawl_run.
    warc_path      TEXT,
    browsertrix_id TEXT,

    FOREIGN KEY (collection_id) REFERENCES collection(id) ON DELETE CASCADE
);

-- ---------------------------------------------------------------------------
-- Archive files — one or more WACZ/WARC files per entry
-- ---------------------------------------------------------------------------

CREATE TABLE archive_file (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id   INTEGER NOT NULL,
    path       TEXT NOT NULL,
    file_type  TEXT NOT NULL DEFAULT 'wacz',
    size_bytes INTEGER,
    sha256     TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);

-- ---------------------------------------------------------------------------
-- Crawl runs — one or more crawl attempts per entry
-- ---------------------------------------------------------------------------

CREATE TABLE crawl_run (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id            INTEGER NOT NULL,
    browsertrix_id      TEXT,
    docker_container_id TEXT,
    status              TEXT NOT NULL DEFAULT 'created',  -- created | running | stopped | finished | failed
    started_at          DATETIME,
    stopped_at          DATETIME,
    exit_code           INTEGER,
    error_message       TEXT,
    log_path            TEXT,

    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);

-- ---------------------------------------------------------------------------
-- Capture metadata — optional per-entry crawl metadata
-- ---------------------------------------------------------------------------

CREATE TABLE capture_metadata (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id        INTEGER NOT NULL,
    final_url       TEXT,
    http_status     INTEGER,
    content_type    TEXT,
    screenshot_path TEXT,
    page_title      TEXT,
    captured_at     DATETIME DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);

-- ---------------------------------------------------------------------------
-- Tags
-- ---------------------------------------------------------------------------

CREATE TABLE tag (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE
);

CREATE TABLE entry_tag (
    entry_id INTEGER NOT NULL,
    tag_id   INTEGER NOT NULL,

    PRIMARY KEY (entry_id, tag_id),
    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id)   REFERENCES tag(id)   ON DELETE CASCADE
);

-- ---------------------------------------------------------------------------
-- Entry notes
-- ---------------------------------------------------------------------------

CREATE TABLE entry_note (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id   INTEGER NOT NULL,
    body       TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,

    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);
