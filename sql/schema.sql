-- warc-studio SQLite schema
-- This file is a human-readable reference only.
-- The application manages schema creation and migrations automatically at startup.

PRAGMA foreign_keys = ON;

-- Tracks the current schema version.
-- Always contains exactly one row.
-- The application increments this value after each successful migration.
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER NOT NULL
);

-- Schema version history:
--   1 — initial schema (collection + entry tables)

CREATE TABLE IF NOT EXISTS collection (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL UNIQUE,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS entry (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    collection_id  INTEGER NOT NULL,
    url            TEXT NOT NULL,
    title          TEXT,
    warc_path      TEXT,
    browsertrix_id TEXT,
    created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (collection_id) REFERENCES collection(id) ON DELETE CASCADE
);
