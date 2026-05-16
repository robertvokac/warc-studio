#pragma once

#include <crow.h>

#include <filesystem>
#include <optional>
#include <string>

namespace warc_studio {

class FileService {
public:
    explicit FileService(std::filesystem::path dataRoot);

    [[nodiscard]] std::filesystem::path dataRoot() const;
    [[nodiscard]] std::filesystem::path archivesRoot() const;
    [[nodiscard]] std::filesystem::path crawlsRoot() const;

    [[nodiscard]] std::filesystem::path localArchivePath(int collectionId, int entryId) const;
    [[nodiscard]] std::string storedArchivePath(int collectionId, int entryId) const;
    [[nodiscard]] std::string publicArchivePath(const std::string& storedPath) const;

    crow::response serveArchive(const crow::request& request, const std::string& relativePath) const;
    crow::response archiveOptionsResponse() const;
    void deleteStoredArchiveIfPresent(const std::optional<std::string>& storedPath) const;

private:
    std::filesystem::path dataRoot_;

    [[nodiscard]] std::filesystem::path safeArchivePath(const std::string& relativePath) const;
};

} // namespace warc_studio
