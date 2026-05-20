#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <map>
#include <sstream>

namespace warc_studio {

// ---------------------------------------------------------------------------
// HTML helpers
// ---------------------------------------------------------------------------

std::string statusBadge(const std::string& status) {
    return "<span class=\"badge badge-" + htmlEscape(status) + "\">"
         + htmlEscape(status) + "</span>";
}

namespace {

std::string displayTitle(const Entry& entry) {
    if (entry.title && !entry.title->empty()) {
        return *entry.title;
    }
    return entry.url;
}

// Returns the human-readable file size string.
std::string formatBytes(std::int64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

// ---------------------------------------------------------------------------
// Shared page shell — uses external CSS + favicon
// ---------------------------------------------------------------------------

std::string pageHeader(const std::string& title, const std::string& activeNav) {
    std::ostringstream html;
    html << R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>)HTML" << htmlEscape(title) << R"HTML( — warc-studio</title>
  <link rel="icon" href="/favicon.svg" type="image/svg+xml">
  <link rel="stylesheet" href="/static/style.css">
</head>
<body>
<header class="site-header">
  <a class="logo" href="/">&#127760; warc-studio</a>
  <nav class="site-nav">
    <a href="/collections")HTML";
    if (activeNav == "collections") html << " class=\"active\"";
    html << R"HTML(>Collections</a>
    <a href="/entries")HTML";
    if (activeNav == "entries") html << " class=\"active\"";
    html << R"HTML(>Entries</a>
    <a href="/archives-browser")HTML";
    if (activeNav == "archives") html << " class=\"active\"";
    html << R"HTML(>Archive Files</a>
    <a href="/about")HTML";
    if (activeNav == "about") html << " class=\"active\"";
    html << R"HTML(>About</a>
  </nav>
</header>
<div class="container">
)HTML";
    return html.str();
}

std::string pageFooter() {
    return "</div>\n</body>\n</html>\n";
}

void renderMessage(std::ostringstream& html, const std::optional<std::string>& message) {
    if (message && !message->empty()) {
        html << "<div class=\"message\">" << htmlEscape(*message) << "</div>\n";
    }
}

// Renders a collection selector dropdown form.
void renderCollectionSwitcher(std::ostringstream& html,
                               const std::vector<Collection>& allCollections,
                               int currentId,
                               const std::string& action) {
    html << "<form method=\"get\" action=\"" << htmlEscape(action) << "\" style=\"display:inline-block;\">\n";
    html << "  <select name=\"collection_id\" onchange=\"this.form.submit()\">\n";
    for (const auto& c : allCollections) {
        html << "    <option value=\"" << c.id << "\"";
        if (c.id == currentId) html << " selected";
        html << ">" << htmlEscape(c.name) << "</option>\n";
    }
    html << "  </select>\n";
    html << "</form>\n";
}

// Renders the archive files list for one entry (used in entry detail page).
void renderArchiveFilesList(std::ostringstream& html,
                             const std::vector<ArchiveFile>& files,
                             [[maybe_unused]] int entryId) {
    if (files.empty()) {
        html << "<p class=\"muted\">No archive files yet.</p>\n";
        return;
    }
    for (const auto& af : files) {
        const auto slash = af.path.rfind('/');
        const std::string fname = slash == std::string::npos ? af.path : af.path.substr(slash + 1);
        const bool isWacz = af.fileType == "wacz";

        html << "<div class=\"archive-file-row\">\n";
        html << "  <span class=\"badge badge-" << htmlEscape(af.fileType) << "\" style=\"background:#444\">"
             << htmlEscape(af.fileType) << "</span>\n";
        html << "  <span class=\"archive-file-name\" title=\"" << htmlEscape(af.path) << "\">"
             << htmlEscape(fname) << "</span>\n";
        if (af.source && !af.source->empty()) {
            html << "  <span class=\"archive-file-meta\">source: " << htmlEscape(*af.source) << "</span>\n";
        }
        if (af.sizeBytes) {
            html << "  <span class=\"archive-file-meta\">" << formatBytes(*af.sizeBytes) << "</span>\n";
        }
        if (af.sha256 && !af.sha256->empty()) {
            html << "  <span class=\"archive-file-meta muted mono\">sha256: " << htmlEscape(af.sha256->substr(0, 16)) << "…</span>\n";
        }
        html << "  <span class=\"archive-file-meta muted\">" << htmlEscape(af.createdAt) << "</span>\n";
        // Show serve URL for debugging (always visible so user can test the link directly).
        {
            const std::string archiveHref = "/archives/"
                + (af.path.rfind("archives/", 0) == 0 ? af.path.substr(9) : af.path);
            html << "  <br><span class=\"muted\" style=\"font-size:0.75rem\">URL: </span>"
                 << "<a class=\"muted mono\" style=\"font-size:0.75rem\" href=\""
                 << htmlEscape(archiveHref) << "\" target=\"_blank\">"
                 << htmlEscape(archiveHref) << "</a>"
                 << "  <a class=\"muted\" style=\"font-size:0.75rem\" href=\""
                 << htmlEscape(archiveHref) << "\" download>&#11015;</a>\n";
        }
        if (isWacz) {
            html << "  <form method=\"get\" action=\"/archive/" << af.id << "/replay\" target=\"_blank\">"
                 << "<button class=\"btn-sm\" type=\"submit\">Replay</button></form>\n";
            html << "  <a href=\"/archive/" << af.id
                 << "/replay/local\" target=\"_blank\"><button class=\"btn-sm btn-secondary\" type=\"button\">Debug replay</button></a>\n";
        } else {
            html << "  <span class=\"muted\">WARC replay not supported yet</span>\n";
        }
        // Edit label inline form
        html << "  <details style=\"display:inline-block;margin-left:0.5em;\">\n";
        html << "    <summary class=\"btn-sm btn-secondary\" style=\"cursor:pointer;display:inline;\">";
        if (af.label && !af.label->empty()) {
            html << htmlEscape(*af.label);
        } else {
            html << "Edit label";
        }
        html << "</summary>\n";
        html << "    <form method=\"post\" action=\"/archive/" << af.id << "/update\" style=\"display:inline-flex;gap:0.3em;align-items:center;\">\n";
        html << "      <input type=\"text\" name=\"label\" value=\"" << htmlEscape(af.label.value_or("")) << "\" placeholder=\"Label\" size=\"20\">\n";
        html << "      <button class=\"btn-sm\" type=\"submit\">Save</button>\n";
        html << "    </form>\n";
        html << "  </details>\n";
        // Delete archive file button
        html << "  <form method=\"post\" action=\"/archive/" << af.id
             << "/delete\" style=\"display:inline;\" onsubmit=\"return confirm('Delete this archive file?');\">";
        html << "<button class=\"btn-sm btn-danger\" type=\"submit\">Delete</button></form>\n";
        html << "</div>\n";
    }
}

