#pragma once

#include "warc_studio/Models.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace warc_studio {

// Status badge HTML helper
std::string statusBadge(const std::string& status);

// Page: Collections list (/collections)
std::string renderCollectionsPage(
    const std::vector<Collection>& collections,
    const std::optional<std::string>& message,
    const std::string& activeNav = "collections"
);

// Page: Entries list for a collection (/collections/<id>/entries or /entries?collection_id=X)
std::string renderEntriesPage(
    const std::optional<Collection>& currentCollection,
    const std::vector<Collection>& allCollections,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message,
    const std::string& activeNav = "entries"
);

// Page: Entry detail (/entry/<id>)
std::string renderEntryDetailPage(
    const Entry& entry,
    const std::vector<ArchiveFile>& archiveFiles,
    const std::vector<CrawlRun>& crawlRuns,
    const std::optional<std::string>& message
);

// Page: Archive files for a collection (/collections/<id>/archives)
std::string renderArchiveFilesPage(
    const std::optional<Collection>& currentCollection,
    const std::vector<Collection>& allCollections,
    const std::vector<ArchiveFile>& archiveFiles,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message
);

// Page: Replay embed — serves ReplayWeb.page web component on our HTTP origin
// so there is no mixed-content issue when the WACZ is also on HTTP.
std::string renderReplayPage(
    const std::string& sourceUrl,   // full http://host/archives/... URL
    const std::string& title        // page title / label
);

// Page: Settings / About (/about)
std::string renderAboutPage(
    const std::string& dataDir,
    const std::string& browsertrixImage,
    bool runBrowsertrix,
    const std::string& appVersion
);

// Legacy pages — kept for backward compatibility during transition
std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
);

std::string renderCollectionDetailPage(
    const Collection& collection,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
);

} // namespace warc_studio
