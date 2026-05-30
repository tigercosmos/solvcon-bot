// In-process HTTP transport tests for src/github_client.*. A local
// httplib::Server replays canned responses on 127.0.0.1:<random port>
// and we point a GithubClient at it via the new GITHUB_API_BASE_URL
// configuration. These tests exercise the parts of GithubClient that
// the pure-function tests in test_github_client.cpp cannot reach:
// pagination, 5xx/429 retry behavior, POST non-retry, collaborator
// 204/404/403 dispatch, streaming diff truncation, 4xx surface, and
// default-header round-trip.

#include "config.hpp"
#include "github_client.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

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
        else { ++g_passed; }                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b                          \
                      << "\n  got: <" << _a << ">"                           \
                      << "\n  want: <" << _b << ">\n";                       \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

using modmesh_bot::Config;
using modmesh_bot::DiffResult;
using modmesh_bot::GithubClient;
using modmesh_bot::GithubError;

class TestServer
{
public:
    TestServer()
    {
        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0)
        {
            throw std::runtime_error("TestServer: bind_to_any_port failed");
        }
        thread_ = std::thread([this]() { svr_.listen_after_bind(); });
        while (!svr_.is_running())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    ~TestServer()
    {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    httplib::Server & svr() { return svr_; }
    std::string base_url() const
    {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    httplib::Server svr_;
    int port_ = 0;
    std::thread thread_;
};

Config make_cfg(const TestServer & ts)
{
    Config c;
    c.github_token = "test-token";
    c.github_owner = "o";
    c.github_repo = "r";
    c.bot_handle = "bot";
    // Reviewer is unused in transport tests (we don't dispatch a review).
    c.github_api_base_url = ts.base_url();
    c.http_connect_timeout_sec = 5;
    c.http_read_timeout_sec = 5;
    c.http_write_timeout_sec = 5;
    c.max_diff_bytes = 1024;
    return c;
}

// --- list_open_prs --------------------------------------------------------

void test_list_open_prs_single_page()
{
    TestServer ts;
    ts.svr().Get("/repos/o/r/pulls",
        [](const httplib::Request &, httplib::Response & res) {
            res.status = 200;
            res.set_content(
                R"([{"number":42,"head":{"sha":"deadbeef"},"updated_at":"2026-05-01T10:00:00Z"}])",
                "application/json");
        });
    GithubClient gh(make_cfg(ts));
    auto prs = gh.list_open_prs();
    EXPECT_EQ(prs.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(prs[0].number, 42);
    EXPECT_EQ(prs[0].head.sha, std::string("deadbeef"));
}

void test_list_open_prs_empty()
{
    TestServer ts;
    ts.svr().Get("/repos/o/r/pulls",
        [](const httplib::Request &, httplib::Response & res) {
            res.status = 200;
            res.set_content("[]", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    auto prs = gh.list_open_prs();
    EXPECT(prs.empty());
}

void test_list_open_prs_paginates_link_next()
{
    TestServer ts;
    std::atomic<int> call_count{0};
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            ++call_count;
            const std::string page = req.has_param("page")
                ? req.get_param_value("page") : "1";
            if (page == "1")
            {
                res.status = 200;
                res.set_header("Link",
                    std::string("<") + ts.base_url()
                    + "/repos/o/r/pulls?state=open&per_page=100&page=2>; rel=\"next\"");
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else
            {
                res.status = 200;
                res.set_content(
                    R"([{"number":2,"head":{"sha":"b"},"updated_at":"2026-05-01T11:00:00Z"}])",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto prs = gh.list_open_prs();
    EXPECT_EQ(prs.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(prs[0].number, 1);
    EXPECT_EQ(prs[1].number, 2);
    EXPECT_EQ(call_count.load(), 2);
}

// --- default headers round-trip -------------------------------------------

void test_default_headers_are_attached()
{
    TestServer ts;
    std::string captured_auth, captured_ua, captured_accept, captured_api_ver;
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            captured_auth   = req.get_header_value("Authorization");
            captured_ua     = req.get_header_value("User-Agent");
            captured_accept = req.get_header_value("Accept");
            captured_api_ver= req.get_header_value("X-GitHub-Api-Version");
            res.status = 200;
            res.set_content("[]", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.list_open_prs();
    EXPECT_EQ(captured_auth, std::string("Bearer test-token"));
    EXPECT(captured_ua.find("modmesh-bot/") == 0);
    EXPECT_EQ(captured_accept, std::string("application/vnd.github+json"));
    EXPECT_EQ(captured_api_ver, std::string("2022-11-28"));
}

// --- is_collaborator ------------------------------------------------------

void test_is_collaborator_204_true()
{
    TestServer ts;
    ts.svr().Get(R"(/repos/o/r/collaborators/(.+))",
        [](const httplib::Request &, httplib::Response & res) {
            res.status = 204;
        });
    GithubClient gh(make_cfg(ts));
    EXPECT(gh.is_collaborator("alice"));
}

void test_is_collaborator_404_false()
{
    TestServer ts;
    ts.svr().Get(R"(/repos/o/r/collaborators/(.+))",
        [](const httplib::Request &, httplib::Response & res) {
            res.status = 404;
            res.set_content(R"({"message":"Not Found"})", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    EXPECT(!gh.is_collaborator("alice"));
}

void test_is_collaborator_403_throws()
{
    TestServer ts;
    ts.svr().Get(R"(/repos/o/r/collaborators/(.+))",
        [](const httplib::Request &, httplib::Response & res) {
            res.status = 403;
            res.set_content(R"({"message":"Forbidden"})", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    bool threw = false;
    int status = 0;
    try { (void)gh.is_collaborator("alice"); }
    catch (const GithubError & e) { threw = true; status = e.status(); }
    EXPECT(threw);
    EXPECT_EQ(status, 403);
}

// --- retry policy ---------------------------------------------------------

void test_get_retries_on_503_then_succeeds()
{
    TestServer ts;
    std::atomic<int> count{0};
    ts.svr().Get(R"(/repos/o/r/issues/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            const int n = ++count;
            if (n == 1)
            {
                res.status = 503;
                res.set_content("temporarily unavailable", "text/plain");
            }
            else
            {
                res.status = 200;
                res.set_content(
                    R"({"number":9,"state":"open","pull_request":{}})",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto d = gh.get_issue_detail(9);
    EXPECT_EQ(d.number, 9);
    EXPECT_EQ(d.is_pr, true);
    EXPECT_EQ(count.load(), 2);
}

void test_get_429_with_retry_after_then_succeeds()
{
    TestServer ts;
    std::atomic<int> count{0};
    ts.svr().Get(R"(/repos/o/r/issues/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            const int n = ++count;
            if (n == 1)
            {
                res.status = 429;
                res.set_header("Retry-After", "1");
                res.set_content("slow down", "text/plain");
            }
            else
            {
                res.status = 200;
                res.set_content(
                    R"({"number":9,"state":"open"})",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto d = gh.get_issue_detail(9);
    EXPECT_EQ(d.number, 9);
    EXPECT_EQ(d.is_pr, false);
    EXPECT_EQ(count.load(), 2);
}

void test_get_403_with_rate_limit_remaining_retries()
{
    // GitHub returns 403 (not 429) for primary rate limit. The client
    // should treat that 403 as transient when X-RateLimit-Remaining=0
    // is set.
    TestServer ts;
    std::atomic<int> count{0};
    ts.svr().Get(R"(/repos/o/r/issues/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            const int n = ++count;
            if (n == 1)
            {
                res.status = 403;
                res.set_header("X-RateLimit-Remaining", "0");
                res.set_header("Retry-After", "1");
                res.set_content("rate limited", "text/plain");
            }
            else
            {
                res.status = 200;
                res.set_content(
                    R"({"number":9,"state":"open","pull_request":{}})",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto d = gh.get_issue_detail(9);
    EXPECT_EQ(d.number, 9);
    EXPECT_EQ(count.load(), 2);
}

void test_get_non_retryable_4xx_throws()
{
    TestServer ts;
    std::atomic<int> count{0};
    ts.svr().Get(R"(/repos/o/r/issues/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            ++count;
            res.status = 422;
            res.set_content(R"({"message":"Unprocessable Entity"})",
                            "application/json");
        });
    GithubClient gh(make_cfg(ts));
    bool threw = false;
    int status = 0;
    try { (void)gh.get_issue_detail(9); }
    catch (const GithubError & e) { threw = true; status = e.status(); }
    EXPECT(threw);
    EXPECT_EQ(status, 422);
    EXPECT_EQ(count.load(), 1); // not retried
}

// --- POST (post_comment) is NOT retried ----------------------------------

void test_post_is_not_retried_on_5xx()
{
    TestServer ts;
    std::atomic<int> count{0};
    ts.svr().Post(R"(/repos/o/r/issues/(\d+)/comments)",
        [&](const httplib::Request &, httplib::Response & res) {
            ++count;
            res.status = 502;
            res.set_content("bad gateway", "text/plain");
        });
    GithubClient gh(make_cfg(ts));
    bool threw = false;
    int status = 0;
    try { gh.post_comment(9, "hello"); }
    catch (const GithubError & e) { threw = true; status = e.status(); }
    EXPECT(threw);
    EXPECT_EQ(status, 502);
    // Critical: post called exactly once. Idempotency at the watcher
    // layer (marker dedupe) handles re-posts; mid-retry duplicate
    // comments would be much worse.
    EXPECT_EQ(count.load(), 1);
}

void test_post_success_writes_body()
{
    TestServer ts;
    std::string captured_body, captured_ctype;
    ts.svr().Post(R"(/repos/o/r/issues/(\d+)/comments)",
        [&](const httplib::Request & req, httplib::Response & res) {
            captured_body = req.body;
            captured_ctype = req.get_header_value("Content-Type");
            res.status = 201;
            res.set_content(R"({"id":777})", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    gh.post_comment(9, "hello \"world\"\n");
    // Body is our hand-built JSON: {"body":"hello \"world\"\n"}.
    // Backslash + n is two literal bytes; quotes are escaped; newline
    // becomes \n.
    EXPECT(captured_body.find("\"body\":") != std::string::npos);
    EXPECT(captured_body.find("hello \\\"world\\\"\\n") != std::string::npos);
    EXPECT(captured_ctype.find("application/json") == 0);
}

// --- stream_diff truncation ----------------------------------------------

void test_stream_diff_truncates_at_cap()
{
    TestServer ts;
    // 4 KB of "x" (cap is 1024 per make_cfg).
    const std::string big(4096, 'x');
    ts.svr().Get(R"(/repos/o/r/pulls/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            res.status = 200;
            res.set_content(big, "application/vnd.github.diff");
        });
    GithubClient gh(make_cfg(ts));
    auto d = gh.stream_diff(9);
    EXPECT(d.truncated);
    EXPECT(d.body.size() <= 1024);
    EXPECT(d.body == std::string(d.body.size(), 'x'));
}

void test_stream_diff_below_cap_not_truncated()
{
    TestServer ts;
    const std::string small(200, 'y');
    ts.svr().Get(R"(/repos/o/r/pulls/(\d+))",
        [&](const httplib::Request &, httplib::Response & res) {
            res.status = 200;
            res.set_content(small, "application/vnd.github.diff");
        });
    GithubClient gh(make_cfg(ts));
    auto d = gh.stream_diff(9);
    EXPECT(!d.truncated);
    EXPECT_EQ(d.body.size(), small.size());
    EXPECT_EQ(d.body, small);
}

// --- diff accept header verification -------------------------------------

void test_stream_diff_sends_diff_accept_header()
{
    TestServer ts;
    std::string captured_accept;
    ts.svr().Get(R"(/repos/o/r/pulls/(\d+))",
        [&](const httplib::Request & req, httplib::Response & res) {
            captured_accept = req.get_header_value("Accept");
            res.status = 200;
            res.set_content("diff body", "application/vnd.github.diff");
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.stream_diff(9);
    EXPECT_EQ(captured_accept, std::string("application/vnd.github.diff"));
}

} // namespace

int main()
{
    test_list_open_prs_single_page();
    test_list_open_prs_empty();
    test_list_open_prs_paginates_link_next();
    test_default_headers_are_attached();
    test_is_collaborator_204_true();
    test_is_collaborator_404_false();
    test_is_collaborator_403_throws();
    test_get_retries_on_503_then_succeeds();
    test_get_429_with_retry_after_then_succeeds();
    test_get_403_with_rate_limit_remaining_retries();
    test_get_non_retryable_4xx_throws();
    test_post_is_not_retried_on_5xx();
    test_post_success_writes_body();
    test_stream_diff_truncates_at_cap();
    test_stream_diff_below_cap_not_truncated();
    test_stream_diff_sends_diff_accept_header();

    std::cerr << "github_transport tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
