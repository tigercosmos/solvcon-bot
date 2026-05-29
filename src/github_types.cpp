#include "github_types.hpp"

#include <modmesh/serialization/SerializableItem.hpp>

#include <cctype>
#include <sstream>
#include <string>

namespace modmesh_bot
{

namespace detail = modmesh::detail;

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

} // namespace modmesh_bot
