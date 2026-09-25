#include "warc_studio/Database.hpp"
#include "warc_studio/CaptureUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>

namespace warc_studio {
namespace {

// ---------------------------------------------------------------------------
// Statement — RAII wrapper for sqlite3_stmt
// ---------------------------------------------------------------------------

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    ~Statement() {
        sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const {
        return stmt_;
    }

    void bindInt(int index, int value) {
        if (sqlite3_bind_int(stmt_, index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void bindInt64(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void bindText(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void bindOptionalText(int index, const std::optional<std::string>& value) {
        if (value) {
            bindText(index, *value);
            return;
        }
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void bindOptionalInt(int index, const std::optional<int>& value) {
        if (value) {
            bindInt(index, *value);
            return;
        }
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void bindOptionalInt64(int index, const std::optional<std::int64_t>& value) {
        if (value) {
            bindInt64(index, *value);
            return;
        }
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};

// ---------------------------------------------------------------------------
// Column helpers
// ---------------------------------------------------------------------------

std::string columnText(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

std::optional<std::string> optionalColumnText(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return columnText(stmt, index);
}

[[maybe_unused]] std::optional<int> optionalColumnInt(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(stmt, index);
}

std::optional<std::int64_t> optionalColumnInt64(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(stmt, index);
}

// ---------------------------------------------------------------------------
// Row readers
// ---------------------------------------------------------------------------

constexpr const char* kCaptureColumns = R"SQL(
    c.id, c.url, c.url_key, c.timestamp, c.title, c.note, c.status, c.source,
    c.max_depth, c.crawl_scope, c.page_limit, c.file_path, c.file_type, c.size_bytes, c.sha256,
    c.error, c.created_at, c.started_at, c.finished_at,
    (SELECT group_concat(t.name, ',') FROM capture_tag ct JOIN tag t ON t.id = ct.tag_id
     WHERE ct.capture_id = c.id) AS tags
)SQL";

Capture readCapture(sqlite3_stmt* stmt) {
    Capture capture{
        .id = sqlite3_column_int(stmt, 0),
        .url = columnText(stmt, 1),
        .urlKey = columnText(stmt, 2),
        .timestamp = columnText(stmt, 3),
        .title = optionalColumnText(stmt, 4),
        .note = optionalColumnText(stmt, 5),
        .status = columnText(stmt, 6),
        .source = columnText(stmt, 7),
        .maxDepth = sqlite3_column_int(stmt, 8),
        .scope = crawlScopeFromString(columnText(stmt, 9)),
        .pageLimit = optionalColumnInt(stmt, 10),
        .filePath = optionalColumnText(stmt, 11),
        .fileType = optionalColumnText(stmt, 12),
        .sizeBytes = optionalColumnInt64(stmt, 13),
        .sha256 = optionalColumnText(stmt, 14),
        .error = optionalColumnText(stmt, 15),
        .createdAt = columnText(stmt, 16),
        .startedAt = optionalColumnText(stmt, 17),
        .finishedAt = optionalColumnText(stmt, 18),
        .tags = parseTags(columnText(stmt, 19)),
    };
    std::sort(capture.tags.begin(), capture.tags.end());
    return capture;
}

void stepDone(sqlite3* db, Statement& stmt) {
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

// Builds the WHERE clause for a CaptureQuery; parameters are appended to `params`
// in order and bound as text.
std::string captureWhereClause(const CaptureQuery& query, std::vector<std::string>& params) {
    std::string where = " WHERE 1=1";
    if (!query.urlContains.empty()) {
        where += " AND (c.url LIKE ? ESCAPE '\\' OR c.title LIKE ? ESCAPE '\\')";
        std::string pattern = "%";
        for (const char ch : query.urlContains) {
            if (ch == '%' || ch == '_' || ch == '\\') pattern.push_back('\\');
            pattern.push_back(ch);
        }
        pattern.push_back('%');
        params.push_back(pattern);
        params.push_back(pattern);
    }
    if (query.urlKey) {
        where += " AND c.url_key = ?";
        params.push_back(*query.urlKey);
    }
    for (const auto& tag : query.tags) {
        where += " AND EXISTS (SELECT 1 FROM capture_tag ct JOIN tag t ON t.id = ct.tag_id"
                 " WHERE ct.capture_id = c.id AND t.name = ?)";
        params.push_back(tag);
    }
    if (!query.status.empty()) {
        where += " AND c.status = ?";
        params.push_back(query.status);
    }
    return where;
}

} // namespace

// ---------------------------------------------------------------------------
// Database constructor / destructor
// ---------------------------------------------------------------------------

Database::Database(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());

    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ == nullptr ? "Could not open SQLite database" : sqlite3_errmsg(db_);
        throw std::runtime_error(message);
    }

    // Wait instead of failing when another process (e.g. the sqlite3 CLI) holds a lock briefly.
    sqlite3_busy_timeout(db_, 5000);
    execute("PRAGMA foreign_keys = ON;");
    initializeSchema();
}

Database::~Database() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Database::execute(const char* sql) const {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "SQLite error" : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// Schema versioning
// ---------------------------------------------------------------------------

// Bump this constant and append to kMigrations when adding new schema changes.
static constexpr int kCurrentSchemaVersion = 8;

// kMigrations[N] migrates the database from version N to version N+1.
static const char* kMigrations[] = {
    // v0 → v1 : initial schema (collection + entry)
    R"SQL(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER NOT NULL
        );
        INSERT INTO schema_version (version) VALUES (0);

        CREATE TABLE IF NOT EXISTS collection (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS entry (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            collection_id INTEGER NOT NULL,
            url TEXT NOT NULL,
            title TEXT,
            warc_path TEXT,
            browsertrix_id TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (collection_id) REFERENCES collection(id) ON DELETE CASCADE
        );
    )SQL",

    // v1 → v2 : extended schema
    //   - add columns to collection and entry
    //   - add new tables: archive_file, crawl_run, capture_metadata, tag, entry_tag, entry_note
    //   - migrate existing warc_path → archive_file, browsertrix_id → crawl_run
    R"SQL(
        ALTER TABLE collection ADD COLUMN description TEXT;
        ALTER TABLE collection ADD COLUMN updated_at DATETIME;

        ALTER TABLE entry ADD COLUMN normalized_url TEXT;
        ALTER TABLE entry ADD COLUMN status TEXT NOT NULL DEFAULT 'new';
        ALTER TABLE entry ADD COLUMN note TEXT;
        ALTER TABLE entry ADD COLUMN updated_at DATETIME;
        ALTER TABLE entry ADD COLUMN archived_at DATETIME;
        ALTER TABLE entry ADD COLUMN last_error TEXT;

        CREATE TABLE IF NOT EXISTS archive_file (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            entry_id   INTEGER NOT NULL,
            path       TEXT NOT NULL,
            file_type  TEXT NOT NULL DEFAULT 'wacz',
            size_bytes INTEGER,
            sha256     TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS crawl_run (
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

        CREATE TABLE IF NOT EXISTS capture_metadata (
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

        CREATE TABLE IF NOT EXISTS tag (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE
        );

        CREATE TABLE IF NOT EXISTS entry_tag (
            entry_id INTEGER NOT NULL,
            tag_id   INTEGER NOT NULL,
            PRIMARY KEY (entry_id, tag_id),
            FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)   REFERENCES tag(id)   ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS entry_note (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            entry_id   INTEGER NOT NULL,
            body       TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            FOREIGN KEY (entry_id) REFERENCES entry(id) ON DELETE CASCADE
        );

        INSERT INTO archive_file (entry_id, path, file_type)
            SELECT id, warc_path, 'wacz'
            FROM entry
            WHERE warc_path IS NOT NULL AND warc_path != '';

        INSERT INTO crawl_run (entry_id, browsertrix_id, status)
            SELECT id, browsertrix_id, 'finished'
            FROM entry
            WHERE browsertrix_id IS NOT NULL AND browsertrix_id != '';

        UPDATE entry
            SET status = 'archived', archived_at = created_at
            WHERE warc_path IS NOT NULL AND warc_path != '';
    )SQL",

    // v2 → v3 : per-collection entry numbering + archive_file label/source columns
    //   - add number_per_collection to entry; populate it per-collection ordered by id
    //   - add label and source columns to archive_file
    R"SQL(
        ALTER TABLE entry ADD COLUMN number_per_collection INTEGER NOT NULL DEFAULT 0;

        ALTER TABLE archive_file ADD COLUMN label TEXT;
        ALTER TABLE archive_file ADD COLUMN source TEXT NOT NULL DEFAULT 'browsertrix';
    )SQL",

    // v3 → v4 : unique URL per collection
    //   - create a unique index on (collection_id, url) in entry
    R"SQL(
        CREATE UNIQUE INDEX IF NOT EXISTS entry_collection_url_unique
            ON entry (collection_id, url);
    )SQL",

    // v4 → v5 : capture depth field
    //   - add capture_depth column to entry (default CURRENT_PAGE_ONLY)
    R"SQL(
        ALTER TABLE entry ADD COLUMN capture_depth TEXT NOT NULL DEFAULT 'CURRENT_PAGE_ONLY';
    )SQL",

    // v5 → v6 : collections and entries are replaced by timestamped captures
    //   - every archive_file becomes one capture (a capture owns exactly one archive file)
    //   - entries without any archive file become failed captures, so the URL is not lost
    //   - entry tags are copied to all captures of the entry
    //   - collection, entry, archive_file, crawl_run, capture_metadata, entry_tag, entry_note are dropped
    //   - url_key is filled in by Database::populateUrlKeys() after the migration
    R"SQL(
        CREATE TABLE capture (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            url           TEXT NOT NULL,
            url_key       TEXT NOT NULL DEFAULT '',
            timestamp     TEXT NOT NULL,
            title         TEXT,
            note          TEXT,
            status        TEXT NOT NULL DEFAULT 'queued',
            source        TEXT NOT NULL DEFAULT 'crawl',
            capture_depth TEXT NOT NULL DEFAULT 'CURRENT_PAGE_ONLY',
            page_limit    INTEGER,
            file_path     TEXT,
            file_type     TEXT,
            size_bytes    INTEGER,
            sha256        TEXT,
            error         TEXT,
            created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,
            started_at    DATETIME,
            finished_at   DATETIME,
            legacy_entry_id INTEGER
        );
        CREATE INDEX capture_url_key_idx   ON capture (url_key);
        CREATE INDEX capture_timestamp_idx ON capture (timestamp);
        CREATE INDEX capture_status_idx    ON capture (status);

        CREATE TABLE capture_tag (
            capture_id INTEGER NOT NULL,
            tag_id     INTEGER NOT NULL,
            PRIMARY KEY (capture_id, tag_id),
            FOREIGN KEY (capture_id) REFERENCES capture(id) ON DELETE CASCADE,
            FOREIGN KEY (tag_id)     REFERENCES tag(id)     ON DELETE CASCADE
        );
        CREATE INDEX capture_tag_tag_idx ON capture_tag (tag_id);

        INSERT INTO capture (url, timestamp, title, note, status, source, capture_depth,
                             file_path, file_type, size_bytes, sha256, created_at, finished_at, legacy_entry_id)
            SELECT e.url,
                   strftime('%Y%m%d%H%M%S', COALESCE(af.created_at, e.created_at, CURRENT_TIMESTAMP)),
                   e.title, e.note, 'archived',
                   CASE WHEN af.source = 'manual_upload' THEN 'upload' ELSE 'crawl' END,
                   e.capture_depth, af.path, af.file_type, af.size_bytes, af.sha256,
                   COALESCE(af.created_at, e.created_at), COALESCE(af.created_at, e.created_at), e.id
            FROM archive_file af JOIN entry e ON e.id = af.entry_id
            ORDER BY af.created_at, af.id;

        INSERT INTO capture (url, timestamp, title, note, status, source, capture_depth, error,
                             created_at, legacy_entry_id)
            SELECT e.url,
                   strftime('%Y%m%d%H%M%S', COALESCE(e.created_at, CURRENT_TIMESTAMP)),
                   e.title, e.note, 'failed', 'crawl', e.capture_depth,
                   'Migrated from an entry without an archive file. Use "Archive again".',
                   e.created_at, e.id
            FROM entry e
            WHERE NOT EXISTS (SELECT 1 FROM archive_file af WHERE af.entry_id = e.id);

        INSERT OR IGNORE INTO capture_tag (capture_id, tag_id)
            SELECT c.id, et.tag_id FROM capture c JOIN entry_tag et ON et.entry_id = c.legacy_entry_id;

        ALTER TABLE capture DROP COLUMN legacy_entry_id;

        DROP TABLE entry_note;
        DROP TABLE capture_metadata;
        DROP TABLE crawl_run;
        DROP TABLE archive_file;
        DROP TABLE entry_tag;
        DROP TABLE entry;
        DROP TABLE collection;
    )SQL",

    // v6 → v7 : covering index for the archive statistics (counts per status and total size),
    //   so they do not need a full table scan
    R"SQL(
        DROP INDEX capture_status_idx;
        CREATE INDEX capture_status_size_idx ON capture (status, size_bytes);
    )SQL",

    // v7 → v8 : crawl depth as a number of link hops plus a crawl scope, replacing capture_depth
    //   - CURRENT_PAGE_ONLY         -> max_depth 0
    //   - CURRENT_PAGE_AND_SUBPAGES -> max_depth -1 (unlimited), scope PREFIX (same path)
    R"SQL(
        ALTER TABLE capture ADD COLUMN max_depth INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE capture ADD COLUMN crawl_scope TEXT NOT NULL DEFAULT 'PREFIX';
        UPDATE capture SET max_depth = -1 WHERE capture_depth = 'CURRENT_PAGE_AND_SUBPAGES';
        ALTER TABLE capture DROP COLUMN capture_depth;
    )SQL",
};

static_assert(std::size(kMigrations) == kCurrentSchemaVersion);

void Database::initializeSchema() {
    // A fresh database has no schema_version table and starts at version 0.
    const bool isNew = [this]() {
        sqlite3_stmt* stmt{};
        const int rc = sqlite3_prepare_v2(
            db_,
            "SELECT name FROM sqlite_master WHERE type='table' AND name='schema_version';",
            -1, &stmt, nullptr);
        if (rc != SQLITE_OK) { return true; }
        const bool found = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        return !found;
    }();

    if (isNew) {
        // Fresh database: apply every migration in a single transaction.
        execute("BEGIN;");
        for (const char* sql : kMigrations) {
            execute(sql);
        }
        setSchemaVersion(kCurrentSchemaVersion);
        execute("COMMIT;");
    } else {
        migrateSchema();
    }
}

int Database::getSchemaVersion() const {
    Statement stmt(db_, "SELECT version FROM schema_version LIMIT 1;");
    return sqlite3_step(stmt.get()) == SQLITE_ROW ? sqlite3_column_int(stmt.get(), 0) : 0;
}

void Database::setSchemaVersion(int version) {
    // schema_version always holds exactly one row.
    Statement stmt(db_, "UPDATE schema_version SET version = ?;");
    stmt.bindInt(1, version);
    stepDone(db_, stmt);
}

void Database::migrateSchema() {
    const int current = getSchemaVersion();
    for (int v = current; v < kCurrentSchemaVersion; ++v) {
        // Dropping tables with foreign keys requires foreign key enforcement to be off.
        execute("PRAGMA foreign_keys = OFF;");
        execute("BEGIN;");
        try {
            execute(kMigrations[v]);
            if (v + 1 == 3) {
                populateNumberPerCollection();
            }
            if (v + 1 == 6) {
                populateUrlKeys();
            }
            setSchemaVersion(v + 1);
            execute("COMMIT;");
        } catch (...) {
            execute("ROLLBACK;");
            execute("PRAGMA foreign_keys = ON;");
            throw;
        }
        execute("PRAGMA foreign_keys = ON;");
    }
}

void Database::populateNumberPerCollection() {
    // v3 introduced per-collection entry numbers; number existing entries by id.
    execute(R"SQL(
        UPDATE entry SET number_per_collection = (
            SELECT COUNT(*) FROM entry e2
            WHERE e2.collection_id = entry.collection_id AND e2.id <= entry.id
        );
    )SQL");
}

void Database::populateUrlKeys() {
    std::vector<std::pair<int, std::string>> rows;
    {
        Statement stmt(db_, "SELECT id, url FROM capture WHERE url_key = '';");
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            rows.emplace_back(sqlite3_column_int(stmt.get(), 0), columnText(stmt.get(), 1));
        }
    }
    for (const auto& [id, url] : rows) {
        Statement stmt(db_, "UPDATE capture SET url_key = ? WHERE id = ?;");
        stmt.bindText(1, makeUrlKey(url));
        stmt.bindInt(2, id);
        stepDone(db_, stmt);
    }
}

// ---------------------------------------------------------------------------
// Captures
// ---------------------------------------------------------------------------

int Database::insertCapture(const Capture& capture) {
    std::lock_guard lock(mutex_);
    execute("BEGIN;");
    try {
        Statement stmt(db_, R"SQL(
            INSERT INTO capture (url, url_key, timestamp, title, note, status, source, max_depth, crawl_scope,
                                 page_limit, file_path, file_type, size_bytes, sha256, error, finished_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                    CASE WHEN ? = 'archived' THEN CURRENT_TIMESTAMP END);
        )SQL");
        stmt.bindText(1, capture.url);
        stmt.bindText(2, makeUrlKey(capture.url));
        stmt.bindText(3, capture.timestamp);
        stmt.bindOptionalText(4, capture.title);
        stmt.bindOptionalText(5, capture.note);
        stmt.bindText(6, capture.status);
        stmt.bindText(7, capture.source);
        stmt.bindInt(8, capture.maxDepth);
        stmt.bindText(9, crawlScopeToString(capture.scope));
        stmt.bindOptionalInt(10, capture.pageLimit);
        stmt.bindOptionalText(11, capture.filePath);
        stmt.bindOptionalText(12, capture.fileType);
        stmt.bindOptionalInt64(13, capture.sizeBytes);
        stmt.bindOptionalText(14, capture.sha256);
        stmt.bindOptionalText(15, capture.error);
        stmt.bindText(16, capture.status);
        stepDone(db_, stmt);
        const int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
        setCaptureTags(id, capture.tags);
        execute("COMMIT;");
        return id;
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

std::optional<Capture> Database::getCapture(int id) const {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, (std::string("SELECT ") + kCaptureColumns + " FROM capture c WHERE c.id = ?;").c_str());
    stmt.bindInt(1, id);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readCapture(stmt.get());
    }
    return std::nullopt;
}

std::vector<Capture> Database::findCaptures(const CaptureQuery& query) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> params;
    const std::string order = query.oldestFirst ? "ASC" : "DESC";
    const std::string sql = std::string("SELECT ") + kCaptureColumns + " FROM capture c"
        + captureWhereClause(query, params)
        + " ORDER BY c.timestamp " + order + ", c.id " + order + " LIMIT ? OFFSET ?;";
    Statement stmt(db_, sql.c_str());
    int index = 1;
    for (const auto& param : params) {
        stmt.bindText(index++, param);
    }
    stmt.bindInt(index++, query.limit);
    stmt.bindInt(index, query.offset);

    std::vector<Capture> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readCapture(stmt.get()));
    }
    return result;
}

