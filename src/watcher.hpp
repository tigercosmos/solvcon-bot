#pragma once

#include "config.hpp"
#include "github_client.hpp"
#include "github_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace modmesh_bot
{

class GithubClient;
class Reviewer;
class StateStore;

// Abstract IO surface used by Watcher. Production wires this to the
// concrete GithubClient + Reviewer + StateStore via LiveWatcherIo;
// tests substitute their own implementation.
class WatcherIo
{
public:
    virtual ~WatcherIo() = default;

    virtual std::vector<PrSummary> list_open_prs() = 0;
    virtual std::vector<Review> list_reviews(int pr_number) = 0;
    virtual std::vector<IssueComment> list_pr_comments(int pr_number) = 0;
    virtual std::vector<IssueComment> list_issue_comments(
        const std::string & since_iso8601) = 0;
    virtual DiffResult stream_diff(int pr_number) = 0;
    virtual void post_comment(int pr_number, const std::string & body) = 0;
    virtual PrDetail get_issue_detail(int issue_number) = 0;
    virtual bool is_collaborator(const std::string & user) = 0;

    virtual std::string run_reviewer(const std::string & diff) = 0;

    virtual bool reviewed(int pr_number) = 0;
    virtual void mark_reviewed(int pr_number) = 0;

    virtual bool handled(std::int64_t comment_id) = 0;
    virtual void mark_handled(std::int64_t comment_id) = 0;
    virtual std::string cursor_updated_at() = 0;
    virtual bool is_at_or_before_cursor(const std::string & updated_at,
                                        std::int64_t comment_id) = 0;
    virtual void advance_cursor(const std::string & updated_at,
                                std::int64_t comment_id) = 0;

    virtual void save_state() = 0;
};

class LiveWatcherIo : public WatcherIo
{
public:
    LiveWatcherIo(GithubClient & gh, Reviewer & rv, StateStore & state);

    std::vector<PrSummary> list_open_prs() override;
    std::vector<Review> list_reviews(int pr_number) override;
    std::vector<IssueComment> list_pr_comments(int pr_number) override;
    std::vector<IssueComment> list_issue_comments(
        const std::string & since_iso8601) override;
    DiffResult stream_diff(int pr_number) override;
    void post_comment(int pr_number, const std::string & body) override;
    PrDetail get_issue_detail(int issue_number) override;
    bool is_collaborator(const std::string & user) override;
    std::string run_reviewer(const std::string & diff) override;
    bool reviewed(int pr_number) override;
    void mark_reviewed(int pr_number) override;
    bool handled(std::int64_t comment_id) override;
    void mark_handled(std::int64_t comment_id) override;
    std::string cursor_updated_at() override;
    bool is_at_or_before_cursor(const std::string & updated_at,
                                std::int64_t comment_id) override;
    void advance_cursor(const std::string & updated_at,
                        std::int64_t comment_id) override;
    void save_state() override;

private:
    GithubClient & gh_;
    Reviewer & rv_;
    StateStore & state_;
};

class Watcher
{
public:
    Watcher(const Config & cfg, WatcherIo & io);

    // Run one polling tick: auto path now, ping path in M8.
    void tick();

private:
    void run_auto_path();
    void run_ping_path();
    void dispatch_review(int pr_number, const std::string & source,
                         std::optional<long long> trigger_comment_id);

    const Config & cfg_;
    WatcherIo & io_;
};

// Format the full hidden HTML-comment marker that the bot prefixes onto
// every comment it posts. Includes the bot version for traceability:
// "<!-- modmesh-bot/<ver> source=<s> pr=<n> trigger=<t> -->".
std::string build_marker(const std::string & source, int pr_number,
                         std::optional<long long> trigger_comment_id);

// Stable substring of `build_marker` that does NOT include the bot
// version. Idempotency dedupe must use this so that a version bump
// between "we posted" and "we checked" doesn't cause a duplicate post.
// Returns e.g. "source=auto pr=42 trigger=first-approval -->".
std::string build_marker_key(const std::string & source, int pr_number,
                             std::optional<long long> trigger_comment_id);

// True iff `body` contains the version-agnostic marker key.
bool body_has_marker_key(const std::string & body, const std::string & key);

} // namespace modmesh_bot
