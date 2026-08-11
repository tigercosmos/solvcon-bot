// Tests for src/watcher.* — marker builder/key, plus a fake-driven
// end-to-end run_auto_path control flow exercise.

#include "config.hpp"
#include "github_client.hpp"
#include "github_types.hpp"
#include "reviewer.hpp"
#include "watcher.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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

using solvcon_bot::body_has_marker_key;
using solvcon_bot::build_marker;
using solvcon_bot::build_marker_key;
using solvcon_bot::Config;
using solvcon_bot::DiffResult;
using solvcon_bot::IssueComment;
using solvcon_bot::PrSummary;
using solvcon_bot::Review;
using solvcon_bot::User;
using solvcon_bot::Watcher;
using solvcon_bot::WatcherIo;

// --- Marker contract ------------------------------------------------------

void test_marker_exact_string_auto()
{
    const std::string m = build_marker("auto", 42, std::nullopt);
    EXPECT_EQ(m, std::string("<!-- solvcon-bot/") + SOLVCON_BOT_VERSION
                 + " source=auto pr=42 trigger=first-approval -->");
}

void test_marker_exact_string_ping()
{
    const std::string m = build_marker("ping", 7, 12345678LL);
    EXPECT_EQ(m, std::string("<!-- solvcon-bot/") + SOLVCON_BOT_VERSION
                 + " source=ping pr=7 trigger=12345678 -->");
}

void test_marker_key_excludes_version()
{
    const std::string k = build_marker_key("auto", 42, std::nullopt);
    EXPECT_EQ(k, std::string("source=auto pr=42 trigger=first-approval -->"));
    EXPECT(k.find("solvcon-bot/") == std::string::npos);
}

void test_marker_key_present_in_marker()
{
    const std::string m = build_marker("auto", 42, std::nullopt);
    const std::string k = build_marker_key("auto", 42, std::nullopt);
    EXPECT(m.find(k) != std::string::npos);
}

void test_marker_key_survives_version_bump()
{
    // A comment was posted by an old version; the key still matches.
    const std::string old_marker =
        "<!-- solvcon-bot/0.0.0-old source=auto pr=42 trigger=first-approval -->";
    const std::string body = old_marker + "\n\nold review\n";
    const std::string k = build_marker_key("auto", 42, std::nullopt);
    EXPECT(body_has_marker_key(body, k));
}

void test_marker_key_distinct_per_pr_and_trigger()
{
    EXPECT(build_marker_key("auto", 1, std::nullopt)
           != build_marker_key("auto", 2, std::nullopt));
    EXPECT(build_marker_key("ping", 7, 100LL)
           != build_marker_key("ping", 7, 101LL));
    EXPECT(build_marker_key("auto", 7, std::nullopt)
           != build_marker_key("ping", 7, std::nullopt));
}

void test_body_has_marker_key_unrelated_html_does_not_match()
{
    const std::string k = build_marker_key("auto", 42, std::nullopt);
    EXPECT(!body_has_marker_key("<!-- some unrelated comment -->", k));
}

// --- Fake WatcherIo for control-flow tests --------------------------------

struct FakeWatcherIo : solvcon_bot::WatcherIo
{
    std::vector<PrSummary> prs;
    std::map<int, std::vector<Review>> reviews;
    std::map<int, std::vector<IssueComment>> comments; // PR-scoped
    std::vector<IssueComment> issue_comments_stream;   // repo-wide stream
    std::map<int, solvcon_bot::PrDetail> details;
    std::map<int, solvcon_bot::PrInfo> pr_infos;
    std::map<std::string, solvcon_bot::FileContent> files_at_ref;
    bool throw_on_get_pr_info = false;
    // is_collaborator scripting, checked in this order: a login in
    // collab_fatal_throws raises a scope-style (fatal) GithubError, one in
    // collab_transient_throws raises a plain transient failure, one in
    // collab_rate_limited_throws raises a transient-flagged 403, and
    // anything else answers membership from `collaborators`.
    std::set<std::string> collaborators;
    std::set<std::string> collab_fatal_throws;
    std::set<std::string> collab_transient_throws;
    std::set<std::string> collab_rate_limited_throws;
    std::vector<std::string> is_collaborator_calls;
    std::map<int, DiffResult> diffs;
    std::string reviewer_output = "## review\n\nlgtm\n";

