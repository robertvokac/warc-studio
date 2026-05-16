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

    // Collections
    int createCollection(const std::string& name, const std::optional<std::string>& description = std::nullopt);
    std::vector<Collection> listCollections() const;
    std::optional<Collection> getCollection(int id) const;

    // Entries
    int createEntry(int collectionId, const std::string& url, const std::optional<std::string>& title = std::nullopt);
    std::vector<Entry> listEntries() const;
    std::optional<Entry> getEntry(int id) const;
    void updateEntryStatus(int entryId, const std::string& status);
    void setEntryError(int entryId, const std::string& errorMessage);
    void markEntryArchived(int entryId);

    // Archive files
    int addArchiveFile(int entryId, const std::string& path, const std::string& fileType = "wacz",
                       const std::optional<int>& sizeBytes = std::nullopt,
                       const std::optional<std::string>& sha256 = std::nullopt);
    std::optional<ArchiveFile> getLatestArchiveFile(int entryId) const;

    // Crawl runs
    int createCrawlRun(int entryId, const std::optional<std::string>& browsertrixId,
                       const std::optional<std::string>& dockerContainerId);
    void updateCrawlRunStarted(int crawlRunId);
    void updateCrawlRunStopped(int crawlRunId, const std::string& status,
                               const std::optional<int>& exitCode = std::nullopt,
                               const std::optional<std::string>& errorMessage = std::nullopt);

    // Entry notes
    int addEntryNote(int entryId, const std::string& body);
    std::vector<EntryNote> listEntryNotes(int entryId) const;

    // Tags
    int addTag(const std::string& name);
    void assignTagToEntry(int entryId, int tagId);
    std::vector<Tag> listEntryTags(int entryId) const;

    // Legacy helpers (kept for compatibility; prefer archive_file and crawl_run)
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
