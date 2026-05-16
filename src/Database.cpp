#include "warc_studio/Database.hpp"

#include <filesystem>
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

std::optional<int> optionalColumnInt(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(stmt, index);
}

// ---------------------------------------------------------------------------
// Row readers
// ---------------------------------------------------------------------------

Collection readCollection(sqlite3_stmt* stmt) {
    // Columns: id, name, description, created_at, updated_at
    return Collection{
        .id = sqlite3_column_int(stmt, 0),
        .name = columnText(stmt, 1),
        .description = optionalColumnText(stmt, 2),
        .createdAt = columnText(stmt, 3),
        .updatedAt = columnText(stmt, 4),
    };
}

Entry readEntry(sqlite3_stmt* stmt) {
    // Columns: e.id, e.collection_id, c.name, e.url, e.normalized_url,
    //          e.title, e.status, e.note, e.created_at, e.updated_at,
    //          e.archived_at, e.last_error, e.warc_path, e.browsertrix_id
    return Entry{
        .id = sqlite3_column_int(stmt, 0),
        .collectionId = sqlite3_column_int(stmt, 1),
        .collectionName = columnText(stmt, 2),
        .url = columnText(stmt, 3),
        .normalizedUrl = optionalColumnText(stmt, 4),
        .title = optionalColumnText(stmt, 5),
        .status = columnText(stmt, 6),
        .note = optionalColumnText(stmt, 7),
        .createdAt = columnText(stmt, 8),
        .updatedAt = columnText(stmt, 9),
        .archivedAt = optionalColumnText(stmt, 10),
        .lastError = optionalColumnText(stmt, 11),
        .warcPath = optionalColumnText(stmt, 12),
        .browsertrixId = optionalColumnText(stmt, 13),
    };
}

ArchiveFile readArchiveFile(sqlite3_stmt* stmt) {
    // Columns: id, entry_id, path, file_type, size_bytes, sha256, created_at
    return ArchiveFile{
        .id = sqlite3_column_int(stmt, 0),
        .entryId = sqlite3_column_int(stmt, 1),
        .path = columnText(stmt, 2),
        .fileType = columnText(stmt, 3),
        .sizeBytes = optionalColumnInt(stmt, 4),
        .sha256 = optionalColumnText(stmt, 5),
        .createdAt = columnText(stmt, 6),
    };
}

Tag readTag(sqlite3_stmt* stmt) {
    // Columns: id, name
    return Tag{
        .id = sqlite3_column_int(stmt, 0),
        .name = columnText(stmt, 1),
    };
}

EntryNote readEntryNote(sqlite3_stmt* stmt) {
    // Columns: id, entry_id, body, created_at
    return EntryNote{
        .id = sqlite3_column_int(stmt, 0),
        .entryId = sqlite3_column_int(stmt, 1),
        .body = columnText(stmt, 2),
        .createdAt = columnText(stmt, 3),
    };
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
static constexpr int kCurrentSchemaVersion = 2;

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
};

