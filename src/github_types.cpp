#include "github_types.hpp"

#include <solvcon/serialization/SerializableItem.hpp>

#include <cctype>
#include <sstream>
#include <string>

namespace solvcon_bot
{

namespace detail = solvcon::detail;

std::string PrDetail::to_json() const
{
    std::ostringstream oss;
    oss << "{\"number\":" << number
        << ",\"state\":\"" << detail::escape_string(state) << "\"";
    if (is_pr)
    {
        // Empty object is the smallest payload that round-trips back to
        // is_pr = true via from_json; we don't care about the inner fields.
        oss << ",\"pull_request\":{}";
    }
    oss << "}";
    return oss.str();
}

void PrDetail::from_json(const std::string & json)
{
    auto node = std::make_unique<detail::JsonNode>(detail::JsonType::Object, json);
    const auto & obj = std::get<detail::JsonMap>(node->value);

    number = 0;
    state.clear();
    is_pr = false;

    if (auto it = obj.find("number"); it != obj.end())
    {
        detail::JsonHelper::from_json_string(it->second, number);
    }
    if (auto it = obj.find("state"); it != obj.end())
    {
        detail::JsonHelper::from_json_string(it->second, state);
    }
    // The marker for "this is a PR": presence of the pull_request key, even
    // if the value itself is null or {}.
    is_pr = obj.find("pull_request") != obj.end();
}

namespace
{

// Hex quad -> code unit. Returns -1 on anything that is not exactly
// four hex digits.
int parse_hex4(const std::string & s, std::size_t pos)
{
    if (pos + 4 > s.size()) return -1;
    int v = 0;
    for (std::size_t i = pos; i < pos + 4; ++i)
    {
        const char c = s[i];
        int d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

void append_utf8(std::string & out, unsigned int cp)
{
    if (cp <= 0x7F)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The parse tree keeps String nodes as their raw quoted source text, so
// every consumer that wants the actual characters must strip the quotes
// and undo the escapes. Returns "" for absent, null, or non-string
// values — all three mean "no text" for the fields PrInfo carries.
std::string string_field(const solvcon_bot::detail::JsonMap & obj,
                         const char * key)
{
    auto it = obj.find(key);
    if (it == obj.end()
        || it->second->type != solvcon_bot::detail::JsonType::String)
    {
        return "";
    }
    const std::string & quoted = std::get<std::string>(it->second->value);
    if (quoted.size() < 2) return "";
    return solvcon_bot::json_unescape(quoted.substr(1, quoted.size() - 2));
}

} // namespace

std::string json_unescape(const std::string & raw)
{
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size())
    {
        const char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size())
        {
            out.push_back(c);
            ++i;
            continue;
        }
        const char esc = raw[i + 1];
        switch (esc)
        {
        case '"': out.push_back('"'); i += 2; continue;
        case '\\': out.push_back('\\'); i += 2; continue;
        case '/': out.push_back('/'); i += 2; continue;
        case 'b': out.push_back('\b'); i += 2; continue;
        case 'f': out.push_back('\f'); i += 2; continue;
        case 'n': out.push_back('\n'); i += 2; continue;
        case 'r': out.push_back('\r'); i += 2; continue;
        case 't': out.push_back('\t'); i += 2; continue;
        case 'u':
        {
            const int hi = parse_hex4(raw, i + 2);
            if (hi < 0) break; // malformed: keep the backslash verbatim
            if (hi >= 0xD800 && hi <= 0xDBFF)
            {
                // High surrogate: only meaningful with a low surrogate
                // right behind it.
                if (i + 12 <= raw.size() && raw[i + 6] == '\\'
                    && raw[i + 7] == 'u')
                {
                    const int lo = parse_hex4(raw, i + 8);
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        const unsigned int cp = 0x10000
                            + ((static_cast<unsigned int>(hi) - 0xD800) << 10)
                            + (static_cast<unsigned int>(lo) - 0xDC00);
                        append_utf8(out, cp);
                        i += 12;
                        continue;
                    }
                }
                // Unpaired surrogate: emit U+FFFD instead of invalid UTF-8.
                append_utf8(out, 0xFFFD);
                i += 6;
                continue;
            }
            if (hi >= 0xDC00 && hi <= 0xDFFF)
            {
                append_utf8(out, 0xFFFD); // stray low surrogate
                i += 6;
                continue;
            }
            append_utf8(out, static_cast<unsigned int>(hi));
            i += 6;
            continue;
        }
        default: break;
        }
        // Unknown or malformed escape: keep the backslash verbatim.
        out.push_back('\\');
        ++i;
    }
    return out;
}

std::string PrInfo::to_json() const
{
    std::ostringstream oss;
    oss << "{\"number\":" << number
        << ",\"state\":\"" << detail::escape_string(state) << "\""
        << ",\"title\":\"" << detail::escape_string(title) << "\""
        << ",\"body\":\"" << detail::escape_string(body) << "\""
        << ",\"head\":{\"sha\":\"" << detail::escape_string(head_sha) << "\"}}";
    return oss.str();
}

void PrInfo::from_json(const std::string & json)
{
    auto node = std::make_unique<detail::JsonNode>(detail::JsonType::Object, json);
    const auto & obj = std::get<detail::JsonMap>(node->value);

    number = 0;
    state.clear();
    title.clear();
    body.clear();
    head_sha.clear();

    if (auto it = obj.find("number"); it != obj.end())
    {
        detail::JsonHelper::from_json_string(it->second, number);
    }
    state = string_field(obj, "state");
    title = string_field(obj, "title");
    body = string_field(obj, "body"); // null body (no PR description) -> ""
    if (auto it = obj.find("head");
        it != obj.end() && it->second->type == detail::JsonType::Object)
    {
        head_sha = string_field(std::get<detail::JsonMap>(it->second->value),
                                "sha");
    }
}

int parse_issue_number_from_url(const std::string & issue_url)
{
    // Walk back from the end of the URL, skipping any trailing slash, then
    // collecting trailing digits. Returns -1 if no digits are present.
    if (issue_url.empty()) return -1;
    std::size_t end = issue_url.size();
    while (end > 0 && issue_url[end - 1] == '/') --end;
    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(issue_url[start - 1])))
    {
        --start;
    }
    if (start == end) return -1;
    if (start > 0 && issue_url[start - 1] != '/') return -1;
    try
    {
        return std::stoi(issue_url.substr(start, end - start));
    }
    catch (...)
    {
        return -1;
    }
}

} // namespace solvcon_bot
