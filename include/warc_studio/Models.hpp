#pragma once
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

struct Entry {
    int id{};
    int collectionId{};
    std::string collectionName;
    std::string url;
    std::optional<std::string> normalizedUrl;
    std::optional<std::string> title;
    std::string status;   // new | recording | archived | failed
    std::optional<std::string> note;
    std::string createdAt;
    std::string updatedAt;
    std::optional<std::string> archivedAt;
    std::optional<std::string> lastError;

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
    std::optional<int> sizeBytes;
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
