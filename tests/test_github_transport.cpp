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

// --- conditional caching (If-None-Match / ETag) -------------------------

void test_conditional_etag_round_trip_304()
{
    // First call: 200 + ETag stored. Second call: send If-None-Match,
    // server returns 304, client returns the previously parsed items
    // without another network fetch into the body.
    TestServer ts;
    std::atomic<int> calls{0};
    std::string first_inm; // captured If-None-Match on second call
    std::string second_inm;
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            const int n = ++calls;
            if (n == 1)
            {
                first_inm = req.get_header_value("If-None-Match");
                res.set_header("ETag", "\"pulls-v1\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":7,"head":{"sha":"abc"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else
            {
                second_inm = req.get_header_value("If-None-Match");
                res.status = 304;
                // 304 carries no body.
            }
        });

    GithubClient gh(make_cfg(ts));
    auto first = gh.list_open_prs();
    EXPECT_EQ(first.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(first[0].number, 7);
    EXPECT(first_inm.empty()); // nothing to send on the first call

    auto second = gh.list_open_prs();
    // Same items from the cache.
    EXPECT_EQ(second.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(second[0].number, 7);
    EXPECT_EQ(second[0].head.sha, std::string("abc"));
    EXPECT_EQ(second_inm, std::string("\"pulls-v1\""));
    EXPECT_EQ(calls.load(), 2); // both calls hit server (one returned 304)
}

void test_conditional_200_invalidates_cache_with_new_body()
{
    // First call 200 with body1+etag1; second call returns 200 with
    // body2+etag2; third call sends If-None-Match: etag2 and gets 304.
    TestServer ts;
    std::atomic<int> calls{0};
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            const int n = ++calls;
            if (n == 1)
            {
                res.set_header("ETag", "\"v1\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else if (n == 2)
            {
                // Bot sends If-None-Match: "v1" but the resource changed.
                EXPECT_EQ(req.get_header_value("If-None-Match"),
                          std::string("\"v1\""));
                res.set_header("ETag", "\"v2\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"},
                        {"number":2,"head":{"sha":"b"},"updated_at":"2026-05-01T11:00:00Z"}])",
                    "application/json");
            }
            else
            {
                EXPECT_EQ(req.get_header_value("If-None-Match"),
                          std::string("\"v2\""));
                res.status = 304;
            }
        });

    GithubClient gh(make_cfg(ts));
    auto r1 = gh.list_open_prs();
    EXPECT_EQ(r1.size(), static_cast<std::size_t>(1));
    auto r2 = gh.list_open_prs();
    EXPECT_EQ(r2.size(), static_cast<std::size_t>(2));
    auto r3 = gh.list_open_prs();
    EXPECT_EQ(r3.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(r3[1].number, 2);
    EXPECT_EQ(calls.load(), 3);
}

void test_conditional_no_etag_response_does_not_cache()
{
    // Server returns 200 but no ETag header. We must NOT send
    // If-None-Match on the next call (we have no etag), and we
    // re-fetch fresh data.
    TestServer ts;
    std::atomic<int> calls{0};
    std::atomic<int> with_inm{0};
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            ++calls;
            if (!req.get_header_value("If-None-Match").empty()) ++with_inm;
            res.status = 200;
            res.set_content(
                R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                "application/json");
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.list_open_prs();
    (void)gh.list_open_prs();
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(with_inm.load(), 0);
}

void test_conditional_per_pr_reviews_cache_separately()
{
    // /pulls/1/reviews and /pulls/2/reviews must use independent cache
    // entries — a 304 on PR 1 must not return PR 2's items, and vice
    // versa. We hit each PR twice; the second call should send
    // If-None-Match scoped to that PR's etag and receive 304, then
    // return PR-specific cached items.
    TestServer ts;
    std::atomic<int> pr1_calls{0};
    std::atomic<int> pr2_calls{0};
    std::string pr1_inm_second;
    std::string pr2_inm_second;
    ts.svr().Get(R"(/repos/o/r/pulls/(\d+)/reviews)",
        [&](const httplib::Request & req, httplib::Response & res) {
            const std::string n = req.matches[1];
            const std::string inm = req.get_header_value("If-None-Match");
            if (n == "1")
            {
                ++pr1_calls;
                if (!inm.empty())
                {
                    pr1_inm_second = inm;
                    res.status = 304;
                    return;
                }
                res.set_header("ETag", "\"reviews-1\"");
                res.status = 200;
                res.set_content(
                    R"([{"id":11,"state":"APPROVED","submitted_at":"2026-05-01T10:00:00Z","user":{"login":"a"}}])",
                    "application/json");
            }
            else
            {
                ++pr2_calls;
                if (!inm.empty())
                {
                    pr2_inm_second = inm;
                    res.status = 304;
                    return;
                }
                res.set_header("ETag", "\"reviews-2\"");
                res.status = 200;
                res.set_content(
                    R"([{"id":22,"state":"COMMENTED","submitted_at":"2026-05-01T10:00:00Z","user":{"login":"b"}}])",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto r1a = gh.list_reviews(1);
    auto r2a = gh.list_reviews(2);
    EXPECT_EQ(r1a.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(r1a[0].id, static_cast<std::int64_t>(11));
    EXPECT_EQ(r2a.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(r2a[0].id, static_cast<std::int64_t>(22));

    auto r1b = gh.list_reviews(1);
    auto r2b = gh.list_reviews(2);
    EXPECT_EQ(r1b.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(r1b[0].id, static_cast<std::int64_t>(11));
    EXPECT_EQ(r2b.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(r2b[0].id, static_cast<std::int64_t>(22));

    EXPECT_EQ(pr1_inm_second, std::string("\"reviews-1\""));
    EXPECT_EQ(pr2_inm_second, std::string("\"reviews-2\""));
    EXPECT_EQ(pr1_calls.load(), 2);
    EXPECT_EQ(pr2_calls.load(), 2);
}

void test_conditional_multipage_response_bypasses_cache()
{
    // A multi-page response is NOT cached, because GitHub's pagination
    // ETags validate one page at a time — a new item appended to a
    // later page would not flip page 1's ETag and a cached snapshot
    // would silently go stale. So the second call must re-paginate
    // fully (no If-None-Match) and fetch page 2 again.
    TestServer ts;
    std::atomic<int> calls_p1{0};
    std::atomic<int> calls_p2{0};
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            const std::string page = req.has_param("page")
                ? req.get_param_value("page") : "1";
            if (page == "1")
            {
                ++calls_p1;
                // We must NEVER receive If-None-Match for this multi-page
                // resource — caching is explicitly disabled.
                EXPECT(req.get_header_value("If-None-Match").empty());
                res.set_header("ETag", "\"page1\"");
                res.set_header("Link",
                    std::string("<") + ts.base_url()
                    + "/repos/o/r/pulls?state=open&sort=updated&direction=desc&per_page=100&page=2>; rel=\"next\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else
            {
                ++calls_p2;
                res.status = 200;
                res.set_content(
                    R"([{"number":2,"head":{"sha":"b"},"updated_at":"2026-05-01T11:00:00Z"}])",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    auto first = gh.list_open_prs();
    EXPECT_EQ(first.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(calls_p1.load(), 1);
    EXPECT_EQ(calls_p2.load(), 1);

    auto second = gh.list_open_prs();
    EXPECT_EQ(second.size(), static_cast<std::size_t>(2));
    // Both pages re-fetched fresh; no cache reuse.
    EXPECT_EQ(calls_p1.load(), 2);
    EXPECT_EQ(calls_p2.load(), 2);
}

void test_conditional_existing_single_page_cache_dropped_when_grown_to_multipage()
{
    // First call: single-page response, cached. Second call: same
    // endpoint now multi-page; the old cache entry must be discarded
    // so a third call doesn't send a stale If-None-Match.
    TestServer ts;
    std::atomic<int> calls{0};
    std::vector<std::string> seen_inm;
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            const int n = ++calls;
            const std::string page = req.has_param("page")
                ? req.get_param_value("page") : "1";
            seen_inm.push_back(req.get_header_value("If-None-Match"));

            if (page != "1")
            {
                res.status = 200;
                res.set_content(
                    R"([{"number":2,"head":{"sha":"b"},"updated_at":"2026-05-01T11:00:00Z"}])",
                    "application/json");
                return;
            }
            if (n == 1)
            {
                // Single page initially.
                res.set_header("ETag", "\"v1\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else if (n == 2)
            {
                // Now multi-page. Bot is sending If-None-Match: "v1" —
                // we ignore it and serve 200 with a Link to page 2.
                res.set_header("ETag", "\"v2\"");
                res.set_header("Link",
                    std::string("<") + ts.base_url()
                    + "/repos/o/r/pulls?state=open&sort=updated&direction=desc&per_page=100&page=2>; rel=\"next\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
            else
            {
                // Third request (after grown-to-multipage drop): must
                // NOT carry If-None-Match.
                EXPECT(req.get_header_value("If-None-Match").empty());
                res.set_header("ETag", "\"v3\"");
                res.set_header("Link",
                    std::string("<") + ts.base_url()
                    + "/repos/o/r/pulls?state=open&sort=updated&direction=desc&per_page=100&page=2>; rel=\"next\"");
                res.status = 200;
                res.set_content(
                    R"([{"number":1,"head":{"sha":"a"},"updated_at":"2026-05-01T10:00:00Z"}])",
                    "application/json");
            }
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.list_open_prs(); // n=1, caches "v1"
    (void)gh.list_open_prs(); // n=2, served 200 multi-page; cache MUST drop "v1"
    (void)gh.list_open_prs(); // n=3, must NOT send If-None-Match
}

void test_conditional_request_carries_default_headers()
{
    // The conditional request must still ship Authorization +
    // X-GitHub-Api-Version etc.; the new If-None-Match is additive.
    TestServer ts;
    std::string captured_auth;
    std::string captured_ver;
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            captured_auth = req.get_header_value("Authorization");
            captured_ver = req.get_header_value("X-GitHub-Api-Version");
            res.set_header("ETag", "\"x\"");
            res.status = 200;
            res.set_content("[]", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.list_open_prs();
    (void)gh.list_open_prs(); // second call would carry If-None-Match too
    EXPECT_EQ(captured_auth, std::string("Bearer test-token"));
    EXPECT_EQ(captured_ver, std::string("2022-11-28"));
}

void test_list_open_prs_url_has_sort_updated_desc()
{
    // The sort hint we baked into list_open_prs must be on the wire.
    TestServer ts;
    std::string captured_query;
    ts.svr().Get("/repos/o/r/pulls",
        [&](const httplib::Request & req, httplib::Response & res) {
            captured_query.clear();
            for (const auto & [k, v] : req.params)
            {
                if (!captured_query.empty()) captured_query += '&';
                captured_query += k + "=" + v;
            }
            res.status = 200;
            res.set_content("[]", "application/json");
        });
    GithubClient gh(make_cfg(ts));
    (void)gh.list_open_prs();
    EXPECT(captured_query.find("sort=updated") != std::string::npos);
    EXPECT(captured_query.find("direction=desc") != std::string::npos);
    EXPECT(captured_query.find("state=open") != std::string::npos);
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

    test_conditional_etag_round_trip_304();
    test_conditional_200_invalidates_cache_with_new_body();
    test_conditional_no_etag_response_does_not_cache();
    test_conditional_per_pr_reviews_cache_separately();
    test_conditional_multipage_response_bypasses_cache();
    test_conditional_existing_single_page_cache_dropped_when_grown_to_multipage();
    test_conditional_request_carries_default_headers();
    test_list_open_prs_url_has_sort_updated_desc();

    std::cerr << "github_transport tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