    bool throw_on_list_open = false;
    bool throw_on_list_reviews_for_pr = false;
    int throw_on_list_reviews_for_n = -1;
    bool throw_on_stream_diff = false;
    bool throw_on_post_comment = false;
    bool throw_on_reviewer = false;
    bool throw_on_is_collaborator = false;
    bool throw_on_get_issue_detail_for_pr = false;
    int throw_on_get_issue_detail_for_n = -1;

    // Recorded calls
    struct PostCall { int n; std::string body; };
    std::vector<PostCall> posts;
    std::vector<int> reviewer_calls;
    std::vector<solvcon_bot::ReviewRequest> reviewer_requests;
    std::vector<std::pair<std::string, std::string>> file_fetch_calls;
    std::vector<int> mark_reviewed_calls;
    std::vector<std::int64_t> mark_handled_calls;
    std::vector<std::pair<std::string, std::int64_t>> advance_cursor_calls;
    int save_state_calls = 0;
    std::set<int> already_reviewed;
    std::set<std::int64_t> already_handled;
    std::string cursor_at;
    std::int64_t cursor_idval = 0;

    std::vector<Review> reviews_for(int n) const
    {
        auto it = reviews.find(n);
        return it == reviews.end() ? std::vector<Review>{} : it->second;
    }
    std::vector<IssueComment> comments_for(int n) const
    {
        auto it = comments.find(n);
        return it == comments.end() ? std::vector<IssueComment>{} : it->second;
    }

    // --- IO methods ---
    std::vector<PrSummary> list_open_prs() override
    {
        if (throw_on_list_open)
            throw std::runtime_error("simulated list_open_prs failure");
        return prs;
    }
    std::vector<Review> list_reviews(int n) override
    {
        if (throw_on_list_reviews_for_pr && throw_on_list_reviews_for_n == n)
            throw std::runtime_error("simulated list_reviews failure");
        return reviews_for(n);
    }
    std::vector<IssueComment> list_pr_comments(int n) override
    {
        return comments_for(n);
    }
    std::vector<std::string> list_issue_comments_since_args;
    std::vector<IssueComment> list_issue_comments(
        const std::string & since) override
    {
        list_issue_comments_since_args.push_back(since);
        return issue_comments_stream;
    }
    DiffResult stream_diff(int n) override
    {
        if (throw_on_stream_diff)
            throw std::runtime_error("simulated stream_diff failure");
        auto it = diffs.find(n);
        if (it != diffs.end()) return it->second;
        return DiffResult{"diff body", false};
    }
    void post_comment(int n, const std::string & body) override
    {
        if (throw_on_post_comment)
            throw std::runtime_error("simulated post_comment failure");
        posts.push_back({n, body});
    }
    solvcon_bot::PrDetail get_issue_detail(int n) override
    {
        if (throw_on_get_issue_detail_for_pr
            && throw_on_get_issue_detail_for_n == n)
        {
            throw std::runtime_error("simulated get_issue_detail failure");
        }
        auto it = details.find(n);
        if (it != details.end()) return it->second;
        solvcon_bot::PrDetail d;
        d.number = n;
        d.state = "open";
        d.is_pr = false;
        return d;
    }
    solvcon_bot::PrInfo get_pr_info(int n) override
    {
        if (throw_on_get_pr_info)
            throw std::runtime_error("simulated get_pr_info failure");
        auto it = pr_infos.find(n);
        if (it != pr_infos.end()) return it->second;
        solvcon_bot::PrInfo p;
        p.number = n;
        p.state = "open";
        return p;
    }
    solvcon_bot::FileContent get_file_at_ref(const std::string & path,
                                             const std::string & ref) override
    {
        file_fetch_calls.push_back({path, ref});
        auto it = files_at_ref.find(path);
        if (it != files_at_ref.end()) return it->second;
        return solvcon_bot::FileContent{};
    }
    bool is_collaborator(const std::string & u) override
    {
        is_collaborator_calls.push_back(u);
        if (throw_on_is_collaborator)
            throw std::runtime_error("simulated is_collaborator 403");
        if (collab_fatal_throws.count(u) > 0)
        {
            throw solvcon_bot::GithubError(403, "simulated scope 403");
        }
        if (collab_rate_limited_throws.count(u) > 0)
        {
            throw solvcon_bot::GithubError(403, "simulated rate-limit 403",
                                           /*transient=*/true);
        }
        if (collab_transient_throws.count(u) > 0)
            throw std::runtime_error("simulated is_collaborator failure");
        return collaborators.count(u) > 0;
    }
    std::string run_reviewer(const solvcon_bot::ReviewRequest & r) override
    {
        if (throw_on_reviewer)
            throw std::runtime_error("simulated reviewer failure");
        reviewer_calls.push_back(static_cast<int>(r.diff.size()));
        reviewer_requests.push_back(r);
        return reviewer_output;
    }
    bool reviewed(int n) override
    {
        return already_reviewed.count(n) > 0;
    }
    void mark_reviewed(int n) override
    {
        already_reviewed.insert(n);
        mark_reviewed_calls.push_back(n);
    }
    bool handled(std::int64_t id) override
    {
        return already_handled.count(id) > 0;
    }
    void mark_handled(std::int64_t id) override
    {
        already_handled.insert(id);
        mark_handled_calls.push_back(id);
    }
    std::string cursor_updated_at() override { return cursor_at; }
    bool is_at_or_before_cursor(const std::string & u, std::int64_t id) override
    {
        if (cursor_at.empty()) return false;
        if (u < cursor_at) return true;
        if (u > cursor_at) return false;
        return id <= cursor_idval;
    }
    void advance_cursor(const std::string & u, std::int64_t id) override
    {
        advance_cursor_calls.push_back({u, id});
        if (u > cursor_at || (u == cursor_at && id > cursor_idval))
        {
            cursor_at = u;
            cursor_idval = id;
        }
    }
    void save_state() override { ++save_state_calls; }
};

