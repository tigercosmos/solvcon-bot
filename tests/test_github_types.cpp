// Round-trip tests for src/github_types.* against representative GitHub
// REST response payloads.

#include "github_types.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

int g_failed = 0;
int g_passed = 0;

#define EXPECT(expr)                                                         \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #expr << "\n";                              \
            ++g_failed;                                                      \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b << "\n  got: " << _a     \
                      << "\n  want: " << _b << "\n";                         \
            ++g_failed;                                                      \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

using namespace modmesh_bot;

// --- User -----------------------------------------------------------------

void test_user_parse_and_round_trip()
{
    // GitHub user objects carry many fields we ignore; we only need login.
    const std::string sample = R"({"login":"tigercosmos","id":12345,"node_id":"MDQ6VXNlcjEyMzQ1","avatar_url":"https://example.invalid/a","type":"User","site_admin":false})";

    User u;
    u.from_json(sample);
    EXPECT_EQ(u.login, std::string("tigercosmos"));

    // Round-trip: emit, re-parse, fields preserved.
    User u2;
    u2.from_json(u.to_json());
    EXPECT_EQ(u2.login, u.login);
}

// --- Review ---------------------------------------------------------------

void test_review_approved()
{
    const std::string sample = R"({"id":99887766,"node_id":"PRR_x","user":{"login":"reviewer-1","id":7},"body":"lgtm","state":"APPROVED","submitted_at":"2026-01-15T10:20:30Z","commit_id":"abc"})";

    Review r;
    r.from_json(sample);
    EXPECT_EQ(r.id, static_cast<std::int64_t>(99887766));
    EXPECT_EQ(r.state, std::string("APPROVED"));
    EXPECT_EQ(r.submitted_at, std::string("2026-01-15T10:20:30Z"));
    EXPECT_EQ(r.user.login, std::string("reviewer-1"));

    Review r2;
    r2.from_json(r.to_json());
    EXPECT_EQ(r2.id, r.id);
    EXPECT_EQ(r2.state, r.state);
    EXPECT_EQ(r2.submitted_at, r.submitted_at);
    EXPECT_EQ(r2.user.login, r.user.login);
}

// --- PrSummary ------------------------------------------------------------

void test_pr_summary()
{
    // Real GitHub PR list shape: head is a nested object, sha lives inside it.
    const std::string sample = R"({"number":42,"head":{"sha":"deadbeefcafe","ref":"feat/x","label":"o:feat/x"},"updated_at":"2026-02-03T04:05:06Z","state":"open"})";

    PrSummary p;
    p.from_json(sample);
    EXPECT_EQ(p.number, 42);
    EXPECT_EQ(p.head.sha, std::string("deadbeefcafe"));
    EXPECT_EQ(p.updated_at, std::string("2026-02-03T04:05:06Z"));

    PrSummary p2;
    p2.from_json(p.to_json());
    EXPECT_EQ(p2.number, p.number);
    EXPECT_EQ(p2.head.sha, p.head.sha);
    EXPECT_EQ(p2.updated_at, p.updated_at);
}

// --- PrDetail -------------------------------------------------------------

void test_pr_detail_is_pr_true()
{
    // GET /issues/{n} when {n} is actually a PR. The pull_request object
    // is present (with several fields we don't care about).
    const std::string sample = R"({"number":7,"state":"open","title":"hi","pull_request":{"url":"https://api.github.com/repos/o/r/pulls/7","html_url":"https://example.invalid/r/pull/7"}})";

    PrDetail d;
    d.from_json(sample);
    EXPECT_EQ(d.number, 7);
    EXPECT_EQ(d.state, std::string("open"));
    EXPECT_EQ(d.is_pr, true);

    // Round-trip: our to_json emits a minimal pull_request:{} marker. Parsing
    // it back must still yield is_pr = true.
    PrDetail d2;
    d2.from_json(d.to_json());
    EXPECT_EQ(d2.number, d.number);
    EXPECT_EQ(d2.state, d.state);
    EXPECT_EQ(d2.is_pr, true);
}

void test_pr_detail_is_pr_false()
{
    // GET /issues/{n} for a real Issue (not a PR). No pull_request key.
    const std::string sample = R"({"number":11,"state":"open","title":"bug","body":"…"})";

    PrDetail d;
    d.from_json(sample);
    EXPECT_EQ(d.number, 11);
    EXPECT_EQ(d.state, std::string("open"));
    EXPECT_EQ(d.is_pr, false);

    PrDetail d2;
    d2.from_json(d.to_json());
    EXPECT_EQ(d2.is_pr, false);
}

void test_pr_detail_closed_pr()
{
    const std::string sample = R"({"number":99,"state":"closed","pull_request":{}})";
    PrDetail d;
    d.from_json(sample);
    EXPECT_EQ(d.state, std::string("closed"));
    EXPECT_EQ(d.is_pr, true);
}

// --- IssueComment ---------------------------------------------------------

void test_issue_comment()
{
    const std::string sample = R"({"id":1234567890,"body":"@modmesh-bot please review","user":{"login":"alice","id":1},"created_at":"2026-03-01T00:00:01Z","updated_at":"2026-03-01T00:00:05Z","issue_url":"https://api.github.com/repos/o/r/issues/55","html_url":"https://example.invalid/r/issues/55#issuecomment-1"})";

    IssueComment c;
    c.from_json(sample);
    EXPECT_EQ(c.id, static_cast<std::int64_t>(1234567890));
    EXPECT_EQ(c.body, std::string("@modmesh-bot please review"));
    EXPECT_EQ(c.user.login, std::string("alice"));
    EXPECT_EQ(c.created_at, std::string("2026-03-01T00:00:01Z"));
    EXPECT_EQ(c.updated_at, std::string("2026-03-01T00:00:05Z"));
    EXPECT_EQ(c.issue_url, std::string("https://api.github.com/repos/o/r/issues/55"));

    IssueComment c2;
    c2.from_json(c.to_json());
    EXPECT_EQ(c2.id, c.id);
    EXPECT_EQ(c2.body, c.body);
    EXPECT_EQ(c2.user.login, c.user.login);
    EXPECT_EQ(c2.updated_at, c.updated_at);
    EXPECT_EQ(c2.issue_url, c.issue_url);
}

// --- parse_issue_number_from_url -----------------------------------------

void test_parse_issue_number_from_url()
{
    EXPECT_EQ(parse_issue_number_from_url("https://api.github.com/repos/o/r/issues/55"), 55);
    EXPECT_EQ(parse_issue_number_from_url("https://api.github.com/repos/o/r/issues/55/"), 55);
    EXPECT_EQ(parse_issue_number_from_url("https://api.github.com/repos/o/r/issues/12345"), 12345);
    EXPECT_EQ(parse_issue_number_from_url("https://example.invalid/no-number-here"), -1);
    EXPECT_EQ(parse_issue_number_from_url(""), -1);
    // A path ending in alphanumerics that aren't a slash-prefixed integer.
    EXPECT_EQ(parse_issue_number_from_url("https://api.github.com/repos/o/r/issues42"), -1);
}

} // namespace

int main()
{
    test_user_parse_and_round_trip();
    test_review_approved();
    test_pr_summary();
    test_pr_detail_is_pr_true();
    test_pr_detail_is_pr_false();
    test_pr_detail_closed_pr();
    test_issue_comment();
    test_parse_issue_number_from_url();

    std::cerr << "github_types tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
