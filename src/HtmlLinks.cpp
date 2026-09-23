#include "warc_studio/HtmlLinks.hpp"
#include "warc_studio/CaptureUtil.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace warc_studio {
namespace {

std::string toLower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Decodes the character references that commonly appear in URLs and titles.
std::string decodeEntities(std::string_view value) {
    static const std::unordered_map<std::string, std::string> kNamed = {
        {"amp", "&"}, {"quot", "\""}, {"apos", "'"}, {"lt", "<"}, {"gt", ">"}, {"nbsp", " "},
    };
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '&') {
            const auto semi = value.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                const std::string name(value.substr(i + 1, semi - i - 1));
                if (!name.empty() && name[0] == '#') {
                    try {
                        const unsigned long code = name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
                            ? std::stoul(name.substr(2), nullptr, 16)
                            : std::stoul(name.substr(1));
                        // UTF-8 encode.
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else if (code < 0x10000) {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xF0 | (code >> 18));
                            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        i = semi;
                        continue;
                    } catch (const std::exception&) {
                    }
                } else if (const auto it = kNamed.find(name); it != kNamed.end()) {
                    out += it->second;
                    i = semi;
                    continue;
                }
            }
        }
        out += value[i];
    }
    return out;
}

// Collects unique URLs in discovery order.
class UrlList {
public:
    explicit UrlList(std::vector<std::string>& target) : target_(target) {}

    void add(const std::string& url) {
        if (!url.empty() && seen_.insert(url).second) {
            target_.push_back(url);
        }
    }

private:
    std::vector<std::string>& target_;
    std::unordered_set<std::string> seen_;
};

// "a.jpg 1x, b.jpg 2x" -> {"a.jpg", "b.jpg"}
std::vector<std::string> parseSrcset(std::string_view srcset) {
    std::vector<std::string> urls;
    std::size_t i = 0;
    while (i < srcset.size()) {
        while (i < srcset.size() && (isSpace(srcset[i]) || srcset[i] == ',')) ++i;
        const auto start = i;
        while (i < srcset.size() && !isSpace(srcset[i])) ++i;
        std::string_view url = srcset.substr(start, i - start);
        // A URL directly followed by a comma has no descriptor.
        const bool endsWithComma = !url.empty() && url.back() == ',';
        if (endsWithComma) url.remove_suffix(1);
        if (!url.empty()) urls.emplace_back(url);
        if (!endsWithComma) {
            while (i < srcset.size() && srcset[i] != ',') ++i;  // skip the descriptor
        }
    }
    return urls;
}

struct Tag {
    std::string name;  // lowercase; "/name" for end tags
    std::unordered_map<std::string, std::string> attributes;  // lowercase names, decoded values
};

// Parses the tag starting at html[pos] == '<'. Returns the position after '>'.
std::size_t parseTag(std::string_view html, std::size_t pos, Tag& tag) {
    std::size_t i = pos + 1;
    const bool end = i < html.size() && html[i] == '/';
    if (end) ++i;
    const auto nameStart = i;
    while (i < html.size() && !isSpace(html[i]) && html[i] != '>' && html[i] != '/') ++i;
    tag.name = (end ? "/" : "") + toLower(html.substr(nameStart, i - nameStart));
    tag.attributes.clear();

    while (i < html.size() && html[i] != '>') {
        while (i < html.size() && (isSpace(html[i]) || html[i] == '/')) ++i;
        if (i >= html.size() || html[i] == '>') break;
        const auto attrStart = i;
        while (i < html.size() && !isSpace(html[i]) && html[i] != '=' && html[i] != '>') ++i;
        const std::string name = toLower(html.substr(attrStart, i - attrStart));
        while (i < html.size() && isSpace(html[i])) ++i;
        std::string value;
        if (i < html.size() && html[i] == '=') {
            ++i;
            while (i < html.size() && isSpace(html[i])) ++i;
            if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
                const char quote = html[i++];
                const auto close = html.find(quote, i);
                const auto valueEnd = close == std::string_view::npos ? html.size() : close;
                value = html.substr(i, valueEnd - i);
                i = valueEnd == html.size() ? valueEnd : valueEnd + 1;
            } else {
                const auto valueStart = i;
                while (i < html.size() && !isSpace(html[i]) && html[i] != '>') ++i;
                value = html.substr(valueStart, i - valueStart);
            }
        } else if (name.empty()) {
            ++i;  // stray character
            continue;
        }
        if (!name.empty() && !tag.attributes.contains(name)) {
            tag.attributes.emplace(name, decodeEntities(value));
        }
    }
    return i < html.size() ? i + 1 : html.size();
}