solvcon_bot::IssueComment make_issue_comment(
    std::int64_t id, const std::string & login, const std::string & body,
    const std::string & updated_at, int issue_n)
{
    solvcon_bot::IssueComment c;
    c.id = id;
    c.user.login = login;
    c.body = body;
    c.updated_at = updated_at;
    c.created_at = updated_at;
    c.issue_url = "https://api.github.com/repos/o/r/issues/"
                  + std::to_string(issue_n);
    return c;
}

solvcon_bot::PrDetail make_pr_detail(int n, const std::string & state, bool is_pr)
{
    solvcon_bot::PrDetail d;
    d.number = n;
    d.state = state;
    d.is_pr = is_pr;
    return d;
}

Config make_cfg()
{
    Config c;
    c.bot_handle = "solvcon-bot";
    c.max_diff_bytes = 200000;
    return c;
}

PrSummary make_pr(int n)
{
    PrSummary p;
    p.number = n;
    return p;
}

Review make_review(const std::string & state, const std::string & login)
{
    Review r;
    r.state = state;
    r.user.login = login;
    return r;
}

IssueComment make_comment(int64_t id, const std::string & login,
                          const std::string & body)
{
    IssueComment c;
    c.id = id;
    c.user.login = login;
    c.body = body;
    return c;
}

// --- run_auto_path tests --------------------------------------------------

void test_auto_path_no_prs_does_nothing()
{
    FakeWatcherIo io;
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

void test_auto_path_pr_without_approval_skipped()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("COMMENTED", "alice")};
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

void test_auto_path_dispatch_on_first_approval()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.posts[0].n, 42);
    const std::string k = build_marker_key("auto", 42, std::nullopt);
    EXPECT(body_has_marker_key(io.posts[0].body, k));
    EXPECT(io.posts[0].body.find("lgtm") != std::string::npos);
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
    EXPECT_EQ(io.save_state_calls, 1);
}

void test_auto_path_already_reviewed_pr_skipped()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.already_reviewed.insert(42);
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

