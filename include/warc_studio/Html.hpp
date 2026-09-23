#pragma once

#include "warc_studio/Database.hpp"
#include "warc_studio/Models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace warc_studio {

// Page: Save Page Now + recent captures (/)
struct HomeView {
    std::string url;                         // prefilled URL (e.g. from the bookmarklet)
    std::string title;                       // prefilled title
    std::string tags;                        // prefilled tags
    std::vector<Capture> existingCaptures;   // captures of `url`, when given
    std::vector<Capture> recentCaptures;
    std::vector<TagCount> allTags;
    ArchiveStats stats;
    std::optional<std::string> message;
};
std::string renderHomePage(const HomeView& view);

// Page: bulk save (/save/bulk)
std::string renderBulkSavePage(const std::vector<TagCount>& allTags, const std::optional<std::string>& message);

// Page: browse and search captures (/captures)
struct BrowseView {
    CaptureQuery query;
    std::string tagText;                     // raw tag filter text for the form
    std::vector<Capture> captures;
    int total{};
    int page{1};
    int pageSize{50};
    std::vector<TagCount> allTags;
    std::optional<std::string> message;
};
std::string renderBrowsePage(const BrowseView& view);

// Page: all captures of one URL (/url?url=...), like the Wayback Machine calendar
std::string renderUrlPage(const std::string& url, const std::vector<Capture>& captures,
                          const std::vector<TagCount>& allTags, const std::optional<std::string>& message);

// Page: capture detail (/capture/<id>)
std::string renderCapturePage(const Capture& capture, const std::string& logTail,
                              const std::vector<TagCount>& allTags, const std::optional<std::string>& message);

// Page: tags (/tags)
std::string renderTagsPage(const std::vector<TagCount>& tags, const std::optional<std::string>& message);

// Page: upload own WARC/WACZ (/upload)
std::string renderUploadPage(const std::vector<TagCount>& allTags, const std::optional<std::string>& message);

// Page: replay a capture with ReplayWeb.page (/capture/<id>/replay)
std::string renderReplayPage(const Capture& capture, const std::string& archiveSource);

// Page: settings / about (/about)
struct AboutView {
    std::string appVersion;
    std::string dataDir;
    std::string userAgent;
    int crawlWorkers{};
    int crawlTimeLimitSeconds{};
    std::int64_t maxResourceBytes{};
    std::string baseUrl;
    ArchiveStats stats;
};
std::string renderAboutPage(const AboutView& view);

} // namespace warc_studio
