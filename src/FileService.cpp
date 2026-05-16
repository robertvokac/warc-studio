#include "warc_studio/FileService.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace warc_studio {
namespace {

void addArchiveHeaders(crow::response& response) {
    response.add_header("Accept-Ranges", "bytes");
    response.add_header("Access-Control-Allow-Origin", "*");
    response.add_header("Access-Control-Allow-Methods", "GET, OPTIONS");
    response.add_header("Access-Control-Allow-Headers", "Range, Content-Type");
    response.add_header("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range");
    response.add_header("Content-Type", "application/wacz");
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
    try {
        const auto path = safeArchivePath(relativePath);
        if (!std::filesystem::is_regular_file(path)) {
            return crow::response(404, "Archive file not found");
        }

        const std::uintmax_t fileSize = std::filesystem::file_size(path);
        std::uintmax_t start = 0;
        std::uintmax_t end = fileSize == 0 ? 0 : fileSize - 1;
        bool partial = false;

        const std::string range = request.get_header_value("Range");
        if (!range.empty() && startsWith(range, "bytes=")) {
            const std::string spec = range.substr(6);
            const auto dash = spec.find('-');
            if (dash != std::string::npos) {
                const std::string startText = spec.substr(0, dash);
                const std::string endText = spec.substr(dash + 1);

                if (!startText.empty()) {
                    start = static_cast<std::uintmax_t>(std::stoull(startText));
                }
                if (!endText.empty()) {
                    end = static_cast<std::uintmax_t>(std::stoull(endText));
                }
                if (fileSize > 0 && start < fileSize) {
                    end = std::min(end, fileSize - 1);
                    partial = true;
                }
            }
        }

        if (fileSize == 0) {
            crow::response response(200, "");
            addArchiveHeaders(response);
            response.add_header("Content-Length", "0");
            return response;
        }

        if (start > end || start >= fileSize) {
            crow::response response(416, "Requested range not satisfiable");
            addArchiveHeaders(response);
            response.add_header("Content-Range", "bytes */" + std::to_string(fileSize));
            return response;
        }

        const std::uintmax_t length = end - start + 1;
        crow::response response(partial ? 206 : 200, readBytes(path, start, length));
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
        return crow::response(400, error.what());
    }
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
