#pragma once

#include <string>
#include <string_view>

namespace modmesh_bot
{

// True iff `body` contains a `@<handle>` mention, case-insensitive, at a
// word boundary: the character immediately before `@` must not be in
// [A-Za-z0-9-], and the character immediately after the handle must not
// be in [A-Za-z0-9-]. The handle is a GitHub username (1-39 chars,
// [A-Za-z0-9-]); behavior is unspecified if it contains other chars.
//
// Plan reference: §9 regex (?i)(?<![A-Za-z0-9-])@<handle>(?![A-Za-z0-9-])
bool mention_matches(std::string_view body, std::string_view handle);

// Case-insensitive login equality (GitHub logins are case-insensitive,
// ASCII).
bool eq_login(std::string_view a, std::string_view b);

} // namespace modmesh_bot
