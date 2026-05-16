#pragma once

#include "warc_studio/Models.hpp"

#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace warc_studio {

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    int createCollection(const std::string& name);
    std::vector<Collection> listCollections() const;
    std::optional<Collection> getCollection(int id) const;

    int createEntry(int collectionId, const std::string& url, const std::optional<std::string>& title);
    std::vector<Entry> listEntries() const;
    std::optional<Entry> getEntry(int id) const;
    void setEntryBrowsertrixId(int id, const std::string& browsertrixId);
    void setEntryWarcPath(int id, const std::string& warcPath);
    void deleteEntry(int id);

private:
    sqlite3* db_{};

    void execute(const char* sql) const;
    void initializeSchema();
    int getSchemaVersion() const;
    void setSchemaVersion(int version);
    void migrateSchema();
};

} // namespace warc_studio
