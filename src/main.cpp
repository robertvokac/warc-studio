#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/CrawlService.hpp"
#include "warc_studio/Database.hpp"
#include "warc_studio/FileService.hpp"
#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <crow.h>
#include <crow/multipart.h>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Application version string
static constexpr const char* kAppVersion = "0.4.0";

// Many sites serve reduced pages or block unknown clients, so the crawler presents itself as a browser.
static constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36 "
    "warc-studio/0.4";

// ---------------------------------------------------------------------------
// ReplayWeb.page is hosted on this same origin. Reject cross-origin mutations
// and DNS-rebinding hostnames, and expose byte ranges without cross-origin CORS.
// ---------------------------------------------------------------------------
struct LocalAccessMw {
    struct context {};

    void before_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        auto host = req.get_header_value("Host");
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool localHost = host == "localhost" || host.starts_with("localhost:")
            || host == "127.0.0.1" || host.starts_with("127.0.0.1:");
        if (!localHost) {
            res = crow::response(403, "Local host required");
            res.end();
            return;
        }
        if (req.method == crow::HTTPMethod::POST) {
            auto origin = req.get_header_value("Origin");
            std::transform(origin.begin(), origin.end(), origin.begin(), [](unsigned char c) { return std::tolower(c); });
            const auto fetchSite = req.get_header_value("Sec-Fetch-Site");
            if ((!origin.empty() && origin != "http://" + host)
                || fetchSite == "cross-site" || fetchSite == "same-site") {
                res = crow::response(403, "Cross-origin request rejected");
                res.end();
            }
        }
    }

    void after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        // ReplayWeb.page runs on this origin; only range capability is needed.
        if (req.url.rfind("/archives/", 0) == 0 || req.url == "/archives") {
            res.add_header("Accept-Ranges", "bytes");
        }
    }
};

namespace {

using warc_studio::Capture;
using warc_studio::CrawlScope;
using warc_studio::CaptureQuery;
using FormFields = std::unordered_map<std::string, std::string>;

std::filesystem::path environmentPathOrDefault(const char* name, const std::filesystem::path& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    return std::filesystem::path(value);
}

std::string environmentStringOrDefault(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    return value;
}

int environmentIntOrDefault(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    return std::stoi(value);
}

std::optional<std::filesystem::path> executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::nullopt;
        }
        if (size < buffer.size() - 1) {
            buffer.resize(size);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(buffer, ec);
    return (ec ? std::filesystem::path(buffer) : canonical).parent_path();
#else
    std::string buffer(4096, '\0');
    for (;;) {
        const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (size < 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(size) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(size));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

std::filesystem::path findStaticRoot() {
    if (const char* value = std::getenv("WARC_STUDIO_STATIC_DIR"); value != nullptr && std::string(value).empty() == false) {
        const auto explicitRoot = std::filesystem::weakly_canonical(std::filesystem::path(value));
        if (!std::filesystem::is_directory(explicitRoot)) {
            throw std::runtime_error("WARC_STUDIO_STATIC_DIR does not point to a directory: " + explicitRoot.string());
        }
        return explicitRoot;
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto exeDir = executableDirectory()) {
        candidates.push_back(*exeDir / "static");
        candidates.push_back(*exeDir / ".." / "share" / "warc-studio" / "static");
    }
    candidates.push_back(std::filesystem::current_path() / "static");

    for (const auto& candidate : candidates) {
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
        if (!ec && std::filesystem::is_directory(canonical)) {
            return canonical;
        }
    }

    throw std::runtime_error(
        "Could not find static assets. Expected static/ next to the executable, "
        "or set WARC_STUDIO_STATIC_DIR."
    );
}

crow::response htmlResponse(const std::string& body) {
    crow::response response(200, body);
    response.add_header("Content-Type", "text/html; charset=utf-8");
    return response;
}

crow::response redirectTo(const std::string& location) {
    crow::response response(303);
    response.add_header("Location", location);
    return response;
}

crow::response redirectWithMessage(const std::string& base, const std::string& message) {
    const std::string sep = base.find('?') != std::string::npos ? "&" : "?";
    return redirectTo(base + sep + "message=" + warc_studio::urlEncode(message));
}

std::string captureHref(int id) {
    return "/capture/" + std::to_string(id);
}

std::string urlHistoryHref(const std::string& url) {
    return "/url?url=" + warc_studio::urlEncode(url);
}

std::optional<std::string> queryMessage(const crow::request& request) {
    const char* message = request.url_params.get("message");
    return message == nullptr ? std::nullopt : std::optional<std::string>{message};
}

std::string queryParam(const crow::request& request, const char* name) {
    const char* value = request.url_params.get(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::string formField(const FormFields& form, const std::string& key) {
    const auto it = form.find(key);
    return it == form.end() ? std::string{} : it->second;
}

std::optional<std::string> optionalFormField(const FormFields& form, const std::string& key) {
    const auto value = formField(form, key);
    return value.empty() ? std::nullopt : std::optional<std::string>{value};
}

// Normalizes a user supplied URL and rejects anything that is not http(s).
std::string requireHttpUrl(const std::string& input) {
    const std::string url = warc_studio::normalizeInputUrl(input);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        throw std::runtime_error("Only http:// and https:// URLs can be archived: " + input);
    }
    const auto host = warc_studio::urlHost(url);
    const bool validHost = !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '[' || c == ']' || c >= 0x80;
    });
    const bool hasSpace = std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= ' '; });
    if (!validHost || hasSpace) {
        throw std::runtime_error("Not a valid URL: " + input);
    }
    return url;
}

