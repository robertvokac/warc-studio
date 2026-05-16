#include "warc_studio/Html.hpp"
#include "warc_studio/HttpUtil.hpp"

#include <sstream>

namespace warc_studio {
namespace {

std::string displayTitle(const Entry& entry) {
    if (entry.title && !entry.title->empty()) {
        return *entry.title;
    }
    return entry.url;
}

} // namespace

std::string renderIndexPage(
    const std::vector<Collection>& collections,
    const std::vector<Entry>& entries,
    const std::optional<std::string>& message
) {
    std::ostringstream html;

    html << R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>warc-studio</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { max-width: 1180px; margin: 0 auto; padding: 2rem; }
    header { margin-bottom: 2rem; }
    h1 { margin: 0; font-size: 2rem; }
    h2 { margin-top: 2rem; }
    form { display: inline-block; margin: 0.15rem 0.25rem 0.15rem 0; }
    input, select, button { padding: 0.55rem 0.7rem; border-radius: 0.45rem; border: 1px solid #8886; }
    button { cursor: pointer; }
    table { width: 100%; border-collapse: collapse; margin-top: 1rem; }
    th, td { padding: 0.75rem; border-bottom: 1px solid #8884; text-align: left; vertical-align: top; }
    .card { border: 1px solid #8884; border-radius: 0.8rem; padding: 1rem; margin: 1rem 0; }
    .message { background: #8882; border-radius: 0.8rem; padding: 0.8rem 1rem; margin: 1rem 0; }
    .muted { opacity: 0.75; font-size: 0.9rem; }
    .danger { color: #b00020; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
  </style>
</head>
<body>
<header>
  <h1>warc-studio</h1>
  <p class="muted">C++23 + Crow + SQLite + Browsertrix + ReplayWeb.page</p>
</header>
)HTML";

    if (message && !message->empty()) {
        html << "<div class=\"message\">" << htmlEscape(*message) << "</div>\n";
    }

    html << R"HTML(
<section class="card">
  <h2>New collection</h2>
  <form method="post" action="/collection/new">
    <input name="name" required placeholder="Collection name">
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
  <h2>Entries</h2>
  <table>
    <thead>
      <tr>
        <th>ID</th>
        <th>Collection</th>
        <th>Title / URL</th>
        <th>Browsertrix ID</th>
        <th>WACZ path</th>
        <th>Actions</th>
      </tr>
    </thead>
    <tbody>
)HTML";

    for (const auto& entry : entries) {
        html << "      <tr>\n";
        html << "        <td>" << entry.id << "</td>\n";
        html << "        <td>" << htmlEscape(entry.collectionName) << "</td>\n";
        html << "        <td><strong>" << htmlEscape(displayTitle(entry)) << "</strong><br><span class=\"muted\">" << htmlEscape(entry.url) << "</span></td>\n";
        html << "        <td class=\"mono\">" << htmlEscape(entry.browsertrixId.value_or("")) << "</td>\n";
        html << "        <td class=\"mono\">" << htmlEscape(entry.warcPath.value_or("")) << "</td>\n";
        html << "        <td>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/start\"><button type=\"submit\">Start recording</button></form>\n";
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/stop\"><button type=\"submit\">Stop recording</button></form>\n";
        if (entry.warcPath && !entry.warcPath->empty()) {
            html << "          <form method=\"get\" action=\"/entry/" << entry.id << "/replay\" target=\"_blank\"><button type=\"submit\">Replay</button></form>\n";
        }
        html << "          <form method=\"post\" action=\"/entry/" << entry.id << "/delete\" onsubmit=\"return confirm('Delete this entry and its local WACZ file?')\"><button class=\"danger\" type=\"submit\">Delete</button></form>\n";
        html << "        </td>\n";
        html << "      </tr>\n";
    }

    if (entries.empty()) {
        html << "      <tr><td colspan=\"6\" class=\"muted\">No entries yet.</td></tr>\n";
    }

    html << R"HTML(    </tbody>
  </table>
</section>
</body>
</html>
)HTML";

    return html.str();
}

} // namespace warc_studio