int Database::countCaptures(const CaptureQuery& query) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> params;
    const std::string sql = "SELECT COUNT(*) FROM capture c" + captureWhereClause(query, params) + ";";
    Statement stmt(db_, sql.c_str());
    int index = 1;
    for (const auto& param : params) {
        stmt.bindText(index++, param);
    }
    return sqlite3_step(stmt.get()) == SQLITE_ROW ? sqlite3_column_int(stmt.get(), 0) : 0;
}

std::vector<Capture> Database::listCapturesForUrlKey(const std::string& urlKey) const {
    CaptureQuery query;
    query.urlKey = urlKey;
    query.limit = -1;  // SQLite: no limit
    return findCaptures(query);
}

std::optional<Capture> Database::findNearestCapture(const std::string& urlKey, const std::string& timestamp) const {
    std::lock_guard lock(mutex_);
    if (!isTimestamp(timestamp)) {
        return std::nullopt;
    }
    const std::string target = timestamp.substr(0, 4) + "-" + timestamp.substr(4, 2) + "-"
        + timestamp.substr(6, 2) + " " + timestamp.substr(8, 2) + ":"
        + timestamp.substr(10, 2) + ":" + timestamp.substr(12, 2);
    Statement stmt(db_, (std::string("SELECT ") + kCaptureColumns + R"SQL(
        FROM capture c
        WHERE c.url_key = ? AND c.status = 'archived'
        ORDER BY ABS(CAST(strftime('%s', substr(c.timestamp, 1, 4) || '-' ||
            substr(c.timestamp, 5, 2) || '-' || substr(c.timestamp, 7, 2) || ' ' ||
            substr(c.timestamp, 9, 2) || ':' || substr(c.timestamp, 11, 2) || ':' ||
            substr(c.timestamp, 13, 2)) AS INTEGER) -
            CAST(strftime('%s', ?) AS INTEGER)), c.id DESC
        LIMIT 1;
    )SQL").c_str());
    stmt.bindText(1, urlKey);
    stmt.bindText(2, target);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readCapture(stmt.get());
    }
    return std::nullopt;
}

