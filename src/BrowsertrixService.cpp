#include "warc_studio/BrowsertrixService.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace warc_studio {
namespace {

void runShellCommand(const std::string& command) {
    const int exitCode = std::system(command.c_str());
    if (exitCode != 0) {
        throw std::runtime_error("Command failed: " + command);
    }
}

std::string makeBrowsertrixId(int entryId) {
    return "crawl_entry_" + std::to_string(entryId);
}

} // namespace

BrowsertrixService::BrowsertrixService(FileService fileService, std::string image, bool runBrowsertrix)
    : fileService_(std::move(fileService)), image_(std::move(image)), runBrowsertrix_(runBrowsertrix) {}

std::string BrowsertrixService::containerName(const std::string& browsertrixId) const {
    return "warc-studio-" + browsertrixId;
}

RecordingStartResult BrowsertrixService::startRecording(const Entry& entry) const {
    const std::string browsertrixId = makeBrowsertrixId(entry.id);

    if (!runBrowsertrix_) {
        return RecordingStartResult{
            .browsertrixId = browsertrixId,
            .message = "Recording metadata was created. Browsertrix execution is disabled by configuration."
        };
    }

    std::filesystem::create_directories(fileService_.crawlsRoot());

    // This Docker command is deliberately kept in one place.
    // For a real interactive Browsertrix service, replace this command with an API call
    // that starts an interactive browser session and returns the browser URL.
    const std::string command = std::format(
        "docker run -d --name {} -v {}:/crawls {} crawl --url {} --generateWACZ --text --collection {} --crawlId {}",
        shellQuote(containerName(browsertrixId)),
        shellQuote(fileService_.crawlsRoot().string()),
        shellQuote(image_),
        shellQuote(entry.url),
        shellQuote(browsertrixId),
        shellQuote(browsertrixId)
    );

    runShellCommand(command);

    return RecordingStartResult{
        .browsertrixId = browsertrixId,
        .message = "Browsertrix container was started. Stop the recording when the crawl is complete."
    };
}

std::filesystem::path BrowsertrixService::findGeneratedWacz(const std::string& browsertrixId) const {
    const auto collectionRoot = fileService_.crawlsRoot() / "collections" / browsertrixId;
    const auto expected = collectionRoot / (browsertrixId + ".wacz");

    if (std::filesystem::is_regular_file(expected)) {
        return expected;
    }

    if (!std::filesystem::exists(collectionRoot)) {
        throw std::runtime_error("Browsertrix output directory was not found: " + collectionRoot.string());
    }

    for (const auto& item : std::filesystem::recursive_directory_iterator(collectionRoot)) {
        if (item.is_regular_file() && item.path().extension() == ".wacz") {
            return item.path();
        }
    }

    throw std::runtime_error("No WACZ file was found for Browsertrix ID: " + browsertrixId);
}

RecordingStopResult BrowsertrixService::stopRecording(const Entry& entry) const {
    const std::string browsertrixId = entry.browsertrixId.value_or(makeBrowsertrixId(entry.id));

    if (runBrowsertrix_) {
        // docker stop is allowed to fail when the container already finished.
        const std::string stopCommand = std::format(
            "docker stop {} >/dev/null 2>&1 || true",
            shellQuote(containerName(browsertrixId))
        );
        std::system(stopCommand.c_str());

        const std::string removeCommand = std::format(
            "docker rm -f {} >/dev/null 2>&1 || true",
            shellQuote(containerName(browsertrixId))
        );
        std::system(removeCommand.c_str());
    }

    const auto generatedWacz = findGeneratedWacz(browsertrixId);
    const auto target = fileService_.localArchivePath(entry.collectionId, entry.id);
    std::filesystem::create_directories(target.parent_path());
    std::filesystem::copy_file(generatedWacz, target, std::filesystem::copy_options::overwrite_existing);

    return RecordingStopResult{
        .storedWaczPath = fileService_.storedArchivePath(entry.collectionId, entry.id),
        .message = "WACZ file was saved to the local archive directory."
    };
}

} // namespace warc_studio
