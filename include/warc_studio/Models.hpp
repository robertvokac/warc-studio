#pragma once

#include <optional>
#include <string>

namespace warc_studio {

struct Collection {
    int id{};
    std::string name;
    std::string createdAt;
};

struct Entry {
    int id{};
    int collectionId{};
    std::string collectionName;
    std::string url;
    std::optional<std::string> title;
    std::optional<std::string> warcPath;
    std::optional<std::string> browsertrixId;
    std::string createdAt;
};

} // namespace warc_studio