void test_auto_path_marker_dedupe_skips_post_but_marks_reviewed()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    // Pre-existing bot comment with the marker. Even the OLD version
    // string should still match because dedupe uses the key.
    io.comments[42] = {
        make_comment(99, "solvcon-bot",
            "<!-- solvcon-bot/0.0.0-old source=auto pr=42 trigger=first-approval -->\n"
            "old review")
    };
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty()); // skipped due to dedupe
    EXPECT(io.reviewer_calls.empty()); // also skipped
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_auto_path_marker_dedupe_ignores_other_users_markers()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    // Someone other than the bot posted a body that happens to contain a
    // marker-shaped string. We should still dispatch.
    io.comments[42] = {
        make_comment(99, "alice",
            std::string("see <!-- solvcon-bot/0.0.0-old source=auto pr=42 ")
            + "trigger=first-approval --> note")
    };
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
}

void test_auto_path_truncated_diff_posts_notice_and_skips_reviewer()
{
    // truncated == the download hit MAX_DIFF_FETCH_BYTES: the diff is
    // incomplete, per-file trimming is impossible, only a notice goes out.
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{"partial", true};
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT(io.posts[0].body.find("MAX_DIFF_FETCH_BYTES") != std::string::npos);
    EXPECT(io.posts[0].body.find("skipped") != std::string::npos);
    EXPECT(io.reviewer_calls.empty()); // reviewer not invoked
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

// --- dispatch_review enrichment -------------------------------------------

// A two-file diff used by the enrichment tests. Sections are sized so a
// budget can be picked between them.
const char * kTwoFileDiff =
    "diff --git a/small.cc b/small.cc\n"
    "--- a/small.cc\n"
    "+++ b/small.cc\n"
    "@@ -1 +1 @@\n"
    "-a\n"
    "+b\n"
    "diff --git a/big.cc b/big.cc\n"
    "--- a/big.cc\n"
    "+++ b/big.cc\n"
    "@@ -1 +1 @@\n"
    "-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "+bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";

void test_dispatch_passes_pr_metadata_to_reviewer()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    solvcon_bot::PrInfo info;
    info.number = 42;
    info.title = "Add frobnicator";
    info.body = "Because reasons.";
    info.head_sha = "0123456789abcdef0123456789abcdef01234567";
    io.pr_infos[42] = info;

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.reviewer_requests.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.reviewer_requests[0].pr_title, std::string("Add frobnicator"));
    EXPECT_EQ(io.reviewer_requests[0].pr_body, std::string("Because reasons."));
    EXPECT_EQ(io.reviewer_requests[0].diff, std::string("diff body"));
}

void test_dispatch_get_pr_info_failure_still_reviews()
{
    // Metadata is enrichment: a transient failure falls back to a
    // bare-diff review instead of losing the dispatch.
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.throw_on_get_pr_info = true;

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.reviewer_requests.size(), static_cast<std::size_t>(1));
    EXPECT(io.reviewer_requests[0].pr_title.empty());
    EXPECT(io.file_fetch_calls.empty()); // no head sha -> no context fetch
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_dispatch_oversized_diff_trimmed_per_file()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};

    Config cfg = make_cfg();
    cfg.max_diff_bytes = 100;              // fits small.cc only
    cfg.max_context_total_bytes = 0;       // keep this test about trimming
    Watcher w(cfg, io);
    w.tick();

    EXPECT_EQ(io.reviewer_requests.size(), static_cast<std::size_t>(1));
    const auto & req = io.reviewer_requests[0];
    EXPECT(req.diff.find("small.cc") != std::string::npos);
    EXPECT(req.diff.find("big.cc") == std::string::npos);
    EXPECT_EQ(req.omitted_files.size(), static_cast<std::size_t>(1));
    EXPECT(req.omitted_files[0].find("big.cc") == 0);
    // The posted comment discloses partial coverage.
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT(io.posts[0].body.find("omitted 1") != std::string::npos);
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_dispatch_oversized_diff_nothing_fits_posts_notice()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};

    Config cfg = make_cfg();
    cfg.max_diff_bytes = 10; // smaller than every file section
    Watcher w(cfg, io);
    w.tick();

    EXPECT(io.reviewer_calls.empty());
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT(io.posts[0].body.find("MAX_DIFF_BYTES") != std::string::npos);
    EXPECT(io.posts[0].body.find("skipped") != std::string::npos);
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_dispatch_fetches_context_files_within_caps()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};
    solvcon_bot::PrInfo info;
    info.number = 42;
    info.title = "t";
    info.head_sha = "0123456789abcdef0123456789abcdef01234567";
    io.pr_infos[42] = info;
    io.files_at_ref["small.cc"] =
        solvcon_bot::FileContent{true, false, "int b;\n"};
    io.files_at_ref["big.cc"] =
        solvcon_bot::FileContent{true, false, "int bbbb;\n"};

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.file_fetch_calls.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(io.file_fetch_calls[0].second, info.head_sha);
    EXPECT_EQ(io.reviewer_requests.size(), static_cast<std::size_t>(1));
    const auto & req = io.reviewer_requests[0];
    EXPECT_EQ(req.context_files.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(req.context_files[0].path, std::string("small.cc"));
    EXPECT_EQ(req.context_files[0].content, std::string("int b;\n"));
}

