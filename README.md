# warc-studio

A personal, local web archive — like the [Wayback Machine](https://web.archive.org/), but for the pages *you* want to keep.
Paste a URL, see whether it is already archived, archive it with the built-in crawler and tag it.
Every archiving request becomes its own **capture**: one timestamped WARC file.

Built with **C++23**, **CMake**, **Crow**, **SQLite**, **libcurl** and **ReplayWeb.page**. No Docker, no external crawler.

---

## Concepts

The terminology follows the Wayback Machine:

| Term          | Meaning |
|---------------|---------|
| **Capture**   | One archiving request of one URL at one moment (a.k.a. snapshot). Owns exactly one archive file. |
| **Timestamp** | When the capture was taken, `YYYYMMDDhhmmss` in UTC (e.g. `20260923180211`), as in `web.archive.org/web/<timestamp>/<url>`. |
| **Tag**       | Free-form label; the main way to organise captures. A capture can have any number of tags. |
| **URL key**   | Normalized URL used to answer "is this URL archived?" — scheme, `www.`, default port, fragment and trailing `/` are ignored (`https://www.Example.com/a/` = `example.com/a`). |
| **Depth**     | How many clicks away from the page links are followed: *Page only* (0), 1–10, or *All linked pages*. Each archived page comes with all its resources. |
| **Scope**     | Which links may be followed: *same path* (under the start page's directory), *whole domain* (subdomains included) or *any site* (only with a limited depth). *Max pages* caps the crawl. |

Capture statuses: `queued` → `crawling` → `archived`, or `failed` / `cancelled`.

---

## Features

- **Save Page Now** — paste a URL; while typing, warc-studio shows whether (and when) it was archived before. Save it with tags, depth and an optional title.
- **Bookmarklet** (see *About*) — save the page you are reading with one click from any browser tab.
- **Bulk save** — many URLs at once (one per line), optionally skipping those already archived.
- **Built-in crawler** — see below; runs in background worker threads, no manual start/stop. Captures interrupted by a restart are re-queued. A running crawl can be stopped early (what was fetched so far is kept), a failed one retried.
- **Browse** captures chronologically (newest or oldest first, grouped by day), **search** by URL or title substring, filter by **tags** (all must match) and status.
- **URL history** — all captures of one URL, like the Wayback Machine calendar.
- **Upload** your own `.warc`, `.warc.gz` or `.wacz` — URL, capture date and title are read from the file when not given. The form shows upload progress and limits concurrent uploads.
- **Backup and restore** the database and archive files with the verified offline tool in `tools/backup.py`.
- **Download** any archive file (named `<host>-<timestamp>.<ext>`); file size and SHA-256 are shown everywhere.
- **Replay** WACZ and WARC files with the locally hosted ReplayWeb.page.
- **Tags page** — tag cloud, rename (renaming onto an existing tag merges them) and delete.
- **Wayback-style URLs** — `/web/*/<url>` and `/web/<timestamp>/<url>`.

---

## Built-in crawler

Each capture is crawled by warc-studio itself (`src/Crawler.cpp`) and written to its own `.warc.gz`:

1. The page is fetched with libcurl (HTTP/1.1, browser-like User-Agent, cookies kept for the crawl).
   Redirects are recorded and followed.
2. Its HTML is parsed for everything needed to render it: stylesheets, scripts, images (`src`, `srcset`,
   `data-src`), fonts and images referenced from CSS (`url()`, `@import`), inline `style` attributes,
   `<style>` blocks, media, icons, `og:image` and embedded `<iframe>` documents (with their resources).
   Resources on other hosts (CDNs) are included. They are fetched 6 at a time, like a browser does.
   All `srcset` candidates are fetched, so replay works on any screen size.
3. Inline scripts and fetched `.js` files are scanned for quoted URLs of static files (`.css`, `.js`,
   images, fonts, media, `.json`) — this catches e.g. stylesheets added with `document.write`.
   It is a heuristic: some guesses end as recorded 404s, which is harmless.
4. With a depth above 0, links are followed breadth-first (nearest pages first) up to the depth, within the
   scope and up to the page limit. *Same path*: same host, path under the start page's directory.
   *Whole domain*: the site domain and its subdomains (`blog.example.co.uk` → `example.co.uk`; tenants of
   hosting domains such as `github.io` or `blogspot.com` count as separate sites). *Any site*: everything.
5. Every request and response is stored as WARC 1.1 `request`/`response` records **exactly as received**
   (compressed and chunked bodies are kept raw), each record its own gzip member, with SHA-1 block digests.
   The files validate with `warcio check` and replay in ReplayWeb.page.

**Limitation:** JavaScript is not executed. Classic sites, blogs, documentation, articles, Wikipedia and
portals such as seznam.cz are archived well; content that a page fetches with JavaScript through
dynamically built URLs (single-page apps, infinite scroll, some social networks) is missing from the capture.

Limits: responses larger than `WARC_STUDIO_MAX_RESOURCE_MB` are skipped. Buffered raw responses
across concurrent crawls share `WARC_STUDIO_MAX_BUFFER_MB`; the crawler skips a response when
that budget is exhausted. Each capture allows at most 3000 resources and 10000 pages, with an
optional time limit. Decoded HTML, temporary WARC compression buffers, and uploads use additional memory.

---

## Requirements

- C++23 compiler (GCC 13+ or Clang 16+)
- CMake 3.26+
- SQLite3, OpenSSL, zlib and libcurl development headers
  (`libsqlite3-dev libssl-dev zlib1g-dev libcurl4-openssl-dev`)
- `unzip` (inspects uploaded WACZ files with bounded metadata reads)
- Python 3.9+ for the backup/restore tool and integration test

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Tests: `ctest --test-dir build --output-on-failure`. The integration test starts a local fixture site and WARC Studio, crawls and uploads a real WARC, checks replay routes and byte ranges, then tests backup and restore. When Chrome or Chromium is installed, it also checks that ReplayWeb.page renders the archived page in a browser. Python 3 is required for this test.

## Run

```bash
cd build
./warc-studio
```

Then open [http://localhost:18080/](http://localhost:18080/). The server listens only on `127.0.0.1`.

---

## Configuration (environment variables)

| Variable                        | Default                                  | Description |
|---------------------------------|------------------------------------------|-------------|
| `WARC_STUDIO_DATA_DIR`          | `data`                                   | SQLite DB, archives, crawl output and logs |
| `WARC_STUDIO_PORT`              | `18080`                                  | HTTP port |
| `WARC_STUDIO_REPLAY_PORT`       | main port + 1                           | Read-only replay origin on `127.0.0.1` |
| `WARC_STUDIO_MAX_REQUEST_MB`    | `128`                                    | Maximum HTTP request body, including uploaded archives; larger requests receive 413 |
| `WARC_STUDIO_MAX_CONCURRENT_UPLOADS` | `2`                               | Uploads accepted at once; extra uploads receive 503 and may be retried |
| `WARC_STUDIO_CRAWL_WORKERS`     | `2`                                      | Number of captures crawled in parallel |
| `WARC_STUDIO_CRAWL_TIME_LIMIT`  | `0`                                      | Time limit per capture in seconds, `0` = none |
| `WARC_STUDIO_MAX_RESOURCE_MB`   | `100`                                    | Larger responses are skipped |
| `WARC_STUDIO_MAX_BUFFER_MB`     | `256`                                    | Shared budget for buffered raw crawler responses |
| `WARC_STUDIO_USER_AGENT`        | Chrome on Linux + `warc-studio/0.4`      | User-Agent sent by the crawler |
| `WARC_STUDIO_STATIC_DIR`        | `static/` next to the executable         | Static assets directory |

---

## Data layout

```
data/
  warc-studio.sqlite3
  archives/<YYYY>/<MM>/<timestamp>-<n>.<warc.gz|warc|wacz>   one file per capture
  crawls/capture-<id>.warc.gz                                WARC being written (moved to archives/ when done)
  logs/capture-<id>.log                                      crawl log (every URL with status), shown on the capture page
```

## Backup and restore

Stop WARC Studio before using the backup tool. It checks the same data lock as the server, verifies SQLite and every archived capture file, and writes a SHA-256 manifest for all backed-up files. The backup includes the database, archives, crawl logs and unfinished crawl files.

```bash
python3 tools/backup.py backup /path/to/backup-2026-09-25 --data-dir build/data
python3 tools/backup.py restore /path/to/backup-2026-09-25 --data-dir build/data
```

Use the directory from `WARC_STUDIO_DATA_DIR` for `--data-dir` (the default is `data`, relative to the server's working directory). The backup destination must be outside the data directory. Restore validates checksums before changing data and keeps the previous data directory beside it as `data.pre-restore-<timestamp>-<pid>`. Keep a copy of the backup on another disk as well.

## Database

Schema version **8** — tables `capture`, `tag`, `capture_tag` (see `src/Database.cpp`).
Migrations run automatically at startup. Migration 6 converts the old collections/entries model:
every old archive file becomes an archived capture, entries without an archive file become failed
captures (so no URL is lost — use *Archive again*), entry tags are kept, collections are dropped.

---

## Routes

```
GET  /                          Save Page Now (+ ?url=&title=&tags= prefill, used by the bookmarklet)
POST /save                      Queue a capture (url, tags, title, capture_depth, page_limit)
GET  /save/bulk, POST /save/bulk
GET  /api/lookup?url=           JSON: captures of a URL (count, last capture)
GET  /captures                  Browse/search (?q=, tag=, status=, order=oldest, page=)
GET  /url?url=                  All captures of one URL
GET  /web/*/<url>               -> /url?url=<url>
GET  /web/<timestamp>/<url>     -> replay of the capture closest to the timestamp
GET  /capture/<id>              Capture detail
POST /capture/<id>/update       Edit URL, title, tags, note
POST /capture/<id>/cancel       Cancel a queued capture
POST /capture/<id>/stop         Stop a running crawl early (keeps what was captured)
POST /capture/<id>/retry        Re-queue a failed/cancelled crawl
POST /capture/<id>/recapture    New capture of the same URL ("Archive again")
POST /capture/<id>/delete       Delete capture, its archive file and log
GET  /capture/<id>/replay       Replay with ReplayWeb.page
GET  /capture/<id>/download     Download the archive file
GET  /tags, POST /tags/rename, POST /tags/delete
GET  /upload, POST /upload      Upload own WARC/WACZ (file, url, tags, title, timestamp, note)
GET  /about                     Settings, statistics, bookmarklet
GET  /health

Read-only replay port:
GET  /capture/<id>/replay       Replay page after redirect from the main app
GET  /archives/<path>           Archive bytes (Range support)
GET  /replay/ui.js, /replay/sw.js, /replay/...   ReplayWeb.page assets and shell
```

---

## Replay troubleshooting

ReplayWeb.page (`ui.js`, `sw.js`, vendored from replaywebpage@2.4.6) runs on the separate
read-only replay port. Its archive files and player share that origin. Archived scripts cannot
access the main application's pages or mutation routes.

If a replay shows "No Results Found":

1. Check the file downloads: `curl -o /tmp/test.wacz http://localhost:18080/capture/<id>/download`
2. Check Range support: `curl -I -H "Range: bytes=0-1023" http://127.0.0.1:18081/archives/<path>` — expect `206` with `Content-Range`.
3. Clear a stale service worker: DevTools → Application → Service Workers → Unregister, then hard refresh.
4. Try a private window.