void Database::initializeSchema() {
    // Detect fresh database: schema_version table is absent.
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
    sqlite3_stmt* stmt{};
    if (sqlite3_prepare_v2(db_, "SELECT version FROM schema_version LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

void Database::setSchemaVersion(int version) {
    // schema_version always holds exactly one row.
    char* error = nullptr;
    const std::string sql = "UPDATE schema_version SET version = " + std::to_string(version) + ";";
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? "SQLite error" : error;
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void Database::migrateSchema() {
    const int current = getSchemaVersion();
    if (current >= kCurrentSchemaVersion) {
        return; // already up-to-date
    }
    for (int v = current; v < kCurrentSchemaVersion; ++v) {
        execute("BEGIN;");
        execute(kMigrations[v]);
        setSchemaVersion(v + 1);
        execute("COMMIT;");
    }
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

int Database::createCollection(const std::string& name, const std::optional<std::string>& description) {
    Statement stmt(db_, "INSERT INTO collection (name, description) VALUES (?, ?);");
    stmt.bindText(1, name);
    stmt.bindOptionalText(2, description);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Collection> Database::listCollections() const {
    Statement stmt(db_, "SELECT id, name, description, created_at, updated_at FROM collection ORDER BY name;");
    std::vector<Collection> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readCollection(stmt.get()));
    }
    return result;
}

std::optional<Collection> Database::getCollection(int id) const {
    Statement stmt(db_, "SELECT id, name, description, created_at, updated_at FROM collection WHERE id = ?;");
    stmt.bindInt(1, id);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readCollection(stmt.get());
    }
    return std::nullopt;
}

void Database::updateCollection(int id, const std::string& name, const std::optional<std::string>& description) {
    Statement stmt(db_,
        "UPDATE collection SET name = ?, description = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, name);
    stmt.bindOptionalText(2, description);
    stmt.bindInt(3, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::deleteCollection(int id) {
    // ON DELETE CASCADE will remove all entries (and their children) automatically.
    Statement stmt(db_, "DELETE FROM collection WHERE id = ?;");
    stmt.bindInt(1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

static const char* kListEntriesSQL = R"SQL(
    SELECT
        e.id,
        e.collection_id,
        c.name,
        e.url,
        e.normalized_url,
        e.title,
        e.status,
        e.note,
        e.created_at,
        e.updated_at,
        e.archived_at,
        e.last_error,
        e.warc_path,
        e.browsertrix_id
    FROM entry e
    JOIN collection c ON c.id = e.collection_id
    ORDER BY e.id DESC;
)SQL";

static const char* kGetEntrySQL = R"SQL(
    SELECT
        e.id,
        e.collection_id,
        c.name,
        e.url,
        e.normalized_url,
        e.title,
        e.status,
        e.note,
        e.created_at,
        e.updated_at,
        e.archived_at,
        e.last_error,
        e.warc_path,
        e.browsertrix_id
    FROM entry e
    JOIN collection c ON c.id = e.collection_id
    WHERE e.id = ?;
)SQL";

int Database::createEntry(int collectionId, const std::string& url, const std::optional<std::string>& title) {
    Statement stmt(db_, "INSERT INTO entry (collection_id, url, title) VALUES (?, ?, ?);");
    stmt.bindInt(1, collectionId);
    stmt.bindText(2, url);
    stmt.bindOptionalText(3, title);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Entry> Database::listEntries() const {
    Statement stmt(db_, kListEntriesSQL);
    std::vector<Entry> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readEntry(stmt.get()));
    }
    return result;
}

std::vector<Entry> Database::listEntriesByCollection(int collectionId) const {
    Statement stmt(db_, R"SQL(
        SELECT
            e.id,
            e.collection_id,
            c.name,
            e.url,
            e.normalized_url,
            e.title,
            e.status,
            e.note,
            e.created_at,
            e.updated_at,
            e.archived_at,
            e.last_error,
            e.warc_path,
            e.browsertrix_id
        FROM entry e
        JOIN collection c ON c.id = e.collection_id
        WHERE e.collection_id = ?
        ORDER BY e.id DESC;
    )SQL");
    stmt.bindInt(1, collectionId);
    std::vector<Entry> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readEntry(stmt.get()));
    }
    return result;
}

std::optional<Entry> Database::getEntry(int id) const {
    Statement stmt(db_, kGetEntrySQL);
    stmt.bindInt(1, id);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readEntry(stmt.get());
    }
    return std::nullopt;
}

void Database::updateEntryStatus(int entryId, const std::string& status) {
    Statement stmt(db_,
        "UPDATE entry SET status = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, status);
    stmt.bindInt(2, entryId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::setEntryError(int entryId, const std::string& errorMessage) {
    Statement stmt(db_,
        "UPDATE entry SET status = 'failed', last_error = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, errorMessage);
    stmt.bindInt(2, entryId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::markEntryArchived(int entryId) {
    Statement stmt(db_,
        "UPDATE entry SET status = 'archived', archived_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindInt(1, entryId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

// ---------------------------------------------------------------------------
// Archive files
// ---------------------------------------------------------------------------

int Database::addArchiveFile(int entryId, const std::string& path, const std::string& fileType,
                             const std::optional<int>& sizeBytes,
                             const std::optional<std::string>& sha256) {
    Statement stmt(db_,
        "INSERT INTO archive_file (entry_id, path, file_type, size_bytes, sha256) VALUES (?, ?, ?, ?, ?);");
    stmt.bindInt(1, entryId);
    stmt.bindText(2, path);
    stmt.bindText(3, fileType);
    stmt.bindOptionalInt(4, sizeBytes);
    stmt.bindOptionalText(5, sha256);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::optional<ArchiveFile> Database::getLatestArchiveFile(int entryId) const {
    Statement stmt(db_,
        "SELECT id, entry_id, path, file_type, size_bytes, sha256, created_at "
        "FROM archive_file WHERE entry_id = ? ORDER BY id DESC LIMIT 1;");
    stmt.bindInt(1, entryId);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readArchiveFile(stmt.get());
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Crawl runs
// ---------------------------------------------------------------------------

int Database::createCrawlRun(int entryId, const std::optional<std::string>& browsertrixId,
                              const std::optional<std::string>& dockerContainerId) {
    Statement stmt(db_,
        "INSERT INTO crawl_run (entry_id, browsertrix_id, docker_container_id, status) "
        "VALUES (?, ?, ?, 'created');");
    stmt.bindInt(1, entryId);
    stmt.bindOptionalText(2, browsertrixId);
    stmt.bindOptionalText(3, dockerContainerId);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

void Database::updateCrawlRunStarted(int crawlRunId) {
    Statement stmt(db_,
        "UPDATE crawl_run SET status = 'running', started_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindInt(1, crawlRunId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::updateCrawlRunStopped(int crawlRunId, const std::string& status,
                                     const std::optional<int>& exitCode,
                                     const std::optional<std::string>& errorMessage) {
    Statement stmt(db_,
        "UPDATE crawl_run SET status = ?, stopped_at = CURRENT_TIMESTAMP, exit_code = ?, error_message = ? "
        "WHERE id = ?;");
    stmt.bindText(1, status);
    stmt.bindOptionalInt(2, exitCode);
    stmt.bindOptionalText(3, errorMessage);
    stmt.bindInt(4, crawlRunId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

// ---------------------------------------------------------------------------
// Entry notes
// ---------------------------------------------------------------------------

int Database::addEntryNote(int entryId, const std::string& body) {
    Statement stmt(db_, "INSERT INTO entry_note (entry_id, body) VALUES (?, ?);");
    stmt.bindInt(1, entryId);
    stmt.bindText(2, body);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<EntryNote> Database::listEntryNotes(int entryId) const {
    Statement stmt(db_,
        "SELECT id, entry_id, body, created_at FROM entry_note WHERE entry_id = ? ORDER BY id ASC;");
    stmt.bindInt(1, entryId);
    std::vector<EntryNote> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readEntryNote(stmt.get()));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

int Database::addTag(const std::string& name) {
    Statement stmt(db_, "INSERT OR IGNORE INTO tag (name) VALUES (?);");
    stmt.bindText(1, name);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    // Return the id whether or not the row was newly inserted.
    Statement idStmt(db_, "SELECT id FROM tag WHERE name = ?;");
    idStmt.bindText(1, name);
    if (sqlite3_step(idStmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int(idStmt.get(), 0);
    }
    throw std::runtime_error("Failed to retrieve tag id after insert");
}

void Database::assignTagToEntry(int entryId, int tagId) {
    Statement stmt(db_, "INSERT OR IGNORE INTO entry_tag (entry_id, tag_id) VALUES (?, ?);");
    stmt.bindInt(1, entryId);
    stmt.bindInt(2, tagId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

std::vector<Tag> Database::listEntryTags(int entryId) const {
    Statement stmt(db_,
        "SELECT t.id, t.name FROM tag t "
        "JOIN entry_tag et ON et.tag_id = t.id "
        "WHERE et.entry_id = ? ORDER BY t.name;");
    stmt.bindInt(1, entryId);
    std::vector<Tag> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readTag(stmt.get()));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Legacy helpers
// ---------------------------------------------------------------------------

void Database::setEntryBrowsertrixId(int id, const std::string& browsertrixId) {
    Statement stmt(db_,
        "UPDATE entry SET browsertrix_id = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, browsertrixId);
    stmt.bindInt(2, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::setEntryWarcPath(int id, const std::string& warcPath) {
    Statement stmt(db_,
        "UPDATE entry SET warc_path = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?;");
    stmt.bindText(1, warcPath);
    stmt.bindInt(2, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::deleteEntry(int id) {
    Statement stmt(db_, "DELETE FROM entry WHERE id = ?;");
    stmt.bindInt(1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

} // namespace warc_studio
