#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/Database.hpp"
#include "warc_studio/Html.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace warc_studio;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testNearestCaptureAcrossYear() {
    const auto temp = std::filesystem::temp_directory_path()
        / ("warc-studio-regression-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
    try {
        {
            Database db(temp / "test.sqlite3");
            Capture capture;
            capture.url = "https://example.com/";
            capture.status = "archived";
            capture.source = "upload";
            capture.timestamp = "20261231235959";
            const int beforeId = db.insertCapture(capture);
            capture.timestamp = "20270102000000";
            const int afterId = db.insertCapture(capture);
            const auto key = makeUrlKey(capture.url);
            const auto before = db.findNearestCapture(key, "20270101000000");
            expect(before && before->id == beforeId,
                   "the capture one second before New Year must be nearest");
            const auto after = db.findNearestCapture(key, "20270101235959");
            expect(after && after->id == afterId,
                   "the capture one second after the target must be nearest");
        }
        std::filesystem::remove_all(temp);
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
}

void testTagCannotInjectConfirmationScript() {
    const std::string tag = "'+alert(1)+'";
    const std::string html = renderTagsPage({TagCount{tag, 1}}, std::nullopt);
    const auto action = html.find("action=\"/tags/delete\"");
    expect(action != std::string::npos, "tag delete form missing");
    const auto confirm = html.find("onsubmit=\"", action);
    expect(confirm != std::string::npos, "tag confirmation missing");
    const auto end = html.find("\"", confirm + 10);
    expect(end != std::string::npos, "tag confirmation is malformed");
    expect(html.substr(confirm, end - confirm).find("alert") == std::string::npos,
           "tag name must not be inserted into JavaScript");
    expect(html.find("&#39;+alert(1)+&#39;") != std::string::npos,
           "tag must still be rendered as escaped text");
}

int main() {
    try {
        testNearestCaptureAcrossYear();
        testTagCannotInjectConfirmationScript();
        std::cout << "2 regression checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Regression test failed: " << error.what() << '\n';
        return 1;
    }
}