void test_dispatch_context_skips_binary_truncated_missing()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};
    solvcon_bot::PrInfo info;
    info.number = 42;
    info.head_sha = "0123456789abcdef0123456789abcdef01234567";
    io.pr_infos[42] = info;
    io.files_at_ref["small.cc"] =
        solvcon_bot::FileContent{true, false, std::string("a\0b", 3)}; // binary
    io.files_at_ref["big.cc"] =
        solvcon_bot::FileContent{true, true, "partial"}; // over per-file cap

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.reviewer_requests.size(), static_cast<std::size_t>(1));
    EXPECT(io.reviewer_requests[0].context_files.empty());
}

void test_dispatch_context_disabled_by_zero_total()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};
    solvcon_bot::PrInfo info;
    info.number = 42;
    info.head_sha = "0123456789abcdef0123456789abcdef01234567";
    io.pr_infos[42] = info;

    Config cfg = make_cfg();
    cfg.max_context_total_bytes = 0;
    Watcher w(cfg, io);
    w.tick();

    EXPECT(io.file_fetch_calls.empty());
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
}

void test_dispatch_context_respects_file_count_cap()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.diffs[42] = DiffResult{kTwoFileDiff, false};
    solvcon_bot::PrInfo info;
    info.number = 42;
    info.head_sha = "0123456789abcdef0123456789abcdef01234567";
    io.pr_infos[42] = info;
    io.files_at_ref["small.cc"] =
        solvcon_bot::FileContent{true, false, "int b;\n"};

    Config cfg = make_cfg();
    cfg.max_context_files = 1;
    Watcher w(cfg, io);
    w.tick();

    EXPECT_EQ(io.file_fetch_calls.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.reviewer_requests[0].context_files.size(),
              static_cast<std::size_t>(1));
}

void test_auto_path_dispatch_failure_does_not_mark_reviewed()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.throw_on_reviewer = true;
    Watcher w(make_cfg(), io);
    w.tick();
    // Reviewer threw; we did NOT call post_comment, did NOT mark reviewed.
    EXPECT(io.posts.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

void test_auto_path_list_reviews_failure_continues_to_next_pr()
{
    FakeWatcherIo io;
    io.prs = {make_pr(1), make_pr(2)};
    io.reviews[2] = {make_review("APPROVED", "alice")};
    io.collaborators.insert("alice");
    io.throw_on_list_reviews_for_pr = true;
    io.throw_on_list_reviews_for_n = 1;
    Watcher w(make_cfg(), io);
    w.tick();
    // PR 1 failed and was skipped; PR 2 was reviewed.
    EXPECT(io.mark_reviewed_calls == std::vector<int>{2});
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.posts[0].n, 2);
}

void test_auto_path_takes_first_approval_only_one_review_posted()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    // Two APPROVED reviews on the same PR — we should only post once.
    io.reviews[42] = {
        make_review("APPROVED", "alice"),
        make_review("APPROVED", "bob"),
    };
    io.collaborators.insert("alice");
    io.collaborators.insert("bob");
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_auto_path_non_collaborator_approval_skipped()
{
    // On a public repo any account can approve. That must not spend an
    // AI review run.
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "drive-by")};
    // No collaborators configured.
    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    // Crucially NOT marked reviewed: a later real approval must still fire.
    EXPECT(io.mark_reviewed_calls.empty());
    EXPECT(io.is_collaborator_calls == std::vector<std::string>{"drive-by"});
}

