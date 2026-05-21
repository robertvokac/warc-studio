#include "warc_studio/BrowsertrixService.hpp"
#include "warc_studio/Database.hpp"
#include "warc_studio/FileService.hpp"
#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <crow.h>
#include <crow/multipart.h>
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
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Application version string
static constexpr const char* kAppVersion = "0.2.0";

// ---------------------------------------------------------------------------
// CORS middleware — injects full CORS + Private Network Access headers on
// every /archives/... response so that ReplayWeb.page (which may be loaded
// from a different origin) can fetch WACZ files from our local HTTP server.
// Crow handles OPTIONS internally before our route handlers run, so we must
// inject these headers via after_handle to cover preflight responses.
// ---------------------------------------------------------------------------
struct CorsMw {
    struct context {};

    void before_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {}

    void after_handle(crow::request& req, crow::response& res, context& /*ctx*/) {
        // Only add CORS headers to /archives/ responses to avoid polluting others.
        if (req.url.rfind("/archives/", 0) == 0 || req.url == "/archives") {
            res.add_header("Access-Control-Allow-Origin", "*");
            res.add_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
            res.add_header("Access-Control-Allow-Headers",
                "Range, Content-Type, Access-Control-Request-Private-Network");
            res.add_header("Access-Control-Expose-Headers",
                "Accept-Ranges, Content-Length, Content-Range");
            res.add_header("Access-Control-Allow-Private-Network", "true");
            res.add_header("Accept-Ranges", "bytes");
        }
    }
};