void Database::updateCaptureDetails(int id, const std::string& url, const std::optional<std::string>& title,
                                    const std::optional<std::string>& note, const std::vector<std::string>& tags) {
    std::lock_guard lock(mutex_);
    execute("BEGIN;");
    try {
        Statement stmt(db_, "UPDATE capture SET url = ?, url_key = ?, title = ?, note = ? WHERE id = ?;");
        stmt.bindText(1, url);
        stmt.bindText(2, makeUrlKey(url));
        stmt.bindOptionalText(3, title);
        stmt.bindOptionalText(4, note);
        stmt.bindInt(5, id);
        stepDone(db_, stmt);
        setCaptureTags(id, tags);
        execute("COMMIT;");
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

void Database::deleteCapture(int id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM capture WHERE id = ?;");
    stmt.bindInt(1, id);
    stepDone(db_, stmt);
    deleteUnusedTags();
}

// ---------------------------------------------------------------------------
// Crawl queue
// ---------------------------------------------------------------------------

std::optional<Capture> Database::claimNextQueuedCapture() {
    std::lock_guard lock(mutex_);
    std::optional<int> id;
    {
        Statement stmt(db_, "SELECT id FROM capture WHERE status = 'queued' ORDER BY id ASC LIMIT 1;");
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            id = sqlite3_column_int(stmt.get(), 0);
        }
    }
    if (!id) {
        return std::nullopt;
    }
    // The Wayback timestamp is the time the page was actually captured, not when it was queued.
    Statement stmt(db_,
        "UPDATE capture SET status = 'crawling', timestamp = ?, started_at = CURRENT_TIMESTAMP, "
        "finished_at = NULL, error = NULL WHERE id = ?;");
    stmt.bindText(1, nowTimestamp());
    stmt.bindInt(2, *id);
    stepDone(db_, stmt);
    return getCapture(*id);
}

void Database::markCaptureArchived(int id, const std::string& filePath, const std::string& fileType,
                                   std::int64_t sizeBytes, const std::string& sha256,
                                   const std::optional<std::string>& pageTitle,
                                   const std::optional<std::string>& error) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, R"SQL(
        UPDATE capture
        SET status = 'archived', file_path = ?, file_type = ?, size_bytes = ?, sha256 = ?,
            title = COALESCE(NULLIF(title, ''), ?), error = ?, finished_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )SQL");
    stmt.bindText(1, filePath);
    stmt.bindText(2, fileType);
    stmt.bindInt64(3, sizeBytes);
    stmt.bindOptionalText(4, sha256.empty() ? std::nullopt : std::optional<std::string>{sha256});
    stmt.bindOptionalText(5, pageTitle);
    stmt.bindOptionalText(6, error);
    stmt.bindInt(7, id);
    stepDone(db_, stmt);
}