void test_auto_path_non_collaborator_then_collaborator_dispatches()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {
        make_review("APPROVED", "drive-by"),
        make_review("APPROVED", "alice"),
    };
    io.collaborators.insert("alice");
    Watcher w(make_cfg(), io);
    w.tick();

    // The non-collaborator approval is skipped, scanning continues, and
    // alice's approval dispatches exactly one review.
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.posts[0].n, 42);
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_auto_path_non_collaborator_retried_on_later_tick()
{
    // Tick 1 sees only a non-collaborator approval; tick 2 sees the
    // collaborator's. Because tick 1 did not mark_reviewed, tick 2 fires.
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "drive-by")};
    // Named Config: Watcher holds it by reference and this test outlives
    // the full-expression a make_cfg() temporary would be bound to.
    const Config cfg = make_cfg();
    Watcher w(cfg, io);
    w.tick();
    EXPECT(io.posts.empty());

    io.reviews[42].push_back(make_review("APPROVED", "alice"));
    io.collaborators.insert("alice");
    w.tick();
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_reviewed_calls == std::vector<int>{42});
}

void test_auto_path_is_collaborator_transient_failure_skips_pr()
{
    FakeWatcherIo io;
    io.prs = {make_pr(1), make_pr(2)};
    io.reviews[1] = {make_review("APPROVED", "flaky")};
    io.reviews[2] = {make_review("APPROVED", "alice")};
    io.collab_transient_throws.insert("flaky");
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    // PR 1 skipped without a durable decision; PR 2 still processed.
    EXPECT(io.mark_reviewed_calls == std::vector<int>{2});
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.posts[0].n, 2);
}

void test_auto_path_is_collaborator_rate_limited_403_is_not_fatal()
{
    // A transient-flagged 403 (rate limit) must behave like any other
    // transient failure rather than killing the process.
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collab_rate_limited_throws.insert("alice");

    Watcher w(make_cfg(), io);
    bool threw = false;
    try { w.tick(); }
    catch (const std::exception &) { threw = true; }
    EXPECT(!threw);
    EXPECT(io.posts.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

void test_auto_path_is_collaborator_scope_403_is_fatal()
{
    FakeWatcherIo io;
    io.prs = {make_pr(42)};
    io.reviews[42] = {make_review("APPROVED", "alice")};
    io.collab_fatal_throws.insert("alice");

    Watcher w(make_cfg(), io);
    bool threw = false;
    try { w.tick(); }
    catch (const solvcon_bot::GithubError &) { threw = true; }
    EXPECT(threw);
    EXPECT(io.posts.empty());
    EXPECT(io.mark_reviewed_calls.empty());
}

// --- run_ping_path tests --------------------------------------------------

void test_ping_path_no_comments()
{
    FakeWatcherIo io;
    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.mark_handled_calls.empty());
    EXPECT(io.advance_cursor_calls.empty());
}

void test_ping_path_collaborator_with_mention_dispatches()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.posts[0].n, 42);
    EXPECT(io.posts[0].body.find("trigger=1001 -->") != std::string::npos);
    EXPECT_EQ(io.reviewer_calls.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.advance_cursor_calls[0].first, std::string("2026-05-01T10:00:00Z"));
    EXPECT_EQ(io.advance_cursor_calls[0].second, static_cast<std::int64_t>(1001));
}

void test_ping_path_non_collaborator_ignored()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "drive-by", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    // No collaborators set.

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    // Still marked handled + cursor advanced — we decided "no" durably.
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
}

void test_ping_path_issue_not_pr_ignored()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 11)
    };
    io.details[11] = make_pr_detail(11, "open", /*is_pr=*/false);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
}

void test_ping_path_closed_pr_ignored()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "closed", true);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
}

void test_ping_path_self_mention_ignored()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "solvcon-bot",
                           "<!-- our previous post --> @solvcon-bot self-ref",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("solvcon-bot");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    // Our own comment never reaches the mention check, so it stays out of
    // the handled set; the cursor alone covers it.
    EXPECT(io.mark_handled_calls.empty());
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
}

void test_ping_path_no_mention_ignored()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "Just a regular comment",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    // Non-mention comments must NOT be recorded — that is what made the
    // state file grow without bound. The cursor still advances past them.
    EXPECT(io.mark_handled_calls.empty());
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(io.advance_cursor_calls[0].second, static_cast<std::int64_t>(1001));
}

