#include "warc_studio/CrawlService.hpp"
#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/Crawler.hpp"

#include <fstream>
#include <iostream>

namespace warc_studio {

CrawlService::CrawlService(Database& database, const FileService& files, CrawlConfig config)
    : database_(database), files_(files), config_(std::move(config)) {
    if (config_.workers < 1) {
        config_.workers = 1;
    }
}

CrawlService::~CrawlService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [id, flag] : running_) {
            *flag = true;
        }
    }
    wake_.notify_all();
    // Workers access this service, the database and the file service. Wait for them
    // before any of those objects can be destroyed.
    for (auto& thread : threads_) {
        thread.join();
    }
}

std::filesystem::path CrawlService::logPath(int captureId) const {
    return files_.logsRoot() / ("capture-" + std::to_string(captureId) + ".log");
}

void CrawlService::start() {
    for (const int id : database_.requeueInterruptedCaptures()) {
        std::cout << "Re-queued interrupted capture " << id << "\n";
    }
    for (int i = 0; i < config_.workers; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

void CrawlService::notify() {
    wake_.notify_all();
}

void CrawlService::requestStop(int captureId) {
    std::lock_guard lock(mutex_);
    if (const auto it = running_.find(captureId); it != running_.end()) {
        *it->second = true;
    }
}

void CrawlService::workerLoop() {
    for (;;) {
        std::optional<Capture> capture;
        auto stop = std::make_shared<std::atomic<bool>>(false);
        {
            std::unique_lock lock(mutex_);
            if (stopping_) return;
            capture = database_.claimNextQueuedCapture();
            if (!capture) {
                // Poll occasionally as a safety net in case a notify() was missed.
                wake_.wait_for(lock, std::chrono::seconds(30));
                continue;
            }
            running_[capture->id] = stop;
        }
        try {
            runCapture(*capture, *stop);
        } catch (const std::exception& error) {
            database_.markCaptureFailed(capture->id, std::string("Crawl failed: ") + error.what());
        }
        std::lock_guard lock(mutex_);
        running_.erase(capture->id);
    }
}

void CrawlService::runCapture(const Capture& capture, const std::atomic<bool>& stop) {
    const auto partial = files_.crawlsRoot() / ("capture-" + std::to_string(capture.id) + ".warc.gz");
    std::error_code ec;
    std::filesystem::remove(partial, ec);  // leftover of an interrupted attempt

    std::ofstream log(logPath(capture.id), std::ios::app);
    log << "=== " << formatTimestamp(nowTimestamp()) << " UTC  capture " << capture.id << ": " << capture.url
        << " (" << crawlDepthLabel(capture) << ")\n" << std::flush;

    const CrawlOptions options{
        .url = capture.url,
        .maxDepth = capture.maxDepth,
        .scope = capture.scope,
        .pageLimit = capture.pageLimit.value_or(0),
        .userAgent = config_.userAgent,
        .timeLimitSeconds = config_.timeLimitSeconds,
        .maxResourceBytes = config_.maxResourceBytes,
        .maxBufferedBytes = config_.maxBufferedBytes,
    };

    CrawlResult result;
    try {
        result = crawlToWarc(options, partial, log, stop);
    } catch (const std::exception& error) {
        std::filesystem::remove(partial, ec);
        log << "=== failed: " << error.what() << "\n";
        if (stop) {
            database_.markCaptureFailed(capture.id, "Stopped before the page was archived.", "cancelled");
        } else {
            database_.markCaptureFailed(capture.id, error.what());
        }
        return;
    }

    log << "=== " << result.pages << " page(s)"
        << (result.pagesNotCrawled > 0 ? " (page limit, " + std::to_string(result.pagesNotCrawled) + " not crawled)" : "")
        << ", " << result.resources << " resource(s), "
        << result.failedResources << " failed\n" << std::flush;

    const auto stored = files_.storeArchiveFile(capture.timestamp, partial);

    std::string warnings;
    const auto warn = [&warnings](const std::string& text) {
        warnings += (warnings.empty() ? "" : " ") + text;
    };
    if (result.status >= 400) {
        warn("The server answered HTTP " + std::to_string(result.status) + ".");
    }
    if (result.stopped) {
        warn("Stopped early by user; the archive contains what was fetched until then.");
    } else if (result.timedOut) {
        warn("The time limit was reached; the archive may be incomplete.");
    }
    if (result.pagesNotCrawled > 0) {
        warn("The page limit was reached; " + std::to_string(result.pagesNotCrawled)
             + " more linked page(s) were not archived.");
    }
    if (result.resourceLimitReached) {
        warn("The resource limit was reached; some resources are missing.");
    }
    if (result.failedResources > 0) {
        warn(std::to_string(result.failedResources) + " resource(s) could not be fetched (see the crawl log).");
    }
    database_.markCaptureArchived(
        capture.id, stored.storedPath, stored.fileType, stored.sizeBytes, stored.sha256,
        result.title.empty() ? std::nullopt : std::optional<std::string>{result.title},
        warnings.empty() ? std::nullopt : std::optional<std::string>{warnings});
    log << "=== archived as " << stored.storedPath << " (" << stored.sizeBytes << " bytes)\n";
}

} // namespace warc_studio
