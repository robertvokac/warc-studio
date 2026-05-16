# warc-studio

`warc-studio` is a lightweight C++23/CMake web application that lets you manage web archive collections, record interactive browsing sessions with Browsertrix Crawler, store metadata in SQLite, serve WACZ files over HTTP, and replay them with [ReplayWeb.page](https://replayweb.page).

The code is intentionally simple and modular:

- **Crow** is used as the embedded HTTP server.
- **SQLite** stores collections, entries, archive files, crawl runs, tags, notes, and capture metadata.
- **BrowsertrixService** isolates all Browsertrix Crawler integration.
- WACZ files are served from `/archives/…` with CORS and HTTP Range support.
- Replay is done by redirecting to `https://replayweb.page/?source=…`.

---

## How it works

1. You create a **collection** (logical group, e.g. "research", "project-x").
2. You add an **entry** (a URL you want to archive).
3. You click **Start recording** → a `crawl_run` row is created, the entry status is set to `recording`, a Browsertrix Crawler container starts, Chromium opens interactively, and you browse the page manually.
4. You click **Stop recording** → the container stops, the generated WACZ file is copied to `data/archives/<collectionId>/<entryId>.wacz`, a row is inserted into `archive_file`, and the entry status is set to `archived`.
5. You click **Replay** → the latest `archive_file` is used to build the ReplayWeb.page URL; the archived site replays in your browser with no backend replay server needed.

---

## Project layout

```text
warc-studio/
  CMakeLists.txt          # CMake build — Crow and Asio via FetchContent, SQLite3 via system package
  .gitignore
  include/warc_studio/    # Public headers
    BrowsertrixService.hpp
    Database.hpp
    FileService.hpp
    Html.hpp
    HttpUtil.hpp
    Models.hpp
  src/                    # Implementation
    main.cpp
    BrowsertrixService.cpp
    Database.cpp
    FileService.cpp
    Html.cpp
    HttpUtil.cpp
  sql/schema.sql          # SQLite schema reference (human-readable)
  scripts/
    run-browsertrix-example.sh
  data/                   # Created at runtime (excluded from git)
    warc-studio.sqlite3
    archives/             # WACZ files: archives/<collId>/<entryId>.wacz
    crawls/               # Browsertrix working directory
```

---

## Requirements

- **C++23** compiler (GCC 13+, Clang 16+)
- **CMake 3.26+**
- **Ninja** (recommended) or another CMake generator
- **libsqlite3-dev** (system package)
- **Git** (CMake FetchContent downloads Crow and Asio automatically)
- **Docker** (only for Browsertrix recording)

---

## Build

```bash
# Install system dependencies (Debian / Ubuntu)
sudo apt install -y build-essential cmake ninja-build git libsqlite3-dev

# Clone
git clone <your-repo-url> warc-studio
cd warc-studio

# Configure (Crow and Asio are fetched automatically by CMake)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j
```

No vcpkg, no extra toolchain file needed.

---

## Run

```bash
./build/warc-studio
```

Open in your browser:

```text
http://localhost:18080/
```

---

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `WARC_STUDIO_DATA_DIR` | `./data` | Root directory for the SQLite database, archives, and crawl data |
| `WARC_STUDIO_BROWSERTRIX_IMAGE` | `webrecorder/browsertrix-crawler:latest` | Docker image for Browsertrix Crawler. Pin a specific tag for production. |
| `WARC_STUDIO_PORT` | `18080` | HTTP port the server listens on |
| `WARC_STUDIO_RUN_BROWSERTRIX` | `1` | Set to `0` to skip actual Docker calls (useful for testing the UI and DB logic) |

---

## HTTP routes

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/` | Main UI: lists all collections and entries |
| `GET` | `/health` | Health check — returns `ok` |
| `POST` | `/collection/new` | Create a new collection (fields: `name`, `description`) |
| `GET` | `/collection/<id>` | Collection detail: shows info, edit form, delete button, and its entries |
| `POST` | `/collection/<id>/edit` | Update collection name and/or description |
| `POST` | `/collection/<id>/delete` | Delete collection, all its entries, and their archive files from disk |
| `POST` | `/entry/new` | Create a new entry (fields: `collection_id`, `url`, `title`) |
| `POST` | `/entry/<id>/start` | Start interactive recording (creates crawl_run, launches Browsertrix) |
| `POST` | `/entry/<id>/stop` | Stop recording, copy WACZ, insert archive_file row, set status to archived |
| `GET` | `/entry/<id>/replay` | Redirect to ReplayWeb.page using the latest archive_file |
| `POST` | `/entry/<id>/delete` | Delete entry, its archive files from disk, and all related DB rows |
| `GET` | `/archives/<path>` | Serve WACZ files with CORS and HTTP Range support |

---

## Database schema

The application automatically creates and migrates the SQLite schema at every startup.
The current schema version is stored in `schema_version` and is incremented after each successful migration.
The complete schema reference is in `sql/schema.sql`.

### `schema_version`

Tracks the active schema version. Always contains exactly one row.

```sql
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);
```

| Column | Type | Notes |
|---|---|---|
| `version` | `INTEGER` | Current schema version (currently `2`) |

---

### `collection`

Logical group of archived entries (e.g. "research", "project-x").

```sql
CREATE TABLE collection (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL UNIQUE,
    description TEXT,
    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `name` | `TEXT UNIQUE` | Required, must be unique |
| `description` | `TEXT` | Optional human-readable description |
| `created_at` | `DATETIME` | Set at insert |
| `updated_at` | `DATETIME` | Updated on every edit |

---

### `entry`

One archiving session — a URL you want to capture.

```sql
CREATE TABLE entry (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    collection_id  INTEGER NOT NULL,
    url            TEXT NOT NULL,
    normalized_url TEXT,
    title          TEXT,
    status         TEXT NOT NULL DEFAULT 'new',
    note           TEXT,
    created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    archived_at    DATETIME,
    last_error     TEXT,
    -- legacy columns kept for backward compatibility:
    warc_path      TEXT,
    browsertrix_id TEXT,
    FOREIGN KEY (collection_id) REFERENCES collection(id) ON DELETE CASCADE
);
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `collection_id` | `INTEGER FK` | References `collection.id`; cascade delete |
| `url` | `TEXT` | Original URL submitted by the user |
| `normalized_url` | `TEXT` | Optional normalized form of the URL |
| `title` | `TEXT` | Optional human-readable title |
| `status` | `TEXT` | `new` \| `recording` \| `archived` \| `failed` |
| `note` | `TEXT` | Short freeform note on the entry |
| `created_at` | `DATETIME` | Set at insert |
| `updated_at` | `DATETIME` | Updated on status changes |
| `archived_at` | `DATETIME` | Set when status becomes `archived` |
| `last_error` | `TEXT` | Last error message if status is `failed` |
| `warc_path` | `TEXT` | **Legacy** — kept for backward compatibility; prefer `archive_file` |
| `browsertrix_id` | `TEXT` | **Legacy** — kept for backward compatibility; prefer `crawl_run` |

---

### `archive_file`

One or more WACZ/WARC archive files per entry. New code reads the latest archive via `getLatestArchiveFile(entryId)`.

```sql
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
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `entry_id` | `INTEGER FK` | References `entry.id`; cascade delete |
| `path` | `TEXT` | Stored path relative to the data directory (e.g. `archives/3/15.wacz`) |
| `file_type` | `TEXT` | `wacz` (default) or `warc` |
| `size_bytes` | `INTEGER` | Optional file size in bytes |
| `sha256` | `TEXT` | Optional SHA-256 checksum |
| `created_at` | `DATETIME` | Set at insert |

---

### `crawl_run`

One row per crawl attempt. Tracks the full lifecycle of a Browsertrix recording.

```sql
CREATE TABLE crawl_run (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id            INTEGER NOT NULL,
    browsertrix_id      TEXT,
    docker_container_id TEXT,
    status              TEXT NOT NULL DEFAULT 'created',
    started_at          DATETIME,
    stopped_at          DATETIME,
    exit_code           INTEGER,
    error_message       TEXT,
    log_path            TEXT,
    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `entry_id` | `INTEGER FK` | References `entry.id`; cascade delete |
| `browsertrix_id` | `TEXT` | Browsertrix crawl ID (e.g. `crawl_entry_42`) |
| `docker_container_id` | `TEXT` | Docker container name/ID |
| `status` | `TEXT` | `created` → `running` → `stopped` \| `finished` \| `failed` |
| `started_at` | `DATETIME` | Set when recording starts |
| `stopped_at` | `DATETIME` | Set when recording stops |
| `exit_code` | `INTEGER` | Docker container exit code |
| `error_message` | `TEXT` | Error detail if the crawl failed |
| `log_path` | `TEXT` | Optional path to a crawl log file |

---

### `capture_metadata`

Optional per-entry metadata collected during or after a crawl.

```sql
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
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `entry_id` | `INTEGER FK` | References `entry.id`; cascade delete |
| `final_url` | `TEXT` | Final URL after redirects |
| `http_status` | `INTEGER` | HTTP status code of the main resource |
| `content_type` | `TEXT` | Content-Type header value |
| `screenshot_path` | `TEXT` | Path to a screenshot file |
| `page_title` | `TEXT` | Title of the captured page |
| `captured_at` | `DATETIME` | When the metadata was collected |

---

### `tag` and `entry_tag`

Many-to-many tag support for entries.

```sql
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
```

| Table | Column | Type | Notes |
|---|---|---|---|
| `tag` | `id` | `INTEGER PK` | Auto-increment |
| `tag` | `name` | `TEXT UNIQUE` | Tag label |
| `entry_tag` | `entry_id` | `INTEGER FK` | References `entry.id`; cascade delete |
| `entry_tag` | `tag_id` | `INTEGER FK` | References `tag.id`; cascade delete |

---

### `entry_note`

Timestamped note history per entry. Each append creates a new row.

```sql
CREATE TABLE entry_note (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    entry_id   INTEGER NOT NULL,
    body       TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
);
```

| Column | Type | Notes |
|---|---|---|
| `id` | `INTEGER PK` | Auto-increment |
| `entry_id` | `INTEGER FK` | References `entry.id`; cascade delete |
| `body` | `TEXT` | Note content |
| `created_at` | `DATETIME` | When the note was added |

---

### Schema versioning and migrations

Versioning is managed entirely inside `Database.cpp`:

- `kCurrentSchemaVersion` — compile-time constant; bump this when adding new migrations.
- `kMigrations[]` — array of SQL strings, one element per version step (`kMigrations[N]` migrates from version `N` to `N+1`).
- On **first run** (fresh DB): all migrations are executed in a single transaction.
- On **subsequent runs**: only missing migrations are applied in order, each in its own transaction.
- Migration v1→v2 copies existing `entry.warc_path` values into `archive_file` and `entry.browsertrix_id` into `crawl_run`.

To add a new migration:

1. Append a new SQL string to `kMigrations[]` in `Database.cpp`.
2. Increment `kCurrentSchemaVersion`.
3. Add a comment to the version history in `sql/schema.sql` (reference only).

---

## Browsertrix integration

`BrowsertrixService` uses the Docker CLI to start and stop Browsertrix Crawler containers.

- `startRecording()` runs `docker run` with a deterministic crawl ID derived from the entry ID, and returns the `browsertrixId` and `dockerContainerName` used.
- `stopRecording()` runs `docker stop`, then locates the generated WACZ in the crawls directory and copies it to `data/archives/<collectionId>/<entryId>.wacz`.

This is the intended **extension point**. If you want to use a long-running Browsertrix service or a custom recording API, replace only `startRecording()` and `stopRecording()` — the database, UI, file serving, and replay routes remain unchanged.

The `scripts/run-browsertrix-example.sh` script shows the exact Docker command shape used, for reference.

---

## Replay via ReplayWeb.page

Crow serves WACZ files as static HTTP content (with Range support), making them accessible at:

```
http://localhost:18080/archives/<collectionId>/<entryId>.wacz
```

The `/entry/<id>/replay` route reads the latest `archive_file` for the entry and builds the full replay URL:

```
https://replayweb.page/?source=http://localhost:18080/archives/<collectionId>/<entryId>.wacz
```

ReplayWeb.page is a purely client-side application — it downloads the WACZ, decompresses it in the browser, and replays the archived site without any server-side replay component.

---

## Security notes

This is a **local prototype**, not a hardened multi-user application.

Before exposing it on a network, consider adding:

- Authentication and session management
- CSRF protection (tokens on all state-changing forms)
- Strict path validation for archive access
- Rate limiting
- URL validation for submitted entry URLs
- A safer subprocess runner instead of `std::system`
- A pinned Browsertrix Docker image version (avoid `latest` in production)
