#include "warc_studio/FileService.hpp"
#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <zlib.h>

namespace warc_studio {
namespace {

void addArchiveHeaders(crow::response& response) {
    // Content-Type is set here per-response. Accept-Ranges is added by the
    // middleware for all archive responses, including Crow-internal OPTIONS.
    response.set_header("Content-Type", "application/octet-stream");
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

std::string trimCopy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string sha256OfFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        return {};
    }
    std::string result;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        std::vector<char> buf(1 << 16);
        bool ok = true;
        while (ok && (file.read(buf.data(), static_cast<std::streamsize>(buf.size())) || file.gcount() > 0)) {
            ok = EVP_DigestUpdate(ctx, buf.data(), static_cast<std::size_t>(file.gcount())) == 1;
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLen = 0;
        if (ok && EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1) {
            std::ostringstream hex;
            hex << std::hex << std::setfill('0');
            for (unsigned int i = 0; i < digestLen; ++i) {
                hex << std::setw(2) << static_cast<int>(digest[i]);
            }
            result = hex.str();
        }
    }
    EVP_MD_CTX_free(ctx);
    return result;
}

// Extracts and whitespace-collapses the <title> of an HTML document.
std::string extractHtmlTitle(const std::string& html) {
    const std::string lower = toLower(html);
    const auto open = lower.find("<title");
    if (open == std::string::npos) return {};
    const auto start = lower.find('>', open);
    if (start == std::string::npos) return {};
    const auto end = lower.find("</title", start);
    if (end == std::string::npos) return {};
    std::string title;
    bool space = false;
    for (const char c : html.substr(start + 1, end - start - 1)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !title.empty();
        } else {
            if (space) title.push_back(' ');
            space = false;
            title.push_back(c);
        }
    }
    return title.substr(0, 300);
}

// Reads WARC records (plain or gzip; zlib reads both) until the first HTTP response
// record and returns its target URI, date and HTML title.
ArchiveInfo inspectWarc(const std::filesystem::path& path) {
    ArchiveInfo info;
    gzFile file = gzopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return info;
    }
    char line[8192];
    for (int record = 0; record < 500; ++record) {
        // Seek to the record's version line.
        bool found = false;
        while (gzgets(file, line, sizeof(line)) != nullptr) {
            if (startsWith(line, "WARC/")) {
                found = true;
                break;
            }
        }
        if (!found) break;

        std::string type, targetUri, date;
        long long contentLength = 0;
        while (gzgets(file, line, sizeof(line)) != nullptr) {
            const std::string header = trimCopy(line);
            if (header.empty()) break;
            const auto colon = header.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = toLower(header.substr(0, colon));
            const std::string value = trimCopy(header.substr(colon + 1));
            if (name == "warc-type") type = value;
            else if (name == "warc-target-uri") targetUri = value;
            else if (name == "warc-date") date = value;
            else if (name == "content-length") contentLength = std::atoll(value.c_str());
        }
        if (!targetUri.empty() && targetUri.front() == '<' && targetUri.back() == '>') {
            targetUri = targetUri.substr(1, targetUri.size() - 2);
        }
        if (info.timestamp.empty() && !date.empty()) {
            info.timestamp = parseTimestamp(date);
        }
        if (type == "response" && startsWith(targetUri, "http")) {
            info.url = targetUri;
            info.timestamp = parseTimestamp(date);
            std::string content(static_cast<std::size_t>(std::min<long long>(contentLength, 256 * 1024)), '\0');
            const int read = gzread(file, content.data(), static_cast<unsigned>(content.size()));
            content.resize(read > 0 ? static_cast<std::size_t>(read) : 0);
            info.title = extractHtmlTitle(content);
            break;
        }
        if (contentLength > 0 && gzseek(file, static_cast<z_off_t>(contentLength), SEEK_CUR) < 0) {
            break;
        }
    }
    gzclose(file);
    return info;
}

// Reads pages/pages.jsonl from a WACZ (via unzip) and returns the first page.
ArchiveInfo inspectWacz(const std::filesystem::path& path) {
    ArchiveInfo info;
    const std::string command = "unzip -p " + shellQuote(path.string()) + " pages/pages.jsonl 2>/dev/null";
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return info;
    }
    std::string line;
    char buf[4096];
    int lines = 0;
    while (lines < 20 && std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        line += buf;
        if (line.empty() || line.back() != '\n') continue;
        ++lines;
        const auto json = crow::json::load(line);
        line.clear();
        if (!json || json.t() != crow::json::type::Object || !json.has("url")) continue;
        info.url = std::string(json["url"].s());
        if (json.has("ts") && json["ts"].t() == crow::json::type::String) {
            info.timestamp = parseTimestamp(std::string(json["ts"].s()));
        }
        if (json.has("title") && json["title"].t() == crow::json::type::String) {
            info.title = std::string(json["title"].s());
        }
        break;
    }
    // Drain so unzip does not die of SIGPIPE while we pclose.
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {}
    ::pclose(pipe);
    return info;
}

std::mutex& reserveMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

