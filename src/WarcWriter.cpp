#include "warc_studio/WarcWriter.hpp"

#include <array>
#include <chrono>
#include <format>
#include <random>
#include <stdexcept>

#include <openssl/evp.h>
#include <zlib.h>

namespace warc_studio {
namespace {

std::string newRecordId() {
    thread_local std::mt19937_64 random{std::random_device{}()};
    const std::uint64_t high = (random() & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // version 4
    const std::uint64_t low = (random() & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // variant 1
    return std::format("<urn:uuid:{:08x}-{:04x}-{:04x}-{:04x}-{:012x}>",
        high >> 32, (high >> 16) & 0xFFFF, high & 0xFFFF, low >> 48, low & 0xFFFFFFFFFFFFULL);
}

// "sha1:<base32>" as used by WARC-Block-Digest.
std::string sha1Digest(std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha1(), nullptr) != 1) {
        return {};
    }
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out = "sha1:";
    unsigned int buffer = 0;
    int bits = 0;
    for (unsigned int i = 0; i < length; ++i) {
        buffer = (buffer << 8) | digest[i];
        bits += 8;
        while (bits >= 5) {
            out += kAlphabet[(buffer >> (bits - 5)) & 0x1F];
            bits -= 5;
        }
    }
    if (bits > 0) out += kAlphabet[(buffer << (5 - bits)) & 0x1F];
    return out;
}

std::string gzipMember(std::string_view data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    std::string out(deflateBound(&stream, static_cast<uLong>(data.size())) + 32, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const auto produced = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        throw std::runtime_error("gzip compression failed");
    }
    out.resize(produced);
    return out;
}

} // namespace

WarcWriter::WarcWriter(const std::filesystem::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("Could not create WARC file " + path.string());
    }
}

std::string WarcWriter::warcDate() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

void WarcWriter::writeRecord(const std::string& headers, std::string_view block) {
    std::string record = "WARC/1.1\r\n" + headers
        + "WARC-Block-Digest: " + sha1Digest(block) + "\r\n"
        + "Content-Length: " + std::to_string(block.size()) + "\r\n\r\n";
    record.append(block);
    record += "\r\n\r\n";
    const auto compressed = gzipMember(record);
    out_.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    if (!out_) {
        throw std::runtime_error("Writing the WARC file failed (disk full?)");
    }
    bytesWritten_ += static_cast<std::int64_t>(compressed.size());
}

void WarcWriter::writeWarcinfo(const std::string& software, const std::string& description) {
    const std::string block = "software: " + software + "\r\n"
        "format: WARC File Format 1.1\r\n"
        "conformsTo: http://iipc.github.io/warc-specifications/specifications/warc-format/warc-1.1/\r\n"
        "description: " + description + "\r\n";
    writeRecord("WARC-Type: warcinfo\r\n"
                "WARC-Record-ID: " + newRecordId() + "\r\n"
                "WARC-Date: " + warcDate() + "\r\n"
                "Content-Type: application/warc-fields\r\n",
                block);
}

std::string WarcWriter::writeExchange(const std::string& targetUri, const std::string& ipAddress,
                                      std::string_view httpRequest, std::string_view httpResponse) {
    const std::string date = warcDate();
    const std::string responseId = newRecordId();
    std::string headers = "WARC-Type: response\r\n"
        "WARC-Record-ID: " + responseId + "\r\n"
        "WARC-Date: " + date + "\r\n"
        "WARC-Target-URI: " + targetUri + "\r\n";
    if (!ipAddress.empty()) {
        headers += "WARC-IP-Address: " + ipAddress + "\r\n";
    }
    headers += "Content-Type: application/http; msgtype=response\r\n";
    writeRecord(headers, httpResponse);

    writeRecord("WARC-Type: request\r\n"
                "WARC-Record-ID: " + newRecordId() + "\r\n"
                "WARC-Date: " + date + "\r\n"
                "WARC-Target-URI: " + targetUri + "\r\n"
                "WARC-Concurrent-To: " + responseId + "\r\n"
                "Content-Type: application/http; msgtype=request\r\n",
                httpRequest);
    return responseId;
}

void WarcWriter::close() {
    out_.close();
}

} // namespace warc_studio