std::string attribute(const Tag& tag, const char* name) {
    const auto it = tag.attributes.find(name);
    return it == tag.attributes.end() ? std::string{} : it->second;
}

bool containsWord(const std::string& list, const std::string& word) {
    std::size_t start = 0;
    const std::string lower = toLower(list);
    while (start < lower.size()) {
        while (start < lower.size() && isSpace(lower[start])) ++start;
        auto end = start;
        while (end < lower.size() && !isSpace(lower[end])) ++end;
        if (lower.compare(start, end - start, word) == 0 && end - start == word.size()) return true;
        start = end;
    }
    return false;
}

bool hasStaticExtension(std::string_view url) {
    url = url.substr(0, std::min(url.find_first_of("?#"), url.size()));
    const auto dot = url.rfind('.');
    if (dot == std::string_view::npos || url.find('/', dot) != std::string_view::npos) return false;
    static const std::unordered_set<std::string> kExtensions = {
        "css", "js", "mjs", "json", "png", "jpg", "jpeg", "gif", "webp", "avif", "svg", "ico", "bmp",
        "woff", "woff2", "ttf", "otf", "eot", "mp4", "webm", "mp3", "ogg", "m4a", "wav",
    };
    return kExtensions.contains(toLower(url.substr(dot + 1)));
}

constexpr std::size_t kMaxScriptUrls = 300;

} // namespace

std::vector<std::string> extractScriptUrls(std::string_view script, const std::string& baseUrl) {
    std::vector<std::string> result;
    UrlList urls(result);
    for (std::size_t i = 0; i < script.size() && result.size() < kMaxScriptUrls; ++i) {
        const char quote = script[i];
        if (quote != '"' && quote != '\'' && quote != '`') continue;
        // Candidate: the text after a quote up to the next quote (JSON-escaped slashes allowed).
        std::size_t end = i + 1;
        while (end < script.size() && end - i < 2048 && script[end] != quote && script[end] != '"'
               && script[end] != '\'' && script[end] != '`' && script[end] != '\n' && script[end] != '<'
               && script[end] != '>' && script[end] != ' ') {
            ++end;
        }
        std::string candidate;
        for (std::size_t k = i + 1; k < end; ++k) {
            if (script[k] == '\\' && k + 1 < end && script[k + 1] == '/') continue;  // "\/" -> "/"
            candidate += script[k];
        }
        const bool absolute = candidate.starts_with("http://") || candidate.starts_with("https://")
            || (candidate.starts_with("//") && candidate.size() > 2 && candidate[2] != '/');
        const bool rootRelative = candidate.size() > 1 && candidate[0] == '/' && candidate[1] != '/';
        // Skip template/concatenation fragments such as "https://${host}/a.js" or "/img/{id}.png".
        if ((absolute || rootRelative) && candidate.find_first_of("\\+{}") == std::string::npos
            && hasStaticExtension(candidate)) {
            urls.add(resolveUrl(baseUrl, candidate));
        }
        i = end - 1;
    }
    return result;
}

std::vector<std::string> extractCssLinks(std::string_view css, const std::string& stylesheetUrl) {
    std::vector<std::string> result;
    UrlList urls(result);
    std::size_t i = 0;
    while (i < css.size()) {
        // Skip comments.
        if (css.compare(i, 2, "/*") == 0) {
            const auto end = css.find("*/", i + 2);
            i = end == std::string_view::npos ? css.size() : end + 2;
            continue;
        }
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(css[i])));
        if (c == 'u' && i + 4 <= css.size() && toLower(css.substr(i, 4)) == "url(") {
            i += 4;
            while (i < css.size() && isSpace(css[i])) ++i;
            std::string url;
            if (i < css.size() && (css[i] == '"' || css[i] == '\'')) {
                const char quote = css[i++];
                const auto close = css.find(quote, i);
                const auto end = close == std::string_view::npos ? css.size() : close;
                url = css.substr(i, end - i);
                i = end;
            } else {
                const auto close = css.find(')', i);
                const auto end = close == std::string_view::npos ? css.size() : close;
                url = css.substr(i, end - i);
                i = end;
            }
            urls.add(resolveUrl(stylesheetUrl, url));
            continue;
        }
        if (c == '@' && i + 7 <= css.size() && toLower(css.substr(i, 7)) == "@import") {
            i += 7;
            while (i < css.size() && isSpace(css[i])) ++i;
            if (i < css.size() && (css[i] == '"' || css[i] == '\'')) {
                const char quote = css[i++];
                const auto close = css.find(quote, i);
                const auto end = close == std::string_view::npos ? css.size() : close;
                urls.add(resolveUrl(stylesheetUrl, css.substr(i, end - i)));
                i = end;
            }
            continue;  // @import url(...) is handled by the url( branch
        }
        ++i;
    }
    return result;
}

