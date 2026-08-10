#pragma once

#include <solvcon/serialization/SerializableItem.hpp>

#include <cstdint>
#include <string>

namespace solvcon_bot
{

// The SC_DECL_SERIALIZABLE macro expands to unqualified detail::JsonNode
// etc., so we alias solvcon::detail into the enclosing namespace.
namespace detail = solvcon::detail;

struct User : solvcon::SerializableItem
{
    std::string login;

    SC_DECL_SERIALIZABLE(
        register_member("login", login);)
};

struct Review : solvcon::SerializableItem
{
    std::int64_t id = 0;
    std::string state;
    std::string submitted_at;
    User user;

    SC_DECL_SERIALIZABLE(
        register_member("id", id);
        register_member("state", state);
        register_member("submitted_at", submitted_at);
        register_member("user", user);)
};

struct PrHead : solvcon::SerializableItem
{
    std::string sha;

    SC_DECL_SERIALIZABLE(
        register_member("sha", sha);)
};

struct PrSummary : solvcon::SerializableItem
{
    int number = 0;
    PrHead head;
    std::string updated_at;

    SC_DECL_SERIALIZABLE(
        register_member("number", number);
        register_member("head", head);
        register_member("updated_at", updated_at);)
};

// GET /repos/{o}/{r}/issues/{n} — `pull_request` is present iff the issue
// is actually a PR. is_pr is derived from key presence; the macro can't
// express that, so PrDetail has hand-rolled to/from JSON.
struct PrDetail : solvcon::SerializableItem
{
    int number = 0;
    std::string state;
    bool is_pr = false;

    std::string to_json() const override;
    void from_json(const std::string & json) override;
};

struct IssueComment : solvcon::SerializableItem
{
    std::int64_t id = 0;
    std::string body;
    std::string created_at;
    std::string updated_at;
    std::string issue_url;
    User user;

    SC_DECL_SERIALIZABLE(
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

} // namespace solvcon_bot
