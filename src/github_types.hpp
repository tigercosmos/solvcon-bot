#pragma once

#include <modmesh/serialization/SerializableItem.hpp>

#include <cstdint>
#include <string>

namespace modmesh_bot
{

// The MM_DECL_SERIALIZABLE macro expands to unqualified detail::JsonNode
// etc., so we alias modmesh::detail into the enclosing namespace.
namespace detail = modmesh::detail;

struct User : modmesh::SerializableItem
{
    std::string login;

    MM_DECL_SERIALIZABLE(
        register_member("login", login);)
};

struct Review : modmesh::SerializableItem
{
    std::int64_t id = 0;
    std::string state;
    std::string submitted_at;
    User user;

    MM_DECL_SERIALIZABLE(
        register_member("id", id);
        register_member("state", state);
        register_member("submitted_at", submitted_at);
        register_member("user", user);)
};

struct PrHead : modmesh::SerializableItem
{
    std::string sha;

    MM_DECL_SERIALIZABLE(
        register_member("sha", sha);)
};

struct PrSummary : modmesh::SerializableItem
{
    int number = 0;
    PrHead head;
    std::string updated_at;

    MM_DECL_SERIALIZABLE(
        register_member("number", number);
        register_member("head", head);
        register_member("updated_at", updated_at);)
};

// GET /repos/{o}/{r}/issues/{n} — `pull_request` is present iff the issue
// is actually a PR. is_pr is derived from key presence; the macro can't
// express that, so PrDetail has hand-rolled to/from JSON.
struct PrDetail : modmesh::SerializableItem
{
    int number = 0;
    std::string state;
    bool is_pr = false;

    std::string to_json() const override;
    void from_json(const std::string & json) override;
};

struct IssueComment : modmesh::SerializableItem
{
    std::int64_t id = 0;
    std::string body;
    std::string created_at;
    std::string updated_at;
    std::string issue_url;
    User user;

    MM_DECL_SERIALIZABLE(
        register_member("id", id);
        register_member("body", body);
        register_member("created_at", created_at);
        register_member("updated_at", updated_at);
        register_member("issue_url", issue_url);
        register_member("user", user);)
};

// Extract the trailing integer from an issue_url such as
// "https://api.github.com/repos/foo/bar/issues/42". Returns -1 if no
// integer is found at the end.
int parse_issue_number_from_url(const std::string & issue_url);

} // namespace modmesh_bot
