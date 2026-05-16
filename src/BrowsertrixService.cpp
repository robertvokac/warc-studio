#include "warc_studio/BrowsertrixService.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace warc_studio {
namespace {

std::string runShellCommandWithOutput(const std::string& command) {
    // Captures both stdout and stderr from the command.
    std::string output;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    ::pclose(pipe);
    return output;
}

void runShellCommand(const std::string& command, const std::string& description) {
    // Run via shell, capturing stderr so it appears in the error message on failure.
    const std::string wrapped = command + " 2>&1; echo \"__EXIT__:$?\"";
    const std::string raw = runShellCommandWithOutput(wrapped);

    // Extract exit code appended at the end.
    const std::string marker = "__EXIT__:";
    const auto pos = raw.rfind(marker);
    int exitCode = 0;
    std::string dockerOutput = raw;
    if (pos != std::string::npos) {
        exitCode = std::stoi(raw.substr(pos + marker.size()));
        dockerOutput = raw.substr(0, pos);
    }

    if (exitCode != 0) {
        // Trim trailing whitespace from output for a cleaner message.
        while (!dockerOutput.empty() && (dockerOutput.back() == '\n' || dockerOutput.back() == '\r' || dockerOutput.back() == ' ')) {
            dockerOutput.pop_back();
        }
        throw std::runtime_error(
            "Command failed (" + description + "): " + dockerOutput
        );
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
            .dockerContainerName = {},
            .message = "Recording metadata was created. Browsertrix execution is disabled by configuration."
        };
    }

    std::filesystem::create_directories(fileService_.crawlsRoot());

    // Remove any leftover container with the same name (e.g. from a previous failed run).
    // First stop (gracefully), then force-remove. Both are no-ops when the container is absent.
    const std::string stopCleanupCommand = std::format(
        "docker stop {} >/dev/null 2>&1 || true",
        shellQuote(containerName(browsertrixId))
    );
    std::system(stopCleanupCommand.c_str());
    const std::string rmCleanupCommand = std::format(
        "docker rm -f {} >/dev/null 2>&1 || true",
        shellQuote(containerName(browsertrixId))
    );
    std::system(rmCleanupCommand.c_str());

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

    runShellCommand(command, "docker run for entry " + std::to_string(entry.id));

    return RecordingStartResult{
        .browsertrixId = browsertrixId,
        .dockerContainerName = containerName(browsertrixId),
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
        throw std::runtime_error(
            "Browsertrix output directory was not found: " + collectionRoot.string()
            + ". The crawl may still be running, may have failed to start, or the Docker container"
              " may not have been able to write to the crawls directory."
        );
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

    if (!runBrowsertrix_) {
        // Browsertrix execution is disabled — no container was started and no WACZ will be produced.
        return RecordingStopResult{
            .storedWaczPath = {},
            .message = "Recording was stopped (Browsertrix execution is disabled by configuration; no WACZ was produced)."
        };
    }

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

    // findGeneratedWacz throws a descriptive error when the directory or file is missing.
    // This usually means the crawl did not finish or the container failed to start.
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