void test_ping_path_edited_comment_adding_mention_triggers()
{
    // A comment with no mention is only cursor-protected. When it is
    // edited to add one, GitHub re-surfaces it with a newer updated_at
    // and the bot now responds — the intended consequence of not
    // recording every comment id.
    FakeWatcherIo io;
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "Just a regular comment",
                           "2026-05-01T10:00:00Z", 42)
    };
    // Named Config: Watcher holds it by reference and this test outlives
    // the full-expression a make_cfg() temporary would be bound to.
    const Config cfg = make_cfg();
    Watcher w(cfg, io);
    w.tick();
    EXPECT(io.posts.empty());

    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:05:00Z", 42)
    };
    w.tick();
    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1));
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
}

void test_ping_path_already_handled_skipped()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");
    io.already_handled.insert(1001);

    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    // Already in the handled set, so no redundant re-marking; the cursor
    // still advances past it.
    EXPECT(io.mark_handled_calls.empty());
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(1));
}

void test_ping_path_at_cursor_skipped()
{
    FakeWatcherIo io;
    io.cursor_at = "2026-05-01T10:00:00Z";
    io.cursor_idval = 1001;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
}

void test_ping_path_marker_dedupe_skips_post()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");
    // Pre-existing bot post matching this exact trigger.
    io.comments[42] = {
        make_comment(99, "solvcon-bot",
            "<!-- solvcon-bot/0.0.1 source=ping pr=42 trigger=1001 -->\n"
            "previous response")
    };

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT(io.posts.empty());
    EXPECT(io.reviewer_calls.empty());
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
}

void test_ping_path_collaborator_403_is_fatal()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.throw_on_is_collaborator = true;

    Watcher w(make_cfg(), io);
    bool threw = false;
    try { w.tick(); }
    catch (const std::exception &) { threw = true; }
    EXPECT(threw); // propagates up
    EXPECT(io.posts.empty());
    EXPECT(io.mark_handled_calls.empty()); // did not decide
}

void test_ping_path_get_issue_detail_transient_keeps_for_retry()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42)
    };
    io.throw_on_get_issue_detail_for_pr = true;
    io.throw_on_get_issue_detail_for_n = 42;
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();
    EXPECT(io.posts.empty());
    // Not marked handled, cursor not advanced — we'll retry next tick.
    EXPECT(io.mark_handled_calls.empty());
    EXPECT(io.advance_cursor_calls.empty());
}

void test_ping_path_uses_cursor_as_since_argument()
{
    FakeWatcherIo io;
    io.cursor_at = "2026-05-01T09:59:59Z";
    io.cursor_idval = 500;
    io.issue_comments_stream = {};
    Watcher w(make_cfg(), io);
    w.tick();
    // tick called list_issue_comments(cursor_at).
    EXPECT_EQ(io.list_issue_comments_since_args.size(),
              static_cast<std::size_t>(1));
    EXPECT_EQ(io.list_issue_comments_since_args[0],
              std::string("2026-05-01T09:59:59Z"));
}

void test_ping_path_sorts_by_updated_at_then_id()
{
    FakeWatcherIo io;
    // Adversarial order: id-descending at the same updated_at.
    io.issue_comments_stream = {
        make_issue_comment(1003, "alice", "@solvcon-bot one", "2026-05-01T10:00:00Z", 42),
        make_issue_comment(1001, "alice", "@solvcon-bot two", "2026-05-01T10:00:00Z", 42),
        make_issue_comment(1002, "alice", "@solvcon-bot three", "2026-05-01T10:00:00Z", 42),
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");
    Watcher w(make_cfg(), io);
    w.tick();
    // Expect to see them processed in (updated_at, id) order: 1001, 1002, 1003.
    EXPECT(io.mark_handled_calls
           == (std::vector<std::int64_t>{1001, 1002, 1003}));
}

void test_ping_path_transient_failure_stops_loop_no_cursor_leak()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42),
        make_issue_comment(1002, "alice", "@solvcon-bot also this",
                           "2026-05-01T10:00:05Z", 99),
    };
    io.details[99] = make_pr_detail(99, "open", true);
    // 42 throws transient — we want this to halt the loop so 1002 is
    // not processed (and the cursor doesn't move past 1001).
    io.throw_on_get_issue_detail_for_pr = true;
    io.throw_on_get_issue_detail_for_n = 42;
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    // Neither comment got a durable decision.
    EXPECT(io.mark_handled_calls.empty());
    EXPECT(io.advance_cursor_calls.empty());
    EXPECT(io.posts.empty());
}

