#pragma once

#include "warc_studio/Database.hpp"
#include "warc_studio/FileService.hpp"
#include "warc_studio/Models.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace warc_studio {

struct CrawlConfig {
    std::string userAgent;              // User-Agent header sent by the crawler
    int workers{2};                     // number of crawls running in parallel
    int timeLimitSeconds{0};            // per crawl, 0 = none
    std::int64_t maxResourceBytes{};    // larger responses are skipped
    std::int64_t maxBufferedBytes{};    // shared raw response budget across crawls
};

// Runs queued captures in background worker threads with the built-in crawler (see Crawler.hpp).
// Every capture is written to its own WARC file, which is moved into the archive directory
// when the crawl ends.
class CrawlService {
public:
    CrawlService(Database& database, const FileService& files, CrawlConfig config);
    ~CrawlService();

    CrawlService(const CrawlService&) = delete;
    CrawlService& operator=(const CrawlService&) = delete;

    // Re-queues crawls interrupted by a restart and starts the worker threads.
    void start();
    // Wakes the workers after captures were queued.
    void notify();
    // Asks a running crawl to finish early; what was fetched so far is kept.
    void requestStop(int captureId);

    [[nodiscard]] const CrawlConfig& config() const { return config_; }
    [[nodiscard]] std::filesystem::path logPath(int captureId) const;

private:
    Database& database_;
    const FileService& files_;
    CrawlConfig config_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::map<int, std::shared_ptr<std::atomic<bool>>> running_;  // capture id -> stop flag
    std::vector<std::thread> threads_;

    void workerLoop();
    void runCapture(const Capture& capture, const std::atomic<bool>& stop);
};

} // namespace warc_studio