HtmlLinks extractHtmlLinks(std::string_view html, const std::string& documentUrl) {
    HtmlLinks links;
    UrlList resources(links.resources);
    UrlList frames(links.frames);
    UrlList pages(links.pages);
    std::string base = documentUrl;
    bool baseSeen = false;

    const auto addResource = [&](const std::string& value) { resources.add(resolveUrl(base, value)); };
    const auto addSrcset = [&](const std::string& value) {
        for (const auto& url : parseSrcset(value)) addResource(url);
    };
    const auto addCss = [&](std::string_view css) {
        for (const auto& url : extractCssLinks(css, base)) resources.add(url);
    };

    Tag tag;
    std::size_t i = 0;
    while (i < html.size()) {
        const auto lt = html.find('<', i);
        if (lt == std::string_view::npos) break;
        if (html.compare(lt, 4, "<!--") == 0) {
            const auto end = html.find("-->", lt + 4);
            i = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        if (lt + 1 >= html.size() || !(std::isalpha(static_cast<unsigned char>(html[lt + 1])) || html[lt + 1] == '/')) {
            i = lt + 1;
            continue;
        }
        i = parseTag(html, lt, tag);
        const std::string& name = tag.name;

        if (const auto style = attribute(tag, "style"); !style.empty()) {
            addCss(style);
        }
        if (const auto background = attribute(tag, "background"); !background.empty()) {
            addResource(background);
        }

        if (name == "base" && !baseSeen) {
            if (const auto href = attribute(tag, "href"); !href.empty()) {
                if (auto resolved = resolveUrl(documentUrl, href); !resolved.empty()) base = resolved;
                baseSeen = true;
            }
        } else if (name == "a" || name == "area") {
            pages.add(resolveUrl(base, attribute(tag, "href")));
        } else if (name == "img" || name == "source" || name == "input" || name == "embed" || name == "track") {
            if (name != "input" || toLower(attribute(tag, "type")) == "image") {
                addResource(attribute(tag, "src"));
            }
            addSrcset(attribute(tag, "srcset"));
            // Common lazy-loading attributes.
            addResource(attribute(tag, "data-src"));
            addSrcset(attribute(tag, "data-srcset"));
        } else if (name == "video" || name == "audio") {
            addResource(attribute(tag, "src"));
            addResource(attribute(tag, "poster"));
        } else if (name == "object") {
            addResource(attribute(tag, "data"));
        } else if (name == "image") {  // SVG
            addResource(attribute(tag, "href"));
            addResource(attribute(tag, "xlink:href"));
        } else if (name == "link") {
            const auto rel = attribute(tag, "rel");
            for (const char* kind : {"stylesheet", "icon", "apple-touch-icon", "apple-touch-icon-precomposed",
                                     "preload", "modulepreload", "manifest", "mask-icon"}) {
                if (containsWord(rel, kind)) {
                    addResource(attribute(tag, "href"));
                    addSrcset(attribute(tag, "imagesrcset"));
                    break;
                }
            }
        } else if (name == "iframe" || name == "frame") {
            frames.add(resolveUrl(base, attribute(tag, "src")));
        } else if (name == "meta") {
            const auto property = toLower(attribute(tag, "property") + attribute(tag, "name"));
            if (property == "og:image" || property == "twitter:image") {
                addResource(attribute(tag, "content"));
            }
        } else if (name == "title" && links.title.empty()) {
            const auto end = html.find("</", i);
            std::string title;
            bool space = false;
            for (const char c : decodeEntities(html.substr(i, (end == std::string_view::npos ? html.size() : end) - i))) {
                if (isSpace(c)) {
                    space = !title.empty();
                } else {
                    if (space) title += ' ';
                    space = false;
                    title += c;
                }
            }
            links.title = title.substr(0, 300);
        } else if (name == "script" || name == "style") {
            if (name == "script") {
                addResource(attribute(tag, "src"));
            }
            // Raw text element: skip to the matching end tag without parsing tags inside.
            const std::string close = "</" + name;
            std::size_t end = i;
            while (true) {
                end = html.find("</", end);
                if (end == std::string_view::npos || toLower(html.substr(end, close.size())) == close) break;
                end += 2;
            }
            const auto contentEnd = end == std::string_view::npos ? html.size() : end;
            if (name == "style") {
                addCss(html.substr(i, contentEnd - i));
            } else if (attribute(tag, "src").empty()) {
                for (const auto& url : extractScriptUrls(html.substr(i, contentEnd - i), base)) resources.add(url);
            }
            i = contentEnd;
        }
    }
    return links;
}

} // namespace warc_studio
