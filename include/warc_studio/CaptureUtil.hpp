#pragma once

#include "warc_studio/Models.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace warc_studio {

// Trims the input, prepends "https://" when no scheme is given and adds the "/" path when missing.
std::string normalizeInputUrl(std::string_view input);

// Normalized lookup key for "is this URL already archived?" checks, similar in
// spirit to the SURT keys used by the Wayback Machine:
//   - scheme is dropped (http and https are the same page)
//   - host is lowercased, a leading "www." and default ports are dropped
//   - the fragment is dropped, and so is a trailing '/'
// e.g. "https://www.Example.com/a/?q=1#top" -> "example.com/a?q=1"
std::string makeUrlKey(std::string_view url);

// Resolves a (possibly relative) link against an absolute http(s) base URL, RFC 3986 style.
// The fragment is dropped and spaces/non-ASCII bytes in path and query are percent-encoded.
// Returns "" for links that cannot be fetched over http(s) (data:, mailto:, javascript:, "#x", ...).
std::string resolveUrl(std::string_view base, std::string_view reference);

// Host part of an absolute URL, lowercased, without port ("" when not parseable).
std::string urlHost(std::string_view url);

// Current UTC time as a 14-digit Wayback timestamp (YYYYMMDDhhmmss).
std::string nowTimestamp();

// True when value is exactly 14 digits.
bool isTimestamp(std::string_view value);

// Accepts "YYYYMMDDhhmmss", "YYYY-MM-DD", "YYYY-MM-DDThh:mm[:ss]", "YYYY-MM-DD hh:mm:ss"
// and ISO 8601 with a trailing 'Z' (as used in WARC-Date). Returns "" when not parseable.
std::string parseTimestamp(std::string_view value);

// "20260923180211" -> "2026-09-23 18:02:11"
std::string formatTimestamp(std::string_view timestamp);

// Splits a comma separated tag list; trims, lowercases and de-duplicates the tags.
std::vector<std::string> parseTags(std::string_view text);

std::string joinTags(const std::vector<std::string>& tags);

// File name offered when downloading a capture, e.g. "example.com-20260923180211.wacz".
std::string captureDownloadName(const Capture& capture);

} // namespace warc_studio
