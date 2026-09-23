#include "warc_studio/Crawler.hpp"
#include "warc_studio/CrawlScope.hpp"
#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/HtmlLinks.hpp"
#include "warc_studio/WarcWriter.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <zlib.h>

namespace warc_studio {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kAcceptPage = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
constexpr const char* kAcceptAny = "*/*";
constexpr std::size_t kMaxParseBytes = 30 * 1024 * 1024;
constexpr int kMaxRedirects = 10;
constexpr int kMaxFrameDepth = 2;
constexpr int kHardPageLimit = 10000;
constexpr int kParallelFetches = 6;  // like a browser's connections per host

std::string toLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string formatSize(std::size_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024);
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024 * 1024));
}

// Value of the last occurrence of a header in a raw HTTP header block ("" if absent).
std::string headerValue(const std::string& headers, std::string_view name) {
    std::string value;
    std::size_t start = headers.find("\r\n");
    while (start != std::string::npos && start + 2 < headers.size()) {
        start += 2;
        const auto end = headers.find("\r\n", start);
        const auto line = std::string_view(headers).substr(start, (end == std::string::npos ? headers.size() : end) - start);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && toLower(line.substr(0, colon)) == name) {
            auto v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
            value = v;
        }
        start = end;
    }
    return value;
}

// Undoes "Transfer-Encoding: chunked" (the WARC keeps the raw bytes; parsing needs the payload).
std::string dechunk(std::string_view raw) {
    std::string out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) break;
        std::size_t size = 0;
        try {
            size = std::stoul(std::string(raw.substr(pos, lineEnd - pos)), nullptr, 16);
        } catch (const std::exception&) {
            break;
        }
        if (size == 0) break;
        pos = lineEnd + 2;
        out.append(raw.substr(pos, std::min(size, raw.size() - pos)));
        pos += size + 2;
    }
    return out;
}

// gzip / zlib / raw deflate. Returns nullopt when the data cannot be decoded.
std::optional<std::string> inflateBody(std::string_view data) {
    for (const int windowBits : {15 + 32, -15}) {
        z_stream stream{};
        if (inflateInit2(&stream, windowBits) != Z_OK) return std::nullopt;
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        std::string out;
        char buffer[65536];
        int result = Z_OK;
        while (result == Z_OK && out.size() < kMaxParseBytes) {
            stream.next_out = reinterpret_cast<Bytef*>(buffer);
            stream.avail_out = sizeof(buffer);
            result = inflate(&stream, Z_NO_FLUSH);
            out.append(buffer, sizeof(buffer) - stream.avail_out);
            if (result == Z_BUF_ERROR && stream.avail_in == 0) break;  // truncated input: keep what we have
        }
        inflateEnd(&stream);
        if (result == Z_STREAM_END || result == Z_OK || (result == Z_BUF_ERROR && !out.empty())) {
            return out;
        }
    }
    return std::nullopt;
}

struct Exchange {
    bool ok{};                 // a complete HTTP response was received
    std::string error;
    std::string request;       // raw request header block as sent
    std::string headers;       // raw status line + response headers, ending with an empty line
    std::string body;          // raw body as received (not dechunked, not decompressed)
    long status{};
    std::string ip;

    [[nodiscard]] std::string contentType() const { return toLower(headerValue(headers, "content-type")); }

    [[nodiscard]] bool isHtml() const {
        const auto type = contentType();
        return type.find("text/html") != std::string::npos || type.find("application/xhtml") != std::string::npos;
    }

    // Payload with transfer and content encodings removed, for link extraction.
    [[nodiscard]] std::optional<std::string> decodedBody() const {
        std::string payload = toLower(headerValue(headers, "transfer-encoding")).find("chunked") != std::string::npos
            ? dechunk(body) : body;
        const auto encoding = toLower(headerValue(headers, "content-encoding"));
        if (encoding.empty() || encoding == "identity") {
            if (payload.size() > kMaxParseBytes) payload.resize(kMaxParseBytes);
            return payload;
        }
        if (encoding.find("gzip") != std::string::npos || encoding.find("deflate") != std::string::npos) {
            return inflateBody(payload);
        }
        return std::nullopt;  // br, zstd, ...: not requested, not decodable here
    }
};

