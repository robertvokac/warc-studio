#pragma once

#include "warc_studio/FileService.hpp"
#include "warc_studio/Models.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace warc_studio {

struct RecordingStartResult {
    std::string browsertrixId;
    std::string dockerContainerName;  // docker container name used, may be empty if Browsertrix is disabled
    std::string message;
};

struct RecordingStopResult {
    std::string storedWaczPath;
    std::string message;
};

class BrowsertrixService {
public:
    BrowsertrixService(FileService fileService, std::string image, bool runBrowsertrix);

    RecordingStartResult startRecording(const Entry& entry) const;
    RecordingStopResult stopRecording(const Entry& entry) const;

    // Returns true when the Docker container has exited (status "exited" or missing).
    // Returns false when Browsertrix is disabled or the container is still running.
    [[nodiscard]] bool isContainerFinished(const std::string& browsertrixId) const;

private:
    FileService fileService_;
    std::string image_;
    bool runBrowsertrix_{};

    [[nodiscard]] std::string containerName(const std::string& browsertrixId) const;
    [[nodiscard]] std::filesystem::path findGeneratedWacz(const std::string& browsertrixId) const;
};

} // namespace warc_studio
