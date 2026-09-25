#pragma once

#include "warc_studio/Models.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

namespace warc_studio {

struct CrawlOptions {
    std::string url;
    int maxDepth{0};                      // link hops followed from the start page, kUnlimitedDepth = no limit
    CrawlScope scope{CrawlScope::PREFIX}; // which linked pages may be followed
    int pageLimit{0};                     // max pages when maxDepth != 0, 0 = no limit
    std::string userAgent;
    int timeLimitSeconds{0};              // whole crawl, 0 = no limit
    std::int64_t maxResourceBytes{100LL * 1024 * 1024};  // larger responses are skipped
    std::int64_t maxBufferedBytes{256LL * 1024 * 1024};  // shared raw response budget across crawls
    int maxResources{3000};               // fetched URLs per crawl (pages excluded)
};

struct CrawlResult {
    std::string finalUrl;                 // start URL after redirects
    long status{};                        // HTTP status of the start page
    std::string title;                    // <title> of the start page
    int pages{};
    int resources{};
    int failedResources{};
    std::int64_t warcBytes{};
    bool stopped{};                       // stop flag was raised
    bool timedOut{};                      // time limit reached
    bool resourceLimitReached{};
    int pagesNotCrawled{};                // in-scope pages left in the queue when the page limit was reached
};

// Archives a page — and the pages it links to, up to maxDepth clicks away and within the
// scope — into a WARC file without a browser: every page is fetched with libcurl, its
// resources (CSS, JS, images, fonts, media, iframes, url() in stylesheets) are found by
// parsing HTML, CSS and scripts, and all HTTP exchanges are written as request/response
// records exactly as received. Pages are crawled breadth-first, nearest first.
// JavaScript is not executed, so content that pages load dynamically is not captured.
//
// Throws when the start page cannot be fetched at all (DNS, TLS, connection errors).
CrawlResult crawlToWarc(const CrawlOptions& options, const std::filesystem::path& warcPath,
                        std::ostream& log, const std::atomic<bool>& stop);

} // namespace warc_studio