// Renders the entries table shared by collections detail and entries pages.
void renderEntriesTable(std::ostringstream& html,
                         const std::vector<Entry>& entries,
                         bool showCollection = false) {
    html << "<table>\n<thead><tr>\n";
    html << "  <th>#</th>\n";
    if (showCollection) html << "  <th>Collection</th>\n";
    html << "  <th>Title / URL</th>\n";
    html << "  <th>Status</th>\n";
    html << "  <th>Archives</th>\n";
    html << "  <th>Created</th>\n";
    html << "  <th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";

    if (entries.empty()) {
        const int cols = showCollection ? 7 : 6;
        html << "<tr><td colspan=\"" << cols << "\" class=\"muted\">No entries yet.</td></tr>\n";
    }

    for (const auto& entry : entries) {
        html << "<tr>\n";
        // Entry number
        html << "  <td><span class=\"num-badge\">#" << entry.numberPerCollection << "</span></td>\n";
        if (showCollection) {
            html << "  <td class=\"muted\">" << htmlEscape(entry.collectionName) << "</td>\n";
        }
        // Title / URL
        html << "  <td><a href=\"/entry/" << entry.id << "\">"
             << "<strong>" << htmlEscape(displayTitle(entry)) << "</strong></a>"
             << "<br><span class=\"muted mono\">" << htmlEscape(entry.url) << "</span>";
        if (entry.lastError && !entry.lastError->empty()) {
            html << "<div class=\"error-text\">" << htmlEscape(*entry.lastError) << "</div>";
        }
        html << "</td>\n";
        // Status
        html << "  <td>" << statusBadge(entry.status) << "</td>\n";
        // Archive count
        html << "  <td class=\"muted\">" << entry.archiveFileCount << "</td>\n";
        // Created
        html << "  <td class=\"muted\">" << htmlEscape(entry.createdAt.substr(0, 10)) << "</td>\n";
        // Actions
        html << "  <td class=\"td-actions\">\n";
        html << "    <form method=\"post\" action=\"/entry/" << entry.id << "/start\">"
             << "<button class=\"btn-sm btn-secondary\" type=\"submit\">Start rec</button></form>\n";
        html << "    <form method=\"post\" action=\"/entry/" << entry.id << "/stop\">"
             << "<button class=\"btn-sm btn-secondary\" type=\"submit\">Stop rec</button></form>\n";
        html << "    <a href=\"/entry/" << entry.id << "\"><button class=\"btn-sm btn-secondary\" type=\"button\">Detail</button></a>\n";
        if (entry.archiveFileCount > 0) {
            html << "    <form method=\"get\" action=\"/entry/" << entry.id << "/replay/latest\" target=\"_blank\">"
                 << "<button class=\"btn-sm\" type=\"submit\">Replay</button></form>\n";
        }
        html << "    <form method=\"post\" action=\"/entry/" << entry.id
             << "/delete\" onsubmit=\"return confirm('Delete entry #"
             << entry.numberPerCollection << " and its archive files?')\">"
             << "<button class=\"btn-sm btn-danger\" type=\"submit\">Delete</button></form>\n";
        html << "  </td>\n";
        html << "</tr>\n";
    }

    html << "</tbody>\n</table>\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Collections page
// ---------------------------------------------------------------------------

std::string renderCollectionsPage(
    const std::vector<Collection>& collections,
    const std::optional<std::string>& message,
    const std::string& activeNav
) {
    std::ostringstream html;
    html << pageHeader("Collections", activeNav);
    renderMessage(html, message);

    html << "<div class=\"page-title\"><h1>Collections</h1></div>\n";

    // List of existing collections
    html << "<div class=\"card\">\n";
    html << "<table>\n<thead><tr>\n";
    html << "  <th>Name</th><th>Description</th><th>Created</th><th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";

    if (collections.empty()) {
        html << "<tr><td colspan=\"4\" class=\"muted\">No collections yet.</td></tr>\n";
    }
    for (const auto& col : collections) {
        html << "<tr>\n";
        html << "  <td><a href=\"/collections/" << col.id << "/entries\">"
             << htmlEscape(col.name) << "</a></td>\n";
        html << "  <td class=\"muted\">" << htmlEscape(col.description.value_or("")) << "</td>\n";
        html << "  <td class=\"muted\">" << htmlEscape(col.createdAt.substr(0, 10)) << "</td>\n";
        html << "  <td class=\"td-actions\">\n";
        html << "    <a href=\"/collections/" << col.id << "/entries\">"
             << "<button class=\"btn-sm\" type=\"button\">Open entries</button></a>\n";
        html << "    <a href=\"/collection/" << col.id << "\">"
             << "<button class=\"btn-sm btn-secondary\" type=\"button\">Edit</button></a>\n";
        html << "    <form method=\"post\" action=\"/collection/" << col.id
             << "/delete\" onsubmit=\"return confirm('Delete collection &quot;" << htmlEscape(col.name)
             << "&quot; and ALL its entries?')\">"
             << "<button class=\"btn-sm btn-danger\" type=\"submit\">Delete</button></form>\n";
        html << "  </td>\n";
        html << "</tr>\n";
    }

    html << "</tbody>\n</table>\n</div>\n";

    // Create new collection form
    html << "<div class=\"card\">\n<h2>New collection</h2>\n";
    html << "<form method=\"post\" action=\"/collection/new\">\n";
    html << "<div class=\"form-row\">\n";
    html << "  <input type=\"text\" name=\"name\" required placeholder=\"Collection name\">\n";
    html << "  <input type=\"text\" name=\"description\" placeholder=\"Optional description\" size=\"40\">\n";
    html << "  <button type=\"submit\">Create collection</button>\n";
    html << "</div>\n</form>\n</div>\n";

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Entries page
// ---------------------------------------------------------------------------

std::string renderEntriesPage(
    const std::optional<Collection>& currentCollection,
    const std::vector<Collection>& allCollections,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message,
    const std::string& activeNav
) {
    std::ostringstream html;
    const std::string title = currentCollection
        ? "Entries — " + currentCollection->name
        : "Entries";
    html << pageHeader(title, activeNav);
    renderMessage(html, message);

    html << "<div class=\"page-title\">\n";
    html << "<h1>Entries</h1>\n";

    if (!allCollections.empty()) {
        html << "<div class=\"inline-row mt-1\">\n";
        html << "<span class=\"muted\">Collection:</span>\n";
        if (currentCollection) {
            renderCollectionSwitcher(html, allCollections,
                                     currentCollection->id, "/entries");
            html << "<a href=\"/collection/" << currentCollection->id << "\">"
                 << "<button class=\"btn-sm btn-secondary\" type=\"button\">Edit collection</button></a>\n";
        } else {
            renderCollectionSwitcher(html, allCollections, 0, "/entries");
        }
        html << "</div>\n";
    }
    html << "</div>\n";

    if (!currentCollection) {
        html << "<div class=\"card\"><p class=\"muted\">No collection selected. "
             << "<a href=\"/collections\">Create or select a collection</a> first.</p></div>\n";
        html << pageFooter();
        return html.str();
    }

    // Create new entry form
    html << "<div class=\"card\">\n<h2>New entry in \"" << htmlEscape(currentCollection->name) << "\"</h2>\n";
    html << "<form method=\"post\" action=\"/entry/new\">\n";
    html << "<input type=\"hidden\" name=\"collection_id\" value=\"" << currentCollection->id << "\">\n";
    html << "<div class=\"form-row\">\n";
    html << "  <input type=\"url\" name=\"url\" required placeholder=\"https://example.com\" size=\"44\">\n";
    html << "  <input type=\"text\" name=\"title\" placeholder=\"Optional title\" size=\"28\">\n";
    html << "  <button type=\"submit\">Create entry</button>\n";
    html << "</div>\n</form>\n</div>\n";

    // Entries table
    html << "<div class=\"card\">\n";
    renderEntriesTable(html, entries, false);
    html << "</div>\n";

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Entry detail page
// ---------------------------------------------------------------------------

std::string renderEntryDetailPage(
    const Entry& entry,
    const std::vector<ArchiveFile>& archiveFiles,
    const std::vector<CrawlRun>& crawlRuns,
    const std::optional<std::string>& message
) {
    std::ostringstream html;
    html << pageHeader("Entry #" + std::to_string(entry.numberPerCollection)
                      + " — " + entry.collectionName, "entries");
    renderMessage(html, message);

    // Auto-refresh every 20 seconds when a crawl is in progress so the user
    // sees the status change to "archived" without manually reloading.
    if (entry.status == "recording") {
        html << "<script>\n";
        html << "(function() {\n";
        html << "  var CHECK_URL = '/entry/" << entry.id << "/check';\n";
        html << "  var INTERVAL_MS = 20000;\n";
        html << "  function poll() {\n";
        html << "    fetch(CHECK_URL, {method:'GET', redirect:'follow'})\n";
        html << "      .then(function(r) { if (r.ok || r.redirected) window.location.reload(); })\n";
        html << "      .catch(function() {});\n";
        html << "  }\n";
        html << "  var timer = setInterval(function() {\n";
        html << "    fetch('/entry/" << entry.id << "', {method:'GET'})\n";
        html << "      .then(function(r) { return r.text(); })\n";
        html << "      .then(function(body) {\n";
        html << "        if (body.indexOf('status-recording') === -1) { clearInterval(timer); window.location.reload(); return; }\n";
        html << "        poll();\n";
        html << "      })\n";
        html << "      .catch(function() {});\n";
        html << "  }, INTERVAL_MS);\n";
        html << "})();\n";
        html << "</script>\n";
    }

    html << "<div class=\"page-title\">\n";
    html << "<p class=\"muted\"><a href=\"/collections/" << entry.collectionId
         << "/entries\">&#8592; " << htmlEscape(entry.collectionName) << "</a></p>\n";
    html << "<h1><span class=\"num-badge\">#" << entry.numberPerCollection << "</span> "
         << htmlEscape(displayTitle(entry)) << "</h1>\n";
    html << "</div>\n";

    // Entry info card
    html << "<div class=\"card\">\n<h2>Entry info</h2>\n";
    html << "<table>\n";
    html << "<tr><th>Collection</th><td>" << htmlEscape(entry.collectionName) << "</td></tr>\n";
    html << "<tr><th>Number</th><td>#" << entry.numberPerCollection << "</td></tr>\n";
    html << "<tr><th>URL</th><td class=\"mono\"><a href=\"" << htmlEscape(entry.url) << "\" target=\"_blank\">"
         << htmlEscape(entry.url) << "</a></td></tr>\n";
    html << "<tr><th>Status</th><td>" << statusBadge(entry.status) << "</td></tr>\n";
    html << "<tr><th>Created</th><td class=\"muted\">" << htmlEscape(entry.createdAt) << "</td></tr>\n";
    if (entry.archivedAt) {
        html << "<tr><th>Archived</th><td class=\"muted\">" << htmlEscape(*entry.archivedAt) << "</td></tr>\n";
    }
    if (entry.note && !entry.note->empty()) {
        html << "<tr><th>Note</th><td>" << htmlEscape(*entry.note) << "</td></tr>\n";
    }
    if (entry.lastError && !entry.lastError->empty()) {
        html << "<tr><th>Last error</th><td class=\"error-text\">" << htmlEscape(*entry.lastError) << "</td></tr>\n";
    }
    html << "</table>\n";

    // Entry edit form
    html << "<h3 class=\"mt-2\">Edit entry</h3>\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id << "/update\">\n";
    html << "<div class=\"form-group\">\n";
    html << "  <label>URL</label>\n";
    html << "  <input type=\"url\" name=\"url\" required value=\"" << htmlEscape(entry.url) << "\" style=\"width:100%;max-width:480px;\">\n";
    html << "</div>\n";
    html << "<div class=\"form-group\">\n";
    html << "  <label>Title</label>\n";
    html << "  <input type=\"text\" name=\"title\" value=\"" << htmlEscape(entry.title.value_or("")) << "\" size=\"44\">\n";
    html << "</div>\n";
    html << "<div class=\"form-group\">\n";
    html << "  <label>Note</label>\n";
    html << "  <textarea name=\"note\" rows=\"3\" style=\"width:100%;max-width:480px;\">" << htmlEscape(entry.note.value_or("")) << "</textarea>\n";
    html << "</div>\n";
    html << "<button type=\"submit\">Save changes</button>\n";
    html << "</form>\n";

    // Status change form
    html << "<h3 class=\"mt-2\">Change status</h3>\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id << "/status\">\n";
    html << "<div class=\"form-row\">\n";
    html << "<select name=\"status\">\n";
    const char* statuses[] = {
        "new", "queued", "recording", "archived", "imported",
        "failed", "needs_review", "ignored", nullptr
    };
    for (int i = 0; statuses[i] != nullptr; ++i) {
        html << "<option value=\"" << statuses[i] << "\"";
        if (entry.status == statuses[i]) html << " selected";
        html << ">" << statuses[i] << "</option>\n";
    }
    html << "</select>\n";
    html << "<button type=\"submit\">Update status</button>\n";
    html << "</div>\n</form>\n";
    html << "</div>\n";

    // Browsertrix recording actions
    html << "<div class=\"card\">\n<h2>Browsertrix recording</h2>\n";
    if (entry.status == "recording") {
        html << "<p class=\"recording-notice\">&#9679; Crawl is running in the background. "
             << "The page auto-checks every 20&nbsp;s and will reload when the crawl finishes.</p>\n";
    }
    html << "<div class=\"inline-row\">\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id << "/start\">"
         << "<button type=\"submit\">Start recording</button></form>\n";
    html << "<a href=\"/entry/" << entry.id << "/check\" class=\"btn btn-secondary\">Check / auto-stop if done</a>\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id << "/stop\">"
         << "<button class=\"btn-secondary\" type=\"submit\">Stop recording (force)</button></form>\n";
    html << "</div>\n</div>\n";

    // Archive files
    html << "<div class=\"card\">\n<h2>Archive files (" << archiveFiles.size() << ")</h2>\n";
    renderArchiveFilesList(html, archiveFiles, entry.id);

    // Upload form
    html << "<h3 class=\"mt-2\">Upload archive file</h3>\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id
         << "/upload\" enctype=\"multipart/form-data\">\n";
    html << "<div class=\"form-row\">\n";
    html << "  <input type=\"file\" name=\"file\" accept=\".warc,.warc.gz,.wacz\" required>\n";
    html << "  <input type=\"text\" name=\"label\" placeholder=\"Optional label (e.g. manual import)\" size=\"28\">\n";
    html << "  <button type=\"submit\">Upload</button>\n";
    html << "</div>\n";
    html << "<p class=\"muted\">Accepted: .warc, .warc.gz, .wacz</p>\n";
    html << "</form>\n";
    html << "</div>\n";

    // Replay section
    const bool hasWacz = !archiveFiles.empty() && [&]() {
        for (const auto& af : archiveFiles) {
            if (af.fileType == "wacz") return true;
        }
        return false;
    }();

    if (hasWacz) {
        html << "<div class=\"card\">\n<h2>Replay</h2>\n";
        html << "<form method=\"get\" action=\"/entry/" << entry.id << "/replay/latest\" target=\"_blank\">"
             << "<button type=\"submit\">Replay latest WACZ</button></form>\n";
        html << "</div>\n";
    }

    // Crawl runs history
    html << "<div class=\"card\">\n<h2>Crawl run history (" << crawlRuns.size() << ")</h2>\n";
    if (crawlRuns.empty()) {
        html << "<p class=\"muted\">No crawl runs recorded yet.</p>\n";
    } else {
        html << "<table>\n";
        html << "<thead><tr>"
             << "<th>#</th><th>Status</th><th>Browsertrix ID</th>"
             << "<th>Container</th><th>Started</th><th>Stopped</th>"
             << "<th>Exit</th><th>Error</th>"
             << "</tr></thead>\n<tbody>\n";
        for (const auto& cr : crawlRuns) {
            html << "<tr>";
            html << "<td class=\"mono\">" << cr.id << "</td>";
            html << "<td>" << statusBadge(cr.status) << "</td>";
            html << "<td class=\"mono\">" << htmlEscape(cr.browsertrixId.value_or("—")) << "</td>";
            html << "<td class=\"mono\">" << htmlEscape(cr.dockerContainerId.value_or("—")) << "</td>";
            html << "<td class=\"muted\">" << htmlEscape(cr.startedAt.value_or("—")) << "</td>";
            html << "<td class=\"muted\">" << htmlEscape(cr.stoppedAt.value_or("—")) << "</td>";
            html << "<td>";
            if (cr.exitCode) html << *cr.exitCode;
            else html << "—";
            html << "</td>";
            html << "<td>";
            if (cr.errorMessage && !cr.errorMessage->empty()) {
                html << "<span class=\"error-text\">" << htmlEscape(*cr.errorMessage) << "</span>";
            } else {
                html << "—";
            }
            html << "</td>";
            html << "</tr>\n";
        }
        html << "</tbody></table>\n";
    }
    html << "</div>\n";

    // Delete entry
    html << "<div class=\"card\">\n<h2>Danger zone</h2>\n";
    html << "<form method=\"post\" action=\"/entry/" << entry.id
         << "/delete\" onsubmit=\"return confirm('Delete this entry and all its archive files?')\">"
         << "<button class=\"btn-danger\" type=\"submit\">Delete entry</button></form>\n";
    html << "</div>\n";

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Archive files browser page
// ---------------------------------------------------------------------------

std::string renderArchiveFilesPage(
    const std::optional<Collection>& currentCollection,
    const std::vector<Collection>& allCollections,
    const std::vector<ArchiveFile>& archiveFiles,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message
) {
    // Build a map from entry_id → entry for quick lookup
    std::map<int, const Entry*> entryMap;
    for (const auto& e : entries) {
        entryMap[e.id] = &e;
    }

    std::ostringstream html;
    const std::string title = currentCollection
        ? "Archive Files — " + currentCollection->name
        : "Archive Files";
    html << pageHeader(title, "archives");
    renderMessage(html, message);

    html << "<div class=\"page-title\">\n<h1>Archive Files</h1>\n";
    if (!allCollections.empty() && currentCollection) {
        html << "<div class=\"inline-row mt-1\"><span class=\"muted\">Collection:</span>\n";
        renderCollectionSwitcher(html, allCollections, currentCollection->id, "/archives-browser");
        html << "</div>\n";
    }
    html << "</div>\n";

    html << "<div class=\"card\">\n";
    html << "<table>\n<thead><tr>\n";
    html << "  <th>#</th><th>Entry</th><th>Type</th><th>Source</th>"
         << "<th>Label</th><th>Size</th><th>Created</th><th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";

    if (archiveFiles.empty()) {
        html << "<tr><td colspan=\"8\" class=\"muted\">No archive files.</td></tr>\n";
    }

    for (const auto& af : archiveFiles) {
        const auto* entryPtr = entryMap.count(af.entryId) ? entryMap.at(af.entryId) : nullptr;
        const auto slash = af.path.rfind('/');
        const std::string fname = slash == std::string::npos ? af.path : af.path.substr(slash + 1);

        html << "<tr>\n";
        if (entryPtr) {
            html << "  <td><span class=\"num-badge\">#" << entryPtr->numberPerCollection << "</span></td>\n";
            html << "  <td><a href=\"/entry/" << af.entryId << "\">"
                 << htmlEscape(displayTitle(*entryPtr)) << "</a></td>\n";
        } else {
            html << "  <td>—</td>\n";
            html << "  <td class=\"muted\">entry #" << af.entryId << "</td>\n";
        }
        html << "  <td><span class=\"badge\" style=\"background:#444\">"
             << htmlEscape(af.fileType) << "</span></td>\n";
        html << "  <td class=\"muted\">" << htmlEscape(af.source.value_or("")) << "</td>\n";
        html << "  <td class=\"muted\">" << htmlEscape(af.label.value_or("")) << "</td>\n";
        html << "  <td class=\"muted\">" << (af.sizeBytes ? formatBytes(*af.sizeBytes) : "—") << "</td>\n";
        html << "  <td class=\"muted\">" << htmlEscape(af.createdAt.substr(0, 10)) << "</td>\n";
        html << "  <td class=\"td-actions\">\n";
        if (af.fileType == "wacz") {
            html << "    <form method=\"get\" action=\"/archive/" << af.id << "/replay\" target=\"_blank\">"
                 << "<button class=\"btn-sm\" type=\"submit\">Replay</button></form>\n";
        } else {
            html << "    <span class=\"muted\" style=\"font-size:0.8rem\">WARC replay N/A</span>\n";
        }
        html << "  </td>\n";
        html << "</tr>\n";
    }

    html << "</tbody>\n</table>\n</div>\n";
    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// About / settings page
// ---------------------------------------------------------------------------

std::string renderAboutPage(
    const std::string& dataDir,
    const std::string& browsertrixImage,
    bool runBrowsertrix,
    const std::string& appVersion
) {
    std::ostringstream html;
    html << pageHeader("About", "about");

    html << "<div class=\"page-title\"><h1>Settings &amp; About</h1></div>\n";
    html << "<div class=\"card\">\n<h2>Application info</h2>\n";
    html << "<table>\n";
    html << "<tr><th>Version</th><td>" << htmlEscape(appVersion) << "</td></tr>\n";
    html << "<tr><th>Data directory</th><td class=\"mono\">" << htmlEscape(dataDir) << "</td></tr>\n";
    html << "<tr><th>Browsertrix image</th><td class=\"mono\">" << htmlEscape(browsertrixImage) << "</td></tr>\n";
    html << "<tr><th>Browsertrix enabled</th><td>"
         << (runBrowsertrix ? "<span class=\"badge badge-archived\">yes</span>"
                            : "<span class=\"badge badge-failed\">no</span>")
         << "</td></tr>\n";
    html << "</table>\n</div>\n";

    html << "<div class=\"card\">\n<h2>Technology stack</h2>\n";
    html << "<ul>\n";
    html << "<li>C++23 / CMake</li>\n";
    html << "<li><a href=\"https://crowcpp.org/\" target=\"_blank\">Crow</a> HTTP server</li>\n";
    html << "<li>SQLite (direct, no ORM)</li>\n";
    html << "<li><a href=\"https://crawler.docs.browsertrix.com/\" target=\"_blank\">Browsertrix Crawler</a> (Docker)</li>\n";
    html << "<li><a href=\"https://replayweb.page/\" target=\"_blank\">ReplayWeb.page</a></li>\n";
    html << "</ul>\n</div>\n";

    html << pageFooter();
    return html.str();
}

// ---------------------------------------------------------------------------
// Replay embed page
// ---------------------------------------------------------------------------

std::string renderReplayPage(const std::string& sourceUrl, const std::string& title) {
    std::ostringstream html;
    // Minimal standalone page — no nav, no external CSS dependency needed.
    // We embed the ReplayWeb.page UI script so the WACZ is fetched from our
    // HTTP origin, avoiding the HTTPS→HTTP mixed-content block.
    html << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
    html << "<meta charset=\"UTF-8\">\n";
    html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html << "<title>Replay: " << htmlEscape(title) << " — warc-studio</title>\n";
    html << "<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">\n";
    html << "<style>\n";
    html << "html, body { margin: 0; padding: 0; height: 100%; background: #111; color: #eee; font-family: sans-serif; }\n";
    html << ".topbar { display: flex; align-items: center; gap: 1rem; padding: 0.5rem 1rem; background: #1a1a2e; border-bottom: 1px solid #333; }\n";
    html << ".topbar a { color: #7ec8e3; text-decoration: none; font-size: 0.9rem; }\n";
    html << ".topbar .label { color: #aaa; font-size: 0.85rem; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 60vw; }\n";
    html << "replay-web-page { display: block; width: 100%; height: calc(100vh - 46px); }\n";
    html << "</style>\n";
    // ReplayWeb.page UI bundle — served locally so that both the script and
    // the WACZ fetch come from the same HTTP origin.  This avoids the
    // HTTPS→HTTP mixed-content block that occurs when loading ui.js from CDN.
    // The service worker is at /replay/sw.js with scope /replay/ (as recommended
    // by the ReplayWeb.page documentation for self-hosted deployments).
    // Load ui.js from /replay/ui.js — same path prefix as replayBase="/replay/"
    // so the web component resolves all sub-resources (including sw.js) consistently.
    html << "<script src=\"/replay/ui.js\"></script>\n";
    html << "</head>\n<body>\n";
    html << "<div class=\"topbar\">\n";
    html << "  <a href=\"/\">&#8592; warc-studio</a>\n";
    html << "  <span class=\"label\">Replaying: " << htmlEscape(title) << "</span>\n";
    html << "  <a href=\"" << htmlEscape(sourceUrl) << "\" download style=\"margin-left:auto\">&#11015; Download WACZ</a>\n";
    html << "</div>\n";
    html << "<replay-web-page source=\"" << htmlEscape(sourceUrl) << "\""
         << " replayBase=\"/replay/\""
         << " embed=\"default\""
         << "></replay-web-page>\n";
    html << "</body>\n</html>\n";
    return html.str();
}

// ---------------------------------------------------------------------------
// Legacy pages — kept for backward compatibility
// ---------------------------------------------------------------------------

namespace {

void renderEntryRowsLegacy(std::ostringstream& html, const std::vector<Entry>& entries,
                     const std::map<int, ArchiveFile>& latestArchiveFiles) {
    for (const auto& entry : entries) {
        const auto archiveIt = latestArchiveFiles.find(entry.id);
        const bool hasArchive = archiveIt != latestArchiveFiles.end();

        html << "      <tr>\n";
        html << "        <td><span class=\"num-badge\">#" << entry.numberPerCollection << "</span></td>\n";
        html << "        <td>" << htmlEscape(entry.collectionName) << "</td>\n";

        html << "        <td><a href=\"/entry/" << entry.id << "\"><strong>"
             << htmlEscape(displayTitle(entry)) << "</strong></a>"
             << "<br><span class=\"muted mono\">" << htmlEscape(entry.url) << "</span></td>\n";

        html << "        <td>" << statusBadge(entry.status);
        if (entry.lastError && !entry.lastError->empty()) {
            html << "<div class=\"error-text\">" << htmlEscape(*entry.lastError) << "</div>";
        }
        html << "</td>\n";

        html << "        <td class=\"muted\">";
        if (hasArchive) {
            const auto& af = archiveIt->second;
            const auto slash = af.path.rfind('/');
            const std::string fname = slash == std::string::npos ? af.path : af.path.substr(slash + 1);
            html << "<span class=\"mono\" title=\"" << htmlEscape(af.path) << "\">" << htmlEscape(fname) << "</span>";
        }
        html << "</td>\n";

        html << "        <td>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/start\">"
             << "<button class=\"btn-secondary btn-sm\" type=\"submit\">Start rec</button></form>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/stop\">"
             << "<button class=\"btn-secondary btn-sm\" type=\"submit\">Stop rec</button></form>\n";
        if (hasArchive) {
            html << "          <form method=\"get\" action=\"/entry/" << entry.id << "/replay/latest\" target=\"_blank\">"
                 << "<button class=\"btn-sm\" type=\"submit\">Replay</button></form>\n";
        }
        html << "          <a href=\"/entry/" << entry.id << "\"><button class=\"btn-secondary btn-sm\" type=\"button\">Detail</button></a>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id
             << "/delete\" onsubmit=\"return confirm('Delete this entry?')\">"
             << "<button class=\"btn-danger btn-sm\" type=\"submit\">Delete</button></form>\n";
        html << "        </td>\n";
        html << "      </tr>\n";
    }

    if (entries.empty()) {
        html << "      <tr><td colspan=\"6\" class=\"muted\">No entries yet.</td></tr>\n";
    }
}

} // anonymous namespace

std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
) {
    std::ostringstream html;
    html << pageHeader("warc-studio", "collections");

    renderMessage(html, message);

    // Collections section
    html << "<div class=\"page-title\"><h1>warc-studio</h1>"
         << "<p class=\"muted\">C++23 + Crow + SQLite + Browsertrix + ReplayWeb.page</p></div>\n";

    html << "<div class=\"card\">\n<h2>Collections</h2>\n";
    html << "<table>\n<thead><tr>\n";
    html << "  <th>Name</th><th>Description</th><th>Created</th><th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";

    for (const auto& col : collections) {
        html << "  <tr>\n";
        html << "    <td><a href=\"/collections/" << col.id << "/entries\">"
             << htmlEscape(col.name) << "</a></td>\n";
        html << "    <td class=\"muted\">" << htmlEscape(col.description.value_or("")) << "</td>\n";
        html << "    <td class=\"muted\">" << htmlEscape(col.createdAt.substr(0, 10)) << "</td>\n";
        html << "    <td class=\"td-actions\">\n";
        html << "      <a href=\"/collections/" << col.id << "/entries\">"
             << "<button class=\"btn-sm\" type=\"button\">Open entries</button></a>\n";
        html << "      <form method=\"post\" action=\"/collection/" << col.id
             << "/delete\" onsubmit=\"return confirm('Delete collection?')\">"
             << "<button class=\"btn-danger btn-sm\" type=\"submit\">Delete</button></form>\n";
        html << "    </td>\n";
        html << "  </tr>\n";
    }
    if (collections.empty()) {
        html << "  <tr><td colspan=\"4\" class=\"muted\">No collections yet.</td></tr>\n";
    }
    html << "</tbody>\n</table>\n</div>\n";

    html << "<div class=\"card\">\n<h2>New collection</h2>\n";
    html << "<form method=\"post\" action=\"/collection/new\">\n";
    html << "<div class=\"form-row\">\n";
    html << "  <input type=\"text\" name=\"name\" required placeholder=\"Collection name\">\n";
    html << "  <input type=\"text\" name=\"description\" placeholder=\"Optional description\" size=\"40\">\n";
    html << "  <button type=\"submit\">Create collection</button>\n";
    html << "</div>\n</form>\n</div>\n";

    html << "<div class=\"card\">\n<h2>New entry</h2>\n";
    html << "<form method=\"post\" action=\"/entry/new\">\n";
    html << "<div class=\"form-row\">\n";
    html << "<select name=\"collection_id\" required>\n";
    for (const auto& collection : collections) {
        html << "<option value=\"" << collection.id << "\">" << htmlEscape(collection.name) << "</option>\n";
    }
    html << "</select>\n";
    html << "<input type=\"url\" name=\"url\" required placeholder=\"https://example.com\" size=\"42\">\n";
    html << "<input type=\"text\" name=\"title\" placeholder=\"Optional title\" size=\"28\">\n";
    html << "<button type=\"submit\">Create entry</button>\n";
    html << "</div>\n</form>\n</div>\n";

    html << "<div class=\"card\">\n<h2>All entries</h2>\n";
    html << "<table>\n<thead><tr>\n";
    html << "  <th>#</th><th>Collection</th><th>Title / URL</th><th>Status</th>"
         << "<th>Archive</th><th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";
    renderEntryRowsLegacy(html, entries, latestArchiveFiles);
    html << "</tbody>\n</table>\n</div>\n";

    html << pageFooter();
    return html.str();
}

std::string renderCollectionDetailPage(
    const Collection& collection,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
) {
    std::ostringstream html;
    html << pageHeader("Collection: " + collection.name, "collections");

    html << "<p class=\"muted\"><a href=\"/\">&#8592; Back to collections</a></p>\n";

    renderMessage(html, message);

    html << "<div class=\"card\">\n";
    html << "<h2>Collection: " << htmlEscape(collection.name) << "</h2>\n";
    if (collection.description && !collection.description->empty()) {
        html << "<p class=\"muted\">" << htmlEscape(*collection.description) << "</p>\n";
    }
    html << "<p class=\"muted\">Created: " << htmlEscape(collection.createdAt) << "</p>\n";

    html << "<h3>Edit collection</h3>\n";
    html << "<form method=\"post\" action=\"/collection/" << collection.id << "/edit\">\n";
    html << "<div class=\"form-row\">\n";
    html << "<input type=\"text\" name=\"name\" required value=\"" << htmlEscape(collection.name) << "\">\n";
    html << "<input type=\"text\" name=\"description\" placeholder=\"Description\" size=\"40\" value=\""
         << htmlEscape(collection.description.value_or("")) << "\">\n";
    html << "<button type=\"submit\">Save changes</button>\n";
    html << "</div>\n</form>\n";

    html << "<h3>Delete collection</h3>\n";
    html << "<form method=\"post\" action=\"/collection/" << collection.id
         << "/delete\" onsubmit=\"return confirm('Delete collection &quot;" << htmlEscape(collection.name)
         << "&quot; and ALL its entries?')\">\n";
    html << "<button class=\"btn-danger\" type=\"submit\">Delete this collection</button>\n";
    html << "</form>\n</div>\n";

    // New entry form
    html << "<div class=\"card\">\n<h2>Add entry to this collection</h2>\n";
    html << "<form method=\"post\" action=\"/entry/new\">\n";
    html << "<input type=\"hidden\" name=\"collection_id\" value=\"" << collection.id << "\">\n";
    html << "<div class=\"form-row\">\n";
    html << "<input type=\"url\" name=\"url\" required placeholder=\"https://example.com\" size=\"44\">\n";
    html << "<input type=\"text\" name=\"title\" placeholder=\"Optional title\" size=\"28\">\n";
    html << "<button type=\"submit\">Create entry</button>\n";
    html << "</div>\n</form>\n</div>\n";

    html << "<div class=\"card\">\n<h2>Entries in this collection</h2>\n";
    html << "<table>\n<thead><tr>\n";
    html << "  <th>#</th><th>Collection</th><th>Title / URL</th><th>Status</th>"
         << "<th>Archive</th><th>Actions</th>\n";
    html << "</tr></thead>\n<tbody>\n";
    renderEntryRowsLegacy(html, entries, latestArchiveFiles);
    html << "</tbody>\n</table>\n</div>\n";

    html << pageFooter();
    return html.str();
}

} // namespace warc_studio
