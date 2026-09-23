#include "warc_studio/CrawlScope.hpp"
#include "warc_studio/CaptureUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

// Multi-tenant hosting domains: every subdomain belongs to someone else, so they act as public suffixes.
constexpr std::array kHostingSuffixes = {
    "github.io", "gitlab.io", "blogspot.com", "wordpress.com", "tumblr.com", "substack.com",
    "netlify.app", "vercel.app", "pages.dev", "web.app", "firebaseapp.com", "herokuapp.com",
    "neocities.org", "wixsite.com", "weebly.com", "webnode.cz", "blog.cz",
};

// Second-level labels used under country TLDs, e.g. "co.uk", "com.au", "ac.jp".
constexpr std::array kSecondLevelLabels = {"co", "com", "net", "org", "gov", "ac", "edu", "or", "ne", "go", "mil"};

bool endsWithLabel(std::string_view host, std::string_view suffix) {
    return host == suffix
        || (host.size() > suffix.size() && host.ends_with(suffix) && host[host.size() - suffix.size() - 1] == '.');
}

} // namespace

std::string siteDomain(std::string_view host) {
    std::string h(host);
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    if (h.empty() || h.front() == '[' || std::all_of(h.begin(), h.end(), [](unsigned char c) {
            return std::isdigit(c) || c == '.';
        })) {
        return h;  // IP address
    }

    std::vector<std::string_view> labels;
    std::string_view rest(h);
    while (!rest.empty()) {
        const auto dot = rest.find('.');
        labels.push_back(rest.substr(0, dot));
        rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
    }

    // Number of labels forming the public suffix.
    std::size_t suffixLabels = 1;
    for (const std::string_view suffix : kHostingSuffixes) {
        if (endsWithLabel(h, suffix)) {
            suffixLabels = static_cast<std::size_t>(std::count(suffix.begin(), suffix.end(), '.')) + 1;
            break;
        }
    }
    if (suffixLabels == 1 && labels.size() >= 3 && labels.back().size() == 2
        && std::find(kSecondLevelLabels.begin(), kSecondLevelLabels.end(), labels[labels.size() - 2])
               != kSecondLevelLabels.end()) {
        suffixLabels = 2;
    }
    if (labels.size() <= suffixLabels + 1) {
        return h;
    }
    std::string domain;
    for (std::size_t i = labels.size() - suffixLabels - 1; i < labels.size(); ++i) {
        if (!domain.empty()) domain += '.';
        domain += labels[i];
    }
    return domain;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool isInCrawlScope(const std::string& startUrl, const std::string& candidateUrl, CrawlScope scope) {
    // Strip fragments from both URLs before any comparison.
    const std::string start = stripFragment(startUrl);
    const std::string candidate = stripFragment(candidateUrl);

    const std::string startScheme = extractScheme(start);
    const std::string candScheme  = extractScheme(candidate);
    const auto isHttp = [](const std::string& scheme) { return scheme == "http" || scheme == "https"; };
    if (!isHttp(startScheme) || !isHttp(candScheme)) return false;

    const std::string startHost = extractHost(start);
    const std::string candHost  = extractHost(candidate);
    if (startHost.empty() || candHost.empty()) return false;

    if (scope == CrawlScope::ANY) {
        return true;
    }
    if (scope == CrawlScope::DOMAIN) {
        return endsWithLabel(urlHost(candidate), siteDomain(urlHost(start)));
    }

    // PREFIX
    if (startScheme != candScheme) return false;
    if (startHost != candHost) return false;

    const std::string startPath = normalisePath(extractPath(start));
    const std::string candPath  = normalisePath(extractPath(candidate));
    const std::string basePrefix = basePrefixFromPath(startPath);

    // Candidate path must begin with the base prefix.
    return candPath.substr(0, basePrefix.size()) == basePrefix;
}

} // namespace warc_studio
