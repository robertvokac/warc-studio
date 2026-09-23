// Unit tests for CaptureUtil (URL keys and resolution, timestamps, tags) and HtmlLinks.
// No external test framework — uses simple checks.
// Run:  ./warc-studio-util-tests

#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/HtmlLinks.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace warc_studio;

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT_EQ(actual, expected)                                               \
    do {                                                                          \
        const auto a_ = (actual);                                                 \
        const auto e_ = (expected);                                               \
        if (a_ == e_) {                                                           \
            ++g_passed;                                                           \
        } else {                                                                  \
            ++g_failed;                                                           \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << #actual \
                      << "\n  expected: " << e_ << "\n  actual:   " << a_ << "\n";  \
        }                                                                         \
    } while (false)

static void test_url_key() {
    EXPECT_EQ(makeUrlKey("https://www.Example.com/a/?q=1#top"), std::string("example.com/a?q=1"));
    EXPECT_EQ(makeUrlKey("http://example.com"), std::string("example.com"));
    EXPECT_EQ(makeUrlKey("https://example.com/"), std::string("example.com"));
    EXPECT_EQ(makeUrlKey("example.com/"), std::string("example.com"));
    EXPECT_EQ(makeUrlKey("https://example.com:443/x"), std::string("example.com/x"));
    EXPECT_EQ(makeUrlKey("https://example.com:8080/x"), std::string("example.com:8080/x"));
    EXPECT_EQ(makeUrlKey("https://example.com/Path/Case"), std::string("example.com/Path/Case"));
    EXPECT_EQ(makeUrlKey("https://example.com/?"), std::string("example.com"));
    EXPECT_EQ(makeUrlKey("  https://example.com/a  "), std::string("example.com/a"));
}

static void test_input_url() {
    EXPECT_EQ(normalizeInputUrl(" example.com/x "), std::string("https://example.com/x"));
    EXPECT_EQ(normalizeInputUrl("http://example.com"), std::string("http://example.com/"));
    EXPECT_EQ(normalizeInputUrl("example.com?q=1"), std::string("https://example.com/?q=1"));
    EXPECT_EQ(normalizeInputUrl("https://example.com/a"), std::string("https://example.com/a"));
    EXPECT_EQ(normalizeInputUrl(""), std::string(""));
    EXPECT_EQ(urlHost("https://user@Example.COM:8080/x?y"), std::string("example.com"));
    EXPECT_EQ(urlHost("not a url"), std::string(""));
}

static void test_timestamps() {
    EXPECT_EQ(isTimestamp(nowTimestamp()), true);
    EXPECT_EQ(parseTimestamp("20260923180211"), std::string("20260923180211"));
    EXPECT_EQ(parseTimestamp("2026-09-23T18:02:11.123Z"), std::string("20260923180211"));
    EXPECT_EQ(parseTimestamp("2026-09-23 18:02"), std::string("20260923180200"));
    EXPECT_EQ(parseTimestamp("2026-09-23"), std::string("20260923000000"));
    EXPECT_EQ(parseTimestamp("yesterday"), std::string(""));
    EXPECT_EQ(formatTimestamp("20260923180211"), std::string("2026-09-23 18:02:11"));
}

static void test_tags() {
    const auto tags = parseTags(" Linux, c++ ,,linux, Web Archive ");
    EXPECT_EQ(tags.size(), std::size_t{3});
    EXPECT_EQ(joinTags(tags), std::string("linux, c++, web archive"));
    EXPECT_EQ(parseTags("").size(), std::size_t{0});
}

static void test_download_name() {
    Capture capture;
    capture.url = "https://www.example.com/a";
    capture.timestamp = "20260923180211";
    capture.filePath = "archives/2026/09/20260923180211-1.warc.gz";
    EXPECT_EQ(captureDownloadName(capture), std::string("www.example.com-20260923180211.warc.gz"));
}

static void test_resolve_url() {
    const std::string base = "https://example.com/blog/post.html?x=1";
    EXPECT_EQ(resolveUrl(base, "img/a.png"), std::string("https://example.com/blog/img/a.png"));
    EXPECT_EQ(resolveUrl(base, "../style.css"), std::string("https://example.com/style.css"));
    EXPECT_EQ(resolveUrl(base, "../../../x"), std::string("https://example.com/x"));
    EXPECT_EQ(resolveUrl(base, "/root.js"), std::string("https://example.com/root.js"));
    EXPECT_EQ(resolveUrl(base, "//cdn.example.net/f.woff2"), std::string("https://cdn.example.net/f.woff2"));
    EXPECT_EQ(resolveUrl(base, "?page=2"), std::string("https://example.com/blog/post.html?page=2"));
    EXPECT_EQ(resolveUrl(base, ""), std::string(""));
    EXPECT_EQ(resolveUrl(base, "#top"), std::string(""));
    EXPECT_EQ(resolveUrl(base, "other.html#top"), std::string("https://example.com/blog/other.html"));
    EXPECT_EQ(resolveUrl(base, "./"), std::string("https://example.com/blog/"));
    EXPECT_EQ(resolveUrl(base, "HTTP://Other.org"), std::string("http://Other.org/"));
    EXPECT_EQ(resolveUrl(base, "data:image/png;base64,AAAA"), std::string(""));
    EXPECT_EQ(resolveUrl(base, "mailto:me@example.com"), std::string(""));
    EXPECT_EQ(resolveUrl(base, "javascript:void(0)"), std::string(""));
    EXPECT_EQ(resolveUrl(base, " a b.png\n"), std::string("https://example.com/blog/a%20b.png"));
    EXPECT_EQ(resolveUrl(base, "a%20b.png"), std::string("https://example.com/blog/a%20b.png"));
    EXPECT_EQ(resolveUrl("https://example.com", "a.css"), std::string("https://example.com/a.css"));
}