FileService::FileService(std::filesystem::path dataRoot)
    : dataRoot_(std::filesystem::absolute(std::move(dataRoot))) {
    std::filesystem::create_directories(archivesRoot());
    std::filesystem::create_directories(crawlsRoot());
    std::filesystem::create_directories(logsRoot());
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

std::filesystem::path FileService::logsRoot() const {
    return dataRoot_ / "logs";
}

std::string FileService::publicArchivePath(const std::string& storedPath) const {
    if (startsWith(storedPath, "archives/")) {
        return "/" + storedPath;
    }
    return "/archives/" + storedPath;
}

std::filesystem::path FileService::absoluteArchivePath(const std::string& storedPath) const {
    return safeArchivePath(startsWith(storedPath, "archives/") ? storedPath.substr(9) : storedPath);
}

std::filesystem::path FileService::safeArchivePath(const std::string& relativePath) const {
    if (relativePath.empty() || relativePath.front() == '/' || relativePath.find("..") != std::string::npos) {
        throw std::runtime_error("Unsafe archive path");
    }

    const auto candidate = std::filesystem::weakly_canonical(archivesRoot() / relativePath);
    const auto root = std::filesystem::weakly_canonical(archivesRoot());

    if (!startsWith(candidate.string(), root.string())) {
        throw std::runtime_error("Archive path escapes archive root");
    }

    return candidate;
}

std::string FileService::archiveExtension(const std::string& filename) {
    const std::string lower = toLower(filename);
    for (const std::string ext : {".warc.gz", ".wacz", ".warc"}) {
        if (lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return ext;
        }
    }
    return {};
}

std::pair<std::string, std::filesystem::path> FileService::reserveArchivePath(
    const std::string& timestamp, const std::string& extension) const {
    const std::string dir = "archives/" + timestamp.substr(0, 4) + "/" + timestamp.substr(4, 2);
    std::filesystem::create_directories(dataRoot_ / dir);

    std::lock_guard lock(reserveMutex());
    for (int n = 1;; ++n) {
        const std::string stored = dir + "/" + timestamp + "-" + std::to_string(n) + extension;
        const auto absolute = dataRoot_ / stored;
        if (!std::filesystem::exists(absolute)) {
            std::ofstream(absolute, std::ios::binary);  // reserve the name
            return {stored, absolute};
        }
    }
}

StoredArchive FileService::describeStored(const std::string& storedPath) const {
    const auto absolute = dataRoot_ / storedPath;
    return StoredArchive{
        .storedPath = storedPath,
        .fileType = archiveExtension(storedPath) == ".wacz" ? "wacz" : "warc",
        .sizeBytes = static_cast<std::int64_t>(std::filesystem::file_size(absolute)),
        .sha256 = sha256OfFile(absolute),
    };
}

StoredArchive FileService::storeUploadedArchive(const std::string& timestamp, const std::string& filename,
                                                const std::string& body) const {
    const std::string extension = archiveExtension(filename);
    if (extension.empty()) {
        throw std::runtime_error("Unsupported file type. Use .warc, .warc.gz or .wacz.");
    }
    if (body.empty()) {
        throw std::runtime_error("The uploaded file is empty.");
    }
    const auto [stored, absolute] = reserveArchivePath(timestamp, extension);
    std::ofstream out(absolute, std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    out.close();
    if (!out) {
        std::filesystem::remove(absolute);
        throw std::runtime_error("Could not write the archive file to disk.");
    }
    return describeStored(stored);
}

StoredArchive FileService::storeArchiveFile(const std::string& timestamp,
                                            const std::filesystem::path& source) const {
    const auto [stored, absolute] = reserveArchivePath(timestamp, archiveExtension(source.filename().string()));
    std::error_code ec;
    std::filesystem::rename(source, absolute, ec);
    if (ec) {
        // Different filesystem: copy, then remove the source.
        std::filesystem::copy_file(source, absolute, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(source, ec);
    }
    return describeStored(stored);
}

ArchiveInfo FileService::inspectArchive(const std::string& storedPath) const {
    const auto path = absoluteArchivePath(storedPath);
    return archiveExtension(storedPath) == ".wacz" ? inspectWacz(path) : inspectWarc(path);
}

crow::response FileService::archiveOptionsResponse() const {
    crow::response response(204);
    addArchiveHeaders(response);
    return response;
}

crow::response FileService::downloadArchive(const std::string& storedPath, const std::string& downloadName) const {
    crow::response response;
    // Streams the file from disk instead of loading it into memory.
    response.set_static_file_info_unsafe(absoluteArchivePath(storedPath).string());
    if (response.code != 200) {
        return crow::response(404, "Archive file not found");
    }
    response.set_header("Content-Type", "application/octet-stream");
    response.set_header("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
    return response;
}

crow::response FileService::serveArchive(const crow::request& request, const std::string& relativePath) const {
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

        const bool isHead = request.method == crow::HTTPMethod::Head;
        if (!isHead) {
            crow::response response;
            if (partial) {
                response.set_static_file_range_info_unsafe(path.string(), start, length);
                if (response.code != 200) return crow::response(404, "Archive file not found");
                response.code = 206;
                response.set_header("Content-Range",
                    "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(fileSize));
            } else {
                response.set_static_file_info_unsafe(path.string());
            }
            addArchiveHeaders(response);
            return response;
        }
        crow::response response(status, "");
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
    const auto path = absoluteArchivePath(*storedPath);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace warc_studio
