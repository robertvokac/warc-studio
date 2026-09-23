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
--   3 — per-collection entry numbering (number_per_collection on entry);
--         label and source columns on archive_file
--   4 — unique URL per collection: UNIQUE INDEX on (collection_id, url) in entry
--   5 — capture depth: capture_depth TEXT column on entry (default CURRENT_PAGE_ONLY)
--   6 — collections and entries replaced by timestamped captures (capture, capture_tag);
--         old tables migrated and dropped
--   7 — covering index (status, size_bytes) for archive statistics, replaces capture_status_idx
--   8 — max_depth (link hops) and crawl_scope replace capture_depth

-- ---------------------------------------------------------------------------
-- Captures — one archiving request of one URL at one moment, with one archive file
-- ---------------------------------------------------------------------------

CREATE TABLE capture (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    url           TEXT NOT NULL,
    url_key       TEXT NOT NULL DEFAULT '',    -- normalized URL for lookups (see makeUrlKey)
    timestamp     TEXT NOT NULL,               -- YYYYMMDDhhmmss UTC, set when the crawl starts
    title         TEXT,
    note          TEXT,
    status        TEXT NOT NULL DEFAULT 'queued',
        -- queued | crawling | archived | failed | cancelled
    source        TEXT NOT NULL DEFAULT 'crawl',   -- crawl | upload
    max_depth     INTEGER NOT NULL DEFAULT 0,  -- link hops followed from the start page: 0 = page only, -1 = unlimited
    crawl_scope   TEXT NOT NULL DEFAULT 'PREFIX',
        -- PREFIX (same host, path under the start directory) | DOMAIN (site domain incl. subdomains) | ANY
    page_limit    INTEGER,                     -- max pages when max_depth != 0, 0 = none
    file_path     TEXT,                        -- relative to the data dir: archives/YYYY/MM/<timestamp>-<n>.<ext>
    file_type     TEXT,                        -- wacz | warc
    size_bytes    INTEGER,
    sha256        TEXT,
    error         TEXT,                        -- failure reason, or a warning for archived captures
    created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
    started_at    DATETIME,
    finished_at   DATETIME
);

CREATE INDEX capture_url_key_idx   ON capture (url_key);
CREATE INDEX capture_timestamp_idx ON capture (timestamp);
CREATE INDEX capture_status_size_idx ON capture (status, size_bytes);

-- ---------------------------------------------------------------------------
-- Tags
-- ---------------------------------------------------------------------------

CREATE TABLE tag (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE                  -- lowercase, no commas
);

CREATE TABLE capture_tag (
    capture_id INTEGER NOT NULL,
    tag_id     INTEGER NOT NULL,

    PRIMARY KEY (capture_id, tag_id),
    FOREIGN KEY (capture_id) REFERENCES capture(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id)     REFERENCES tag(id)     ON DELETE CASCADE
);

CREATE INDEX capture_tag_tag_idx ON capture_tag (tag_id);
