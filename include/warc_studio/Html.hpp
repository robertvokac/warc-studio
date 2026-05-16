#pragma once

#include "warc_studio/Models.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace warc_studio {

// latestArchiveFiles maps entry_id → the most recent ArchiveFile for that entry.
std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
);

} // namespace warc_studio
