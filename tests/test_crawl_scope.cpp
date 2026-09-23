// Unit tests for CrawlScope: isInCrawlScope and siteDomain
// No external test framework — uses simple assert-based checks.
// Run:  ./warc-studio-tests

#include "warc_studio/CrawlScope.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

using warc_studio::CrawlScope;
using warc_studio::isInCrawlScope;
using warc_studio::siteDomain;

// ---------------------------------------------------------------------------
// Helper macros
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT_TRUE(expr)                                                    \
    do {                                                                     \
        if (expr) {                                                          \
            ++g_passed;                                                      \
        } else {                                                             \
            ++g_failed;                                                      \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "    \
                      << "Expected TRUE: " << #expr << "\n";                 \
        }                                                                    \
    } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

// ---------------------------------------------------------------------------
// PREFIX scope tests (start URL ends with '/')
// ---------------------------------------------------------------------------

static void test_subpages_with_trailing_slash() {
    const std::string start = "https://example.com/docs/";
    const CrawlScope d = CrawlScope::PREFIX;

    // Self — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/", d));

    // Direct child — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/a.html", d));

    // Nested child — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/tutorial/b.html", d));

    // Deep nesting — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/x/y/z.html", d));

    // Fragment on child — allowed (fragment ignored)
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/a.html#section", d));

    // Root — forbidden (parent)
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/", d));

    // About page (sibling of docs) — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/about.html", d));

    // Blog (sibling) — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/blog/", d));

    // docs2 (sibling) — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/docs2/", d));

    // External domain — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://other.com/docs/", d));

    // Different scheme — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "http://example.com/docs/", d));
}

// ---------------------------------------------------------------------------
// PREFIX scope tests (start URL does NOT end with '/')
// ---------------------------------------------------------------------------

static void test_subpages_without_trailing_slash() {
    const std::string start = "https://example.com/docs/index.html";
    const CrawlScope d = CrawlScope::PREFIX;
    // Base prefix should be "/docs/"

    // Self — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/index.html", d));

    // Sibling in same dir — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/other.html", d));

    // Child subdir — allowed
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/tutorial/b.html", d));

    // Parent dir — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/", d));
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/about.html", d));

    // Sibling dir at top level — forbidden
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/other/page.html", d));
}

// ---------------------------------------------------------------------------
// Path normalisation edge cases
// ---------------------------------------------------------------------------

static void test_path_normalisation() {
    const std::string start = "https://example.com/docs/";
    const CrawlScope d = CrawlScope::PREFIX;

    // Redundant dots in candidate — should normalise and still match
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/docs/./a.html", d));

    // ".." escape attempt — must be rejected
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/docs/../about.html", d));
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com/docs/sub/../../about.html", d));
}

// ---------------------------------------------------------------------------
// DOMAIN and ANY scopes
// ---------------------------------------------------------------------------

static void test_domain_scope() {
    const std::string start = "https://www.example.com/blog/post.html";
    const CrawlScope d = CrawlScope::DOMAIN;

    EXPECT_TRUE(isInCrawlScope(start, "https://www.example.com/about.html", d));
    EXPECT_TRUE(isInCrawlScope(start, "https://example.com/", d));
    EXPECT_TRUE(isInCrawlScope(start, "https://shop.example.com/cart", d));
    EXPECT_TRUE(isInCrawlScope(start, "http://docs.example.com/", d));        // http is fine too
    EXPECT_TRUE(isInCrawlScope(start, "https://EXAMPLE.com:8443/x", d));      // case and port

    EXPECT_FALSE(isInCrawlScope(start, "https://notexample.com/", d));        // not a subdomain
    EXPECT_FALSE(isInCrawlScope(start, "https://example.com.evil.org/", d));
    EXPECT_FALSE(isInCrawlScope(start, "https://other.org/", d));
    EXPECT_FALSE(isInCrawlScope(start, "ftp://example.com/file", d));

    // Multi-tenant hosting: another user's site is out of scope.
    EXPECT_TRUE(isInCrawlScope("https://alice.github.io/", "https://alice.github.io/post/", d));
    EXPECT_FALSE(isInCrawlScope("https://alice.github.io/", "https://bob.github.io/", d));
}

static void test_any_scope() {
    const CrawlScope d = CrawlScope::ANY;
    EXPECT_TRUE(isInCrawlScope("https://example.com/", "https://other.org/page", d));
    EXPECT_TRUE(isInCrawlScope("https://example.com/", "http://third.net/", d));
    EXPECT_FALSE(isInCrawlScope("https://example.com/", "mailto:someone@example.com", d));
    EXPECT_FALSE(isInCrawlScope("https://example.com/", "not a url", d));
}

static void test_site_domain() {
    EXPECT_TRUE(siteDomain("www.example.com") == "example.com");
    EXPECT_TRUE(siteDomain("example.com") == "example.com");
    EXPECT_TRUE(siteDomain("a.b.c.example.cz") == "example.cz");
    EXPECT_TRUE(siteDomain("news.bbc.co.uk") == "bbc.co.uk");
    EXPECT_TRUE(siteDomain("www.abc.net.au") == "abc.net.au");
    EXPECT_TRUE(siteDomain("bbc.co.uk") == "bbc.co.uk");
    EXPECT_TRUE(siteDomain("alice.github.io") == "alice.github.io");
    EXPECT_TRUE(siteDomain("www.alice.github.io") == "alice.github.io");
    EXPECT_TRUE(siteDomain("192.168.1.10") == "192.168.1.10");
    EXPECT_TRUE(siteDomain("localhost") == "localhost");
    EXPECT_TRUE(siteDomain("WWW.Seznam.CZ") == "seznam.cz");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    test_domain_scope();
    test_any_scope();
    test_site_domain();
    test_subpages_with_trailing_slash();
    test_subpages_without_trailing_slash();
    test_path_normalisation();

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
