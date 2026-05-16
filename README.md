# warc-studio

`warc-studio` is a lightweight C++23/CMake web application that lets you manage web archive collections, record interactive browsing sessions with Browsertrix Crawler, store metadata in SQLite, serve WACZ files over HTTP, and replay them with [ReplayWeb.page](https://replayweb.page).

The code is intentionally simple and modular:

- **Crow** is used as the embedded HTTP server.
- **SQLite** stores collections and entries (with WARC/WACZ paths).
- **BrowsertrixService** isolates all Browsertrix Crawler integration.
- WACZ files are served from `/archives/…` with CORS and HTTP Range support.
- Replay is done by redirecting to `https://replayweb.page/?source=…`.

---

## How it works

1. You create a **collection** (logical group, e.g. "research", "project-x").
2. You add an **entry** (a URL you want to archive).
3. You click **Start recording** → a Browsertrix Crawler container starts, Chromium opens interactively, and you browse the page manually.
4. You click **Stop recording** → the container stops, the generated WACZ file is copied to `data/archives/<collectionId>/<entryId>.wacz`, and the path is stored in SQLite.
5. You click **Replay** → you are redirected to ReplayWeb.page, which downloads the WACZ from the local server and replays the archived site in your browser — no backend replay server needed.

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
  sql/schema.sql          # SQLite schema reference
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
| `POST` | `/entry/<id>/start` | Start interactive recording (launches Browsertrix) |
| `POST` | `/entry/<id>/stop` | Stop recording, copy WACZ, save path to DB |
| `GET` | `/entry/<id>/replay` | Redirect to ReplayWeb.page with local WACZ as source |
| `POST` | `/entry/<id>/delete` | Delete entry and its local WACZ file |
| `GET` | `/archives/<path>` | Serve WACZ files with CORS and HTTP Range support |

---

## Database schema

The application automatically creates and migrates the SQLite schema at every startup.
The current schema version is stored in `schema_version` and is incremented after each successful migration.

```sql
-- Always contains exactly one row.
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

CREATE TABLE collection (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL UNIQUE,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE entry (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    collection_id  INTEGER NOT NULL,
    url            TEXT NOT NULL,
    title          TEXT,
    warc_path      TEXT,
    browsertrix_id TEXT,
    created_at     DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (collection_id) REFERENCES collection(id) ON DELETE CASCADE
);
```

### Schema versioning and migrations

Versioning is managed entirely inside `Database.cpp`:

- `kCurrentSchemaVersion` — compile-time constant; bump this when adding new migrations.
- `kMigrations[]` — array of SQL strings, one element per version step (`kMigrations[N]` migrates from version `N` to `N+1`).
- On **first run** (fresh DB): all migrations are executed in a single transaction and the version is set to `kCurrentSchemaVersion`.
- On **subsequent runs**: the version stored in `schema_version` is compared to `kCurrentSchemaVersion`; any missing migrations are applied in order, each wrapped in a transaction.

To add a new migration:

1. Append a new SQL string to `kMigrations[]` in `Database.cpp`.
2. Increment `kCurrentSchemaVersion`.
3. Add a comment to the version history in `sql/schema.sql` (reference only).

---

## Browsertrix integration

`BrowsertrixService` uses the Docker CLI to start and stop Browsertrix Crawler containers.

- `startRecording()` runs `docker run` with a deterministic crawl ID derived from the entry ID.
- `stopRecording()` runs `docker stop`, then locates the generated WACZ in the crawls directory and copies it to `data/archives/<collectionId>/<entryId>.wacz`.

This is the intended **extension point**. If you want to use a long-running Browsertrix service or a custom recording API, replace only `startRecording()` and `stopRecording()` — the database, UI, file serving, and replay routes remain unchanged.

The `scripts/run-browsertrix-example.sh` script shows the exact Docker command shape used, for reference.

---

## Replay via ReplayWeb.page

Crow serves WACZ files as static HTTP content (with Range support), making them accessible at:

```
http://localhost:18080/archives/<collectionId>/<entryId>.wacz
```

The `/entry/<id>/replay` route builds the full replay URL:

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
