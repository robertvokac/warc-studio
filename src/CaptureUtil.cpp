#include "warc_studio/CaptureUtil.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

namespace warc_studio {
namespace {

std::string toLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

bool allDigits(std::string_view value) {
    return !value.empty()
        && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
}

// Splits "scheme://rest" and returns the position right after "://", or 0 when there is no scheme.
std::size_t authorityStart(std::string_view url) {
    const auto sep = url.find("://");
    if (sep == std::string_view::npos) return 0;
    for (std::size_t i = 0; i < sep; ++i) {
        const auto c = static_cast<unsigned char>(url[i]);
        if (!std::isalnum(c) && c != '+' && c != '-' && c != '.') return 0;
    }
    return sep + 3;
}

// Removes "." and ".." segments from a URL path (RFC 3986, section 5.2.4).
std::string removeDotSegments(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 1;  // path always starts with '/'
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto segment = path.substr(start, end - start);
        const bool last = slash == std::string_view::npos;
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
            if (last) segments.emplace_back();
        } else if (segment == ".") {
            if (last) segments.emplace_back();
        } else {
            segments.push_back(segment);
        }
        if (last) break;
        start = slash + 1;
    }
    std::string out;
    for (const auto segment : segments) {
        out += '/';
        out += segment;
    }
    return out.empty() ? "/" : out;
}

// Percent-encodes bytes that are not allowed in a URL (spaces, quotes, non-ASCII, ...).
std::string encodeUnsafe(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^'
            || c == '`' || c == '{' || c == '|' || c == '}') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0f];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

std::string resolveUrl(std::string_view base, std::string_view reference) {
    std::string ref;
    for (const char c : trim(reference)) {
        if (c != '\n' && c != '\r' && c != '\t') ref.push_back(c);
    }
    if (ref.empty() || ref.front() == '#') return {};
    if (const auto hash = ref.find('#'); hash != std::string::npos) ref.erase(hash);

    // Absolute reference: only http(s) can be fetched.
    const auto colon = ref.find(':');
    if (colon != std::string::npos && colon < ref.find_first_of("/?")
        && std::isalpha(static_cast<unsigned char>(ref.front()))) {
        const auto scheme = toLower(std::string_view(ref).substr(0, colon));
        if (scheme != "http" && scheme != "https") return {};
        if (authorityStart(ref) == 0) return {};
        const auto pathStart = std::min(ref.find_first_of("/?", authorityStart(ref)), ref.size());
        std::string path = ref.substr(pathStart);
        if (path.empty() || path.front() == '?') path.insert(0, "/");
        return scheme + ref.substr(colon, pathStart - colon) + encodeUnsafe(path);
    }

    base = trim(base);
    const auto baseAuthority = authorityStart(base);
    if (baseAuthority == 0) return {};
    const auto baseScheme = std::string(base.substr(0, baseAuthority - 3));
    if (ref.rfind("//", 0) == 0) {
        return resolveUrl(base, baseScheme + ":" + ref);
    }

    std::string_view baseRest = base.substr(baseAuthority);
    if (const auto hash = baseRest.find('#'); hash != std::string_view::npos) baseRest = baseRest.substr(0, hash);
    const auto basePathStart = std::min(baseRest.find_first_of("/?"), baseRest.size());
    const std::string origin = baseScheme + "://" + std::string(baseRest.substr(0, basePathStart));
    std::string_view basePathQuery = baseRest.substr(basePathStart);
    const auto baseQueryPos = std::min(basePathQuery.find('?'), basePathQuery.size());
    std::string basePath(basePathQuery.substr(0, baseQueryPos));
    if (basePath.empty()) basePath = "/";

    std::string path;
    std::string query;
    const auto refQueryPos = std::min(ref.find('?'), ref.size());
    const std::string refPath = ref.substr(0, refQueryPos);
    query = ref.substr(refQueryPos);
    if (refPath.empty()) {
        path = basePath;
        if (query.empty()) query = std::string(basePathQuery.substr(baseQueryPos));
    } else if (refPath.front() == '/') {
        path = refPath;
    } else {
        path = basePath.substr(0, basePath.rfind('/') + 1) + refPath;
    }
    return origin + encodeUnsafe(removeDotSegments(path) + query);
}

