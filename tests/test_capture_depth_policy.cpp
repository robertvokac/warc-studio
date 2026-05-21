// Unit tests for CaptureDepthPolicy::isCrawlUrlAllowed
// No external test framework — uses simple assert-based checks.
// Run:  ./warc-studio-tests

#include "warc_studio/CaptureDepthPolicy.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

using warc_studio::CaptureDepth;
using warc_studio::isCrawlUrlAllowed;

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
// CURRENT_PAGE_ONLY tests
// ---------------------------------------------------------------------------

static void test_current_page_only() {
    const std::string start = "https://example.com/docs/";
    const CaptureDepth d = CaptureDepth::CURRENT_PAGE_ONLY;

    // Same URL — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/", d));

    // Fragment is ignored — same page, allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/#section", d));
    EXPECT_TRUE(isCrawlUrlAllowed("https://example.com/page",
                                  "https://example.com/page#top", d));

    // Child page — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/docs/a.html", d));

    // Sibling page — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/docs2/", d));

    // Root — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/", d));

    // About page — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/about.html", d));

    // External domain — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://other.com/docs/", d));

    // Different scheme — not allowed
    EXPECT_FALSE(isCrawlUrlAllowed(start, "http://example.com/docs/", d));

    // Query string on same path — allowed (same page, different query)
    EXPECT_TRUE(isCrawlUrlAllowed("https://example.com/page",
                                  "https://example.com/page?lang=en", d));
}

// ---------------------------------------------------------------------------
// CURRENT_PAGE_AND_SUBPAGES tests (start URL ends with '/')
// ---------------------------------------------------------------------------

static void test_subpages_with_trailing_slash() {
    const std::string start = "https://example.com/docs/";
    const CaptureDepth d = CaptureDepth::CURRENT_PAGE_AND_SUBPAGES;

    // Self — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/", d));

    // Direct child — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/a.html", d));

    // Nested child — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/tutorial/b.html", d));

    // Deep nesting — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/x/y/z.html", d));

    // Fragment on child — allowed (fragment ignored)
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/a.html#section", d));

    // Root — forbidden (parent)
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/", d));

    // About page (sibling of docs) — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/about.html", d));

    // Blog (sibling) — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/blog/", d));

    // docs2 (sibling) — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/docs2/", d));

    // External domain — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://other.com/docs/", d));

    // Different scheme — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "http://example.com/docs/", d));
}

// ---------------------------------------------------------------------------
// CURRENT_PAGE_AND_SUBPAGES tests (start URL does NOT end with '/')
// ---------------------------------------------------------------------------

static void test_subpages_without_trailing_slash() {
    const std::string start = "https://example.com/docs/index.html";
    const CaptureDepth d = CaptureDepth::CURRENT_PAGE_AND_SUBPAGES;
    // Base prefix should be "/docs/"

    // Self — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/index.html", d));

    // Sibling in same dir — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/other.html", d));

    // Child subdir — allowed
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/tutorial/b.html", d));

    // Parent dir — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/", d));
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/about.html", d));

    // Sibling dir at top level — forbidden
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/other/page.html", d));
}

// ---------------------------------------------------------------------------
// Path normalisation edge cases
// ---------------------------------------------------------------------------

static void test_path_normalisation() {
    const std::string start = "https://example.com/docs/";
    const CaptureDepth d = CaptureDepth::CURRENT_PAGE_AND_SUBPAGES;

    // Redundant dots in candidate — should normalise and still match
    EXPECT_TRUE(isCrawlUrlAllowed(start, "https://example.com/docs/./a.html", d));

    // ".." escape attempt — must be rejected
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/docs/../about.html", d));
    EXPECT_FALSE(isCrawlUrlAllowed(start, "https://example.com/docs/sub/../../about.html", d));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    test_current_page_only();
    test_subpages_with_trailing_slash();
    test_subpages_without_trailing_slash();
    test_path_normalisation();

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
