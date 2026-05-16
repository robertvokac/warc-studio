#include "warc_studio/BrowsertrixService.hpp"
#include "warc_studio/Database.hpp"
#include "warc_studio/FileService.hpp"
#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <crow.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

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

std::optional<std::string> optionalTextField(const std::unordered_map<std::string, std::string>& form, const std::string& key) {
    const auto it = form.find(key);
    if (it == form.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

std::string requiredTextField(const std::unordered_map<std::string, std::string>& form, const std::string& key) {
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

} // namespace

int main() {
    try {
        const auto dataRoot = environmentPathOrDefault("WARC_STUDIO_DATA_DIR", "data");
        const auto browsertrixImage = environmentStringOrDefault("WARC_STUDIO_BROWSERTRIX_IMAGE", "webrecorder/browsertrix-crawler:latest");
        const auto port = environmentPortOrDefault("WARC_STUDIO_PORT", 18080);

        const char* runBrowsertrixEnv = std::getenv("WARC_STUDIO_RUN_BROWSERTRIX");
        const bool runBrowsertrix = runBrowsertrixEnv == nullptr ? true : warc_studio::isTruthyEnvironmentValue(runBrowsertrixEnv);

        warc_studio::Database database(dataRoot / "warc-studio.sqlite3");
        warc_studio::FileService fileService(dataRoot);
        warc_studio::BrowsertrixService browsertrix(fileService, browsertrixImage, runBrowsertrix);

        crow::SimpleApp app;

        CROW_ROUTE(app, "/")([&database](const crow::request& request) {
            const char* message = request.url_params.get("message");
            return htmlResponse(warc_studio::renderIndexPage(
                database.listCollections(),
                database.listEntries(),
                message == nullptr ? std::optional<std::string>{} : std::optional<std::string>{message}
            ));
        });

        CROW_ROUTE(app, "/health")([] {
            return crow::response(200, "ok");
        });

        CROW_ROUTE(app, "/collection/new").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
            try {
                const auto form = warc_studio::parseUrlEncoded(request.body);
                database.createCollection(requiredTextField(form, "name"));
                return redirectWithMessage("Collection was created.");
            } catch (const std::exception& error) {
                return redirectWithMessage(std::string("Error: ") + error.what());
            }
        });

        CROW_ROUTE(app, "/entry/new").methods(crow::HTTPMethod::POST)([&database](const crow::request& request) {
            try {
                const auto form = warc_studio::parseUrlEncoded(request.body);
                const int collectionId = std::stoi(requiredTextField(form, "collection_id"));
                const std::string url = requiredTextField(form, "url");
                const auto title = optionalTextField(form, "title");

                database.createEntry(collectionId, url, title);
                return redirectWithMessage("Entry was created.");
            } catch (const std::exception& error) {
                return redirectWithMessage(std::string("Error: ") + error.what());
            }
        });

        CROW_ROUTE(app, "/entry/<int>/start").methods(crow::HTTPMethod::POST)([&database, &browsertrix](int entryId) {
            try {
                const auto entry = database.getEntry(entryId);
                if (!entry) {
                    return redirectWithMessage("Entry was not found.");
                }

                const auto result = browsertrix.startRecording(*entry);
                database.setEntryBrowsertrixId(entryId, result.browsertrixId);
                return redirectWithMessage(result.message);
            } catch (const std::exception& error) {
                return redirectWithMessage(std::string("Error: ") + error.what());
            }
        });

        CROW_ROUTE(app, "/entry/<int>/stop").methods(crow::HTTPMethod::POST)([&database, &browsertrix](int entryId) {
            try {
                const auto entry = database.getEntry(entryId);
                if (!entry) {
                    return redirectWithMessage("Entry was not found.");
                }

                const auto result = browsertrix.stopRecording(*entry);
                database.setEntryWarcPath(entryId, result.storedWaczPath);
                return redirectWithMessage(result.message);
            } catch (const std::exception& error) {
                return redirectWithMessage(std::string("Error: ") + error.what());
            }
        });

        CROW_ROUTE(app, "/entry/<int>/replay")([&database, &fileService](const crow::request& request, int entryId) {
            try {
                const auto entry = database.getEntry(entryId);
                if (!entry || !entry->warcPath || entry->warcPath->empty()) {
                    return crow::response(404, "Entry or WACZ file was not found");
                }

                const std::string sourceUrl = requestBaseUrl(request) + fileService.publicArchivePath(*entry->warcPath);
                crow::response response(302);
                response.add_header("Location", "https://replayweb.page/?source=" + warc_studio::urlEncode(sourceUrl));
                return response;
            } catch (const std::exception& error) {
                return crow::response(500, error.what());
            }
        });

        CROW_ROUTE(app, "/entry/<int>/delete").methods(crow::HTTPMethod::POST)([&database, &fileService](int entryId) {
            try {
                const auto entry = database.getEntry(entryId);
                if (entry) {
                    fileService.deleteStoredArchiveIfPresent(entry->warcPath);
                    database.deleteEntry(entryId);
                }
                return redirectWithMessage("Entry was deleted.");
            } catch (const std::exception& error) {
                return redirectWithMessage(std::string("Error: ") + error.what());
            }
        });

        CROW_ROUTE(app, "/archives/<path>").methods(crow::HTTPMethod::OPTIONS)([&fileService](const crow::request&, const std::string&) {
            return fileService.archiveOptionsResponse();
        });

        CROW_ROUTE(app, "/archives/<path>")([&fileService](const crow::request& request, const std::string& path) {
            return fileService.serveArchive(request, path);
        });

        std::cout << "warc-studio is listening on http://localhost:" << port << "/\n";
        app.port(port).multithreaded().run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
