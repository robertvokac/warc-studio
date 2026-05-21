#include "warc_studio/FileService.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>

namespace warc_studio {
namespace {

void addArchiveHeaders(crow::response& response) {
    // Content-Type is set here per-response.
    // Accept-Ranges, CORS, and Private Network Access headers are injected
    // globally by CorsMw middleware in main.cpp so that Crow-internal OPTIONS
    // responses also receive them without duplication.
    response.add_header("Content-Type", "application/octet-stream");
}

std::string readBytes(const std::filesystem::path& path, std::uintmax_t start, std::uintmax_t length) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open archive file");
    }

    input.seekg(static_cast<std::streamoff>(start), std::ios::beg);

    std::string buffer;
    buffer.resize(static_cast<std::size_t>(length));
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<std::size_t>(input.gcount()));
    return buffer;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

FileService::FileService(std::filesystem::path dataRoot)
    : dataRoot_(std::filesystem::absolute(std::move(dataRoot))) {
    std::filesystem::create_directories(archivesRoot());
    std::filesystem::create_directories(crawlsRoot());
}

std::filesystem::path FileService::dataRoot() const {
    return dataRoot_;
}

std::filesystem::path FileService::archivesRoot() const {
    return dataRoot_ / "archives";
}

std::filesystem::path FileService::crawlsRoot() const {
    return dataRoot_ / "crawls";
}

std::filesystem::path FileService::localArchivePath(int collectionId, int entryId) const {
    return archivesRoot() / std::to_string(collectionId) / (std::to_string(entryId) + ".wacz");
}

std::string FileService::storedArchivePath(int collectionId, int entryId) const {
    return "archives/" + std::to_string(collectionId) + "/" + std::to_string(entryId) + ".wacz";
}

std::string FileService::publicArchivePath(const std::string& storedPath) const {
    if (startsWith(storedPath, "archives/")) {
        return "/" + storedPath;
    }
    return "/archives/" + storedPath;
}

std::filesystem::path FileService::safeArchivePath(const std::string& relativePath) const {
    if (relativePath.empty() || relativePath.front() == '/' || relativePath.find("..") != std::string::npos) {
        throw std::runtime_error("Unsafe archive path");
    }

    const auto candidate = std::filesystem::weakly_canonical(archivesRoot() / relativePath);
    const auto root = std::filesystem::weakly_canonical(archivesRoot());

    const auto candidateText = candidate.string();
    const auto rootText = root.string();
    if (!startsWith(candidateText, rootText)) {
        throw std::runtime_error("Archive path escapes archive root");
    }

    return candidate;
}

crow::response FileService::archiveOptionsResponse() const {
    crow::response response(204);
    addArchiveHeaders(response);
    return response;
}

crow::response FileService::serveArchive(const crow::request& request, const std::string& relativePath) const {
    // Log every archive request for debugging replay issues.
    const auto fsPath = [&]() -> std::filesystem::path {
        try { return safeArchivePath(relativePath); } catch (...) { return {}; }
    }();
    const bool fileExists = !fsPath.empty() && std::filesystem::is_regular_file(fsPath);
    const std::uintmax_t logFileSize = fileExists ? std::filesystem::file_size(fsPath) : 0;
    std::cout << "ARCHIVE REQUEST:\n"
              << "  method  = " << crow::method_name(request.method) << "\n"
              << "  path    = /archives/" << relativePath << "\n"
              << "  fs_path = " << (fsPath.empty() ? "<invalid>" : fsPath.string()) << "\n"
              << "  exists  = " << (fileExists ? "yes" : "no") << "\n"
              << "  size    = " << logFileSize << "\n"
              << "  Range   = " << request.get_header_value("Range") << "\n"
              << "  Origin  = " << request.get_header_value("Origin") << "\n"
              << "  PNA     = " << request.get_header_value("Access-Control-Request-Private-Network") << "\n";

    try {
        const auto path = safeArchivePath(relativePath);
        if (!std::filesystem::is_regular_file(path)) {
            return crow::response(404, "Archive file not found");
        }

        const std::uintmax_t fileSize = std::filesystem::file_size(path);

        if (fileSize == 0) {
            crow::response response(200, "");
            addArchiveHeaders(response);
            response.add_header("Content-Length", "0");
            return response;
        }

        std::uintmax_t start = 0;
        std::uintmax_t end = fileSize - 1;
        bool partial = false;
        bool invalidRange = false;

        const std::string range = request.get_header_value("Range");
        if (!range.empty()) {
            if (!startsWith(range, "bytes=")) {
                invalidRange = true;
            } else {
                // ReplayWeb.page/ZIP readers commonly ask for the end of the
                // WACZ file using a suffix range such as "bytes=-65536" to read
                // the ZIP central directory. That means the *last* 65536 bytes,
                // not bytes 0..65536.
                const std::string spec = range.substr(6);
                const auto dash = spec.find('-');

                // This endpoint supports a single byte range, which is enough
                // for ReplayWeb.page/WACZ. Reject multi-range and malformed input.
                if (dash == std::string::npos || spec.find(',', dash + 1) != std::string::npos) {
                    invalidRange = true;
                } else {
                    const std::string startText = spec.substr(0, dash);
                    const std::string endText = spec.substr(dash + 1);

                    try {
                        if (startText.empty()) {
                            // Suffix byte range: bytes=-N means last N bytes.
                            if (endText.empty()) {
                                invalidRange = true;
                            } else {
                                const auto suffixLength = static_cast<std::uintmax_t>(std::stoull(endText));
                                if (suffixLength == 0) {
                                    invalidRange = true;
                                } else {
                                    start = suffixLength >= fileSize ? 0 : fileSize - suffixLength;
                                    end = fileSize - 1;
                                    partial = true;
                                }
                            }
                        } else {
                            // Normal range: bytes=N-M or open-ended bytes=N-
                            start = static_cast<std::uintmax_t>(std::stoull(startText));
                            end = endText.empty()
                                ? fileSize - 1
                                : static_cast<std::uintmax_t>(std::stoull(endText));

                            if (start >= fileSize || start > end) {
                                invalidRange = true;
                            } else {
                                end = std::min(end, fileSize - 1);
                                partial = true;
                            }
                        }
                    } catch (const std::exception&) {
                        invalidRange = true;
                    }
                }
            }
        }

        if (invalidRange) {
            crow::response response(416, "");
            addArchiveHeaders(response);
            response.add_header("Content-Range", "bytes */" + std::to_string(fileSize));
            response.add_header("Content-Length", "0");
            return response;
        }

        const std::uintmax_t length = end - start + 1;
        const int status = partial ? 206 : 200;
        std::cout << "  status  = " << status
                  << (partial ? (" Content-Range: bytes " + std::to_string(start) + "-"
                                 + std::to_string(end) + "/" + std::to_string(fileSize)) : "")
                  << "\n";

        const bool isHead = request.method == crow::HTTPMethod::Head;
        crow::response response(status, isHead ? "" : readBytes(path, start, length));
        addArchiveHeaders(response);
        response.add_header("Content-Length", std::to_string(length));
        if (partial) {
            response.add_header(
                "Content-Range",
                "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize)
            );
        }
        return response;
    } catch (const std::exception& error) {
        std::cout << "  status  = 400 (" << error.what() << ")\n";
        return crow::response(400, error.what());
    }
}

