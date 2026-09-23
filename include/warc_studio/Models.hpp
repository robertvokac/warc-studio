#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace warc_studio {

// Which linked pages a crawl may follow (the page's own resources — CSS, JS, images,
// fonts, media, iframes — are always captured, whatever their host).
enum class CrawlScope {
    PREFIX,  // same host, path under the start page's directory
    DOMAIN,  // the start page's site domain, subdomains included
    ANY,     // any http(s) site (only with a limited depth)
};

inline std::string crawlScopeToString(CrawlScope scope) {
    switch (scope) {
        case CrawlScope::DOMAIN: return "DOMAIN";
        case CrawlScope::ANY: return "ANY";
        default: return "PREFIX";
    }
}

inline CrawlScope crawlScopeFromString(const std::string& s) {
    if (s == "DOMAIN") return CrawlScope::DOMAIN;
    if (s == "ANY") return CrawlScope::ANY;
    return CrawlScope::PREFIX;
}

inline std::string crawlScopeLabel(CrawlScope scope) {
    switch (scope) {
        case CrawlScope::DOMAIN: return "whole domain";
        case CrawlScope::ANY: return "any site";
        default: return "same path";
    }
}

constexpr int kUnlimitedDepth = -1;
constexpr int kMaxCrawlDepth = 10;

// A capture (a.k.a. snapshot, in Wayback Machine terms) is one archiving request
// for one URL at one point in time. Every capture owns exactly one archive file
// (a WARC written by the built-in crawler, or a user-uploaded WARC/WACZ).
struct Capture {
    int id{};
    std::string url;
    std::string urlKey;       // normalized URL used for "is this URL archived?" lookups
    std::string timestamp;    // YYYYMMDDhhmmss (UTC), Wayback style
    std::optional<std::string> title;
    std::optional<std::string> note;
    std::string status;       // queued | crawling | archived | failed | cancelled
    std::string source;       // crawl | upload
    int maxDepth{0};          // link hops followed from the start page: 0 = page only, kUnlimitedDepth = no limit
    CrawlScope scope{CrawlScope::PREFIX};  // which linked pages may be followed when maxDepth != 0
    std::optional<int> pageLimit;          // max pages when maxDepth != 0, 0 = no limit
    std::optional<std::string> filePath;   // stored path relative to the data dir (archives/...)
    std::optional<std::string> fileType;   // wacz | warc
    std::optional<std::int64_t> sizeBytes;
    std::optional<std::string> sha256;
    std::optional<std::string> error;
    std::string createdAt;
    std::optional<std::string> startedAt;
    std::optional<std::string> finishedAt;
    std::vector<std::string> tags;
};

// Filters for browsing/searching captures.
struct CaptureQuery {
    std::string urlContains;             // substring of URL or title
    std::optional<std::string> urlKey;   // exact normalized URL
    std::vector<std::string> tags;       // all must match
    std::string status;                  // empty = any
    int limit{50};
    int offset{0};
    bool oldestFirst{false};
};

// "Page only", "Depth 2 · whole domain", "All linked pages · same path"
inline std::string crawlDepthLabel(const Capture& capture) {
    if (capture.maxDepth == 0) return "Page only";
    const std::string depth = capture.maxDepth == kUnlimitedDepth
        ? "All linked pages" : "Depth " + std::to_string(capture.maxDepth);
    return depth + " · " + crawlScopeLabel(capture.scope);
}

struct TagCount {
    std::string name;
    int count{};
};

} // namespace warc_studio