int intFormField(const FormFields& form, const std::string& key, int fallback) {
    const auto value = formField(form, key);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw std::runtime_error("Not a number in field " + key + ": " + value);
    }
}

// A new crawl request built from the save form fields (url, tags, title, max_depth, crawl_scope, page_limit).
Capture crawlRequest(const std::string& url, const FormFields& form) {
    Capture capture;
    capture.url = url;
    capture.timestamp = warc_studio::nowTimestamp();
    capture.title = optionalFormField(form, "title");
    capture.status = "queued";
    capture.source = "crawl";
    capture.maxDepth = std::clamp(intFormField(form, "max_depth", 0),
                                  warc_studio::kUnlimitedDepth, warc_studio::kMaxCrawlDepth);
    if (capture.maxDepth != 0) {
        capture.scope = warc_studio::crawlScopeFromString(formField(form, "crawl_scope"));
        capture.pageLimit = std::max(0, intFormField(form, "page_limit", 0));
        if (capture.scope == CrawlScope::ANY && capture.maxDepth == warc_studio::kUnlimitedDepth) {
            throw std::runtime_error("Following links to any site needs a limited depth.");
        }
    }
    capture.tags = warc_studio::parseTags(formField(form, "tags"));
    return capture;
}

std::string requestBaseUrl(const crow::request& request) {
    std::string scheme = request.get_header_value("X-Forwarded-Proto");
    if (scheme.empty()) {
        scheme = "http";
    }
    std::string host = request.get_header_value("Host");
    if (host.empty()) {
        host = "localhost:18080";
    }
    return scheme + "://" + host;
}

std::string isoTimestamp(const std::string& ts) {
    if (!warc_studio::isTimestamp(ts)) return ts;
    auto formatted = warc_studio::formatTimestamp(ts);
    formatted[10] = 'T';
    return formatted + "Z";
}

// Last ~16 KB of a crawl log, starting at a line boundary.
std::string readLogTail(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::int64_t>(file.tellg());
    const std::int64_t start = std::max<std::int64_t>(0, size - 16 * 1024);
    file.seekg(start);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (start > 0) {
        const auto newline = text.find('\n');
        text = "…\n" + (newline == std::string::npos ? text : text.substr(newline + 1));
    }
    return text;
}