namespace {

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

std::uint16_t environmentPortOrDefault(const char* name, std::uint16_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return fallback;
    }
    return static_cast<std::uint16_t>(std::stoi(value));
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

crow::response redirectWithMessage(const std::string& message) {
    return redirectTo("/?message=" + warc_studio::urlEncode(message));
}

crow::response redirectWithMessageTo(const std::string& base, const std::string& message) {
    const bool hasQuery = base.find('?') != std::string::npos;
    const std::string sep = hasQuery ? "&" : "?";
    return redirectTo(base + sep + "message=" + warc_studio::urlEncode(message));
}

std::optional<std::string> optionalTextField(
    const std::unordered_map<std::string, std::string>& form,
    const std::string& key
) {
    const auto it = form.find(key);
    if (it == form.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

std::string requiredTextField(
    const std::unordered_map<std::string, std::string>& form,
    const std::string& key
) {
    const auto it = form.find(key);
    if (it == form.end() || it->second.empty()) {
        throw std::runtime_error("Missing required form field: " + key);
    }
    return it->second;
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

// Build the latestArchiveFiles map used by legacy page renderers.
std::map<int, warc_studio::ArchiveFile> buildLatestArchiveFiles(
    warc_studio::Database& database,
    const std::vector<warc_studio::Entry>& entries
) {
    std::map<int, warc_studio::ArchiveFile> result;
    for (const auto& entry : entries) {
        const auto af = database.getLatestArchiveFile(entry.id);
        if (af) {
            result.emplace(entry.id, *af);
        }
    }
    return result;
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
        const auto browsertrixImage = environmentStringOrDefault(
            "WARC_STUDIO_BROWSERTRIX_IMAGE", "webrecorder/browsertrix-crawler:latest");
        const auto port = environmentPortOrDefault("WARC_STUDIO_PORT", 18080);
        const char* runBrowsertrixEnv = std::getenv("WARC_STUDIO_RUN_BROWSERTRIX");
        const bool runBrowsertrix = runBrowsertrixEnv == nullptr
            ? true
            : warc_studio::isTruthyEnvironmentValue(runBrowsertrixEnv);

        // Static files root: prefer static/ next to the executable, with an explicit
        // WARC_STUDIO_STATIC_DIR override and a CWD fallback for development runs.
        const auto staticRoot = findStaticRoot();
        std::cout << "Static assets root: " << staticRoot << "\n";

        warc_studio::Database database(dataRoot / "warc-studio.sqlite3");
        warc_studio::FileService fileService(dataRoot);
        warc_studio::BrowsertrixService browsertrix(fileService, browsertrixImage, runBrowsertrix);

        crow::App<CorsMw> app;

        // -----------------------------------------------------------------------
        // Static assets
        // -----------------------------------------------------------------------

        // Serve individual well-known static assets explicitly.
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
        CROW_ROUTE(app, "/replay/ui.js")(
            [&staticRoot]() {
                auto res = serveStaticFile("ui.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            }
        );

        CROW_ROUTE(app, "/replay/sw.js")(
            [&staticRoot]() {
                auto res = serveStaticFile("sw.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            }
        );

        CROW_ROUTE(app, "/static/ui.js")(
            [&staticRoot]() {
                auto res = serveStaticFile("ui.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            }
        );

        CROW_ROUTE(app, "/static/sw.js")(
            [&staticRoot]() {
                auto res = serveStaticFile("sw.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            }
        );

        // /replay/ and /replay — app shell required by ReplayWeb.page service worker.
        // When the SW intercepts /replay/?source=... on first load, the browser may
        // fall through to the server. Crow must return a valid HTML page (not 404).
        auto replayShellHandler = [](const crow::request& req) {
            std::string query = req.raw_url;
            std::cerr << "REPLAY SHELL REQUEST:\n"
                      << "  path = " << req.url << "\n"
                      << "  query = " << query << "\n";
            const std::string html =
                "<!doctype html>\n"
                "<html>\n"
                "<head>\n"
                "  <meta charset=\"utf-8\">\n"
                "  <title>warc-studio replay shell</title>\n"
                "  <script src=\"/replay/ui.js\"></script>\n"
                "</head>\n"
                "<body>\n"
                "  <replay-app-main></replay-app-main>\n"
                "</body>\n"
                "</html>\n";
            crow::response res(200, html);
            res.add_header("Content-Type", "text/html; charset=utf-8");
            res.add_header("Cache-Control", "no-store");
            return res;
        };
        // Crow treats /replay and /replay/ as the same route — register only one.
        // The browser requests /replay/?source=... (with trailing slash).
        CROW_ROUTE(app, "/replay/")(replayShellHandler);

        // Catch-all for /replay/<path> sub-paths (e.g. /replay/w/<ts>/<url>).
        // ReplayWeb.page uses internal navigation paths under /replay/ that are
        // normally intercepted by the service worker. On first load (before the SW
        // is installed/active), these requests fall through to the server.
        // Returning the shell HTML allows the SW to register and take over.
        CROW_ROUTE(app, "/replay/<path>")(
            [replayShellHandler](const crow::request& req, const std::string& /*subPath*/) {
                return replayShellHandler(req);
            }
        );

        // /sw.js — root-scope backward-compat: serves same local sw.js file.
        CROW_ROUTE(app, "/sw.js")(
            [&staticRoot]() {
                auto res = serveStaticFile("sw.js", "application/javascript; charset=utf-8", staticRoot);
                res.add_header("Cache-Control", "no-store");
                return res;
            }
        );

        // -----------------------------------------------------------------------
        // Root / legacy index page
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/")([&database](const crow::request& request) {
            const char* message = request.url_params.get("message");
            const auto entries = database.listEntries();
            const auto latestArchiveFiles = buildLatestArchiveFiles(database, entries);
            return htmlResponse(warc_studio::renderIndexPage(
                database.listCollections(),
                entries,
                latestArchiveFiles,
                message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message}
            ));
        });

        CROW_ROUTE(app, "/health")([] {
            return crow::response(200, "ok");
        });

        // -----------------------------------------------------------------------
        // Collections section
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/collections")([&database](const crow::request& request) {
            const char* message = request.url_params.get("message");
            return htmlResponse(warc_studio::renderCollectionsPage(
                database.listCollections(),
                message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message}
            ));
        });

        CROW_ROUTE(app, "/collection/new").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const std::string name = requiredTextField(form, "name");
                    const auto description = optionalTextField(form, "description");
                    database.createCollection(name, description);
                    return redirectWithMessageTo("/collections", "Collection was created.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo("/collections", std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/collection/<int>")(
            [&database](const crow::request& request, int collectionId) {
                const auto collection = database.getCollection(collectionId);
                if (!collection) {
                    return crow::response(404, "Collection not found");
                }
                const char* message = request.url_params.get("message");
                const auto entries = database.listEntriesByCollection(collectionId);
                const auto latestArchiveFiles = buildLatestArchiveFiles(database, entries);
                return htmlResponse(warc_studio::renderCollectionDetailPage(
                    *collection,
                    entries,
                    latestArchiveFiles,
                    message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message}
                ));
            }
        );

        // GET /collections/<id>/entries — collection-scoped entries view
        CROW_ROUTE(app, "/collections/<int>/entries")(
            [&database](const crow::request& request, int collectionId) {
                const auto collection = database.getCollection(collectionId);
                const char* message = request.url_params.get("message");
                const std::optional<std::string> msg =
                    message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message};
                const auto entries = collection
                    ? database.listEntriesByCollection(collectionId)
                    : std::vector<warc_studio::Entry>{};
                return htmlResponse(warc_studio::renderEntriesPage(
                    collection, database.listCollections(), entries, msg
                ));
            }
        );

        CROW_ROUTE(app, "/collection/<int>/edit").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request, int collectionId) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const std::string name = requiredTextField(form, "name");
                    const auto description = optionalTextField(form, "description");
                    database.updateCollection(collectionId, name, description);
                    return redirectWithMessageTo(
                        "/collection/" + std::to_string(collectionId), "Collection was updated.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo(
                        "/collection/" + std::to_string(collectionId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        CROW_ROUTE(app, "/collection/<int>/delete").methods(crow::HTTPMethod::POST)(
            [&database, &fileService](int collectionId) {
                try {
                    const auto entries = database.listEntriesByCollection(collectionId);
                    for (const auto& entry : entries) {
                        const auto archiveFiles = database.listArchiveFilesForEntry(entry.id);
                        for (const auto& af : archiveFiles) {
                            fileService.deleteStoredArchiveIfPresent(af.path);
                        }
                        if (entry.warcPath && !entry.warcPath->empty()) {
                            fileService.deleteStoredArchiveIfPresent(entry.warcPath);
                        }
                    }
                    database.deleteCollection(collectionId);
                    return redirectWithMessageTo("/collections", "Collection was deleted.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo("/collections", std::string("Error: ") + error.what());
                }
            }
        );

        // -----------------------------------------------------------------------
        // Entries section
        // -----------------------------------------------------------------------

        // GET /entries — entries view; collection_id from query param
        CROW_ROUTE(app, "/entries")([&database](const crow::request& request) {
            const char* collIdParam = request.url_params.get("collection_id");
            const char* message = request.url_params.get("message");
            const std::optional<std::string> msg =
                message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message};

            const auto allCollections = database.listCollections();

            std::optional<warc_studio::Collection> currentCollection;
            std::vector<warc_studio::Entry> entries;

            if (collIdParam != nullptr && std::string(collIdParam) != "0") {
                const int collId = std::stoi(collIdParam);
                currentCollection = database.getCollection(collId);
                if (currentCollection) {
                    entries = database.listEntriesByCollection(collId);
                }
            } else if (!allCollections.empty()) {
                // Auto-select first collection when none specified.
                currentCollection = allCollections.front();
                entries = database.listEntriesByCollection(currentCollection->id);
            }

            return htmlResponse(warc_studio::renderEntriesPage(
                currentCollection, allCollections, entries, msg
            ));
        });

        CROW_ROUTE(app, "/entry/new").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const int collectionId = std::stoi(requiredTextField(form, "collection_id"));
                    const std::string url = requiredTextField(form, "url");
                    const auto title = optionalTextField(form, "title");
                    const std::string depthStr = optionalTextField(form, "capture_depth").value_or("CURRENT_PAGE_ONLY");
                    const warc_studio::CaptureDepth captureDepth = warc_studio::captureDepthFromString(depthStr);
                    const int entryId = database.createEntry(collectionId, url, title, captureDepth);
                    return redirectWithMessageTo(
                        "/collections/" + std::to_string(collectionId) + "/entries",
                        "Entry was created.");
                    (void)entryId;
                } catch (const std::exception& error) {
                    return redirectWithMessage(std::string("Error: ") + error.what());
                }
            }
        );

        // GET /entry/<id> — entry detail page
        CROW_ROUTE(app, "/entry/<int>")(
            [&database](const crow::request& request, int entryId) {
                const auto entry = database.getEntry(entryId);
                if (!entry) {
                    return crow::response(404, "Entry not found");
                }
                const char* message = request.url_params.get("message");
                const auto archiveFiles = database.listArchiveFilesForEntry(entryId);
                const auto crawlRuns = database.listCrawlRunsForEntry(entryId);
                return htmlResponse(warc_studio::renderEntryDetailPage(
                    *entry, archiveFiles, crawlRuns,
                    message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message}
                ));
            }
        );

        // POST /entry/<id>/update — edit entry fields (url, title, note)
        CROW_ROUTE(app, "/entry/<int>/update").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request, int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return redirectWithMessage("Entry was not found.");
                    }
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const std::string url = requiredTextField(form, "url");
                    const std::string title = optionalTextField(form, "title").value_or("");
                    const std::string note = optionalTextField(form, "note").value_or("");
                    const std::string depthStr = optionalTextField(form, "capture_depth").value_or("CURRENT_PAGE_ONLY");
                    const warc_studio::CaptureDepth captureDepth = warc_studio::captureDepthFromString(depthStr);
                    database.updateEntry(entryId, url, title, note, captureDepth);
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId), "Entry was updated.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // POST /entry/<id>/status — change status manually
        CROW_ROUTE(app, "/entry/<int>/status").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request, int entryId) {
                try {
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const std::string status = requiredTextField(form, "status");
                    database.updateEntryStatus(entryId, status);
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId), "Status was updated.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // POST /entry/<id>/start — start Browsertrix recording
        CROW_ROUTE(app, "/entry/<int>/start").methods(crow::HTTPMethod::POST)(
            [&database, &browsertrix](int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return redirectWithMessage("Entry was not found.");
                    }

                    const auto result = browsertrix.startRecording(*entry);

                    const int crawlRunId = database.createCrawlRun(
                        entryId,
                        result.browsertrixId,
                        result.dockerContainerName.empty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{result.dockerContainerName}
                    );
                    database.updateCrawlRunStarted(crawlRunId);
                    database.updateEntryStatus(entryId, "recording");
                    database.setEntryBrowsertrixId(entryId, result.browsertrixId);

                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId), result.message);
                } catch (const std::exception& error) {
                    database.setEntryError(entryId, error.what());
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // POST /entry/<id>/stop — stop Browsertrix recording
        CROW_ROUTE(app, "/entry/<int>/stop").methods(crow::HTTPMethod::POST)(
            [&database, &browsertrix, &fileService](int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return redirectWithMessage("Entry was not found.");
                    }

                    // Resolve the active crawl run before stopping so we can update it.
                    const auto activeCrawlRun = database.getLatestActiveCrawlRunForEntry(entryId);

                    const auto result = browsertrix.stopRecording(*entry);

                    if (!result.storedWaczPath.empty()) {
                        const std::string sha256 = fileService.computeSha256(result.storedWaczPath);
                        database.addArchiveFile(
                            entryId, result.storedWaczPath, "wacz",
                            std::string{"browsertrix recording"},
                            std::string{"browsertrix"},
                            std::nullopt,
                            sha256.empty() ? std::optional<std::string>{} : std::optional<std::string>{sha256}
                        );
                        database.markEntryArchived(entryId);
                        database.setEntryWarcPath(entryId, result.storedWaczPath);
                    }

                    // Mark the crawl run as finished.
                    if (activeCrawlRun) {
                        database.updateCrawlRunStopped(activeCrawlRun->id, "finished",
                                                       std::optional<int>{0}, std::nullopt);
                    }

                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId), result.message);
                } catch (const std::exception& error) {
                    database.setEntryError(entryId, error.what());
                    // Mark the crawl run as failed if one was active.
                    const auto activeCrawlRun = database.getLatestActiveCrawlRunForEntry(entryId);
                    if (activeCrawlRun) {
                        database.updateCrawlRunStopped(activeCrawlRun->id, "failed",
                                                       std::nullopt, std::string{error.what()});
                    }
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // GET /entry/<id>/check — check if Browsertrix container finished; auto-stop if done
        CROW_ROUTE(app, "/entry/<int>/check")(
            [&database, &browsertrix, &fileService](const crow::request& /*req*/, int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return redirectWithMessage("Entry was not found.");
                    }

                    if (entry->status != "recording") {
                        return redirectWithMessageTo(
                            "/entry/" + std::to_string(entryId),
                            "Entry is not currently recording (status: " + entry->status + ").");
                    }

                    const std::string browsertrixId = entry->browsertrixId.value_or(
                        "crawl_entry_" + std::to_string(entry->id));

                    if (!browsertrix.isContainerFinished(browsertrixId)) {
                        return redirectWithMessageTo(
                            "/entry/" + std::to_string(entryId),
                            "Browsertrix container is still running. Check again later.");
                    }

                    // Container finished — run the same stop logic as POST /entry/<id>/stop.
                    const auto activeCrawlRun = database.getLatestActiveCrawlRunForEntry(entryId);
                    const auto result = browsertrix.stopRecording(*entry);

                    if (!result.storedWaczPath.empty()) {
                        const std::string sha256 = fileService.computeSha256(result.storedWaczPath);
                        database.addArchiveFile(
                            entryId, result.storedWaczPath, "wacz",
                            std::string{"browsertrix recording"},
                            std::string{"browsertrix"},
                            std::nullopt,
                            sha256.empty() ? std::optional<std::string>{}
                                           : std::optional<std::string>{sha256}
                        );
                        database.markEntryArchived(entryId);
                        database.setEntryWarcPath(entryId, result.storedWaczPath);
                    }

                    if (activeCrawlRun) {
                        database.updateCrawlRunStopped(activeCrawlRun->id, "finished",
                                                       std::optional<int>{0}, std::nullopt);
                    }

                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        "Crawl completed. " + result.message);
                } catch (const std::exception& error) {
                    database.setEntryError(entryId, error.what());
                    const auto activeCrawlRun = database.getLatestActiveCrawlRunForEntry(entryId);
                    if (activeCrawlRun) {
                        database.updateCrawlRunStopped(activeCrawlRun->id, "failed",
                                                       std::nullopt, std::string{error.what()});
                    }
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // POST /entry/<id>/upload — upload a WARC/WACZ file
        CROW_ROUTE(app, "/entry/<int>/upload").methods(crow::HTTPMethod::POST)(
            [&database, &fileService](const crow::request& request, int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return redirectWithMessage("Entry was not found.");
                    }

                    // Parse multipart form using Crow's structured multipart API.
                    crow::multipart::message mp(request);

                    // Retrieve the "file" part via the part_map (keyed by the name parameter).
                    const auto filePart = mp.get_part_by_name("file");
                    const auto labelPart = mp.get_part_by_name("label");

                    const std::string& fileBody = filePart.body;
                    const std::string label = labelPart.body;

                    // Extract the original filename from the Content-Disposition params.
                    const auto& cd = filePart.get_header_object("Content-Disposition");
                    const auto fnIt = cd.params.find("filename");
                    const std::string filename = (fnIt != cd.params.end()) ? fnIt->second : std::string{};

                    if (filename.empty() || fileBody.empty()) {
                        return redirectWithMessageTo(
                            "/entry/" + std::to_string(entryId),
                            "Error: No file was uploaded or file is empty.");
                    }

                    const auto uploadResult = fileService.saveUploadedArchive(
                        entry->collectionId, entryId, filename, fileBody);

                    if (!uploadResult.success) {
                        return redirectWithMessageTo(
                            "/entry/" + std::to_string(entryId),
                            "Error: " + uploadResult.errorMessage);
                    }

                    const std::optional<std::string> labelOpt =
                        label.empty() ? std::optional<std::string>{"manual upload"}
                                      : std::optional<std::string>{label};

                    database.addArchiveFile(
                        entryId, uploadResult.storedPath, uploadResult.fileType,
                        labelOpt,
                        std::string{"manual_upload"},
                        uploadResult.sizeBytes,
                        uploadResult.sha256.empty()
                            ? std::optional<std::string>{}
                            : std::optional<std::string>{uploadResult.sha256}
                    );
                    database.markEntryImportedIfNew(entryId);

                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        "Archive file was uploaded successfully.");
                } catch (const std::exception& error) {
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId),
                        std::string("Error: ") + error.what());
                }
            }
        );

        // GET /entry/<id>/replay/latest — replay the latest WACZ for an entry
        CROW_ROUTE(app, "/entry/<int>/replay/latest")(
            [&database](int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (!entry) {
                        return crow::response(404, "Entry not found");
                    }

                    const auto archiveFile = database.getLatestWaczArchiveFile(entryId);
                    if (!archiveFile) {
                        return crow::response(404,
                            "<html><body><p>No WACZ archive file is available for this entry. "
                            "Record or upload a WACZ first.</p></body></html>");
                    }

                    // Redirect to the per-archive local embed replay page.
                    crow::response res(302);
                    res.add_header("Location",
                        "/archive/" + std::to_string(archiveFile->id) + "/replay");
                    return res;
                } catch (const std::exception& error) {
                    return crow::response(500, error.what());
                }
            }
        );

        // Legacy replay route — kept for backward compatibility
        CROW_ROUTE(app, "/entry/<int>/replay")(
            [&database](int entryId) {
                try {
                    const auto archiveFile = database.getLatestWaczArchiveFile(entryId);
                    if (!archiveFile) {
                        return crow::response(404,
                            "<html><body><p>No archive file is available for this entry.</p></body></html>");
                    }
                    // Redirect to the per-archive local embed replay page.
                    crow::response res(302);
                    res.add_header("Location",
                        "/archive/" + std::to_string(archiveFile->id) + "/replay");
                    return res;
                } catch (const std::exception& error) {
                    return crow::response(500, error.what());
                }
            }
        );

        // GET /archive/<id>/replay/local — local fallback replay page showing source URL and download link
        CROW_ROUTE(app, "/archive/<int>/replay/local")(
            [&database, &fileService](const crow::request& request, int archiveFileId) {
                try {
                    const auto archiveFile = database.getArchiveFileById(archiveFileId);
                    if (!archiveFile) {
                        return crow::response(404, "Archive file not found");
                    }

                    const std::string sourceUrl = requestBaseUrl(request)
                        + fileService.publicArchivePath(archiveFile->path);
                    const std::string replayWebUrl = "https://replayweb.page/?source="
                        + warc_studio::urlEncode(sourceUrl);

                    std::ostringstream html;
                    html << "<!DOCTYPE html><html lang=\"en\"><head>";
                    html << "<meta charset=\"UTF-8\"><title>Replay info — warc-studio</title>";
                    html << "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">";
                    html << "<link rel=\"stylesheet\" href=\"/static/style.css\">";
                    html << "</head><body>";
                    html << "<header class=\"site-header\"><a class=\"brand\" href=\"/\">warc-studio</a></header>";
                    html << "<div class=\"container\">";
                    html << "<h2>Replay: " << warc_studio::htmlEscape(archiveFile->path) << "</h2>";
                    html << "<table class=\"table\">";
                    html << "<tr><th>Archive file ID</th><td>" << archiveFile->id << "</td></tr>";
                    html << "<tr><th>Path</th><td class=\"mono\">" << warc_studio::htmlEscape(archiveFile->path) << "</td></tr>";
                    html << "<tr><th>File type</th><td>" << warc_studio::htmlEscape(archiveFile->fileType) << "</td></tr>";
                    html << "<tr><th>Archive serve URL</th><td><a href=\"" << warc_studio::htmlEscape(sourceUrl)
                         << "\" target=\"_blank\" class=\"mono\">" << warc_studio::htmlEscape(sourceUrl) << "</a></td></tr>";
                    if (archiveFile->fileType == "wacz") {
                        html << "<tr><th>Open in ReplayWeb.page</th><td>";
                        html << "<a href=\"" << warc_studio::htmlEscape(replayWebUrl)
                             << "\" target=\"_blank\" class=\"btn\">Open in ReplayWeb.page (external)</a>";
                        html << "</td></tr>";
                    }
                    html << "</table>";
                    html << "<p><a href=\"" << warc_studio::htmlEscape(sourceUrl)
                         << "\" download class=\"btn\">&#11015; Download archive file</a></p>";
                    html << "<p><a href=\"/archive/" << archiveFile->id
                         << "/replay\" class=\"btn-secondary\">Try embedded replay</a></p>";
                    html << "</div></body></html>";
                    return htmlResponse(html.str());
                } catch (const std::exception& error) {
                    return crow::response(500, error.what());
                }
            }
        );

        // GET /archive/<id>/replay — replay a specific archive file.
        // Serves an inline HTML page with the <replay-web-page> web component.
        // The WACZ source is a same-origin relative URL (/archives/...) — no mixed-content,
        // no cross-origin issues, no redirect to external paths.
        // replayBase="/replay/" tells the component to load its SW from /replay/sw.js;
        // in embed="default" mode the actual replay runs inside an iframe rooted at /replay/
        // so the SW scope /replay/ covers it correctly even though this page is at /archive/<id>/replay.
        CROW_ROUTE(app, "/archive/<int>/replay")(
            [&database, &fileService](const crow::request& request, int archiveFileId) {
                try {
                    const auto archiveFile = database.getArchiveFileById(archiveFileId);
                    if (!archiveFile) {
                        return crow::response(404, "Archive file not found");
                    }
                    if (archiveFile->fileType != "wacz") {
                        return htmlResponse(
                            "<html><body><p>WARC replay is not supported yet. "
                            "Only WACZ files can be replayed.</p></body></html>");
                    }

                    // Relative same-origin source path for the WACZ file.
                    // Must NOT be a localhost absolute URL — keep it relative so it works
                    // regardless of the host/port the app runs on.
                    const std::string waczSource =
                        fileService.publicArchivePath(archiveFile->path);

                    // Determine the original archived URL to replay.
                    // Priority: explicit ?url= query param → entry seed URL from DB → none (show index).
                    // The url attribute must be the ORIGINAL archived URL (e.g. https://example.com/),
                    // never a local application path like /archives/... or http://localhost/...
                    std::string replayUrl;
                    const char* urlParam = request.url_params.get("url");
                    if (urlParam != nullptr && std::string(urlParam) != "") {
                        replayUrl = std::string(urlParam);
                    }

                    // Logging: print all relevant info to stdout for diagnostics.
                    std::cout << "REPLAY DEBUG:\n"
                              << "  archiveId       = " << archiveFileId << "\n"
                              << "  archive_file.id = " << archiveFile->id << "\n"
                              << "  archive_file.path = " << archiveFile->path << "\n"
                              << "  wacz_source     = " << waczSource << "  (relative same-origin path)\n"
                              << "  replay_url      = " << (replayUrl.empty() ? "(none — will show pages index)" : replayUrl) << "\n"
                              << std::flush;
                    if (replayUrl.empty()) {
                        std::cout << "  replay_url      = (none, url attribute omitted, ReplayWeb.page should show WACZ index)\n";
                    }

                    std::ostringstream html;
                    html << "<!doctype html>\n<html lang=\"en\">\n<head>\n";
                    html << "<meta charset=\"utf-8\">\n";
                    html << "<title>Replay: "
                         << warc_studio::htmlEscape(archiveFile->label.value_or(archiveFile->path))
                         << " — warc-studio</title>\n";
                    html << "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">\n";
                    // ui.js is loaded from /replay/ui.js — same path as replayBase so SW registration works.
                    html << "<script src=\"/replay/ui.js\"></script>\n";
                    html << "<style>\n"
                         << "html,body{margin:0;padding:0;width:100%;height:100%;}\n"
                         << "replay-web-page{display:block;width:100%;height:100vh;}\n"
                         << ".replay-bar{background:#1a1a2e;color:#eee;padding:6px 12px;"
                         << "font-family:sans-serif;font-size:13px;display:flex;gap:12px;align-items:center;flex-wrap:wrap;}\n"
                         << ".replay-bar a{color:#90caf9;text-decoration:none;}\n"
                         << ".replay-bar .replay-url{color:#aed6a0;word-break:break-all;}\n"
                         << "</style>\n";
                    html << "</head>\n<body>\n";
                    html << "<div class=\"replay-bar\">\n";
                    html << "<a href=\"/entry/" << archiveFile->entryId << "\">&larr; Back</a>\n";
                    html << "<span>Replaying: "
                         << warc_studio::htmlEscape(archiveFile->label.value_or(archiveFile->path))
                         << "</span>\n";
                    if (!replayUrl.empty()) {
                        html << "<span class=\"replay-url\">URL: "
                             << warc_studio::htmlEscape(replayUrl) << "</span>\n";
                    }
                    html << "<a href=\"" << warc_studio::htmlEscape(waczSource)
                         << "\" download>&#11015; Download WACZ</a>\n";
                    html << "<a href=\"/archive/" << archiveFileId << "/debug/wacz\" style=\"color:#ffeb3b\">[Debug WACZ]</a>\n";
                    // HTML comment with diagnostic info.
                    html << "<!-- REPLAY: archive_file.id=" << archiveFile->id
                         << " wacz_source=" << warc_studio::htmlEscape(waczSource)
                         << " replay_url=" << warc_studio::htmlEscape(replayUrl) << " -->\n";
                    html << "</div>\n";
                    // <replay-web-page> web component:
                    // - source: relative path to WACZ (same-origin, no mixed-content)
                    // - url: original archived URL (from DB or query param); omit to show pages index
                    // - replayBase: where to find ui.js and sw.js (must match where SW is served)
                    // - embed="default": renders inside an iframe scoped to replayBase — SW scope covers it
                    html << "<replay-web-page\n"
                         << "  source=\"" << warc_studio::htmlEscape(waczSource) << "\"\n";
                    if (!replayUrl.empty()) {
                        html << "  url=\"" << warc_studio::htmlEscape(replayUrl) << "\"\n";
                    }
                    html << "  replayBase=\"/replay/\"\n"
                         << "  embed=\"default\">\n"
                         << "</replay-web-page>\n";
                    html << "</body>\n</html>\n";
                    return htmlResponse(html.str());
                } catch (const std::exception& error) {
                    return crow::response(500, error.what());
                }
            }
        );

        // GET /archive/<id>/debug/wacz — diagnostic endpoint for WACZ files.
        CROW_ROUTE(app, "/archive/<int>/debug/wacz")(
            [&database, &fileService](const crow::request&, int archiveFileId) {
                try {
                    const auto af = database.getArchiveFileById(archiveFileId);
                    if (!af) return crow::response(404, "Archive file not found");

                    std::ostringstream out;
                    out << "<html><head><title>Debug WACZ: " << archiveFileId << "</title>";
                    out << "<style>body{font-family:monospace;white-space:pre;padding:20px;background:#1e1e1e;color:#d4d4d4;}";
                    out << "h1{color:#569cd6;} h2{color:#ce9178;margin-top:2em; border-bottom:1px solid #333;}";
                    out << ".ok{color:#4ec9b0;} .err{color:#f44747;}</style></head><body>";
                    out << "<h1>Debug WACZ: " << archiveFileId << "</h1>";

                    // 1. Database Record
                    out << "<h2>1. Database Record (archive_file)</h2>";
                    out << "ID:         " << af->id << "\n";
                    out << "Entry ID:   " << af->entryId << "\n";
                    out << "Path:       " << af->path << "\n";
                    out << "File Type:  " << af->fileType << "\n";
                    out << "Label:      " << af->label.value_or("(none)") << "\n";

                    const auto entry = database.getEntry(af->entryId);
                    if (entry) {
                        out << "\n<h2>Entry DB Record</h2>";
                        out << "Entry ID:   " << entry->id << "\n";
                        out << "Seed URL:   " << entry->url << "\n";
                        out << "Status:     " << entry->status << "\n";
                    }

                    // 2. Physical File
                    out << "<h2>2. Physical File</h2>";
                    std::filesystem::path fullPath = fileService.dataRoot() / af->path;
                    out << "Absolute Path: " << fullPath.string() << "\n";
                    if (std::filesystem::exists(fullPath)) {
                        out << "Exists:        <span class='ok'>YES</span>\n";
                        out << "Size:          " << std::filesystem::file_size(fullPath) << " bytes\n";

                        // Read first 4 bytes for ZIP signature
                        std::ifstream fs(fullPath, std::ios::binary);
                        char buf[4];
                        if (fs.read(buf, 4)) {
                            out << "Header (hex):  ";
                            for(int i=0; i<4; ++i) {
                                out << std::hex << std::setw(2) << std::setfill('0') << (static_cast<int>(buf[i]) & 0xff) << " ";
                            }
                            out << std::dec << "\n";
                            if (buf[0] == 'P' && buf[1] == 'K') {
                                out << "Signature:     <span class='ok'>PK (ZIP)</span>\n";
                            } else {
                                out << "Signature:     <span class='err'>UNKNOWN (Not a ZIP)</span>\n";
                            }
                        }
                    } else {
                        out << "Exists:        <span class='err'>NO (File missing on disk)</span>\n";
                    }

                    // 3. WACZ Public Serving
                    out << "<h2>3. Public Serving</h2>";
                    out << "Source URL:    " << fileService.publicArchivePath(af->path) << "\n";

                    // 4. ZIP Contents & Index (via unzip)
                    out << "<h2>4. ZIP Contents (unzip -l)</h2>";
                    std::string cmdL = "unzip -l \"" + fullPath.string() + "\" 2>&1";
                    FILE* pipeL = popen(cmdL.c_str(), "r");
                    if (pipeL) {
                        char buffer[128];
                        while (fgets(buffer, sizeof(buffer), pipeL) != NULL) out << buffer;
                        pclose(pipeL);
                    }

                    out << "<h2>5. CDXJ Index (first 20 lines)</h2>";
                    std::string cmdI = "unzip -p \"" + fullPath.string() + "\" indexes/index.cdxj 2>/dev/null | head -20";
                    FILE* pipeI = popen(cmdI.c_str(), "r");
                    if (pipeI) {
                        char buffer[256];
                        while (fgets(buffer, sizeof(buffer), pipeI) != NULL) out << buffer;
                        pclose(pipeI);
                    }

                    out << "<h2>6. Datapackage.json</h2>";
                    std::string cmdD = "unzip -p \"" + fullPath.string() + "\" datapackage.json 2>/dev/null";
                    FILE* pipeD = popen(cmdD.c_str(), "r");
                    if (pipeD) {
                        char buffer[256];
                        while (fgets(buffer, sizeof(buffer), pipeD) != NULL) out << buffer;
                        pclose(pipeD);
                    }

                    out << "</body></html>";
                    return htmlResponse(out.str());
                } catch (const std::exception& e) {
                    return crow::response(500, std::string("Debug failed: ") + e.what());
                }
            }
        );

        // POST /archive/<id>/update — edit archive file label
        CROW_ROUTE(app, "/archive/<int>/update").methods(crow::HTTPMethod::POST)(
            [&database](const crow::request& request, int archiveFileId) {
                try {
                    const auto af = database.getArchiveFileById(archiveFileId);
                    if (!af) {
                        return redirectWithMessage("Archive file was not found.");
                    }
                    const auto form = warc_studio::parseUrlEncoded(request.body);
                    const std::string label = optionalTextField(form, "label").value_or("");
                    database.updateArchiveFileLabel(archiveFileId, label);
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(af->entryId), "Archive file label was updated.");
                } catch (const std::exception& error) {
                    return redirectWithMessage(std::string("Error: ") + error.what());
                }
            }
        );

        // POST /archive/<id>/delete — delete a single archive file
        CROW_ROUTE(app, "/archive/<int>/delete").methods(crow::HTTPMethod::POST)(
            [&database, &fileService](int archiveFileId) {
                try {
                    const auto af = database.getArchiveFileById(archiveFileId);
                    if (!af) {
                        return redirectWithMessage("Archive file was not found.");
                    }
                    const int entryId = af->entryId;
                    fileService.deleteStoredArchiveIfPresent(af->path);
                    database.deleteArchiveFile(archiveFileId);
                    return redirectWithMessageTo(
                        "/entry/" + std::to_string(entryId), "Archive file was deleted.");
                } catch (const std::exception& error) {
                    return redirectWithMessage(std::string("Error: ") + error.what());
                }
            }
        );

        // POST /entry/<id>/delete — delete an entry
        CROW_ROUTE(app, "/entry/<int>/delete").methods(crow::HTTPMethod::POST)(
            [&database, &fileService](int entryId) {
                try {
                    const auto entry = database.getEntry(entryId);
                    if (entry) {
                        const auto archiveFiles = database.listArchiveFilesForEntry(entryId);
                        for (const auto& af : archiveFiles) {
                            fileService.deleteStoredArchiveIfPresent(af.path);
                        }
                        if (entry->warcPath && !entry->warcPath->empty()) {
                            fileService.deleteStoredArchiveIfPresent(entry->warcPath);
                        }
                        const int collId = entry->collectionId;
                        database.deleteEntry(entryId);
                        return redirectWithMessageTo(
                            "/collections/" + std::to_string(collId) + "/entries",
                            "Entry was deleted.");
                    }
                    return redirectWithMessage("Entry was not found.");
                } catch (const std::exception& error) {
                    return redirectWithMessage(std::string("Error: ") + error.what());
                }
            }
        );

        // -----------------------------------------------------------------------
        // Archive files browser
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/archives-browser")([&database](const crow::request& request) {
            const char* collIdParam = request.url_params.get("collection_id");
            const char* message = request.url_params.get("message");
            const std::optional<std::string> msg =
                message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message};

            const auto allCollections = database.listCollections();

            std::optional<warc_studio::Collection> currentCollection;
            std::vector<warc_studio::ArchiveFile> archiveFiles;
            std::vector<warc_studio::Entry> entries;

            if (collIdParam != nullptr && std::string(collIdParam) != "0") {
                const int collId = std::stoi(collIdParam);
                currentCollection = database.getCollection(collId);
                if (currentCollection) {
                    archiveFiles = database.listArchiveFilesForCollection(collId);
                    entries = database.listEntriesByCollection(collId);
                }
            } else if (!allCollections.empty()) {
                currentCollection = allCollections.front();
                archiveFiles = database.listArchiveFilesForCollection(currentCollection->id);
                entries = database.listEntriesByCollection(currentCollection->id);
            }

            return htmlResponse(warc_studio::renderArchiveFilesPage(
                currentCollection, allCollections, archiveFiles, entries, msg
            ));
        });

        // -----------------------------------------------------------------------
        // About page
        // -----------------------------------------------------------------------

        CROW_ROUTE(app, "/about")(
            [&dataRoot, &browsertrixImage, runBrowsertrix]() {
                return htmlResponse(warc_studio::renderAboutPage(
                    std::filesystem::absolute(dataRoot).string(),
                    browsertrixImage,
                    runBrowsertrix,
                    kAppVersion
                ));
            }
        );

        // -----------------------------------------------------------------------
        // Archive file serving (CORS-enabled for ReplayWeb.page)
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

        std::cout << "warc-studio is listening on http://localhost:" << port << "/\n";
        std::cout << "Data directory: " << std::filesystem::absolute(dataRoot) << "\n";
        std::cout << "Static files: " << staticRoot << "\n";
        app.port(port).multithreaded().run();

    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
