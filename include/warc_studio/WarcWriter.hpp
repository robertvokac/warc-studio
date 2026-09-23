#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace warc_studio {

// Writes a WARC 1.1 file where every record is its own gzip member (the usual .warc.gz layout,
// which lets readers seek to single records).
class WarcWriter {
public:
    explicit WarcWriter(const std::filesystem::path& path);

    WarcWriter(const WarcWriter&) = delete;
    WarcWriter& operator=(const WarcWriter&) = delete;

    void writeWarcinfo(const std::string& software, const std::string& description);
    // Writes a response record (raw HTTP response: status line, headers, body as received)
    // followed by the matching request record. Returns the response record id.
    std::string writeExchange(const std::string& targetUri, const std::string& ipAddress,
                              std::string_view httpRequest, std::string_view httpResponse);

    void close();
    [[nodiscard]] std::int64_t bytesWritten() const { return bytesWritten_; }

    // "2026-09-23T18:02:11Z"
    static std::string warcDate();

private:
    std::ofstream out_;
    std::int64_t bytesWritten_{};

    void writeRecord(const std::string& headers, std::string_view block);
};

} // namespace warc_studio