void test_ping_path_two_comments_advances_cursor_to_last()
{
    FakeWatcherIo io;
    io.issue_comments_stream = {
        make_issue_comment(1001, "alice", "@solvcon-bot please review",
                           "2026-05-01T10:00:00Z", 42),
        make_issue_comment(1002, "bob", "not a mention",
                           "2026-05-01T10:00:05Z", 42)
    };
    io.details[42] = make_pr_detail(42, "open", true);
    io.collaborators.insert("alice");

    Watcher w(make_cfg(), io);
    w.tick();

    EXPECT_EQ(io.posts.size(), static_cast<std::size_t>(1)); // alice's only
    // Only the mention is recorded; bob's plain comment moves the cursor.
    EXPECT(io.mark_handled_calls == std::vector<std::int64_t>{1001});
    EXPECT_EQ(io.advance_cursor_calls.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(io.advance_cursor_calls.back().first,
              std::string("2026-05-01T10:00:05Z"));
    EXPECT_EQ(io.advance_cursor_calls.back().second,
              static_cast<std::int64_t>(1002));
}

} // namespace

int main()
{
    test_marker_exact_string_auto();
    test_marker_exact_string_ping();
    test_marker_key_excludes_version();
    test_marker_key_present_in_marker();
    test_marker_key_survives_version_bump();
    test_marker_key_distinct_per_pr_and_trigger();
    test_body_has_marker_key_unrelated_html_does_not_match();

    test_auto_path_no_prs_does_nothing();
    test_auto_path_pr_without_approval_skipped();
    test_auto_path_dispatch_on_first_approval();
    test_auto_path_already_reviewed_pr_skipped();
    test_auto_path_marker_dedupe_skips_post_but_marks_reviewed();
    test_auto_path_marker_dedupe_ignores_other_users_markers();
    test_auto_path_truncated_diff_posts_notice_and_skips_reviewer();
    test_dispatch_passes_pr_metadata_to_reviewer();
    test_dispatch_get_pr_info_failure_still_reviews();
    test_dispatch_oversized_diff_trimmed_per_file();
    test_dispatch_oversized_diff_nothing_fits_posts_notice();
    test_dispatch_fetches_context_files_within_caps();
    test_dispatch_context_skips_binary_truncated_missing();
    test_dispatch_context_disabled_by_zero_total();
    test_dispatch_context_respects_file_count_cap();
    test_auto_path_dispatch_failure_does_not_mark_reviewed();
    test_auto_path_list_reviews_failure_continues_to_next_pr();
    test_auto_path_takes_first_approval_only_one_review_posted();
    test_auto_path_non_collaborator_approval_skipped();
    test_auto_path_non_collaborator_then_collaborator_dispatches();
    test_auto_path_non_collaborator_retried_on_later_tick();
    test_auto_path_is_collaborator_transient_failure_skips_pr();
    test_auto_path_is_collaborator_rate_limited_403_is_not_fatal();
    test_auto_path_is_collaborator_scope_403_is_fatal();

    test_ping_path_no_comments();
    test_ping_path_collaborator_with_mention_dispatches();
    test_ping_path_non_collaborator_ignored();
    test_ping_path_issue_not_pr_ignored();
    test_ping_path_closed_pr_ignored();
    test_ping_path_self_mention_ignored();
    test_ping_path_no_mention_ignored();
    test_ping_path_edited_comment_adding_mention_triggers();
    test_ping_path_already_handled_skipped();
    test_ping_path_at_cursor_skipped();
    test_ping_path_marker_dedupe_skips_post();
    test_ping_path_collaborator_403_is_fatal();
    test_ping_path_get_issue_detail_transient_keeps_for_retry();
    test_ping_path_uses_cursor_as_since_argument();
    test_ping_path_sorts_by_updated_at_then_id();
    test_ping_path_transient_failure_stops_loop_no_cursor_leak();
    test_ping_path_two_comments_advances_cursor_to_last();

    std::cerr << "watcher tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