// One libcurl easy handle, reused so connections are kept alive between requests.
class Fetcher {
public:
    Fetcher(const CrawlOptions& options, const std::atomic<bool>& stop, std::optional<Clock::time_point> deadline)
        : options_(options), stop_(stop), deadline_(deadline), curl_(curl_easy_init()) {
        if (curl_ == nullptr) {
            throw std::runtime_error("curl_easy_init failed");
        }
    }

    ~Fetcher() {
        curl_easy_cleanup(curl_);
    }

    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;

    Exchange fetch(const std::string& url, const char* accept, const std::string& referer) {
        Exchange exchange;
        current_ = &exchange;
        tooLarge_ = false;

        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, (std::string("Accept: ") + accept).c_str());
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9,cs;q=0.8");
        // Only encodings we can decode for link extraction; the WARC stores the compressed bytes.
        headers = curl_slist_append(headers, "Accept-Encoding: gzip, deflate");

        curl_easy_reset(curl_);
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, options_.userAgent.c_str());
        if (!referer.empty()) curl_easy_setopt(curl_, CURLOPT_REFERER, referer.c_str());
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        // Keep the response exactly as sent by the server; redirects are recorded and followed by us.
        curl_easy_setopt(curl_, CURLOPT_HTTP_TRANSFER_DECODING, 0L);
        curl_easy_setopt(curl_, CURLOPT_HTTP_CONTENT_DECODING, 0L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_, CURLOPT_COOKIEFILE, "");  // in-memory cookies for the whole crawl
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 180L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &Fetcher::onHeader);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Fetcher::onBody);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        // The debug callback is the only way to get the request headers exactly as sent.
        curl_easy_setopt(curl_, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl_, CURLOPT_DEBUGFUNCTION, &Fetcher::onDebug);
        curl_easy_setopt(curl_, CURLOPT_DEBUGDATA, this);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, &Fetcher::onProgress);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, this);

        const CURLcode code = curl_easy_perform(curl_);
        curl_slist_free_all(headers);
        current_ = nullptr;

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &exchange.status);
        if (char* ip = nullptr; curl_easy_getinfo(curl_, CURLINFO_PRIMARY_IP, &ip) == CURLE_OK && ip != nullptr) {
            exchange.ip = ip;
        }
        if (code == CURLE_OK) {
            exchange.ok = true;
        } else if (tooLarge_) {
            exchange.error = "larger than " + formatSize(static_cast<std::size_t>(options_.maxResourceBytes)) + ", skipped";
        } else if (code == CURLE_ABORTED_BY_CALLBACK) {
            exchange.error = stop_ ? "stopped" : "time limit reached";
        } else {
            exchange.error = curl_easy_strerror(code);
        }
        return exchange;
    }

private:
    const CrawlOptions& options_;
    const std::atomic<bool>& stop_;
    std::optional<Clock::time_point> deadline_;
    CURL* curl_;
    Exchange* current_{};
    bool tooLarge_{};

    static std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* self) {
        auto& fetcher = *static_cast<Fetcher*>(self);
        const std::string_view line(data, size * count);
        if (line.starts_with("HTTP/")) {
            fetcher.current_->headers.clear();  // a new response (e.g. after "100 Continue")
        }
        fetcher.current_->headers.append(line);
        return size * count;
    }

    static std::size_t onBody(char* data, std::size_t size, std::size_t count, void* self) {
        auto& fetcher = *static_cast<Fetcher*>(self);
        auto& body = fetcher.current_->body;
        if (static_cast<std::int64_t>(body.size() + size * count) > fetcher.options_.maxResourceBytes) {
            fetcher.tooLarge_ = true;
            return 0;  // aborts the transfer
        }
        body.append(data, size * count);
        return size * count;
    }

    static int onDebug(CURL*, curl_infotype type, char* data, std::size_t size, void* self) {
        auto& fetcher = *static_cast<Fetcher*>(self);
        if (type == CURLINFO_HEADER_OUT && fetcher.current_ != nullptr) {
            fetcher.current_->request.assign(data, size);
        }
        return 0;
    }

    static int onProgress(void* self, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto& fetcher = *static_cast<Fetcher*>(self);
        if (fetcher.stop_) return 1;
        if (fetcher.deadline_ && Clock::now() > *fetcher.deadline_) return 1;
        return 0;
    }
};

