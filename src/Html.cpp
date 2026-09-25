#include "warc_studio/Html.hpp"
#include "warc_studio/CaptureUtil.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <algorithm>
#include <format>
#include <sstream>

namespace warc_studio {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string statusBadge(const std::string& status) {
    return "<span class=\"badge badge-" + htmlEscape(status) + "\">" + htmlEscape(status) + "</span>";
}

std::string formatBytes(std::int64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024);
    if (bytes < 1024LL * 1024 * 1024) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024 * 1024));
    return std::format("{:.2f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
}

std::string displayTitle(const Capture& capture) {
    return capture.title && !capture.title->empty() ? *capture.title : capture.url;
}

// Rendered in UTC; the footer script converts it to the browser's local time.
std::string timeTag(const std::string& timestamp) {
    return "<time data-ts=\"" + htmlEscape(timestamp) + "\">" + htmlEscape(formatTimestamp(timestamp))
         + " UTC</time>";
}

std::string urlHistoryHref(const std::string& url) {
    return "/url?url=" + urlEncode(url);
}

std::string tagChips(const std::vector<std::string>& tags) {
    std::string html;
    for (const auto& tag : tags) {
        html += "<a class=\"tag\" href=\"/captures?tag=" + urlEncode(tag) + "\">" + htmlEscape(tag) + "</a>";
    }
    return html;
}

// Text input for a comma separated tag list, with clickable suggestions of the most used tags.
std::string tagInput(const std::string& id, const std::string& value, const std::vector<TagCount>& allTags) {
    std::ostringstream html;
    html << "<input type=\"text\" id=\"" << id << "\" name=\"tags\" value=\"" << htmlEscape(value)
         << "\" placeholder=\"tags, comma separated\" autocomplete=\"off\">\n";
    if (!allTags.empty()) {
        auto top = allTags;
        std::sort(top.begin(), top.end(), [](const TagCount& a, const TagCount& b) {
            return a.count != b.count ? a.count > b.count : a.name < b.name;
        });
        if (top.size() > 30) top.resize(30);
        std::sort(top.begin(), top.end(), [](const TagCount& a, const TagCount& b) { return a.name < b.name; });
        html << "<div class=\"tag-suggestions\">";
        for (const auto& tag : top) {
            html << "<button type=\"button\" class=\"tag tag-suggest\" data-target=\"" << id
                 << "\" data-add-tag=\"" << htmlEscape(tag.name) << "\">+ " << htmlEscape(tag.name) << "</button>";
        }
        html << "</div>\n";
    }
    return html.str();
}

// Depth (link hops from the start page), scope and page limit; scope and limit only matter for depth != 0.
std::string depthFields(const std::string& idPrefix, int depth = 0, CrawlScope scope = CrawlScope::PREFIX,
                        int pageLimit = 50) {
    std::ostringstream html;
    html << "<div class=\"form-group\"><label for=\"" << idPrefix << "-depth\">Depth</label>"
         << "<select id=\"" << idPrefix << "-depth\" name=\"max_depth\" data-depth=\"" << idPrefix << "-links\""
         << " title=\"How many clicks away from the page links are followed\">";
    html << "<option value=\"0\"" << (depth == 0 ? " selected" : "") << ">Page only</option>";
    for (int d = 1; d <= kMaxCrawlDepth; ++d) {
        html << "<option value=\"" << d << "\"" << (depth == d ? " selected" : "") << ">" << d
             << (d == 1 ? " click" : " clicks") << " deep</option>";
    }
    html << "<option value=\"" << kUnlimitedDepth << "\"" << (depth == kUnlimitedDepth ? " selected" : "")
         << ">All linked pages</option></select></div>\n";

    html << "<div class=\"form-row nested\" id=\"" << idPrefix << "-links\">"
         << "<div class=\"form-group\"><label>Follow links to</label><select name=\"crawl_scope\">";
    const std::pair<CrawlScope, const char*> scopes[] = {
        {CrawlScope::PREFIX, "Same path (under the page's directory)"},
        {CrawlScope::DOMAIN, "Whole domain incl. subdomains"},
        {CrawlScope::ANY, "Any site (needs a limited depth)"},
    };
    for (const auto& [value, label] : scopes) {
        html << "<option value=\"" << crawlScopeToString(value) << "\"" << (scope == value ? " selected" : "")
             << ">" << label << "</option>";
    }
    html << "</select></div>"
         << "<div class=\"form-group\"><label>Max pages</label>"
         << "<input type=\"number\" name=\"page_limit\" value=\"" << pageLimit << "\" min=\"0\" style=\"width:7em\" "
         << "title=\"0 = no limit (at most 10000)\"></div></div>\n";
    return html.str();
}

// Replay / download buttons for an archived capture.
std::string captureButtons(const Capture& capture, bool small = true) {
    if (capture.status != "archived" || !capture.filePath) {
        return {};
    }
    std::ostringstream html;
    const std::string size = small ? " btn-sm" : "";
    html << "<a class=\"btn" << size << "\" href=\"/capture/" << capture.id << "/replay\" target=\"_blank\">Replay</a>"
         << "<a class=\"btn" << size << " btn-secondary\" href=\"/capture/" << capture.id << "/download\" title=\"Download "
         << htmlEscape(captureDownloadName(capture)) << "\">&#11015; " << htmlEscape(capture.fileType.value_or("")) << "</a>";
    return html.str();
}

