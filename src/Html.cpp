#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <map>
#include <sstream>

namespace warc_studio {
namespace {

std::string displayTitle(const Entry& entry) {
    if (entry.title && !entry.title->empty()) {
        return *entry.title;
    }
    return entry.url;
}

// Returns a short human-readable label for an entry status value.
std::string statusLabel(const std::string& status) {
    if (status == "new")       return "new";
    if (status == "recording") return "recording";
    if (status == "archived")  return "archived";
    if (status == "failed")    return "failed";
    return status;
}

// Returns an inline CSS color suitable for a status badge.
std::string statusColor(const std::string& status) {
    if (status == "archived")  return "#1a7f37";
    if (status == "recording") return "#bf8700";
    if (status == "failed")    return "#b00020";
    return "#555";
}

// Shared HTML page header with inline CSS.
std::string pageHeader(const std::string& title) {
    std::ostringstream html;
    html << R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>)HTML" << htmlEscape(title) << R"HTML(</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { max-width: 1180px; margin: 0 auto; padding: 2rem; }
    header { margin-bottom: 2rem; }
    h1 { margin: 0; font-size: 2rem; }
    h2 { margin-top: 2rem; }
    a { color: inherit; }
    form { display: inline-block; margin: 0.15rem 0.25rem 0.15rem 0; }
    input, select, button, textarea { padding: 0.55rem 0.7rem; border-radius: 0.45rem; border: 1px solid #8886; }
    textarea { width: 100%; box-sizing: border-box; }
    button { cursor: pointer; }
    table { width: 100%; border-collapse: collapse; margin-top: 1rem; }
    th, td { padding: 0.75rem; border-bottom: 1px solid #8884; text-align: left; vertical-align: top; }
    .card { border: 1px solid #8884; border-radius: 0.8rem; padding: 1rem; margin: 1rem 0; }
    .message { background: #8882; border-radius: 0.8rem; padding: 0.8rem 1rem; margin: 1rem 0; }
    .muted { opacity: 0.75; font-size: 0.9rem; }
    .danger { color: #b00020; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
    .status-badge { display: inline-block; padding: 0.15rem 0.5rem; border-radius: 0.4rem; font-size: 0.8rem; font-weight: 600; color: #fff; }
    .error-text { color: #b00020; font-size: 0.85rem; margin-top: 0.3rem; }
    .inline-row { display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
  </style>
</head>
<body>
<header>
  <h1><a href="/" style="text-decoration:none;">warc-studio</a></h1>
  <p class="muted">C++23 + Crow + SQLite + Browsertrix + ReplayWeb.page</p>
</header>
)HTML";
    return html.str();
}

// Renders the entries table body rows (shared between index and detail page).
void renderEntryRows(std::ostringstream& html, const std::vector<Entry>& entries,
                     const std::map<int, ArchiveFile>& latestArchiveFiles) {
    for (const auto& entry : entries) {
        const auto archiveIt = latestArchiveFiles.find(entry.id);
        const bool hasArchive = archiveIt != latestArchiveFiles.end();

        html << "      <tr>\n";
        html << "        <td>" << entry.id << "</td>\n";
        html << "        <td>" << htmlEscape(entry.collectionName) << "</td>\n";

        // Title / URL cell
        html << "        <td><strong>" << htmlEscape(displayTitle(entry)) << "</strong>"
             << "<br><span class=\"muted\">" << htmlEscape(entry.url) << "</span></td>\n";

        // Status badge + optional error
        html << "        <td>"
             << "<span class=\"status-badge\" style=\"background:" << statusColor(entry.status) << "\">"
             << htmlEscape(statusLabel(entry.status)) << "</span>";
        if (entry.lastError && !entry.lastError->empty()) {
            html << "<div class=\"error-text\">" << htmlEscape(*entry.lastError) << "</div>";
        }
        html << "</td>\n";

        // Archive file cell
        html << "        <td class=\"mono\">";
        if (hasArchive) {
            const auto& af = archiveIt->second;
            // Show only the filename part to keep the cell compact.
            const auto slash = af.path.rfind('/');
            const std::string fname = slash == std::string::npos ? af.path : af.path.substr(slash + 1);
            html << "<span title=\"" << htmlEscape(af.path) << "\">" << htmlEscape(fname) << "</span>";
        }
        html << "</td>\n";

        // Action buttons
        html << "        <td>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/start\">"
             << "<button type=\"submit\">Start recording</button></form>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/stop\">"
             << "<button type=\"submit\">Stop recording</button></form>\n";
        if (hasArchive) {
            html << "          <form method=\"get\" action=\"/entry/" << entry.id << "/replay\" target=\"_blank\">"
                 << "<button type=\"submit\">Replay</button></form>\n";
        }
        html << "          <form method=\"post\" action=\"/entry/" << entry.id
             << "/delete\" onsubmit=\"return confirm('Delete this entry and its local archive file?')\">"
             << "<button class=\"danger\" type=\"submit\">Delete</button></form>\n";
        html << "        </td>\n";
        html << "      </tr>\n";
    }

    if (entries.empty()) {
        html << "      <tr><td colspan=\"6\" class=\"muted\">No entries yet.</td></tr>\n";
    }
}

} // namespace

std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
) {
    std::ostringstream html;
    html << pageHeader("warc-studio");

    if (message && !message->empty()) {
        html << "<div class=\"message\">" << htmlEscape(*message) << "</div>\n";
    }

    // Collections section
    html << R"HTML(
<section class="card">
  <h2>Collections</h2>
  <table>
    <thead>
      <tr>
        <th>ID</th>
        <th>Name</th>
        <th>Description</th>
        <th>Created</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>
)HTML";

    for (const auto& col : collections) {
        html << "      <tr>\n";
        html << "        <td>" << col.id << "</td>\n";
        html << "        <td><a href=\"/collection/" << col.id << "\">" << htmlEscape(col.name) << "</a></td>\n";
        html << "        <td class=\"muted\">" << htmlEscape(col.description.value_or("")) << "</td>\n";
        html << "        <td class=\"muted\">" << htmlEscape(col.createdAt) << "</td>\n";
        html << "        <td>\n";
        html << "          <a href=\"/collection/" << col.id << "\"><button type=\"button\">Detail</button></a>\n";
        html << "          <form method=\"post\" action=\"/collection/" << col.id
             << "/delete\" onsubmit=\"return confirm('Delete collection &quot;" << htmlEscape(col.name)
             << "&quot; and ALL its entries? This cannot be undone.')\">"
             << "<button class=\"danger\" type=\"submit\">Delete</button></form>\n";
        html << "        </td>\n";
        html << "      </tr>\n";
    }

    if (collections.empty()) {
        html << "      <tr><td colspan=\"5\" class=\"muted\">No collections yet.</td></tr>\n";
    }

    html << R"HTML(    </tbody>
  </table>
</section>
<section class="card">
  <h2>New collection</h2>
  <form method="post" action="/collection/new">
    <input name="name" required placeholder="Collection name">
    <input name="description" placeholder="Optional description" size="40">
    <button type="submit">Create collection</button>
  </form>
</section>
<section class="card">
  <h2>New entry</h2>
  <form method="post" action="/entry/new">
    <select name="collection_id" required>
)HTML";

    for (const auto& collection : collections) {
        html << "      <option value=\"" << collection.id << "\">" << htmlEscape(collection.name) << "</option>\n";
    }

    html << R"HTML(    </select>
    <input name="url" required placeholder="https://example.com" size="42">
    <input name="title" placeholder="Optional title" size="28">
    <button type="submit">Create entry</button>
  </form>
</section>
<section>
  <h2>All entries</h2>
  <table>
    <thead>
      <tr>
        <th>ID</th>
        <th>Collection</th>
        <th>Title / URL</th>
        <th>Status</th>
        <th>Archive file</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>
)HTML";

    renderEntryRows(html, entries, latestArchiveFiles);

    html << R"HTML(    </tbody>
  </table>
</section>
</body>
</html>
)HTML";

    return html.str();
}

std::string renderCollectionDetailPage(
    const Collection& collection,
    const std::vector<Entry>& entries,
    const std::map<int, ArchiveFile>& latestArchiveFiles,
    const std::optional<std::string>& message
) {
    std::ostringstream html;
    html << pageHeader("Collection: " + collection.name + " — warc-studio");

    html << "<p class=\"muted\"><a href=\"/\">← Back to all collections</a></p>\n";

    if (message && !message->empty()) {
        html << "<div class=\"message\">" << htmlEscape(*message) << "</div>\n";
    }

    // Collection info + edit form
    html << "<section class=\"card\">\n";
    html << "  <h2>Collection #" << collection.id << ": " << htmlEscape(collection.name) << "</h2>\n";
    if (collection.description && !collection.description->empty()) {
        html << "  <p class=\"muted\">" << htmlEscape(*collection.description) << "</p>\n";
    }
    html << "  <p class=\"muted\">Created: " << htmlEscape(collection.createdAt) << "</p>\n";

    html << "  <h3>Edit collection</h3>\n";
    html << "  <form method=\"post\" action=\"/collection/" << collection.id << "/edit\">\n";
    html << "    <div class=\"inline-row\">\n";
    html << "      <input name=\"name\" required placeholder=\"Collection name\" value=\""
         << htmlEscape(collection.name) << "\">\n";
    html << "      <input name=\"description\" placeholder=\"Optional description\" size=\"40\" value=\""
         << htmlEscape(collection.description.value_or("")) << "\">\n";
    html << "      <button type=\"submit\">Save changes</button>\n";
    html << "    </div>\n";
    html << "  </form>\n";

    html << "  <h3>Delete collection</h3>\n";
    html << "  <form method=\"post\" action=\"/collection/" << collection.id
         << "/delete\" onsubmit=\"return confirm('Delete collection &quot;" << htmlEscape(collection.name)
         << "&quot; and ALL its entries? This cannot be undone.')\">\n";
    html << "    <button class=\"danger\" type=\"submit\">Delete this collection</button>\n";
    html << "  </form>\n";
    html << "</section>\n";

    // Entries in this collection
    html << "<section>\n";
    html << "  <h2>Entries in this collection</h2>\n";
    html << "  <table>\n";
    html << "    <thead>\n";
    html << "      <tr>\n";
    html << "        <th>ID</th>\n";
    html << "        <th>Collection</th>\n";
    html << "        <th>Title / URL</th>\n";
    html << "        <th>Status</th>\n";
    html << "        <th>Archive file</th>\n";
    html << "        <th>Actions</th>\n";
    html << "      </tr>\n";
    html << "    </thead>\n";
    html << "    <tbody>\n";

    renderEntryRows(html, entries, latestArchiveFiles);

    html << "    </tbody>\n";
    html << "  </table>\n";
    html << "</section>\n";
    html << "</body>\n</html>\n";

    return html.str();
}

} // namespace warc_studio
