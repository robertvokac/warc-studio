# warc-studio

A lightweight local web application for managing web archive collections, recording with Browsertrix, and replaying WACZ files via ReplayWeb.page.

Built with **C++23**, **CMake**, **Crow**, **SQLite**, and **Docker** (for Browsertrix).

---

## Features

- Create and manage **collections** of URLs to archive; edit collection name and description.
- Per-collection **entry numbering** (`#1`, `#2`, `#3`, …) that resets for each collection.
- Multiple **archive files per entry** (WACZ and WARC), from Browsertrix or manual upload.
- **SHA-256 checksums** computed automatically for every uploaded or Browsertrix-generated archive file.
- **Entry statuses**: `new`, `queued`, `recording`, `archived`, `imported`, `failed`, `needs_review`, `ignored`.
- **Entry editing**: edit URL, title, and note from the entry detail page.
- **Manual upload** of `.warc`, `.warc.gz`, and `.wacz` files; edit label and delete individual archive files.
- Start and stop **Browsertrix crawler** recordings (via Docker); Start recording returns immediately and does not block the browser.
- **Crawl run history** tracked in the database: each Browsertrix start/stop creates a `crawl_run` row with timestamps, exit code, and error messages; visible on the entry detail page.
- **Physical archive file deletion**: deleting an entry (or a single archive file) removes both the database rows and the files on disk.
- **Replay** WACZ archives through [ReplayWeb.page](https://replayweb.page/).
- Clean **multi-section web UI** with top navigation, collection switcher, status badges, and CSS styling.
- **Favicon** (`/favicon.svg`) and external CSS (`/static/style.css`).
- All data stored locally in a single **SQLite** database.

---

## Requirements

- C++23 compiler (GCC 13+ or Clang 16+)
- CMake 3.26+
- SQLite3 development headers (`libsqlite3-dev`)
- OpenSSL development headers (`libssl-dev`) — used for SHA-256 checksums
- Docker (for Browsertrix recording; optional)

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target warc-studio
```

---

## Run

```bash
cd build
./warc-studio
```

Then open [http://localhost:18080/](http://localhost:18080/) in your browser.

The `static/` directory (from the project root) must be accessible from the working directory when running the app. The app looks for `static/` relative to the current working directory.

---

## Configuration (environment variables)

| Variable                       | Default                                 | Description                      |
|--------------------------------|-----------------------------------------|----------------------------------|
| `WARC_STUDIO_DATA_DIR`         | `data`                                  | Directory for SQLite DB + archives |
| `WARC_STUDIO_PORT`             | `18080`                                 | HTTP port                        |
| `WARC_STUDIO_BROWSERTRIX_IMAGE`| `webrecorder/browsertrix-crawler:latest`| Docker image for Browsertrix     |
| `WARC_STUDIO_RUN_BROWSERTRIX`  | `true`                                  | Set to `false` to disable Docker |

---

## Web UI sections

### Collections (`/collections`)
- List all collections with entry counts and creation date.
- Create a new collection with name and optional description.
- **Edit** button on each collection row opens the collection edit page.
- Open entries for a collection.

### Collection edit (`/collection/<id>`)
- Edit collection name and description.
- Delete collection (cascades to all entries and archive files).
- Add entries directly from this page.

### Entries (`/entries` or `/collections/<id>/entries`)
- Show only entries from the currently selected collection.
- **Collection switcher** dropdown at the top; auto-selects first collection if none is chosen.
- **"Edit collection"** button next to the switcher opens the collection edit page.
- Each entry shows: number (`#1`, `#2`, …), title/URL, status badge, archive count, actions.
- Create a new entry (URL + optional title).
- Actions: Start recording, Stop recording, Detail, Replay latest WACZ, Delete.

### Entry detail (`/entry/<id>`)
- Full entry info: collection, number, URL, status, dates, notes, errors.
- **Edit entry**: update URL, title, and note inline.
- Change status from a dropdown (any of the eight statuses).
- Browsertrix recording controls (Start / Stop); Start recording returns immediately without blocking.
- List of all archive files with type badge, source, size, SHA-256 prefix, replay button.
- **Edit label** inline per archive file (expandable form).
- **Delete** individual archive file (removes DB row and file from disk).
- Upload form for `.warc`, `.warc.gz`, `.wacz` files.
- **Crawl run history**: table of all Browsertrix runs for this entry (status, timestamps, exit code, error message).
- Delete entry (removes DB rows and physical files from disk).

### Archive Files (`/archives-browser`)
- Global view of archive files for the selected collection.
- Shows entry number, title, file type, source, label, size, date, replay button.

### About (`/about`)
- Data directory, Browsertrix image, enabled/disabled status, version, technology stack.

---

## Entry statuses

| Status         | Meaning                                            |
|----------------|----------------------------------------------------|
| `new`          | Entry created; no archive yet                      |
| `queued`       | Planned for recording                              |
| `recording`    | Browsertrix is currently running                   |
| `archived`     | Browsertrix recording completed successfully       |
| `imported`     | At least one manually uploaded archive             |
| `failed`       | Last recording or upload failed                    |
| `needs_review` | Archive exists but flagged for review              |
| `ignored`      | Intentionally not being archived                   |

---

## Database schema

Schema version: **3**.

Key tables:

- **`collection`** — name, description, timestamps.
- **`entry`** — URL, title, status, `number_per_collection` (unique per collection), timestamps, last error.
- **`archive_file`** — path, file_type (`wacz`/`warc`), label, source, size, sha256 (SHA-256 hex digest), timestamps.
- **`crawl_run`** — Browsertrix run records (status, browsertrix_id, docker_container_id, started_at, stopped_at, exit_code, error_message).
- **`capture_metadata`**, **`tag`**, **`entry_tag`**, **`entry_note`** — optional metadata.

Migrations are applied automatically at startup. See `sql/schema.sql` for the full reference schema.

---

## Static files

- `static/favicon.svg` — archive-themed favicon (served at `/favicon.svg`).
- `static/style.css` — all UI styling (served at `/static/style.css`).

---

## Archive file upload

- Upload `.warc`, `.warc.gz`, or `.wacz` files from the entry detail page.
- Files are stored under `data/archives/<collectionId>/<entryId>/<timestamp>.<ext>`.
- File type is determined from the extension; unknown types are rejected.
- Empty files and path traversal attempts are rejected.
- On successful upload, an `archive_file` row is inserted and entry status is set to `imported` if it was `new`.

## WARC replay

- WACZ files are replayed via an embedded [ReplayWeb.page](https://replayweb.page/) player served locally.
- WARC files (`.warc`, `.warc.gz`) are stored but replay is not supported yet; a clear message is shown.
- The ReplayWeb.page `ui.js` and `sw.js` bundles are vendored in `static/` so no CDN is required and there is no mixed-content issue.
- All replay assets are served locally: `/replay/ui.js` (UI bundle), `/replay/sw.js` (service worker with scope `/replay/`).
- The script tag on the replay page uses `/replay/ui.js` which is consistent with `replayBase="/replay/"`, so the web component resolves all sub-resources from the same local path prefix.

### Replay troubleshooting

**Replay opens but archive appears empty ("No Results Found")**

1. Verify the WACZ file downloads correctly: `curl -o /tmp/test.wacz http://localhost:18080/archives/<path>`
2. Verify CORS and Range headers: `curl -I -H "Range: bytes=0-1023" http://localhost:18080/archives/<path>` — expect `206 Partial Content` with `Access-Control-Allow-Origin: *`
3. **Clear stale service worker**: DevTools → Application → Service Workers → Unregister all service workers for `localhost`, then hard refresh (Ctrl+Shift+R / Cmd+Shift+R). Old cached service workers with wrong scope may prevent replay from loading.
4. Try an incognito/private window to avoid stale service worker state.
5. WARC replay is not supported — only WACZ files can be replayed.

---

## Routes

```
GET  /                               — Legacy index page (collections + all entries)
GET  /collections                    — Collections section
POST /collection/new                 — Create collection
GET  /collections/<id>/entries       — Entries for a specific collection
GET  /entries?collection_id=<id>     — Entries with collection switcher
GET  /entry/<id>                     — Entry detail page
POST /entry/<id>/update              — Edit entry fields (url, title, note)
POST /entry/<id>/status              — Change entry status
POST /entry/<id>/upload              — Upload WARC/WACZ file
POST /entry/<id>/start               — Start Browsertrix recording
POST /entry/<id>/stop                — Stop Browsertrix recording
GET  /entry/<id>/replay/latest       — Replay latest WACZ
GET  /entry/<id>/replay              — Legacy replay route
GET  /archive/<id>/replay            — Replay specific archive file
POST /archive/<id>/update            — Edit archive file label
POST /archive/<id>/delete            — Delete single archive file
POST /entry/<id>/delete              — Delete entry
GET  /collection/<id>                — Collection edit page
POST /collection/<id>/edit           — Save collection name/description
POST /collection/<id>/delete         — Delete collection
GET  /archives-browser               — Archive files browser
GET  /about                          — Settings / About
GET  /archives/<path>                — Serve archive files (CORS-enabled, Range support)
GET  /replay/ui.js                   — ReplayWeb.page UI bundle (self-hosted, local)
GET  /replay/sw.js                   — ReplayWeb.page service worker (scope /replay/)
GET  /static/ui.js                   — ReplayWeb.page UI bundle (alternate path)
GET  /static/sw.js                   — ReplayWeb.page service worker (alternate path)
GET  /sw.js                          — ReplayWeb.page service worker (root scope, for compatibility)
GET  /static/style.css               — Application CSS
GET  /favicon.svg                    — Favicon
GET  /health                         — Health check
```
