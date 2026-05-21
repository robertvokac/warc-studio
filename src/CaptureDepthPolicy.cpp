#include "warc_studio/CaptureDepthPolicy.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace warc_studio {

namespace {

// ---------------------------------------------------------------------------
// Minimal URL parser helpers (no external dependencies)
// ---------------------------------------------------------------------------

// Strips the fragment (#...) from a URL string.
std::string stripFragment(const std::string& url) {
    const auto pos = url.find('#');
    return pos == std::string::npos ? url : url.substr(0, pos);
}

// Extracts the scheme (e.g. "https") — everything before "://".
// Returns "" on failure.
std::string extractScheme(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos) return "";
    return url.substr(0, sep);
}

// Extracts the host (including optional port) from an absolute URL.
// e.g. "https://example.com:8080/path?q=1" → "example.com:8080"
// Returns "" on failure.
std::string extractHost(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos) return "";
    const auto hostStart = sep + 3;
    // host ends at '/', '?', '#', or end of string
    auto hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos) hostEnd = url.size();
    return url.substr(hostStart, hostEnd - hostStart);
}

// Extracts the path component from an absolute URL.
// e.g. "https://example.com/docs/a.html?x=1" → "/docs/a.html"
// Returns "/" when no explicit path is present.
std::string extractPath(const std::string& url) {
    const auto sep = url.find("://");
    if (sep == std::string::npos) return "";
    const auto hostStart = sep + 3;
    const auto pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) return "/";
    // path ends at '?' or '#'
    auto pathEnd = url.find_first_of("?#", pathStart);
    if (pathEnd == std::string::npos) pathEnd = url.size();
    return url.substr(pathStart, pathEnd - pathStart);
}

// Normalises a path by resolving "." and ".." segments.
// Input must start with '/'.  Returns the normalised path (always starts with '/').
std::string normalisePath(const std::string& path) {
    std::vector<std::string> segments;
    std::string seg;
    // Walk character by character to split on '/'.
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const char c = (i < path.size()) ? path[i] : '/';
        if (c == '/') {
            if (seg == "..") {
                if (!segments.empty()) segments.pop_back();
            } else if (seg != "." && !seg.empty()) {
                segments.push_back(seg);
            }
            seg.clear();
        } else {
            seg += c;
        }
    }
    std::string result;
    for (const auto& s : segments) {
        result += '/';
        result += s;
    }
    if (result.empty()) result = "/";
    // Preserve trailing slash if the original path ended with one.
    if (path.back() == '/' && result.back() != '/') result += '/';
    return result;
}

// Computes the base prefix from a starting URL path.
// If path ends with '/', use it as-is.
// Otherwise use the directory (up to and including the last '/').
std::string basePrefixFromPath(const std::string& path) {
    if (!path.empty() && path.back() == '/') return path;
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return "/";
    return path.substr(0, slash + 1); // includes the trailing '/'
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool isCrawlUrlAllowed(const std::string& startUrl,
                       const std::string& candidateUrl,
                       CaptureDepth depth) {
    // Strip fragments from both URLs before any comparison.
    const std::string start = stripFragment(startUrl);
    const std::string candidate = stripFragment(candidateUrl);

    if (depth == CaptureDepth::CURRENT_PAGE_ONLY) {
        // Strip query string from both for identity comparison.
        // We compare scheme+host+path only — query strings are stripped so that
        // e.g. "https://example.com/page?lang=en" is still considered the same page.
        auto pathOnly = [](const std::string& u) -> std::string {
            const auto sep = u.find("://");
            if (sep == std::string::npos) return u;
            const auto hostStart = sep + 3;
            const auto pathStart = u.find('/', hostStart);
            const auto qmark = u.find('?');
            const std::size_t end = (qmark != std::string::npos) ? qmark : u.size();
            const std::string path = (pathStart != std::string::npos && pathStart < end)
                ? u.substr(pathStart, end - pathStart)
                : "/";
            const std::string host = u.substr(hostStart,
                (pathStart != std::string::npos ? pathStart : end) - hostStart);
            return u.substr(0, sep) + "://" + host + path;
        };
        return pathOnly(start) == pathOnly(candidate);
    }

    // CURRENT_PAGE_AND_SUBPAGES
    const std::string startScheme = extractScheme(start);
    const std::string candScheme  = extractScheme(candidate);
    if (startScheme.empty() || candScheme.empty()) return false;
    if (startScheme != candScheme) return false;

    const std::string startHost = extractHost(start);
    const std::string candHost  = extractHost(candidate);
    if (startHost.empty() || candHost.empty()) return false;
    if (startHost != candHost) return false;

    const std::string startPath = normalisePath(extractPath(start));
    const std::string candPath  = normalisePath(extractPath(candidate));
    const std::string basePrefix = basePrefixFromPath(startPath);

    // Candidate path must begin with the base prefix.
    return candPath.substr(0, basePrefix.size()) == basePrefix;
}

} // namespace warc_studio