UploadResult FileService::saveUploadedArchive(
    int collectionId, int entryId,
    const std::string& filename,
    const std::string& fileBody
) const {
    if (fileBody.empty()) {
        return UploadResult::failure("Uploaded file is empty.");
    }

    // Determine file type from extension; reject unknown types.
    std::string ext;
    const std::string lower = [&]() {
        std::string s = filename;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
        return s;
    }();

    if (lower.size() > 8 && lower.substr(lower.size() - 8) == ".warc.gz") {
        ext = ".warc.gz";
    } else if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".wacz") {
        ext = ".wacz";
    } else if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".warc") {
        ext = ".warc";
    } else {
        return UploadResult::failure("Unsupported file type. Use .warc, .warc.gz, or .wacz.");
    }

    // Determine file_type value.
    const std::string fileType = (ext == ".wacz") ? "wacz" : "warc";

    // Generate a timestamp-based filename to avoid collisions.
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::string safeName = std::to_string(ms) + ext;

    // Build the storage path: archives/<collectionId>/<entryId>/<timestamp>.<ext>
    const std::string relativeDir = "archives/" + std::to_string(collectionId)
                                  + "/" + std::to_string(entryId);
    const std::string relativePath = relativeDir + "/" + safeName;

    const auto absDir = dataRoot_ / relativeDir;
    std::filesystem::create_directories(absDir);

    const auto absPath = dataRoot_ / relativePath;
    // Do not overwrite existing files (safety check).
    if (std::filesystem::exists(absPath)) {
        return UploadResult::failure("Target file already exists; try again.");
    }

    std::ofstream out(absPath, std::ios::binary);
    if (!out) {
        return UploadResult::failure("Could not write archive file to disk.");
    }
    out.write(fileBody.data(), static_cast<std::streamsize>(fileBody.size()));
    if (!out) {
        return UploadResult::failure("Write failed while saving archive file.");
    }
    out.close();

    // Compute SHA-256 of the uploaded content.
    std::string sha256;
    {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx != nullptr) {
            if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
                && EVP_DigestUpdate(ctx, fileBody.data(), fileBody.size()) == 1)
            {
                unsigned char digest[EVP_MAX_MD_SIZE];
                unsigned int digestLen = 0;
                if (EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1) {
                    std::ostringstream hex;
                    hex << std::hex << std::setfill('0');
                    for (unsigned int i = 0; i < digestLen; ++i) {
                        hex << std::setw(2) << static_cast<int>(digest[i]);
                    }
                    sha256 = hex.str();
                }
            }
            EVP_MD_CTX_free(ctx);
        }
    }

    UploadResult result;
    result.success = true;
    result.storedPath = relativePath;
    result.fileType = fileType;
    result.sizeBytes = static_cast<std::int64_t>(fileBody.size());
    result.sha256 = sha256;
    return result;
}

std::string FileService::computeSha256(const std::string& storedPath) const {
    // Resolve the absolute path safely; storedPath is a relative stored path.
    std::filesystem::path absPath;
    try {
        absPath = safeArchivePath(storedPath.substr(
            storedPath.rfind("archives/") != std::string::npos
                ? storedPath.find("archives/") + std::string("archives/").size()
                : 0
        ));
    } catch (...) {
        // If path resolution fails, return empty (no checksum).
        return {};
    }

    std::ifstream file(absPath, std::ios::binary);
    if (!file) {
        return {};
    }

    // Use OpenSSL EVP_MD_CTX for SHA-256.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return {};
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }

    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, static_cast<std::size_t>(file.gcount())) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);

    // Convert to lowercase hex string.
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        hex << std::setw(2) << static_cast<int>(digest[i]);
    }
    return hex.str();
}

void FileService::deleteStoredArchiveIfPresent(const std::optional<std::string>& storedPath) const {
    if (!storedPath || storedPath->empty()) {
        return;
    }

    std::string relative = *storedPath;
    if (startsWith(relative, "archives/")) {
        relative = relative.substr(std::string("archives/").size());
    }

    const auto path = safeArchivePath(relative);
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
}

} // namespace warc_studio