void Database::markCaptureFailed(int id, const std::string& error, const std::string& status) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
        "UPDATE capture SET status = ?, error = ?, finished_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, status);
    stmt.bindText(2, error);
    stmt.bindInt(3, id);
    stepDone(db_, stmt);
}

void Database::requeueCapture(int id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
        "UPDATE capture SET status = 'queued', error = NULL, started_at = NULL, finished_at = NULL "
        "WHERE id = ? AND source = 'crawl' AND status IN ('failed', 'cancelled');");
    stmt.bindInt(1, id);
    stepDone(db_, stmt);
}

bool Database::cancelQueuedCapture(int id) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_,
        "UPDATE capture SET status = 'cancelled', error = 'Cancelled before the crawl started.', "
        "finished_at = CURRENT_TIMESTAMP WHERE id = ? AND status = 'queued';");
    stmt.bindInt(1, id);
    stepDone(db_, stmt);
    return sqlite3_changes(db_) > 0;
}

std::vector<int> Database::requeueInterruptedCaptures() {
    std::lock_guard lock(mutex_);
    std::vector<int> ids;
    {
        Statement stmt(db_, "SELECT id FROM capture WHERE status = 'crawling';");
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            ids.push_back(sqlite3_column_int(stmt.get(), 0));
        }
    }
    execute("UPDATE capture SET status = 'queued', started_at = NULL WHERE status = 'crawling';");
    return ids;
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