std::string sizeCell(const Capture& capture) {
    return capture.sizeBytes ? formatBytes(*capture.sizeBytes) : "—";
}

// Table of captures; with groupByDay a header row is inserted for each new (UTC) day.
void renderCaptureTable(std::ostringstream& html, const std::vector<Capture>& captures, bool groupByDay,
                        bool showUrl = true) {
    if (captures.empty()) {
        html << "<p class=\"muted\">No captures found.</p>\n";
        return;
    }
    html << "<div class=\"table-wrap\"><table class=\"captures\">\n<thead><tr>"
         << "<th>Captured</th>" << (showUrl ? "<th>Page</th>" : "") << "<th>Tags</th><th>Status</th>"
         << "<th class=\"num\">Size</th><th></th></tr></thead>\n<tbody>\n";
    std::string day;
    const int columns = showUrl ? 6 : 5;
    for (const auto& capture : captures) {
        if (groupByDay && capture.timestamp.substr(0, 8) != day) {
            day = capture.timestamp.substr(0, 8);
            html << "<tr class=\"day-row\"><td colspan=\"" << columns << "\">"
                 << htmlEscape(formatTimestamp(capture.timestamp).substr(0, 10)) << "</td></tr>\n";
        }
        html << "<tr>\n";
        html << "  <td class=\"nowrap\"><a href=\"/capture/" << capture.id << "\">" << timeTag(capture.timestamp)
             << "</a><div class=\"muted mono small\">" << htmlEscape(capture.timestamp) << "</div></td>\n";
        if (showUrl) {
            html << "  <td class=\"page-cell\"><a href=\"/capture/" << capture.id << "\"><strong>"
                 << htmlEscape(displayTitle(capture)) << "</strong></a>"
                 << "<div class=\"mono small\"><a class=\"muted\" href=\"" << htmlEscape(urlHistoryHref(capture.url))
                 << "\" title=\"All captures of this URL\">" << htmlEscape(capture.url) << "</a></div></td>\n";
        }
        html << "  <td>" << tagChips(capture.tags) << "</td>\n";
        html << "  <td>" << statusBadge(capture.status);
        if (capture.source == "upload") html << " <span class=\"muted small\">upload</span>";
        if (capture.maxDepth != 0) {
            html << " <span class=\"muted small\" title=\"" << htmlEscape(crawlDepthLabel(capture)) << "\">"
                 << (capture.maxDepth == kUnlimitedDepth ? "+all" : "+" + std::to_string(capture.maxDepth)) << "</span>";
        }
        html << "</td>\n";
        html << "  <td class=\"num nowrap\">" << sizeCell(capture) << "</td>\n";
        html << "  <td class=\"td-actions\">" << captureButtons(capture) << "</td>\n";
        html << "</tr>\n";
    }
    html << "</tbody>\n</table></div>\n";
}

bool hasActiveCaptures(const std::vector<Capture>& captures) {
    return std::any_of(captures.begin(), captures.end(), [](const Capture& c) {
        return c.status == "queued" || c.status == "crawling";
    });
}

// ---------------------------------------------------------------------------
// Page shell
// ---------------------------------------------------------------------------

std::string pageHeader(const std::string& title, const std::string& activeNav, const std::string& extraHead = {},
                       const std::string& searchValue = {}) {
    std::ostringstream html;
    html << R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>)HTML" << htmlEscape(title) << R"HTML( — warc-studio</title>
  <link rel="icon" href="/favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="/static/style.css">
)HTML" << extraHead << R"HTML(</head>
<body>
<header class="site-header">
  <a class="logo" href="/">&#127760; warc-studio</a>
  <nav class="site-nav">
)HTML";
    const std::pair<const char*, const char*> links[] = {
        {"/", "Save"}, {"/captures", "Browse"}, {"/tags", "Tags"}, {"/upload", "Upload"}, {"/about", "About"},
    };
    for (const auto& [href, label] : links) {
        html << "    <a href=\"" << href << "\"" << (activeNav == href ? " class=\"active\"" : "") << ">"
             << label << "</a>\n";
    }
    html << "  </nav>\n"
         << "  <form class=\"header-search\" method=\"get\" action=\"/captures\">"
         << "<input type=\"search\" name=\"q\" value=\"" << htmlEscape(searchValue)
         << "\" placeholder=\"Search URL or title…\"></form>\n"
         << "</header>\n<div class=\"container\">\n";
    return html.str();
}

std::string pageFooter() {
    return R"HTML(</div>
<script>
// Show Wayback timestamps (UTC) in the browser's local time.
document.querySelectorAll('time[data-ts]').forEach(function (el) {
  var s = el.dataset.ts;
  if (!/^\d{14}$/.test(s)) return;
  var d = new Date(Date.UTC(+s.slice(0, 4), +s.slice(4, 6) - 1, +s.slice(6, 8),
                            +s.slice(8, 10), +s.slice(10, 12), +s.slice(12, 14)));
  el.title = el.textContent;
  el.textContent = d.toLocaleString();
});
// Tag suggestion chips append their tag to the tag input.
document.addEventListener('click', function (e) {
  var b = e.target.closest('[data-add-tag]');
  if (!b) return;
  e.preventDefault();
  var input = document.getElementById(b.dataset.target);
  var tags = input.value.split(',').map(function (t) { return t.trim(); }).filter(Boolean);
  if (tags.indexOf(b.dataset.addTag) === -1) tags.push(b.dataset.addTag);
  input.value = tags.join(', ');
  input.focus();
});
// Scope and page limit only apply when links are followed (depth other than "Page only").
document.querySelectorAll('select[data-depth]').forEach(function (sel) {
  var field = document.getElementById(sel.dataset.depth);
  var update = function () { field.hidden = sel.value === '0'; };
  sel.addEventListener('change', update);
  update();
});
</script>
</body>
</html>
)HTML";
}

