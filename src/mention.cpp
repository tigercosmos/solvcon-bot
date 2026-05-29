#include "mention.hpp"

#include <cctype>
#include <string_view>

namespace modmesh_bot
{

namespace
{

inline char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool is_handle_char(char c)
{
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-';
}

bool case_insensitive_starts_with(std::string_view haystack, std::size_t pos,
                                  std::string_view needle)
{
    if (pos + needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i)
    {
        if (ascii_lower(haystack[pos + i]) != ascii_lower(needle[i]))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool mention_matches(std::string_view body, std::string_view handle)
{
    if (handle.empty()) return false;
    // GitHub usernames are 1-39 chars, but this is also a sanity guard
    // against pathological inputs.
    if (handle.size() > 39) return false;

    std::size_t i = 0;
    while (i < body.size())
    {
        std::size_t at = body.find('@', i);
        if (at == std::string_view::npos) return false;

        // Word boundary on the LEFT: no [A-Za-z0-9-] char immediately
        // before the '@'.
        const bool left_ok = (at == 0) || !is_handle_char(body[at - 1]);

        // Body must contain the handle right after '@'.
        if (left_ok && case_insensitive_starts_with(body, at + 1, handle))
        {
            // Word boundary on the RIGHT: the character following the
            // last handle char must not be in [A-Za-z0-9-].
            const std::size_t after = at + 1 + handle.size();
            const bool right_ok = (after >= body.size())
                                  || !is_handle_char(body[after]);
            if (right_ok) return true;
        }

        i = at + 1;
    }
    return false;
}

bool eq_login(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

} // namespace modmesh_bot