void Database::setCaptureTags(int captureId, const std::vector<std::string>& tags) {
    {
        Statement stmt(db_, "DELETE FROM capture_tag WHERE capture_id = ?;");
        stmt.bindInt(1, captureId);
        stepDone(db_, stmt);
    }
    for (const auto& tag : tags) {
        {
            Statement stmt(db_, "INSERT OR IGNORE INTO tag (name) VALUES (?);");
            stmt.bindText(1, tag);
            stepDone(db_, stmt);
        }
        Statement stmt(db_,
            "INSERT OR IGNORE INTO capture_tag (capture_id, tag_id) SELECT ?, id FROM tag WHERE name = ?;");
        stmt.bindInt(1, captureId);
        stmt.bindText(2, tag);
        stepDone(db_, stmt);
    }
    deleteUnusedTags();
}

void Database::deleteUnusedTags() {
    execute("DELETE FROM tag WHERE id NOT IN (SELECT tag_id FROM capture_tag);");
}

std::vector<TagCount> Database::listTagCounts() const {
    std::lock_guard lock(mutex_);
    // COUNT(*) rather than COUNT(ct.capture_id): this lets SQLite answer from the tag_id index
    // alone instead of reading every capture_tag row (4 s vs 0.1 s with 2M tag assignments).
    Statement stmt(db_,
        "SELECT t.name, COUNT(*) FROM tag t "
        "JOIN capture_tag ct ON ct.tag_id = t.id GROUP BY t.id ORDER BY t.name;");
    std::vector<TagCount> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(TagCount{columnText(stmt.get(), 0), sqlite3_column_int(stmt.get(), 1)});
    }
    return result;
}