static void test_html_links() {
    const std::string html = R"HTML(<!doctype html><html><head>
        <title> Hello &amp; welcome </title>
        <base href="https://cdn.example.com/site/">
        <link rel="stylesheet" href="main.css"><link rel="icon" href="/favicon.ico">
        <link rel="alternate" href="/feed.xml">
        <style>body { background: url('bg.png') } @import "extra.css";</style>
        <script src="app.js"></script>
        <script>var s = "<img src='not-a-resource.png'>";</script>
        <!-- <img src="commented.png"> -->
        </head><body>
        <img src="a.png" srcset="a-1x.png 1x, a-2x.png 2x" data-src="lazy.png">
        <picture><source srcset="b.webp"></picture>
        <div style="background-image:url(div.jpg)"></div>
        <a href="page2.html#s">next</a> <a href="mailto:x@y">mail</a>
        <iframe src="https://player.example.org/embed/1"></iframe>
        <video poster=poster.jpg src=movie.mp4></video>
        </body></html>)HTML";
    const auto links = extractHtmlLinks(html, "https://example.com/index.html");
    EXPECT_EQ(links.title, std::string("Hello & welcome"));
    const std::string expected[] = {
        "https://cdn.example.com/site/main.css", "https://cdn.example.com/favicon.ico",
        "https://cdn.example.com/site/bg.png", "https://cdn.example.com/site/extra.css",
        "https://cdn.example.com/site/app.js", "https://cdn.example.com/site/a.png",
        "https://cdn.example.com/site/a-1x.png", "https://cdn.example.com/site/a-2x.png",
        "https://cdn.example.com/site/lazy.png", "https://cdn.example.com/site/b.webp",
        "https://cdn.example.com/site/div.jpg", "https://cdn.example.com/site/movie.mp4",
        "https://cdn.example.com/site/poster.jpg",
    };
    EXPECT_EQ(links.resources.size(), std::size(expected));
    for (const auto& url : expected) {
        EXPECT_EQ(std::find(links.resources.begin(), links.resources.end(), url) != links.resources.end(), true);
    }
    EXPECT_EQ(links.pages.size(), std::size_t{1});
    EXPECT_EQ(links.pages.empty() ? std::string{} : links.pages[0], std::string("https://cdn.example.com/site/page2.html"));
    EXPECT_EQ(links.frames.size(), std::size_t{1});

    const auto css = extractCssLinks(
        "@import url(\"a.css\"); /* url(skip.png) */ @font-face { src: url(fonts/f.woff2) format('woff2'), "
        "url( 'data:font/woff;base64,AA' ); } .x { background: URL(../img/x.png) }",
        "https://example.com/css/site.css");
    EXPECT_EQ(css.size(), std::size_t{3});
    EXPECT_EQ(css.size() == 3 ? css[2] : std::string{}, std::string("https://example.com/img/x.png"));
}

static void test_script_urls() {
    const std::string js = R"JS(
        document.write('<link rel="stylesheet" href="https://cdn.example.com/css/site.min.css" data-fallback-url="/css/site.css"/>');
        var data = {"img": "https:\/\/cdn.example.com\/a.png", "api": "/api/items", "rel": "img/b.png"};
        loadScript('//static.example.org/app.js?v=3');
        var html = '<img src="' + base + '/x.png">';
        var route = "/about";
        var tpl = `https://${host}/lib.js`, tpl2 = "/img/{id}.png";
    )JS";
    const auto urls = extractScriptUrls(js, "https://www.example.com/page");
    const std::string expected[] = {
        "https://cdn.example.com/css/site.min.css", "https://www.example.com/css/site.css",
        "https://cdn.example.com/a.png", "https://static.example.org/app.js?v=3",
        // Speculative: a fragment of a concatenated string is taken as a root-relative URL.
        // Harmless — at worst the crawl records a 404 for it.
        "https://www.example.com/x.png",
    };
    EXPECT_EQ(urls.size(), std::size(expected));
    for (std::size_t i = 0; i < std::min(urls.size(), std::size(expected)); ++i) {
        EXPECT_EQ(urls[i], expected[i]);
    }
    // Inline scripts of a page are scanned too, external ones are not (they are fetched and scanned then).
    const auto links = extractHtmlLinks(
        "<script>document.write('<link rel=\"stylesheet\" href=\"/s.css\">')</script>"
        "<script src=\"/app.js\">var ignored = '/ignored.png';</script>",
        "https://example.com/");
    EXPECT_EQ(links.resources.size(), std::size_t{2});
}

int main() {
    test_resolve_url();
    test_script_urls();
    test_html_links();
    test_url_key();
    test_input_url();
    test_timestamps();
    test_tags();
    test_download_name();
    std::cout << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
