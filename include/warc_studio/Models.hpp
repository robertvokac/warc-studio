#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace warc_studio {

struct Collection {
    int id{};
    std::string name;
    std::optional<std::string> description;
    std::string createdAt;
    std::string updatedAt;
};

// Capture depth controls which discovered links may be enqueued during crawling.
// Resources required by the current page (CSS, JS, images, fonts, XHR) are
// always captured regardless of this setting.
enum class CaptureDepth {
    CURRENT_PAGE_ONLY,        // only the starting URL; no other page links
    CURRENT_PAGE_AND_SUBPAGES // starting URL + URLs whose path is under the base path
};

inline std::string captureDepthToString(CaptureDepth d) {
    return d == CaptureDepth::CURRENT_PAGE_AND_SUBPAGES
        ? "CURRENT_PAGE_AND_SUBPAGES"
        : "CURRENT_PAGE_ONLY";
}

inline CaptureDepth captureDepthFromString(const std::string& s) {
    return s == "CURRENT_PAGE_AND_SUBPAGES"
        ? CaptureDepth::CURRENT_PAGE_AND_SUBPAGES
        : CaptureDepth::CURRENT_PAGE_ONLY;
}

inline std::string captureDepthLabel(CaptureDepth d) {
    return d == CaptureDepth::CURRENT_PAGE_AND_SUBPAGES
        ? "Page + subpages"
        : "Page only";
}

struct Entry {
    int id{};
    int collectionId{};
    int numberPerCollection{};
    std::string collectionName;
    std::string url;
    std::optional<std::string> normalizedUrl;
    std::optional<std::string> title;
    std::string status;   // new | queued | recording | archived | imported | failed | needs_review | ignored
    std::optional<std::string> note;
    std::string createdAt;
    std::string updatedAt;
    std::optional<std::string> archivedAt;
    std::optional<std::string> lastError;
    int archiveFileCount{};
    CaptureDepth captureDepth{CaptureDepth::CURRENT_PAGE_ONLY};

    // Legacy columns kept for backward compatibility during migration.
    // New code should prefer ArchiveFile and CrawlRun.
    std::optional<std::string> warcPath;
    std::optional<std::string> browsertrixId;
};

struct ArchiveFile {
    int id{};
    int entryId{};
    std::string path;
    std::string fileType;  // wacz | warc
    std::optional<std::string> label;
    std::optional<std::string> source;
    std::optional<std::int64_t> sizeBytes;
    std::optional<std::string> sha256;
    std::string createdAt;
};

struct CrawlRun {
    int id{};
    int entryId{};
    std::optional<std::string> browsertrixId;
    std::optional<std::string> dockerContainerId;
    std::string status;   // created | running | stopped | finished | failed
    std::optional<std::string> startedAt;
    std::optional<std::string> stoppedAt;
    std::optional<int> exitCode;
    std::optional<std::string> errorMessage;
    std::optional<std::string> logPath;
};

struct CaptureMetadata {
    int id{};
    int entryId{};
    std::optional<std::string> finalUrl;
    std::optional<int> httpStatus;
    std::optional<std::string> contentType;
    std::optional<std::string> screenshotPath;
    std::optional<std::string> pageTitle;
    std::string capturedAt;
};

struct Tag {
    int id{};
    std::string name;
};

struct EntryNote {
    int id{};
    int entryId{};
    std::string body;
    std::string createdAt;
};

} // namespace warc_studio
