#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace warc_studio {

// Links found in an HTML document, resolved to absolute http(s) URLs (without fragments).
struct HtmlLinks {
    std::string title;
    std::vector<std::string> resources;  // everything the page needs to render: CSS, JS, images, fonts, media, ...
    std::vector<std::string> frames;     // documents embedded with <iframe>/<frame>
    std::vector<std::string> pages;      // navigational links (<a href>, <area href>)
};

// A small, forgiving HTML scanner (no DOM): finds tags and attributes, honours <base href>,
// skips comments and <script> bodies, and reads url(...) references from <style> and style="".
HtmlLinks extractHtmlLinks(std::string_view html, const std::string& documentUrl);

// url(...) and @import references of a stylesheet, resolved against the stylesheet's URL.
std::vector<std::string> extractCssLinks(std::string_view css, const std::string& stylesheetUrl);

// Speculative extraction from JavaScript (inline <script> or .js files): quoted strings that look
// like absolute ("https://…", "//…") or root-relative ("/…") URLs of static files (.css, .js,
// images, fonts, media, .json). Catches resources that scripts add at runtime, e.g. stylesheets
// written with document.write or JSON-escaped ("https:\/\/…") URLs in inline data.
std::vector<std::string> extractScriptUrls(std::string_view script, const std::string& baseUrl);

} // namespace warc_studio
