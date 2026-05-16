#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace warc_studio {

std::string htmlEscape(std::string_view value);
std::string urlEncode(std::string_view value);
std::string urlDecode(std::string_view value);
std::unordered_map<std::string, std::string> parseUrlEncoded(std::string_view body);
std::string shellQuote(std::string_view value);
bool isTruthyEnvironmentValue(const char* value);

} // namespace warc_studio
