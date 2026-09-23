#pragma once

#include "warc_studio/Models.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace warc_studio {

struct ArchiveStats {
    int captures{};
    int urls{};
    int archived{};
    int queued{};
    int crawling{};
    int failed{};
    std::int64_t totalBytes{};
};

// Thin SQLite wrapper. All public methods are thread-safe (they share one
// connection guarded by a mutex); HTTP handlers and crawl workers use it concurrently.
class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Captures
    // Inserts a capture (id and createdAt are ignored) together with its tags; returns the new id.
    int insertCapture(const Capture& capture);
    std::optional<Capture> getCapture(int id) const;
    std::vector<Capture> findCaptures(const CaptureQuery& query) const;
    int countCaptures(const CaptureQuery& query) const;
    std::vector<Capture> listCapturesForUrlKey(const std::string& urlKey) const;
    // The archived capture of urlKey closest in time to timestamp.
    std::optional<Capture> findNearestCapture(const std::string& urlKey, const std::string& timestamp) const;
    void updateCaptureDetails(int id, const std::string& url, const std::optional<std::string>& title,
                              const std::optional<std::string>& note, const std::vector<std::string>& tags);
    void deleteCapture(int id);

    // Crawl queue
    // Atomically moves the oldest queued capture to 'crawling' and stamps its timestamp.
    std::optional<Capture> claimNextQueuedCapture();
    void markCaptureArchived(int id, const std::string& filePath, const std::string& fileType,
                             std::int64_t sizeBytes, const std::string& sha256,
                             const std::optional<std::string>& pageTitle,
                             const std::optional<std::string>& error);
    void markCaptureFailed(int id, const std::string& error, const std::string& status = "failed");
    void requeueCapture(int id);
    // Cancels a capture that is still waiting in the queue; false if it already left the queue.
    bool cancelQueuedCapture(int id);
    // Moves captures left in 'crawling' by a previous run back to the queue; returns their ids.
    std::vector<int> requeueInterruptedCaptures();

    // Tags
    std::vector<TagCount> listTagCounts() const;
    void renameTag(const std::string& from, const std::string& to);
    void deleteTag(const std::string& name);

    ArchiveStats stats() const;

private:
    sqlite3* db_{};
    mutable std::recursive_mutex mutex_;

    void execute(const char* sql) const;
    void initializeSchema();
    int getSchemaVersion() const;
    void setSchemaVersion(int version);
    void migrateSchema();
    void populateNumberPerCollection();
    void populateUrlKeys();
    void setCaptureTags(int captureId, const std::vector<std::string>& tags);
    void deleteUnusedTags();
};

} // namespace warc_studio
