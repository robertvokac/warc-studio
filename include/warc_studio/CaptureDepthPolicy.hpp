#pragma once

#include "warc_studio/Models.hpp"

#include <string>
#include <string_view>

namespace warc_studio {

// ---------------------------------------------------------------------------
// URL policy for capture depth
// ---------------------------------------------------------------------------
//
// Decides whether a newly discovered page URL may be added to the crawl queue,
// given the starting URL and the configured capture_depth.
//
// IMPORTANT: This function governs only HTML/page links (navigational URLs).
// Resources required by the current page (CSS, JS, images, fonts, XHR, etc.)
// MUST always be captured regardless of capture_depth — they are not subject
// to this policy.
//
// Rules
// -----
// CURRENT_PAGE_ONLY
//   - Only the starting URL itself is allowed (fragments are ignored).
//   - No sibling, parent, child, external, or same-site links may be enqueued.
//
// CURRENT_PAGE_AND_SUBPAGES
//   - Only URLs with the same scheme + host as the starting URL are allowed.
//   - Only URLs whose normalised path begins with the base prefix are allowed.
//   - Base prefix = directory of the starting URL path (ending with '/').
//     If the starting URL path already ends with '/', it is used as-is.
//   - Parent paths ("..") and sibling paths are forbidden.
//   - External domains are forbidden.
//   - URL fragments are ignored.
//   - Query strings do not bypass path restrictions.
//
// ---------------------------------------------------------------------------

// Returns true when `candidateUrl` is permitted to be enqueued for crawling
// given the `startUrl` and `depth` policy.
//
// Both URLs must be absolute HTTP(S) URLs.  Malformed input returns false.
bool isCrawlUrlAllowed(const std::string& startUrl,
                       const std::string& candidateUrl,
                       CaptureDepth depth);

} // namespace warc_studio
