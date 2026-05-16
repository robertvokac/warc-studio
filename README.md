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
  CMakeLists.txt          # CMake build definition
  vcpkg.json              # vcpkg dependencies (Crow, SQLite3)
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
- **vcpkg** (for Crow and SQLite3)
- **Docker** (to run Browsertrix Crawler containers)

---

## Build with vcpkg

```bash
git clone <your-repo-url> warc-studio
cd warc-studio

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build build -j
```

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
| `GET` | `/` | Main UI: lists collections and entries |
| `GET` | `/health` | Health check — returns `ok` |
| `POST` | `/collection/new` | Create a new collection |
| `POST` | `/entry/new` | Create a new entry |
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

### Core tables

```sql
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

### New tables (schema version 2)

**`archive_file`** — one or more WACZ/WARC files per entry.
New code reads the latest archive via `getLatestArchiveFile(entryId)`; `entry.warc_path` is kept for backward compatibility.

**`crawl_run`** — one row per crawl attempt.
Stores Browsertrix ID, Docker container name, status (`created` → `running` → `stopped`/`finished`/`failed`), timestamps, exit code, and error message.

**`capture_metadata`** — optional metadata collected per crawl (final URL, HTTP status, content type, screenshot path, page title).

**`tag`** + **`entry_tag`** — many-to-many tag support for entries.

**`entry_note`** — timestamped note history per entry.

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
