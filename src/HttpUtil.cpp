#include "warc_studio/HttpUtil.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace warc_studio {

std::string htmlEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (char ch : value) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += ch; break;
        }
    }

    return out;
}

std::string urlEncode(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;

    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return out.str();
}

std::string urlDecode(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '+') {
            out.push_back(' ');
            continue;
        }

        if (ch == '%') {
            if (i + 2 >= value.size() || !std::isxdigit(static_cast<unsigned char>(value[i + 1]))
                || !std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
                out.push_back('%');
                continue;
            }
            const auto digit = [](char c) { return c <= '9' ? c - '0' : (c & ~0x20) - 'A' + 10; };
            out.push_back(static_cast<char>(digit(value[i + 1]) * 16 + digit(value[i + 2])));
            i += 2;
            continue;
        }

        out.push_back(ch);
    }

    return out;
}

std::unordered_map<std::string, std::string> parseUrlEncoded(std::string_view body) {
    std::unordered_map<std::string, std::string> result;
    std::size_t start = 0;

    while (start <= body.size()) {
        const std::size_t amp = body.find('&', start);
        const std::size_t end = amp == std::string_view::npos ? body.size() : amp;
        const std::string_view pair = body.substr(start, end - start);

        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                result[urlDecode(pair)] = "";
            } else {
                result[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }

        if (amp == std::string_view::npos) {
            break;
        }
        start = amp + 1;
    }

    return result;
}

std::string shellQuote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

bool isTruthyEnvironmentValue(const char* value) {
    if (value == nullptr) {
        return false;
    }

    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "on" || text == "ON";
}

} // namespace warc_studio