void renderMessage(std::ostringstream& html, const std::optional<std::string>& message) {
    if (message && !message->empty()) {
        const bool error = message->rfind("Error", 0) == 0;
        html << "<div class=\"message" << (error ? " error" : "") << "\">" << htmlEscape(*message) << "</div>\n";
    }
}

std::string autoRefresh(int seconds) {
    return "  <meta http-equiv=\"refresh\" content=\"" + std::to_string(seconds) + "\">\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Home: Save Page Now
// ---------------------------------------------------------------------------

std::string renderHomePage(const HomeView& view) {
    std::ostringstream html;
    // Refresh while something is crawling, but never while the user may be typing a new URL.
    const bool refresh = view.url.empty() && hasActiveCaptures(view.recentCaptures);
    html << pageHeader("Save Page Now", "/", refresh ? autoRefresh(10) : "");
    renderMessage(html, view.message);

    html << "<div class=\"card hero\">\n<h1>Save Page Now</h1>\n"
         << "<p class=\"muted\">Paste a URL to check whether it is already archived, then archive it. "
         << "Every save creates a new timestamped capture with its own WARC file.</p>\n";
    html << "<form method=\"post\" action=\"/save\" class=\"save-form\">\n";
    html << "<div class=\"save-url\"><input type=\"text\" id=\"url\" name=\"url\" required autofocus "
         << "placeholder=\"https://example.com/page\" value=\"" << htmlEscape(view.url) << "\">"
         << "<button type=\"submit\">Save page</button></div>\n";
    html << "<div id=\"lookup\" class=\"lookup\" aria-live=\"polite\"></div>\n";
    html << "<div class=\"form-row\">\n";
    html << "<div class=\"form-group grow\"><label for=\"save-tags\">Tags</label>"
         << tagInput("save-tags", view.tags, view.allTags) << "</div>\n";
    html << "</div>\n<div class=\"form-row\">\n";
    html << "<div class=\"form-group grow\"><label>Title (optional, detected automatically)</label>"
         << "<input type=\"text\" name=\"title\" value=\"" << htmlEscape(view.title) << "\"></div>\n";
    html << depthFields("save");
    html << "</div>\n</form>\n";
    html << "<p class=\"muted small\"><a href=\"/save/bulk\">Save many URLs at once</a> · "
         << "<a href=\"/upload\">Upload your own WARC/WACZ</a> · "
         << "<a href=\"/about#bookmarklet\">Bookmarklet</a> for saving the page you are looking at.</p>\n";
    html << "</div>\n";

    if (!view.url.empty()) {
        html << "<div class=\"card\">\n<h2>Existing captures of this URL (" << view.existingCaptures.size()
             << ")</h2>\n";
        renderCaptureTable(html, view.existingCaptures, false, false);
        html << "</div>\n";
    }

    const auto& s = view.stats;
    html << "<div class=\"stats\">"
         << "<span><strong>" << s.captures << "</strong> captures</span>"
         << "<span><strong>" << s.urls << "</strong> URLs</span>"
         << "<span><strong>" << formatBytes(s.totalBytes) << "</strong> on disk</span>";
    if (s.queued + s.crawling > 0) {
        html << "<span><a href=\"/captures?status=crawling\">" << s.crawling << " crawling</a>, "
             << "<a href=\"/captures?status=queued\">" << s.queued << " queued</a></span>";
    }
    if (s.failed > 0) {
        html << "<span><a href=\"/captures?status=failed\">" << s.failed << " failed</a></span>";
    }
    html << "</div>\n";

    html << "<div class=\"card\">\n<div class=\"card-head\"><h2>Recent captures</h2>"
         << "<a href=\"/captures\">Browse all &rarr;</a></div>\n";
    renderCaptureTable(html, view.recentCaptures, true);
    html << "</div>\n";

    // Live "is it already archived?" check while typing.
    html << R"HTML(<script>
(function () {
  var input = document.getElementById('url');
  var box = document.getElementById('lookup');
  var timer, seq = 0;
  function check() {
    var value = input.value.trim();
    var mine = ++seq;
    box.textContent = '';
    box.className = 'lookup';
    if (!value) return;
    fetch('/api/lookup?url=' + encodeURIComponent(value))
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (mine !== seq) return;
        if (j.count === 0) {
          box.className = 'lookup lookup-new';
          box.textContent = 'Not archived yet.';
          return;
        }
        box.className = 'lookup lookup-found';
        box.append('Already archived ' + j.count + '×' + (j.archived < j.count ? ' (' + j.archived + ' successful)' : '') + ', last capture ');
        var last = document.createElement('a');
        last.href = '/capture/' + j.last.id;
        last.textContent = new Date(j.last.iso).toLocaleString() + ' (' + j.last.status + ')';
        box.append(last, ' · ');
        var all = document.createElement('a');
        all.href = '/url?url=' + encodeURIComponent(value);
        all.textContent = 'show all captures';
        box.append(all);
        if (j.last.tags.length) box.append(' · tags: ' + j.last.tags.join(', '));
      })
      .catch(function () {});
  }
  input.addEventListener('input', function () { clearTimeout(timer); timer = setTimeout(check, 250); });
  check();
})();
</script>
)HTML";

    html << pageFooter();
    return html.str();
}