std::string normalizeInputUrl(std::string_view input) {
    const auto value = trim(input);
    if (value.empty()) return {};
    std::string url = authorityStart(value) == 0 ? "https://" + std::string(value) : std::string(value);
    // "https://example.com" -> "https://example.com/": browsers always request a path, and replay
    // looks the page up by the URL as the browser requests it.
    const auto start = authorityStart(url);
    const auto pathStart = url.find_first_of("/?#", start);
    if (pathStart == std::string::npos) {
        url += '/';
    } else if (url[pathStart] != '/') {
        url.insert(pathStart, "/");
    }
    return url;
}

std::string makeUrlKey(std::string_view url) {
    std::string_view rest = trim(url);
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        rest = rest.substr(0, hash);
    }
    rest.remove_prefix(authorityStart(rest));

    const auto hostEnd = std::min(rest.find_first_of("/?"), rest.size());
    std::string host = toLower(rest.substr(0, hostEnd));
    std::string tail(rest.substr(hostEnd));

    if (host.rfind("www.", 0) == 0) host.erase(0, 4);
    if (const auto colon = host.rfind(':'); colon != std::string::npos) {
        const auto port = host.substr(colon + 1);
        if (port == "80" || port == "443" || port.empty()) host.erase(colon);
    }

    // Drop a trailing '/' from the path (before the query string).
    const auto queryPos = std::min(tail.find('?'), tail.size());
    if (queryPos > 0 && tail[queryPos - 1] == '/') {
        tail.erase(queryPos - 1, 1);
    }
    if (tail == "?") tail.clear();
    return host + tail;
}

std::string urlHost(std::string_view url) {
    std::string_view rest = trim(url);
    const auto start = authorityStart(rest);
    if (start == 0) return {};
    rest.remove_prefix(start);
    rest = rest.substr(0, std::min(rest.find_first_of("/?#"), rest.size()));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) rest.remove_prefix(at + 1);
    if (const auto colon = rest.rfind(':'); colon != std::string_view::npos) rest = rest.substr(0, colon);
    return toLower(rest);
}

std::string nowTimestamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d%H%M%S}", now);
}

bool isTimestamp(std::string_view value) {
    return value.size() == 14 && allDigits(value);
}

std::string parseTimestamp(std::string_view value) {
    value = trim(value);
    if (isTimestamp(value)) return std::string(value);

    // Keep digits only: "2026-09-23T18:02:11.123Z" -> "20260923180211123".
    std::string digits;
    for (const char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if (c != '-' && c != ':' && c != 'T' && c != ' ' && c != 'Z' && c != '.' && c != '/') {
            return {};
        }
    }
    if (digits.size() < 8) return {};
    digits.resize(std::min<std::size_t>(digits.size(), 14));
    while (digits.size() < 14) digits.push_back('0');
    return digits;
}

std::string formatTimestamp(std::string_view ts) {
    if (!isTimestamp(ts)) return std::string(ts);
    return std::format("{}-{}-{} {}:{}:{}",
        ts.substr(0, 4), ts.substr(4, 2), ts.substr(6, 2),
        ts.substr(8, 2), ts.substr(10, 2), ts.substr(12, 2));
}

std::vector<std::string> parseTags(std::string_view text) {
    std::vector<std::string> tags;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        const auto tag = toLower(trim(text.substr(start, end - start)));
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(tag);
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return tags;
}

std::string joinTags(const std::vector<std::string>& tags) {
    std::string out;
    for (const auto& tag : tags) {
        if (!out.empty()) out += ", ";
        out += tag;
    }
    return out;
}

std::string captureDownloadName(const Capture& capture) {
    std::string host = urlHost(capture.url);
    if (host.empty()) host = "capture";
    std::string extension;
    if (capture.filePath) {
        const auto& path = *capture.filePath;
        extension = path.ends_with(".warc.gz") ? ".warc.gz" : path.ends_with(".warc") ? ".warc" : ".wacz";
    }
    return host + "-" + capture.timestamp + extension;
}

} // namespace warc_studio
