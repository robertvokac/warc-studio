#include "warc_studio/BrowsertrixService.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

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

// Fire-and-forget: run command in background shell without blocking the caller.
// Uses double-fork so the child is immediately reparented to init and the parent
// can return without waiting. This avoids any blocking in std::system().
void runShellCommandDetached(const std::string& command) {
    // First fork: parent returns immediately after waitpid on the intermediate child.
    const pid_t child = ::fork();
    if (child < 0) {
        // fork failed — silently ignore; the command will not run.
        return;
    }
    if (child == 0) {
        // Intermediate child: create a new session so the grandchild is not in
        // the same process group and won't receive signals meant for the server.
        ::setsid();

        // Second fork: the grandchild is the actual worker; the intermediate
        // child exits immediately so init adopts the grandchild.
        const pid_t grandchild = ::fork();
        if (grandchild != 0) {
            // Intermediate child exits now (success or fork failure both exit).
            ::_exit(0);
        }

        // Grandchild: redirect stdin/stdout/stderr to /dev/null.
        const int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }

        // Execute command via shell.
        ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        // execl only returns on failure — exit with error code.
        ::_exit(127);
    }

    // Parent: wait for the intermediate child to exit (it exits immediately after
    // the second fork, so this waitpid returns in microseconds).
    int status = 0;
    ::waitpid(child, &status, 0);
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
    // Use force-remove only (skips graceful stop) so cleanup is fast and non-blocking.
    // HACK: wrap with 'sg docker -c ...' so that the current user inherits the docker group
    // without requiring a re-login. Remove once the user is permanently added to the docker group.
    const std::string rmCleanupCommand = std::format(
        "sg docker -c 'docker rm -f {} >/dev/null 2>&1 || true'",
        containerName(browsertrixId)
    );
    runShellCommandDetached(rmCleanupCommand);

    // This Docker command is deliberately kept in one place.
    // For a real interactive Browsertrix service, replace this command with an API call
    // that starts an interactive browser session and returns the browser URL.
    // HACK: wrap with 'sg docker -c ...' so the current user inherits the docker group
    // without requiring a re-login. Remove once the user is permanently added to the docker group.
    const std::string command = std::format(
        "sg docker -c 'docker run -d --name {} -v {}:/crawls {} crawl --url {} --generateWACZ --text --collection {} --crawlId {}'",
        containerName(browsertrixId),
        fileService_.crawlsRoot().string(),
        image_,
        entry.url,
        browsertrixId,
        browsertrixId
    );

    // Launch the container in a fire-and-forget manner so the HTTP handler returns immediately.
    runShellCommandDetached(command);

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

    // Stop and remove the container. docker stop can take up to 10 s by default.
    // We run it synchronously so that the WACZ file is fully written before we look for it.
    // If the container has already finished on its own, docker stop is a fast no-op.
    // HACK: wrap with 'sg docker -c ...' so the current user inherits the docker group
    // without requiring a re-login. Remove once the user is permanently added to the docker group.
    const std::string stopAndRemoveCommand = std::format(
        "sg docker -c 'docker stop {0} >/dev/null 2>&1 || true; docker rm -f {0} >/dev/null 2>&1 || true'",
        containerName(browsertrixId)
    );
    runShellCommand(stopAndRemoveCommand, "docker stop/rm for " + browsertrixId);

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