void Database::renameTag(const std::string& from, const std::string& to) {
    std::lock_guard lock(mutex_);
    if (from == to) {
        return;
    }
    execute("BEGIN;");
    try {
        // Renaming onto an existing tag merges the two tags.
        {
            Statement stmt(db_, "INSERT OR IGNORE INTO tag (name) VALUES (?);");
            stmt.bindText(1, to);
            stepDone(db_, stmt);
        }
        {
            Statement stmt(db_, R"SQL(
                INSERT OR IGNORE INTO capture_tag (capture_id, tag_id)
                SELECT ct.capture_id, (SELECT id FROM tag WHERE name = ?)
                FROM capture_tag ct JOIN tag t ON t.id = ct.tag_id WHERE t.name = ?;
            )SQL");
            stmt.bindText(1, to);
            stmt.bindText(2, from);
            stepDone(db_, stmt);
        }
        {
            Statement stmt(db_, "DELETE FROM tag WHERE name = ?;");
            stmt.bindText(1, from);
            stepDone(db_, stmt);
        }
        deleteUnusedTags();
        execute("COMMIT;");
    } catch (...) {
        execute("ROLLBACK;");
        throw;
    }
}

void Database::deleteTag(const std::string& name) {
    std::lock_guard lock(mutex_);
    Statement stmt(db_, "DELETE FROM tag WHERE name = ?;");
    stmt.bindText(1, name);
    stepDone(db_, stmt);
}

ArchiveStats Database::stats() const {
    std::lock_guard lock(mutex_);
    ArchiveStats result;
    // Two index-only scans (capture_status_size_idx and capture_url_key_idx); a single
    // combined query would need a full table scan.
    {
        Statement stmt(db_, "SELECT status, COUNT(*), COALESCE(SUM(size_bytes), 0) FROM capture GROUP BY status;");
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const std::string status = columnText(stmt.get(), 0);
            const int count = sqlite3_column_int(stmt.get(), 1);
            result.captures += count;
            result.totalBytes += sqlite3_column_int64(stmt.get(), 2);
            if (status == "archived") result.archived = count;
            else if (status == "queued") result.queued = count;
            else if (status == "crawling") result.crawling = count;
            else if (status == "failed") result.failed = count;
        }
    }
    {
        Statement stmt(db_, "SELECT COUNT(DISTINCT url_key) FROM capture;");
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            result.urls = sqlite3_column_int(stmt.get(), 0);
        }
    }
    return result;
}

} // namespace warc_studio
