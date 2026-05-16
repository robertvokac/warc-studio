#pragma once

#include <crow.h>
#include <crow/multipart.h>

#include <filesystem>
#include <optional>
#include <string>

namespace warc_studio {

struct UploadResult {
    bool success{};
    std::string storedPath;     // relative stored path (e.g. archives/1/2/1234567890.wacz)
    std::string fileType;       // wacz | warc
    std::int64_t sizeBytes{};
    std::string sha256;         // hex SHA-256 digest, may be empty if computation failed
    std::string errorMessage;

    static UploadResult failure(std::string msg) {
        UploadResult r;
        r.success = false;
        r.errorMessage = std::move(msg);
        return r;
    }
};

class FileService {
public:
    explicit FileService(std::filesystem::path dataRoot);

    [[nodiscard]] std::filesystem::path dataRoot() const;
    [[nodiscard]] std::filesystem::path archivesRoot() const;
    [[nodiscard]] std::filesystem::path crawlsRoot() const;

    [[nodiscard]] std::filesystem::path localArchivePath(int collectionId, int entryId) const;
    [[nodiscard]] std::string storedArchivePath(int collectionId, int entryId) const;
    [[nodiscard]] std::string publicArchivePath(const std::string& storedPath) const;

    // Save an uploaded archive file for the given collection and entry.
    // Returns a UploadResult describing success or failure.
    [[nodiscard]] UploadResult saveUploadedArchive(
        int collectionId, int entryId,
        const std::string& filename,
        const std::string& fileBody
    ) const;

    // Compute the SHA-256 hex digest of a file on disk (given its stored relative path).
    // Returns an empty string if the file cannot be read or hashing fails.
    [[nodiscard]] std::string computeSha256(const std::string& storedPath) const;

    crow::response serveArchive(const crow::request& request, const std::string& relativePath) const;
    crow::response archiveOptionsResponse() const;
    void deleteStoredArchiveIfPresent(const std::optional<std::string>& storedPath) const;

private:
    std::filesystem::path dataRoot_;

    [[nodiscard]] std::filesystem::path safeArchivePath(const std::string& relativePath) const;
};

} // namespace warc_studio