bool isRedirect(long status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool isStylesheet(const Exchange& exchange, const std::string& url) {
    const auto type = exchange.contentType();
    if (type.find("text/css") != std::string::npos) return true;
    const auto path = url.substr(0, url.find('?'));
    return (type.empty() || type.find("text/plain") != std::string::npos) && path.ends_with(".css");
}

bool isJavaScript(const Exchange& exchange, const std::string& url) {
    const auto type = exchange.contentType();
    if (type.find("javascript") != std::string::npos || type.find("ecmascript") != std::string::npos) return true;
    const auto path = url.substr(0, url.find('?'));
    return (type.empty() || type.find("text/plain") != std::string::npos)
        && (path.ends_with(".js") || path.ends_with(".mjs"));
}

class Crawl {
public:
    Crawl(const CrawlOptions& options, const std::filesystem::path& warcPath, std::ostream& log,
          const std::atomic<bool>& stop)
        : options_(options), log_(log), stop_(stop), warc_(warcPath),
          deadline_(options.timeLimitSeconds > 0
              ? std::optional<Clock::time_point>{Clock::now() + std::chrono::seconds(options.timeLimitSeconds)}
              : std::nullopt) {
        for (int i = 0; i < kParallelFetches; ++i) {
            fetchers_.push_back(std::make_unique<Fetcher>(options, stop, deadline_));
        }
    }

    CrawlResult run() {
        warc_.writeWarcinfo("warc-studio", "Capture of " + options_.url);

        const bool followLinks = options_.maxDepth != 0;
        const int pageLimit = followLinks
            ? (options_.pageLimit > 0 ? std::min(options_.pageLimit, kHardPageLimit) : kHardPageLimit)
            : 1;

        // Normalized the way a browser requests it (e.g. "https://example.com" -> "https://example.com/").
        const auto resolvedStart = resolveUrl(options_.url, options_.url);
        const auto& start = resolvedStart.empty() ? options_.url : resolvedStart;
        // Breadth-first: (url, number of clicks from the start page).
        std::deque<std::pair<std::string, int>> pages{{start, 0}};
        std::unordered_set<std::string> queuedPages{start};
        std::string scopeBase;

        while (!pages.empty() && result_.pages < pageLimit && !interrupted()) {
            const auto [url, depth] = pages.front();
            pages.pop_front();
            const bool startPage = result_.pages == 0 && scopeBase.empty();

            const auto fetched = fetchFollowingRedirects(
                *fetchers_.front(), url, kAcceptPage, "", depth == 0 ? "page" : "page:" + std::to_string(depth));
            if (startPage) {
                if (!fetched || !fetched->second.ok) {
                    throw std::runtime_error("Could not fetch " + url + ": "
                        + (fetched ? fetched->second.error : std::string("no response")));
                }
                result_.finalUrl = fetched->first;
                result_.status = fetched->second.status;
                // Subpages are the pages under the path of the page we ended up on after redirects.
                scopeBase = fetched->first;
                queuedPages.insert(fetched->first);
            }
            if (!fetched || !fetched->second.ok) {
                continue;
            }
            ++result_.pages;
            const auto& [pageUrl, exchange] = *fetched;

            if (exchange.isHtml()) {
                if (const auto html = exchange.decodedBody()) {
                    const auto links = extractHtmlLinks(*html, pageUrl);
                    if (startPage) result_.title = links.title;
                    for (const auto& resource : links.resources) resources_.push_back({resource, pageUrl, 0});
                    for (const auto& frame : links.frames) resources_.push_back({frame, pageUrl, 1});
                    const bool deeper = options_.maxDepth == kUnlimitedDepth || depth < options_.maxDepth;
                    if (deeper) {
                        for (const auto& link : links.pages) {
                            if (isInCrawlScope(scopeBase, link, options_.scope) && queuedPages.insert(link).second) {
                                pages.emplace_back(link, depth + 1);
                            }
                        }
                    }
                } else {
                    log_ << "     (could not decode " << headerValue(exchange.headers, "content-encoding")
                         << " body; resources of this page are not captured)\n";
                }
            }
            fetchResources();
        }

        if (result_.pages >= pageLimit) {
            result_.pagesNotCrawled = static_cast<int>(pages.size());
        }
        result_.stopped = stop_;
        result_.timedOut = deadline_ && Clock::now() > *deadline_;
        warc_.close();
        result_.warcBytes = warc_.bytesWritten();
        return result_;
    }

private:
    struct PendingResource {
        std::string url;
        std::string referer;
        int frameDepth;  // 0 = plain resource, >0 = embedded document
    };

    const CrawlOptions& options_;
    std::ostream& log_;
    const std::atomic<bool>& stop_;
    WarcWriter warc_;
    std::optional<Clock::time_point> deadline_;
    std::vector<std::unique_ptr<Fetcher>> fetchers_;  // one per parallel fetch, each keeps its connections

    // Guards everything below, the WARC writer and the log while resources are fetched in parallel.
    std::mutex mutex_;
    std::condition_variable queueChanged_;
    CrawlResult result_;
    std::unordered_set<std::string> fetched_;
    std::deque<PendingResource> resources_;
    int activeFetches_{};

    bool interrupted() const {
        return stop_ || (deadline_ && Clock::now() > *deadline_);
    }

    // Fetches url, follows redirects and records the responses.
    // Returns the final URL and exchange, or nullopt when the URL was already fetched in this crawl.
    // Must be called without holding mutex_.
    std::optional<std::pair<std::string, Exchange>> fetchFollowingRedirects(
        Fetcher& fetcher, const std::string& url, const char* accept, const std::string& referer,
        const std::string& kind) {
        std::vector<std::pair<std::string, Exchange>> chain;
        std::string current = url;
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            const bool inChain = std::any_of(chain.begin(), chain.end(), [&](const auto& e) { return e.first == current; });
            {
                // A URL fetched elsewhere in this crawl is not fetched again. Within one redirect chain it is:
                // sites often set a cookie on a redirect and then answer the same URL with the real page.
                std::lock_guard lock(mutex_);
                if (!inChain && !fetched_.insert(current).second) {
                    break;
                }
            }
            Exchange exchange = fetcher.fetch(current, accept, referer);
            {
                std::lock_guard lock(mutex_);
                if (exchange.ok) {
                    log_ << std::format("{:<4} {:<8} {:>9}  {}  {}\n", exchange.status, kind,
                                        formatSize(exchange.body.size()), current,
                                        headerValue(exchange.headers, "content-type"));
                } else {
                    log_ << std::format("ERR  {:<8} {} ({})\n", kind, current, exchange.error);
                }
                log_ << std::flush;
            }
            std::string target;
            if (exchange.ok && isRedirect(exchange.status)) {
                target = resolveUrl(current, headerValue(exchange.headers, "location"));
            }
            chain.emplace_back(current, std::move(exchange));
            if (target.empty()) {
                break;
            }
            current = target;
        }
        if (chain.empty()) {
            return std::nullopt;
        }
        {
            // For a URL that occurs several times in the chain only its last response is stored, so a
            // replay does not follow a redirect the live site sent only before its cookie was set.
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i < chain.size(); ++i) {
                const auto& [chainUrl, exchange] = chain[i];
                const bool superseded = std::any_of(chain.begin() + static_cast<std::ptrdiff_t>(i) + 1, chain.end(),
                    [&](const auto& later) { return later.first == chainUrl && later.second.ok; });
                if (exchange.ok && !superseded) {
                    warc_.writeExchange(chainUrl, exchange.ip, exchange.request, exchange.headers + exchange.body);
                }
            }
        }
        return std::move(chain.back());
    }

    // Drains the resource queue with kParallelFetches threads, like a browser loading a page.
    void fetchResources() {
        std::vector<std::jthread> threads;
        for (auto& fetcher : fetchers_) {
            threads.emplace_back([this, &fetcher] { resourceWorker(*fetcher); });
        }
    }

    void resourceWorker(Fetcher& fetcher) {
        for (;;) {
            PendingResource item;
            {
                std::unique_lock lock(mutex_);
                queueChanged_.wait(lock, [this] {
                    return !resources_.empty() || activeFetches_ == 0 || interrupted();
                });
                if (resources_.empty() || interrupted()) {
                    queueChanged_.notify_all();
                    return;
                }
                item = std::move(resources_.front());
                resources_.pop_front();
                if (fetched_.contains(item.url)) {
                    continue;
                }
                if (result_.resources + result_.failedResources >= options_.maxResources) {
                    if (!result_.resourceLimitReached) {
                        log_ << "     resource limit of " << options_.maxResources << " reached\n";
                    }
                    result_.resourceLimitReached = true;
                    resources_.clear();
                    queueChanged_.notify_all();
                    return;
                }
                ++activeFetches_;
            }

            const auto fetched = fetchFollowingRedirects(
                fetcher, item.url, item.frameDepth > 0 ? kAcceptPage : kAcceptAny, item.referer,
                item.frameDepth > 0 ? "frame" : "resource");

            // Parse outside the lock; only queue updates need it.
            std::vector<PendingResource> discovered;
            bool ok = false;
            if (fetched && fetched->second.ok) {
                ok = true;
                const auto& [url, exchange] = *fetched;
                if (isStylesheet(exchange, url)) {
                    if (const auto css = exchange.decodedBody()) {
                        for (auto& link : extractCssLinks(*css, url)) discovered.push_back({std::move(link), url, 0});
                    }
                } else if (isJavaScript(exchange, url)) {
                    // Root-relative URLs in a script resolve against the page that runs it.
                    if (const auto js = exchange.decodedBody()) {
                        const auto& base = item.referer.empty() ? url : item.referer;
                        for (auto& link : extractScriptUrls(*js, base)) discovered.push_back({std::move(link), base, 0});
                    }
                } else if (item.frameDepth > 0 && item.frameDepth <= kMaxFrameDepth && exchange.isHtml()) {
                    if (const auto html = exchange.decodedBody()) {
                        const auto links = extractHtmlLinks(*html, url);
                        for (const auto& resource : links.resources) discovered.push_back({resource, url, 0});
                        for (const auto& frame : links.frames) discovered.push_back({frame, url, item.frameDepth + 1});
                    }
                }
            }

            std::lock_guard lock(mutex_);
            --activeFetches_;
            if (ok) {
                ++result_.resources;
            } else if (fetched && !interrupted()) {
                // A transfer cut off by the stop flag or the time limit is not a failed resource.
                ++result_.failedResources;
            }
            for (auto& resource : discovered) resources_.push_back(std::move(resource));
            queueChanged_.notify_all();
        }
    }
};

} // namespace

CrawlResult crawlToWarc(const CrawlOptions& options, const std::filesystem::path& warcPath,
                        std::ostream& log, const std::atomic<bool>& stop) {
    Crawl crawl(options, warcPath, log, stop);
    return crawl.run();
}

} // namespace warc_studio
