#pragma once

#include <crow.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace warc_studio {

struct StoredArchive {
    std::string storedPath;     // relative to the data dir, e.g. archives/2026/09/20260923180211-1.wacz
    std::string fileType;       // wacz | warc
    std::int64_t sizeBytes{};
    std::string sha256;         // hex SHA-256 digest, empty if hashing failed
};

// Metadata read from the first page/response record of an archive file.
struct ArchiveInfo {
    std::string url;
    std::string timestamp;      // YYYYMMDDhhmmss, empty when unknown
    std::string title;
};

class FileService {
public:
    explicit FileService(std::filesystem::path dataRoot);

    [[nodiscard]] std::filesystem::path dataRoot() const;
    [[nodiscard]] std::filesystem::path archivesRoot() const;
    [[nodiscard]] std::filesystem::path crawlsRoot() const;
    [[nodiscard]] std::filesystem::path logsRoot() const;

    // Absolute path of a stored archive path; throws if the path escapes the archive root.
    [[nodiscard]] std::filesystem::path absoluteArchivePath(const std::string& storedPath) const;
    [[nodiscard]] std::string publicArchivePath(const std::string& storedPath) const;

    // ".wacz", ".warc" or ".warc.gz" for a supported archive filename, "" otherwise.
    [[nodiscard]] static std::string archiveExtension(const std::string& filename);

    // Stores uploaded bytes under archives/<YYYY>/<MM>/<timestamp>-<n><ext>.
    [[nodiscard]] StoredArchive storeUploadedArchive(const std::string& timestamp, const std::string& filename,
                                                     const std::string& body) const;
    // Moves an existing file (e.g. a crawl result) into the archive directory.
    [[nodiscard]] StoredArchive storeArchiveFile(const std::string& timestamp,
                                                 const std::filesystem::path& source) const;

    [[nodiscard]] ArchiveInfo inspectArchive(const std::string& storedPath) const;

    crow::response serveArchive(const crow::request& request, const std::string& relativePath) const;
    crow::response downloadArchive(const std::string& storedPath, const std::string& downloadName) const;
    crow::response archiveOptionsResponse() const;
    void deleteStoredArchiveIfPresent(const std::optional<std::string>& storedPath) const;

private:
    std::filesystem::path dataRoot_;

    [[nodiscard]] std::filesystem::path safeArchivePath(const std::string& relativePath) const;
    [[nodiscard]] std::pair<std::string, std::filesystem::path> reserveArchivePath(
        const std::string& timestamp, const std::string& extension) const;
    [[nodiscard]] StoredArchive describeStored(const std::string& storedPath) const;
};

} // namespace warc_studio