// Serve a static file from the static/ directory relative to the project root.
// Returns 404 if the file is not found.
crow::response serveStaticFile(const std::string& relativePath,
                                const std::string& contentType,
                                const std::filesystem::path& staticRoot) {
    const auto candidate = std::filesystem::weakly_canonical(staticRoot / relativePath);
    const auto root = std::filesystem::weakly_canonical(staticRoot);
    // Path traversal guard
    if (candidate.string().rfind(root.string(), 0) != 0) {
        return crow::response(403, "Forbidden");
    }
    if (!std::filesystem::is_regular_file(candidate)) {
        return crow::response(404, "Not found");
    }
    std::ifstream file(candidate, std::ios::binary);
    if (!file) {
        return crow::response(500, "Could not open file");
    }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    crow::response response(200, std::move(body));
    response.add_header("Content-Type", contentType);
    return response;
}

} // namespace

int main() {
    try {
        const auto dataRoot = environmentPathOrDefault("WARC_STUDIO_DATA_DIR", "data");
        const auto port = static_cast<std::uint16_t>(environmentIntOrDefault("WARC_STUDIO_PORT", 18080));
        const int maxRequestMb = environmentIntOrDefault("WARC_STUDIO_MAX_REQUEST_MB", 128);
        if (maxRequestMb < 1 || maxRequestMb > 1024) {
            throw std::runtime_error("WARC_STUDIO_MAX_REQUEST_MB must be between 1 and 1024");
        }
        crow::max_http_body_size.store(static_cast<std::size_t>(maxRequestMb) * 1024 * 1024);
        warc_studio::CrawlConfig crawlConfig{
            .userAgent = environmentStringOrDefault("WARC_STUDIO_USER_AGENT", kDefaultUserAgent),
            .workers = environmentIntOrDefault("WARC_STUDIO_CRAWL_WORKERS", 2),
            .timeLimitSeconds = environmentIntOrDefault("WARC_STUDIO_CRAWL_TIME_LIMIT", 0),
            .maxResourceBytes = std::int64_t{environmentIntOrDefault("WARC_STUDIO_MAX_RESOURCE_MB", 100)} * 1024 * 1024,
        };
        // libcurl must be initialized before any thread uses it.
        curl_global_init(CURL_GLOBAL_DEFAULT);

        // Static files root: prefer static/ next to the executable, with an explicit
        // WARC_STUDIO_STATIC_DIR override and a CWD fallback for development runs.
        const auto staticRoot = findStaticRoot();

        warc_studio::Database database(dataRoot / "warc-studio.sqlite3");
        warc_studio::FileService fileService(dataRoot);
        warc_studio::CrawlService crawler(database, fileService, crawlConfig);
        crawler.start();

        crow::App<LocalAccessMw> app;

        // -----------------------------------------------------------------------
        // Static assets
        // -----------------------------------------------------------------------

        // Note: Crow 1.2.0 does not support two GET routes with <path> at the same depth,
        // so we cannot use /static/<path> alongside /archives/<path>. Use explicit routes instead.
        CROW_ROUTE(app, "/favicon.svg")(
            [&staticRoot]() {
                return serveStaticFile("favicon.svg", "image/svg+xml", staticRoot);
            }
        );

        CROW_ROUTE(app, "/static/style.css")(
            [&staticRoot]() {
                return serveStaticFile("style.css", "text/css; charset=utf-8", staticRoot);
            }
        );

        // ReplayWeb.page assets — served locally from static/ so that both
        // the JS bundle and the WACZ fetches originate from the same HTTP origin,
        // avoiding mixed-content blocks.  ui.js and sw.js are vendored from
        // replaywebpage@2.4.6 and copied to the build static/ directory.
        //
        // Routes:
        //   /replay/ui.js  — primary path; replayBase="/replay/" causes the
        //                    web component to compute this URL automatically.
        //   /replay/sw.js  — service worker with scope /replay/.
        //   /static/ui.js  — alternate path kept for backward compatibility.
        //   /static/sw.js  — alternate path kept for backward compatibility.
        //   /sw.js         — root-scope SW kept for any old cached registrations.
        for (const char* route : {"/replay/ui.js", "/static/ui.js"}) {
            app.route_dynamic(route)([&staticRoot]() {
                auto res = serveStaticFile("ui.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            });
        }
        for (const char* route : {"/replay/sw.js", "/static/sw.js", "/sw.js"}) {
            app.route_dynamic(route)([&staticRoot]() {
                auto res = serveStaticFile("sw.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            });
        }

        // /replay/ — app shell required by ReplayWeb.page service worker.
        // When the SW intercepts /replay/?source=... on first load, the browser may
        // fall through to the server. Crow must return a valid HTML page (not 404).
        auto replayShellHandler = []() {
            crow::response res(200,
                "<!doctype html>\n<html>\n<head>\n  <meta charset=\"utf-8\">\n"
                "  <title>warc-studio replay shell</title>\n"
                "  <script src=\"/replay/ui.js\"></script>\n</head>\n"
                "<body>\n  <replay-app-main></replay-app-main>\n</body>\n</html>\n");
            res.add_header("Content-Type", "text/html; charset=utf-8");
            res.add_header("Cache-Control", "no-store");
            return res;
        };
        // Crow treats /replay and /replay/ as the same route — register only one.
        CROW_ROUTE(app, "/replay/")(replayShellHandler);

        // Catch-all for /replay/<path> sub-paths (e.g. /replay/w/<ts>/<url>).
        // ReplayWeb.page uses internal navigation paths under /replay/ that are
        // normally intercepted by the service worker. On first load (before the SW
        // is installed/active), these requests fall through to the server.
        // Returning the shell HTML allows the SW to register and take over.
        CROW_ROUTE(app, "/replay/<path>")(
            [replayShellHandler](const std::string& /*subPath*/) { return replayShellHandler(); }
        );

        CROW_ROUTE(app, "/health")([] {
            return crow::response(200, "ok");
        });

        // -----------------------------------------------------------------------
        // Save Page Now
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/")([&database](const crow::request& request) {
            warc_studio::HomeView view;
            view.url = queryParam(request, "url");
            view.title = queryParam(request, "title");
            view.tags = queryParam(request, "tags");
            if (!view.url.empty()) {
                view.existingCaptures = database.listCapturesForUrlKey(
                    warc_studio::makeUrlKey(warc_studio::normalizeInputUrl(view.url)));
            }
            CaptureQuery recent;
            recent.limit = 20;
            view.recentCaptures = database.findCaptures(recent);
            view.allTags = database.listTagCounts();
            view.stats = database.stats();
            view.message = queryMessage(request);
            return htmlResponse(warc_studio::renderHomePage(view));
        });

        CROW_ROUTE(app, "/save").methods(crow::HTTPMethod::POST)(
            [&database, &crawler](const crow::request& request) {
                const auto form = warc_studio::parseUrlEncoded(request.body);
                try {
                    const auto url = requireHttpUrl(formField(form, "url"));
                    const int id = database.insertCapture(crawlRequest(url, form));
                    crawler.notify();
                    return redirectWithMessage(captureHref(id), "Queued for archiving: " + url);
                } catch (const std::exception& error) {
                    return redirectWithMessage("/?url=" + warc_studio::urlEncode(formField(form, "url")),
                                               std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/save/bulk")([&database](const crow::request& request) {
            return htmlResponse(warc_studio::renderBulkSavePage(database.listTagCounts(), queryMessage(request)));
        });

        CROW_ROUTE(app, "/save/bulk").methods(crow::HTTPMethod::POST)(
            [&database, &crawler](const crow::request& request) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const bool skipArchived = !formField(form, "skip_archived").empty();
                    int queued = 0, skipped = 0;
                    std::vector<std::string> invalid;
                    std::istringstream lines(formField(form, "urls"));
                    for (std::string line; std::getline(lines, line);) {
                        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
                        std::string url;
                        try {
                            url = requireHttpUrl(line);
                        } catch (const std::exception&) {
                            invalid.push_back(line);
                            continue;
                        }
                        if (skipArchived) {
                            const auto existing = database.listCapturesForUrlKey(warc_studio::makeUrlKey(url));
                            const bool archived = std::any_of(existing.begin(), existing.end(), [](const Capture& c) {
                                return c.status == "archived" || c.status == "queued" || c.status == "crawling";
                            });
                            if (archived) {
                                ++skipped;
                                continue;
                            }
                        }
                        database.insertCapture(crawlRequest(url, form));
                        ++queued;
                    }
                    crawler.notify();
                    std::string message = "Queued " + std::to_string(queued) + " URLs";
                    if (skipped > 0) message += ", skipped " + std::to_string(skipped) + " already archived";
                    if (!invalid.empty()) message += ", ignored " + std::to_string(invalid.size()) + " invalid lines";
                    return redirectWithMessage("/captures", message + ".");
                } catch (const std::exception& error) {
                    return redirectWithMessage("/save/bulk", std::string("Error: ") + error.what());
                }
            }
        );

        // JSON: is this URL archived already? Used by the Save Page Now form while typing.
        CROW_ROUTE(app, "/api/lookup")([&database](const crow::request& request) {
            const auto url = queryParam(request, "url");
            const auto captures = database.listCapturesForUrlKey(
                warc_studio::makeUrlKey(warc_studio::normalizeInputUrl(url)));
            crow::json::wvalue json;
            json["url_key"] = warc_studio::makeUrlKey(warc_studio::normalizeInputUrl(url));
            json["count"] = static_cast<int>(captures.size());
            json["archived"] = static_cast<int>(std::count_if(captures.begin(), captures.end(),
                [](const Capture& c) { return c.status == "archived"; }));
            if (!captures.empty()) {
                const auto& last = captures.front();
                json["last"]["id"] = last.id;
                json["last"]["timestamp"] = last.timestamp;
                json["last"]["iso"] = isoTimestamp(last.timestamp);
                json["last"]["status"] = last.status;
                crow::json::wvalue::list tags;
                for (const auto& tag : last.tags) tags.emplace_back(tag);
                json["last"]["tags"] = std::move(tags);
            }
            crow::response response(json);
            response.add_header("Cache-Control", "no-store");
            return response;
        });

        // -----------------------------------------------------------------------
        // Browse / search
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/captures")([&database](const crow::request& request) {
            warc_studio::BrowseView view;
            view.query.urlContains = queryParam(request, "q");
            view.tagText = queryParam(request, "tag");
            view.query.tags = warc_studio::parseTags(view.tagText);
            view.query.status = queryParam(request, "status");
            view.query.oldestFirst = queryParam(request, "order") == "oldest";
            try {
                view.page = std::max(1, std::stoi(queryParam(request, "page")));
            } catch (const std::exception&) {
                view.page = 1;
            }
            view.query.limit = view.pageSize;
            view.query.offset = (view.page - 1) * view.pageSize;
            view.captures = database.findCaptures(view.query);
            view.total = database.countCaptures(view.query);
            view.allTags = database.listTagCounts();
            view.message = queryMessage(request);
            return htmlResponse(warc_studio::renderBrowsePage(view));
        });

        CROW_ROUTE(app, "/url")([&database](const crow::request& request) {
            const auto input = queryParam(request, "url");
            if (input.empty()) {
                return redirectTo("/captures");
            }
            const auto url = warc_studio::normalizeInputUrl(input);
            const auto captures = database.listCapturesForUrlKey(warc_studio::makeUrlKey(url));
            return htmlResponse(warc_studio::renderUrlPage(
                captures.empty() ? url : captures.front().url, captures, database.listTagCounts(),
                queryMessage(request)));
        });

        // Wayback Machine style URLs:
        //   /web/*/<url>          -> all captures of the URL
        //   /web/<timestamp>/<url> -> replay of the capture closest to the timestamp
        //   /web/<url>            -> replay of the latest capture
        CROW_ROUTE(app, "/web/<path>")([&database](const crow::request& request, const std::string& /*path*/) {
            // raw_url keeps the query string of the archived URL.
            std::string rest = request.raw_url.substr(std::string("/web/").size());
            std::string timestamp;
            const auto slash = rest.find('/');
            const std::string first = rest.substr(0, slash);
            if (first == "*" || (!first.empty() && std::isdigit(static_cast<unsigned char>(first.front()))
                                 && first.find('.') == std::string::npos)) {
                timestamp = first;
                rest = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);
            }
            // Proxies and browsers sometimes collapse "https://" to "https:/".
            for (const std::string scheme : {"http:/", "https:/"}) {
                if (rest.rfind(scheme, 0) == 0 && rest.compare(scheme.size(), 1, "/") != 0) {
                    rest.insert(scheme.size(), "/");
                }
            }
            if (rest.empty()) {
                return redirectTo("/captures");
            }
            const auto url = warc_studio::normalizeInputUrl(warc_studio::urlDecode(rest));
            if (timestamp == "*") {
                return redirectTo(urlHistoryHref(url));
            }
            std::string digits;
            for (const char c : timestamp) {
                if (!std::isdigit(static_cast<unsigned char>(c))) break;
                digits.push_back(c);
            }
            digits = digits.substr(0, 14);
            while (!digits.empty() && digits.size() < 14) digits.push_back('0');
            const auto capture = database.findNearestCapture(
                warc_studio::makeUrlKey(url), digits.empty() ? warc_studio::nowTimestamp() : digits);
            if (!capture) {
                return redirectWithMessage(urlHistoryHref(url), "This URL has no archived capture yet.");
            }
            return redirectTo(captureHref(capture->id) + "/replay");
        });

        // -----------------------------------------------------------------------
        // Capture detail and actions
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/capture/<int>")(
            [&database, &crawler](const crow::request& request, int id) {
                const auto capture = database.getCapture(id);
                if (!capture) {
                    return crow::response(404, "Capture not found");
                }
                return htmlResponse(warc_studio::renderCapturePage(
                    *capture, readLogTail(crawler.logPath(id)), database.listTagCounts(), queryMessage(request)));
            }
        );

        CROW_ROUTE(app, "/capture/<int>/update").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request, int id) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    database.updateCaptureDetails(id, requireHttpUrl(formField(form, "url")),
                                                  optionalFormField(form, "title"), optionalFormField(form, "note"),
                                                  warc_studio::parseTags(formField(form, "tags")));
                    return redirectWithMessage(captureHref(id), "Capture was updated.");
                } catch (const std::exception& error) {
                    return redirectWithMessage(captureHref(id), std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/capture/<int>/cancel").methods(crow::HTTPMethod::POST)(
            [&database](int id) {
                const bool cancelled = database.cancelQueuedCapture(id);
                return redirectWithMessage(captureHref(id),
                    cancelled ? "Capture was cancelled." : "The crawl has already started; use Stop crawl.");
            }
        );

        CROW_ROUTE(app, "/capture/<int>/stop").methods(crow::HTTPMethod::POST)(
            [&database, &crawler](int id) {
                const auto capture = database.getCapture(id);
                if (!capture || capture->status != "crawling") {
                    return redirectWithMessage(captureHref(id), "This capture is not crawling.");
                }
                crawler.requestStop(id);
                return redirectWithMessage(captureHref(id),
                    "Stopping the crawl; the pages captured so far will be saved.");
            }
        );

        CROW_ROUTE(app, "/capture/<int>/retry").methods(crow::HTTPMethod::POST)(
            [&database, &crawler](int id) {
                database.requeueCapture(id);
                crawler.notify();
                return redirectWithMessage(captureHref(id), "Capture was queued again.");
            }
        );

        CROW_ROUTE(app, "/capture/<int>/recapture").methods(crow::HTTPMethod::POST)(
            [&database, &crawler](int id) {
                const auto original = database.getCapture(id);
                if (!original) {
                    return crow::response(404, "Capture not found");
                }
                Capture capture;
                capture.url = original->url;
                capture.timestamp = warc_studio::nowTimestamp();
                capture.status = "queued";
                capture.source = "crawl";
                capture.maxDepth = original->maxDepth;
                capture.scope = original->scope;
                capture.pageLimit = original->pageLimit;
                capture.tags = original->tags;
                const int newId = database.insertCapture(capture);
                crawler.notify();
                return redirectWithMessage(captureHref(newId), "Queued a new capture of " + capture.url);
            }
        );

        CROW_ROUTE(app, "/capture/<int>/delete").methods(crow::HTTPMethod::POST)(
            [&database, &fileService, &crawler](int id) {
                const auto capture = database.getCapture(id);
                if (!capture) {
                    return redirectTo("/captures");
                }
                if (capture->status == "crawling") {
                    return redirectWithMessage(captureHref(id), "Error: stop the crawl before deleting the capture.");
                }
                try {
                    fileService.deleteStoredArchiveIfPresent(capture->filePath);
                    std::error_code ec;
                    std::filesystem::remove(crawler.logPath(id), ec);
                    database.deleteCapture(id);
                    return redirectWithMessage(urlHistoryHref(capture->url), "Capture was deleted.");
                } catch (const std::exception& error) {
                    return redirectWithMessage(captureHref(id), std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/capture/<int>/replay")(
            [&database, &fileService](int id) {
                const auto capture = database.getCapture(id);
                if (!capture) {
                    return crow::response(404, "Capture not found");
                }
                if (capture->status != "archived" || !capture->filePath) {
                    return redirectWithMessage(captureHref(id), "This capture has no archive file to replay yet.");
                }
                return htmlResponse(warc_studio::renderReplayPage(
                    *capture, fileService.publicArchivePath(*capture->filePath)));
            }
        );

        CROW_ROUTE(app, "/capture/<int>/download")(
            [&database, &fileService](int id) {
                const auto capture = database.getCapture(id);
                if (!capture || !capture->filePath) {
                    return crow::response(404, "Archive file not found");
                }
                try {
                    return fileService.downloadArchive(*capture->filePath, warc_studio::captureDownloadName(*capture));
                } catch (const std::exception& error) {
                    return crow::response(400, error.what());
                }
            }
        );

        // -----------------------------------------------------------------------
        // Tags
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/tags")([&database](const crow::request& request) {
            return htmlResponse(warc_studio::renderTagsPage(database.listTagCounts(), queryMessage(request)));
        });

        CROW_ROUTE(app, "/tags/rename").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const auto to = warc_studio::parseTags(formField(form, "to"));
                    if (to.size() != 1) {
                        throw std::runtime_error("The new tag name must be a single non-empty tag.");
                    }
                    database.renameTag(formField(form, "from"), to.front());
                    return redirectWithMessage("/tags", "Tag was renamed to \"" + to.front() + "\".");
                } catch (const std::exception& error) {
                    return redirectWithMessage("/tags", std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/tags/delete").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request) {
                const auto form = warc_studio::parseUrlEncoded(request.body);
                database.deleteTag(formField(form, "name"));
                return redirectWithMessage("/tags", "Tag was deleted.");
            }
        );

        // -----------------------------------------------------------------------
        // Upload of own WARC/WACZ files
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/upload")([&database](const crow::request& request) {
            return htmlResponse(warc_studio::renderUploadPage(database.listTagCounts(), queryMessage(request)));
        });

        CROW_ROUTE(app, "/upload").methods(crow::HTTPMethod::POST)(
            [&database, &fileService](const crow::request& request) {
                std::optional<std::string> storedPath;
                try {
                    crow::multipart::message multipart(request);
                    const auto field = [&multipart](const char* name) {
                        auto value = multipart.get_part_by_name(name).body;
                        // Browsers send CRLF line breaks in textareas.
                        std::erase(value, '\r');
                        return value;
                    };
                    const auto filePart = multipart.get_part_by_name("file");
                    const auto& disposition = filePart.get_header_object("Content-Disposition");
                    const auto filenameIt = disposition.params.find("filename");
                    const std::string filename = filenameIt == disposition.params.end() ? "" : filenameIt->second;
                    if (filename.empty() || filePart.body.empty()) {
                        throw std::runtime_error("No file was uploaded or the file is empty.");
                    }

                    std::string timestamp = warc_studio::parseTimestamp(field("timestamp"));
                    if (!field("timestamp").empty() && timestamp.empty()) {
                        throw std::runtime_error("Could not understand the capture date \"" + field("timestamp") + "\".");
                    }
                    auto stored = fileService.storeUploadedArchive(
                        timestamp.empty() ? warc_studio::nowTimestamp() : timestamp, filename, filePart.body);
                    storedPath = stored.storedPath;

                    const auto info = fileService.inspectArchive(stored.storedPath);
                    std::string url = field("url");
                    if (url.empty()) url = info.url;
                    if (url.empty()) {
                        throw std::runtime_error("Could not detect the URL from the file; please fill it in.");
                    }
                    url = requireHttpUrl(url);
                    if (timestamp.empty() && !info.timestamp.empty()) {
                        // Name the file after the capture date found inside it.
                        timestamp = info.timestamp;
                        stored = fileService.storeArchiveFile(
                            timestamp, fileService.absoluteArchivePath(stored.storedPath));
                        storedPath = stored.storedPath;
                    }

                    Capture capture;
                    capture.url = url;
                    capture.timestamp = timestamp.empty() ? warc_studio::nowTimestamp() : timestamp;
                    capture.title = field("title").empty()
                        ? (info.title.empty() ? std::nullopt : std::optional<std::string>{info.title})
                        : std::optional<std::string>{field("title")};
                    capture.note = field("note").empty() ? std::nullopt : std::optional<std::string>{field("note")};
                    capture.status = "archived";
                    capture.source = "upload";
                    capture.filePath = stored.storedPath;
                    capture.fileType = stored.fileType;
                    capture.sizeBytes = stored.sizeBytes;
                    if (!stored.sha256.empty()) capture.sha256 = stored.sha256;
                    capture.tags = warc_studio::parseTags(field("tags"));
                    const int id = database.insertCapture(capture);
                    return redirectWithMessage(captureHref(id), "Archive file was uploaded.");
                } catch (const std::exception& error) {
                    if (storedPath) {
                        try { fileService.deleteStoredArchiveIfPresent(storedPath); } catch (...) {}
                    }
                    return redirectWithMessage("/upload", std::string("Error: ") + error.what());
                }
            }
        );

        // -----------------------------------------------------------------------
        // About page
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/about")(
            [&database, &fileService, &crawler](const crow::request& request) {
                return htmlResponse(warc_studio::renderAboutPage(warc_studio::AboutView{
                    .appVersion = kAppVersion,
                    .dataDir = fileService.dataRoot().string(),
                    .userAgent = crawler.config().userAgent,
                    .crawlWorkers = crawler.config().workers,
                    .crawlTimeLimitSeconds = crawler.config().timeLimitSeconds,
                    .maxResourceBytes = crawler.config().maxResourceBytes,
                    .baseUrl = requestBaseUrl(request),
                    .stats = database.stats(),
                }));
            }
        );

        // -----------------------------------------------------------------------
        // Archive file serving for the locally hosted ReplayWeb.page
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/archives/<path>").methods(crow::HTTPMethod::OPTIONS)(
            [&fileService](const crow::request&, const std::string&) {
                return fileService.archiveOptionsResponse();
            }
        );

        CROW_ROUTE(app, "/archives/<path>").methods(crow::HTTPMethod::Head)(
            [&fileService](const crow::request& request, const std::string& path) {
                return fileService.serveArchive(request, path);
            }
        );

        CROW_ROUTE(app, "/archives/<path>")(
            [&fileService](const crow::request& request, const std::string& path) {
                return fileService.serveArchive(request, path);
            }
        );

        std::cout << "warc-studio is listening on http://127.0.0.1:" << port << "/\n";
        std::cout << "Data directory: " << fileService.dataRoot() << "\n";
        std::cout << "Static files: " << staticRoot << "\n";
        app.loglevel(crow::LogLevel::Warning);
        app.bindaddr("127.0.0.1").port(port).multithreaded().run();

    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