std::string renderBulkSavePage(const std::vector<TagCount>& allTags, const std::optional<std::string>& message) {
    std::ostringstream html;
    html << pageHeader("Save many URLs", "/");
    renderMessage(html, message);
    html << "<div class=\"card\">\n<h1>Save many URLs</h1>\n"
         << "<p class=\"muted\">One URL per line. Each URL becomes its own capture in the crawl queue.</p>\n";
    html << "<form method=\"post\" action=\"/save/bulk\">\n";
    html << "<textarea name=\"urls\" rows=\"12\" required placeholder=\"https://example.com/a&#10;https://example.org/b\"></textarea>\n";
    html << "<div class=\"form-row mt-1\">\n<div class=\"form-group grow\"><label>Tags for all URLs</label>"
         << tagInput("bulk-tags", "", allTags) << "</div>\n";
    html << depthFields("bulk");
    html << "</div>\n";
    html << "<label class=\"checkbox\"><input type=\"checkbox\" name=\"skip_archived\" value=\"1\" checked> "
         << "Skip URLs that are already archived</label>\n";
    html << "<div class=\"mt-1\"><button type=\"submit\">Queue all</button></div>\n";
    html << "</form>\n</div>\n";
    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Browse / search
// ---------------------------------------------------------------------------

std::string renderBrowsePage(const BrowseView& view) {
    std::ostringstream html;
    const auto& q = view.query;
    html << pageHeader("Browse captures", "/captures",
                       hasActiveCaptures(view.captures) ? autoRefresh(10) : "", q.urlContains);
    renderMessage(html, view.message);

    html << "<div class=\"page-title\"><h1>Captures</h1></div>\n";
    html << "<div class=\"card\">\n<form method=\"get\" action=\"/captures\" class=\"filter-form\">\n"
         << "<div class=\"form-row\">\n"
         << "<div class=\"form-group grow\"><label>URL or title contains</label>"
         << "<input type=\"text\" name=\"q\" value=\"" << htmlEscape(q.urlContains)
         << "\" placeholder=\"example.com/blog\"></div>\n"
         << "<div class=\"form-group\"><label>Tags (all must match)</label>"
         << "<input type=\"text\" name=\"tag\" value=\"" << htmlEscape(view.tagText) << "\" list=\"tag-list\"></div>\n"
         << "<div class=\"form-group\"><label>Status</label><select name=\"status\">";
    for (const char* status : {"", "archived", "queued", "crawling", "failed", "cancelled"}) {
        html << "<option value=\"" << status << "\"" << (q.status == status ? " selected" : "") << ">"
             << (*status ? status : "any") << "</option>";
    }
    html << "</select></div>\n"
         << "<div class=\"form-group\"><label>Order</label><select name=\"order\">"
         << "<option value=\"newest\">newest first</option>"
         << "<option value=\"oldest\"" << (q.oldestFirst ? " selected" : "") << ">oldest first</option>"
         << "</select></div>\n"
         << "<button type=\"submit\">Search</button>\n"
         << "<a class=\"btn btn-secondary\" href=\"/captures\">Reset</a>\n"
         << "</div>\n</form>\n";
    html << "<datalist id=\"tag-list\">";
    for (const auto& tag : view.allTags) {
        html << "<option value=\"" << htmlEscape(tag.name) << "\">";
    }
    html << "</datalist>\n</div>\n";

    const int pages = std::max(1, (view.total + view.pageSize - 1) / view.pageSize);
    html << "<div class=\"card\">\n<div class=\"card-head\"><h2>" << view.total << " captures</h2>";
    if (!q.urlContains.empty() && view.total > 0) {
        html << "<a href=\"" << htmlEscape(urlHistoryHref(normalizeInputUrl(q.urlContains)))
             << "\">Exact URL history &rarr;</a>";
    }
    html << "</div>\n";
    renderCaptureTable(html, view.captures, true);

    if (pages > 1) {
        auto pageLink = [&](int page) {
            std::string href = "/captures?page=" + std::to_string(page);
            if (!q.urlContains.empty()) href += "&q=" + urlEncode(q.urlContains);
            if (!view.tagText.empty()) href += "&tag=" + urlEncode(view.tagText);
            if (!q.status.empty()) href += "&status=" + urlEncode(q.status);
            if (q.oldestFirst) href += "&order=oldest";
            return href;
        };
        html << "<div class=\"pagination\">";
        if (view.page > 1) html << "<a href=\"" << htmlEscape(pageLink(view.page - 1)) << "\">&larr; Newer</a>";
        html << "<span>Page " << view.page << " / " << pages << "</span>";
        if (view.page < pages) html << "<a href=\"" << htmlEscape(pageLink(view.page + 1)) << "\">Older &rarr;</a>";
        html << "</div>\n";
    }
    html << "</div>\n";
    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// URL history
// ---------------------------------------------------------------------------

std::string renderUrlPage(const std::string& url, const std::vector<Capture>& captures,
                          const std::vector<TagCount>& allTags, const std::optional<std::string>& message) {
    std::ostringstream html;
    html << pageHeader("History of " + url, "/captures", hasActiveCaptures(captures) ? autoRefresh(10) : "");
    renderMessage(html, message);

    html << "<div class=\"page-title\"><h1 class=\"break\">" << htmlEscape(url) << "</h1>\n"
         << "<p class=\"muted\">Saved " << captures.size() << " time" << (captures.size() == 1 ? "" : "s");
    if (!captures.empty()) {
        html << " between " << timeTag(captures.back().timestamp) << " and " << timeTag(captures.front().timestamp);
    }
    html << " · <a href=\"" << htmlEscape(url) << "\" target=\"_blank\" rel=\"noreferrer\">open live page</a></p></div>\n";

    // Years overview, like the Wayback Machine's year bar.
    if (captures.size() > 1) {
        std::vector<std::pair<std::string, int>> years;
        for (const auto& c : captures) {
            const auto year = c.timestamp.substr(0, 4);
            if (years.empty() || years.back().first != year) years.emplace_back(year, 0);
            ++years.back().second;
        }
        html << "<div class=\"years\">";
        for (const auto& [year, count] : years) {
            html << "<span class=\"year\"><strong>" << year << "</strong> " << count << "</span>";
        }
        html << "</div>\n";
    }

    html << "<div class=\"card\">\n";
    renderCaptureTable(html, captures, true, false);
    html << "</div>\n";

    std::vector<std::string> lastTags;
    if (!captures.empty()) lastTags = captures.front().tags;
    html << "<div class=\"card\">\n<h2>Save this URL again</h2>\n"
         << "<form method=\"post\" action=\"/save\">\n<input type=\"hidden\" name=\"url\" value=\""
         << htmlEscape(url) << "\">\n<div class=\"form-row\">\n"
         << "<div class=\"form-group grow\"><label>Tags</label>" << tagInput("again-tags", joinTags(lastTags), allTags)
         << "</div>\n"
         << (captures.empty() ? depthFields("again")
                              : depthFields("again", captures.front().maxDepth, captures.front().scope,
                                            captures.front().pageLimit.value_or(50)))
         << "<button type=\"submit\">Save page</button>\n</div>\n</form>\n</div>\n";

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Capture detail
// ---------------------------------------------------------------------------

std::string renderCapturePage(const Capture& capture, const std::string& logTail,
                              const std::vector<TagCount>& allTags, const std::optional<std::string>& message) {
    std::ostringstream html;
    const bool active = capture.status == "queued" || capture.status == "crawling";
    html << pageHeader(displayTitle(capture), "/captures", active ? autoRefresh(5) : "");
    renderMessage(html, message);

    const std::string base = "/capture/" + std::to_string(capture.id);
    html << "<div class=\"page-title\">\n<p class=\"muted\"><a href=\"" << htmlEscape(urlHistoryHref(capture.url))
         << "\">&larr; All captures of this URL</a></p>\n"
         << "<h1 class=\"break\">" << htmlEscape(displayTitle(capture)) << "</h1>\n"
         << "<div class=\"inline-row\">" << statusBadge(capture.status) << timeTag(capture.timestamp)
         << tagChips(capture.tags) << "</div>\n</div>\n";

    if (capture.error && !capture.error->empty()) {
        html << "<div class=\"message " << (capture.status == "archived" ? "warning" : "error") << "\">"
             << htmlEscape(*capture.error) << "</div>\n";
    }
    if (active) {
        html << "<p class=\"recording-notice\">&#9679; "
             << (capture.status == "queued" ? "Waiting in the crawl queue." : "The page is being crawled.")
             << " This page refreshes automatically.</p>\n";
    }

    // Actions
    html << "<div class=\"card\">\n<div class=\"inline-row\">\n" << captureButtons(capture, false);
    if (capture.status == "queued") {
        html << "<form method=\"post\" action=\"" << base << "/cancel\"><button class=\"btn-secondary\">Cancel</button></form>\n";
    }
    if (capture.status == "crawling") {
        html << "<form method=\"post\" action=\"" << base << "/stop\" title=\"Stop the crawl and keep the pages "
             << "captured so far\"><button class=\"btn-secondary\">Stop crawl</button></form>\n";
    }
    if (capture.source == "crawl" && (capture.status == "failed" || capture.status == "cancelled")) {
        html << "<form method=\"post\" action=\"" << base << "/retry\"><button>Retry</button></form>\n";
    }
    html << "<form method=\"post\" action=\"" << base << "/recapture\" title=\"Create a new capture of this URL\">"
         << "<button class=\"btn-secondary\">Archive again</button></form>\n";
    html << "<form method=\"post\" action=\"" << base << "/delete\" onsubmit=\"return confirm('Delete this capture "
         << "and its archive file?')\"><button class=\"btn-danger\">Delete</button></form>\n";
    html << "</div>\n</div>\n";

    // Info
    html << "<div class=\"card\">\n<h2>Capture</h2>\n<table class=\"info\">\n";
    html << "<tr><th>URL</th><td class=\"mono break\"><a href=\"" << htmlEscape(capture.url)
         << "\" target=\"_blank\" rel=\"noreferrer\">" << htmlEscape(capture.url) << "</a></td></tr>\n";
    html << "<tr><th>Timestamp</th><td>" << timeTag(capture.timestamp) << " <span class=\"mono muted\">"
         << htmlEscape(capture.timestamp) << "</span></td></tr>\n";
    html << "<tr><th>Source</th><td>" << (capture.source == "upload" ? "uploaded file" : "crawl");
    if (capture.source == "crawl") {
        html << " · " << htmlEscape(crawlDepthLabel(capture));
        if (capture.maxDepth != 0) {
            html << " (max pages: "
                 << (capture.pageLimit && *capture.pageLimit > 0 ? std::to_string(*capture.pageLimit) : "unlimited")
                 << ")";
        }
    }
    html << "</td></tr>\n";
    if (capture.filePath) {
        html << "<tr><th>File</th><td><span class=\"mono\">" << htmlEscape(*capture.filePath) << "</span> · "
             << htmlEscape(capture.fileType.value_or("")) << " · <strong>" << sizeCell(capture) << "</strong>";
        if (capture.sizeBytes) html << " <span class=\"muted\">(" << *capture.sizeBytes << " bytes)</span>";
        html << "</td></tr>\n";
    }
    if (capture.sha256) {
        html << "<tr><th>SHA-256</th><td class=\"mono small break\">" << htmlEscape(*capture.sha256) << "</td></tr>\n";
    }
    html << "<tr><th>Requested</th><td class=\"muted\">" << htmlEscape(capture.createdAt) << " UTC</td></tr>\n";
    if (capture.startedAt) {
        html << "<tr><th>Crawl started</th><td class=\"muted\">" << htmlEscape(*capture.startedAt) << " UTC</td></tr>\n";
    }
    if (capture.finishedAt) {
        html << "<tr><th>Finished</th><td class=\"muted\">" << htmlEscape(*capture.finishedAt) << " UTC</td></tr>\n";
    }
    if (capture.note && !capture.note->empty()) {
        html << "<tr><th>Note</th><td class=\"pre\">" << htmlEscape(*capture.note) << "</td></tr>\n";
    }
    html << "</table>\n</div>\n";

    // Edit
    html << "<div class=\"card\">\n<h2>Edit</h2>\n<form method=\"post\" action=\"" << base
         << "/update\" class=\"stack\">\n"
         << "<div class=\"form-group\"><label>URL</label><input type=\"text\" name=\"url\" required value=\""
         << htmlEscape(capture.url) << "\"></div>\n"
         << "<div class=\"form-group\"><label>Title</label><input type=\"text\" name=\"title\" value=\""
         << htmlEscape(capture.title.value_or("")) << "\"></div>\n"
         << "<div class=\"form-group\"><label>Tags</label>" << tagInput("edit-tags", joinTags(capture.tags), allTags)
         << "</div>\n"
         << "<div class=\"form-group\"><label>Note</label><textarea name=\"note\" rows=\"3\">"
         << htmlEscape(capture.note.value_or("")) << "</textarea></div>\n"
         << "<div><button type=\"submit\">Save changes</button></div>\n</form>\n</div>\n";

    if (!logTail.empty()) {
        html << "<div class=\"card\">\n<h2>Crawl log</h2>\n<pre class=\"log\">" << htmlEscape(logTail)
             << "</pre>\n</div>\n";
    }

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

std::string renderTagsPage(const std::vector<TagCount>& tags, const std::optional<std::string>& message) {
    std::ostringstream html;
    html << pageHeader("Tags", "/tags");
    renderMessage(html, message);
    html << "<div class=\"page-title\"><h1>Tags</h1><p class=\"muted\">" << tags.size()
         << " tags. Click a tag to see its captures.</p></div>\n";

    if (tags.empty()) {
        html << "<div class=\"card\"><p class=\"muted\">No tags yet. Add tags when saving a page.</p></div>\n";
        html << pageFooter();
        return html.str();
    }

    int maxCount = 1;
    for (const auto& tag : tags) maxCount = std::max(maxCount, tag.count);
    html << "<div class=\"card tag-cloud\">\n";
    for (const auto& tag : tags) {
        const double size = 0.85 + 0.9 * static_cast<double>(tag.count) / maxCount;
        html << "<a class=\"tag\" style=\"font-size:" << std::format("{:.2f}", size) << "rem\" href=\"/captures?tag="
             << urlEncode(tag.name) << "\">" << htmlEscape(tag.name) << " <span class=\"count\">" << tag.count
             << "</span></a>\n";
    }
    html << "</div>\n";

    html << "<div class=\"card\">\n<h2>Manage tags</h2>\n<div class=\"table-wrap\"><table>\n"
         << "<thead><tr><th>Tag</th><th class=\"num\">Captures</th><th>Rename / merge</th><th></th></tr></thead>\n<tbody>\n";
    for (const auto& tag : tags) {
        html << "<tr><td><a class=\"tag\" href=\"/captures?tag=" << urlEncode(tag.name) << "\">"
             << htmlEscape(tag.name) << "</a></td><td class=\"num\">" << tag.count << "</td>"
             << "<td><form method=\"post\" action=\"/tags/rename\"><input type=\"hidden\" name=\"from\" value=\""
             << htmlEscape(tag.name) << "\"><input type=\"text\" name=\"to\" value=\"" << htmlEscape(tag.name)
             << "\" size=\"18\"><button class=\"btn-sm btn-secondary\">Rename</button></form></td>"
             << "<td><form method=\"post\" action=\"/tags/delete\" onsubmit=\"return confirm('Remove this tag from all captures? The captures are kept.')\">"
             << "<input type=\"hidden\" name=\"name\" value=\"" << htmlEscape(tag.name) << "\">"
             << "<button class=\"btn-sm btn-danger\">Delete</button></form></td></tr>\n";
    }
    html << "</tbody></table></div>\n<p class=\"muted small\">Renaming onto an existing tag merges the two tags.</p>\n"
         << "</div>\n";
    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

std::string renderUploadPage(const std::vector<TagCount>& allTags, const std::optional<std::string>& message) {
    std::ostringstream html;
    html << pageHeader("Upload archive", "/upload");
    renderMessage(html, message);
    html << "<div class=\"card\">\n<h1>Upload a WARC / WACZ file</h1>\n"
         << "<p class=\"muted\">The file becomes a capture of its own. URL, date and title are read from the file "
         << "when you leave them empty.</p>\n"
         << "<form id=\"archive-upload-form\" method=\"post\" action=\"/upload\" enctype=\"multipart/form-data\" class=\"stack\">\n"
         << "<div class=\"form-group\"><label>File (.warc, .warc.gz, .wacz)</label>"
         << "<input type=\"file\" name=\"file\" accept=\".warc,.gz,.wacz\" required></div>\n"
         << "<div class=\"form-group\"><label>URL</label><input type=\"text\" name=\"url\" "
         << "placeholder=\"detected from the file\"></div>\n"
         << "<div class=\"form-group\"><label>Tags</label>" << tagInput("upload-tags", "", allTags) << "</div>\n"
         << "<div class=\"form-row\">\n"
         << "<div class=\"form-group grow\"><label>Title</label><input type=\"text\" name=\"title\" "
         << "placeholder=\"detected from the file\"></div>\n"
         << "<div class=\"form-group\"><label>Capture date (UTC)</label><input type=\"text\" name=\"timestamp\" "
         << "placeholder=\"YYYYMMDDhhmmss or 2026-09-23 18:02\"></div>\n"
         << "</div>\n"
         << "<div class=\"form-group\"><label>Note</label><textarea name=\"note\" rows=\"2\"></textarea></div>\n"
         << "<div><button type=\"submit\">Upload</button></div>\n"
         << "<div id=\"upload-progress-panel\" hidden role=\"status\" aria-live=\"polite\">"
         << "<progress id=\"upload-progress\" max=\"100\" value=\"0\"></progress> "
         << "<span id=\"upload-progress-label\">Preparing upload…</span></div>\n"
         << "</form>\n</div>\n"
         << R"HTML(<script>
(() => {
  const form = document.getElementById('archive-upload-form');
  const panel = document.getElementById('upload-progress-panel');
  const bar = document.getElementById('upload-progress');
  const label = document.getElementById('upload-progress-label');
  const button = form.querySelector('button[type="submit"]');
  form.addEventListener('submit', event => {
    event.preventDefault();
    button.disabled = true;
    panel.hidden = false;
    bar.value = 0;
    label.textContent = 'Starting upload…';
    const request = new XMLHttpRequest();
    request.open('POST', form.action);
    request.upload.onprogress = progress => {
      if (progress.lengthComputable) {
        const percent = Math.round(100 * progress.loaded / progress.total);
        bar.value = percent;
        label.textContent = percent + '% uploaded';
      } else {
        label.textContent = 'Uploading…';
      }
    };
    request.upload.onload = () => { label.textContent = 'Processing archive…'; };
    request.onload = () => {
      if (request.status >= 200 && request.status < 400) {
        window.location.assign(request.responseURL);
      } else {
        label.textContent = request.status === 503
          ? 'Too many uploads in progress. Please retry shortly.'
          : 'Upload failed (HTTP ' + request.status + '). Please retry.';
        button.disabled = false;
      }
    };
    request.onerror = () => {
      label.textContent = 'Connection failed. Please retry.';
      button.disabled = false;
    };
    request.send(new FormData(form));
  });
})();
</script>)HTML";
    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

std::string renderReplayPage(const Capture& capture, const std::string& archiveSource) {
    std::ostringstream html;
    html << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
         << "<title>" << htmlEscape(displayTitle(capture)) << " (" << htmlEscape(formatTimestamp(capture.timestamp))
         << ") — warc-studio</title>\n"
         << "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">\n"
         // ui.js is loaded from /replay/ui.js — same path as replayBase so SW registration works.
         << "<script src=\"/replay/ui.js\"></script>\n"
         << "<style>\n"
         << "html,body{margin:0;padding:0;width:100%;height:100%;}\n"
         << "body{display:flex;flex-direction:column;}\n"
         << "replay-web-page{display:block;width:100%;flex:1;}\n"
         << ".replay-bar{background:#1a2332;color:#d0dde8;padding:6px 12px;font-family:system-ui,sans-serif;"
         << "font-size:13px;display:flex;gap:14px;align-items:center;flex-wrap:wrap;}\n"
         << ".replay-bar a{color:#90caf9;text-decoration:none;}\n"
         << ".replay-bar .url{color:#aed6a0;word-break:break-all;}\n"
         << "</style>\n</head>\n<body>\n<div class=\"replay-bar\">\n"
         << "<a href=\"/capture/" << capture.id << "\">&larr; Capture</a>\n"
         << "<strong>" << htmlEscape(formatTimestamp(capture.timestamp)) << " UTC</strong>\n"
         << "<span class=\"url\">" << htmlEscape(capture.url) << "</span>\n"
         << "<a href=\"" << htmlEscape(urlHistoryHref(capture.url)) << "\">other captures</a>\n"
         << "<a href=\"/capture/" << capture.id << "/download\" style=\"margin-left:auto\">&#11015; Download "
         << htmlEscape(capture.fileType.value_or("")) << "</a>\n</div>\n";
    // <replay-web-page> web component:
    // - source: relative same-origin path to the archive (no mixed content)
    // - url: the original archived URL, so the page opens directly
    // - replayBase: where to find ui.js and sw.js (must match where the SW is served)
    // - embed="default": renders inside an iframe scoped to replayBase — SW scope covers it
    const auto resolvedUrl = resolveUrl(capture.url, capture.url);  // "https://x.org" -> "https://x.org/"
    html << "<replay-web-page source=\"" << htmlEscape(archiveSource) << "\" url=\""
         << htmlEscape(resolvedUrl.empty() ? capture.url : resolvedUrl)
         << "\" replayBase=\"/replay/\" embed=\"default\"></replay-web-page>\n</body>\n</html>\n";
    return html.str();
}

// ---------------------------------------------------------------------------
// About
// ---------------------------------------------------------------------------

std::string renderAboutPage(const AboutView& view) {
    std::ostringstream html;
    html << pageHeader("About", "/about");
    html << "<div class=\"page-title\"><h1>Settings &amp; About</h1></div>\n";

    const std::string bookmarklet =
        "javascript:(function(){window.open('" + view.baseUrl
        + "/?url='+encodeURIComponent(location.href)+'&title='+encodeURIComponent(document.title));})();";
    html << "<div class=\"card\" id=\"bookmarklet\">\n<h2>Bookmarklet</h2>\n"
         << "<p>Drag this link to your bookmarks bar. Clicking it on any page opens warc-studio with the page's URL "
         << "filled in, shows whether it is already archived and lets you save it with tags.</p>\n"
         << "<p><a class=\"btn\" href=\"" << htmlEscape(bookmarklet) << "\">Save to warc-studio</a></p>\n</div>\n";

    html << "<div class=\"card\">\n<h2>Wayback-style URLs</h2>\n<ul>\n"
         << "<li><span class=\"mono\">" << htmlEscape(view.baseUrl) << "/web/*/example.com</span> — all captures of a URL</li>\n"
         << "<li><span class=\"mono\">" << htmlEscape(view.baseUrl) << "/web/20260101000000/example.com</span> — replay "
         << "the capture closest to a timestamp</li>\n</ul>\n</div>\n";

    const auto& s = view.stats;
    html << "<div class=\"card\">\n<h2>Archive</h2>\n<table class=\"info\">\n"
         << "<tr><th>Captures</th><td>" << s.captures << " (" << s.archived << " archived, " << s.queued
         << " queued, " << s.crawling << " crawling, " << s.failed << " failed)</td></tr>\n"
         << "<tr><th>Distinct URLs</th><td>" << s.urls << "</td></tr>\n"
         << "<tr><th>Archive size</th><td>" << formatBytes(s.totalBytes) << "</td></tr>\n"
         << "</table>\n</div>\n";

    html << "<div class=\"card\">\n<h2>Configuration</h2>\n<table class=\"info\">\n"
         << "<tr><th>Version</th><td>" << htmlEscape(view.appVersion) << "</td></tr>\n"
         << "<tr><th>Data directory</th><td class=\"mono\">" << htmlEscape(view.dataDir) << "</td></tr>\n"
         << "<tr><th>Parallel crawls</th><td>" << view.crawlWorkers << "</td></tr>\n"
         << "<tr><th>Crawl time limit</th><td>"
         << (view.crawlTimeLimitSeconds > 0 ? std::to_string(view.crawlTimeLimitSeconds) + " s" : "none") << "</td></tr>\n"
         << "<tr><th>Max resource size</th><td>" << formatBytes(view.maxResourceBytes) << "</td></tr>\n"
         << "<tr><th>User-Agent</th><td class=\"mono small break\">" << htmlEscape(view.userAgent) << "</td></tr>\n"
         << "</table>\n</div>\n";

    html << "<div class=\"card\">\n<h2>Technology stack</h2>\n<ul>\n"
         << "<li>C++23 / CMake</li>\n"
         << "<li><a href=\"https://crowcpp.org/\" target=\"_blank\">Crow</a> HTTP server</li>\n"
         << "<li>SQLite (direct, no ORM)</li>\n"
         << "<li>Built-in crawler: <a href=\"https://curl.se/libcurl/\" target=\"_blank\">libcurl</a>, "
         << "HTML/CSS link extraction, WARC 1.1 writer</li>\n"
         << "<li><a href=\"https://replayweb.page/\" target=\"_blank\">ReplayWeb.page</a></li>\n"
         << "</ul>\n</div>\n";

    html << pageFooter();
    return html.str();
}

} // namespace warc_studio
