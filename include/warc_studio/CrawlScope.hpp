#pragma once

#include "warc_studio/Models.hpp"

#include <string>
#include <string_view>

namespace warc_studio {

// ---------------------------------------------------------------------------
// URL policy for following links during a crawl
// ---------------------------------------------------------------------------
//
// Decides whether a newly discovered page URL may be added to the crawl queue,
// given the start URL and the crawl scope. How far links are followed (the
// depth) is decided by the crawler; this policy only answers "is it in scope?".
//
// IMPORTANT: This function governs only HTML/page links (navigational URLs).
// Resources required by a captured page (CSS, JS, images, fonts, XHR, iframes)
// are always captured regardless of scope — they are not subject to this policy.
//
// Scopes
// ------
// PREFIX
//   - Same scheme + host as the start URL.
//   - Normalised path must begin with the base prefix = directory of the start
//     URL path (ending with '/'); a start path ending with '/' is used as-is.
//   - Parent paths ("..") and sibling paths are forbidden; fragments are ignored;
//     query strings do not bypass path restrictions.
//
// DOMAIN
//   - Host must belong to the start URL's site domain (see siteDomain), so
//     "blog.example.com" and "www.example.com" are in scope for "example.com".
//   - http and https are both allowed.
//
// ANY
//   - Any http(s) URL. Only safe together with a limited depth.
//
// ---------------------------------------------------------------------------

// Returns true when `candidateUrl` is in the crawl scope of `startUrl`.
// Both URLs must be absolute HTTP(S) URLs.  Malformed input returns false.
bool isInCrawlScope(const std::string& startUrl, const std::string& candidateUrl, CrawlScope scope);

// The registrable ("site") domain of a host, approximating the Public Suffix List:
//   "www.example.com" -> "example.com", "news.bbc.co.uk" -> "bbc.co.uk".
// Multi-tenant hosting domains are treated as suffixes, so every tenant is its own site:
//   "alice.github.io" -> "alice.github.io".
// IP addresses and single-label hosts are returned unchanged.
std::string siteDomain(std::string_view host);

} // namespace warc_studio
