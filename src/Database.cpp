#include "warc_studio/Database.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace warc_studio {
namespace {

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

private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};

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

Collection readCollection(sqlite3_stmt* stmt) {
    return Collection{
        .id = sqlite3_column_int(stmt, 0),
        .name = columnText(stmt, 1),
        .createdAt = columnText(stmt, 2),
    };
}

Entry readEntry(sqlite3_stmt* stmt) {
    return Entry{
        .id = sqlite3_column_int(stmt, 0),
        .collectionId = sqlite3_column_int(stmt, 1),
        .collectionName = columnText(stmt, 2),
        .url = columnText(stmt, 3),
        .title = optionalColumnText(stmt, 4),
        .warcPath = optionalColumnText(stmt, 5),
        .browsertrixId = optionalColumnText(stmt, 6),
        .createdAt = columnText(stmt, 7),
    };
}

} // namespace

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

// ---------- schema versioning ----------

// Current schema version — increment when adding new migrations below.
static constexpr int kCurrentSchemaVersion = 1;

// Each element applies one forward migration.
// Index 0 → migrates from version 0 to version 1.
// Index N → migrates from version N to version N+1.
static const char* kMigrations[] = {
    // v0 → v1 : initial schema
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
};

void Database::initializeSchema() {
    // Bootstrap: if schema_version table does not exist yet, run migration from v0.
    // Otherwise just migrate forward from the stored version.
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
        // Fresh database: run all migrations in one transaction.
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
    // schema_version always has exactly one row.
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

    execute("BEGIN;");
    for (int v = current; v < kCurrentSchemaVersion; ++v) {
        execute(kMigrations[v]);
        setSchemaVersion(v + 1);
    }
    execute("COMMIT;");
}

int Database::createCollection(const std::string& name) {
    Statement stmt(db_, "INSERT INTO collection (name) VALUES (?);");
    stmt.bindText(1, name);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }

    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Collection> Database::listCollections() const {
    Statement stmt(db_, "SELECT id, name, created_at FROM collection ORDER BY name;");
    std::vector<Collection> result;

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readCollection(stmt.get()));
    }

    return result;
}

std::optional<Collection> Database::getCollection(int id) const {
    Statement stmt(db_, "SELECT id, name, created_at FROM collection WHERE id = ?;");
    stmt.bindInt(1, id);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readCollection(stmt.get());
    }

    return std::nullopt;
}

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
    Statement stmt(db_, R"SQL(
        SELECT
            e.id,
            e.collection_id,
            c.name,
            e.url,
            e.title,
            e.warc_path,
            e.browsertrix_id,
            e.created_at
        FROM entry e
        JOIN collection c ON c.id = e.collection_id
        ORDER BY e.created_at DESC, e.id DESC;
    )SQL");

    std::vector<Entry> result;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        result.push_back(readEntry(stmt.get()));
    }

    return result;
}

std::optional<Entry> Database::getEntry(int id) const {
    Statement stmt(db_, R"SQL(
        SELECT
            e.id,
            e.collection_id,
            c.name,
            e.url,
            e.title,
            e.warc_path,
            e.browsertrix_id,
            e.created_at
        FROM entry e
        JOIN collection c ON c.id = e.collection_id
        WHERE e.id = ?;
    )SQL");
    stmt.bindInt(1, id);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return readEntry(stmt.get());
    }

    return std::nullopt;
}

void Database::setEntryBrowsertrixId(int id, const std::string& browsertrixId) {
    Statement stmt(db_, "UPDATE entry SET browsertrix_id = ? WHERE id = ?;");
    stmt.bindText(1, browsertrixId);
    stmt.bindInt(2, id);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
}

void Database::setEntryWarcPath(int id, const std::string& warcPath) {
    Statement stmt(db_, "UPDATE entry SET warc_path = ? WHERE id = ?;");
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
