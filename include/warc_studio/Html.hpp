#pragma once

#include "warc_studio/Models.hpp"

#include <optional>
#include <string>
#include <vector>

namespace warc_studio {

std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message
);

} // namespace warc_studio
