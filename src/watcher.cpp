#include "watcher.hpp"

#include "github_client.hpp"
#include "github_types.hpp"
#include "log.hpp"
#include "mention.hpp"
#include "reviewer.hpp"
#include "state_store.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <string>

namespace modmesh_bot
{

namespace
{

inline void log_info(const std::string & m) { modmesh_bot::log_info("watcher", m); }
inline void log_warn(const std::string & m) { modmesh_bot::log_warn("watcher", m); }
inline void log_err (const std::string & m) { modmesh_bot::log_error("watcher", m); }

// 401/403/422 are not transient: continuing to retry would just keep
// failing and the operator would never notice. These get re-thrown out
// of every catch so they reach main() and exit the process non-zero.
inline bool is_fatal_github(const std::exception & e)
{
    if (auto * ge = dynamic_cast<const GithubError *>(&e))
    {
        const int s = ge->status();
        return s == 401 || s == 403 || s == 422;
    }
    return false;
}

std::string trigger_str(std::optional<long long> trigger_comment_id)
{
    if (trigger_comment_id.has_value()) return std::to_string(*trigger_comment_id);
    return "first-approval";
}

} // namespace

std::string build_marker(const std::string & source, int pr_number,
                         std::optional<long long> trigger_comment_id)
{
    std::ostringstream oss;
    oss << "<!-- modmesh-bot/" << MODMESH_BOT_VERSION
        << " source=" << source
        << " pr=" << pr_number
        << " trigger=" << trigger_str(trigger_comment_id)
        << " -->";
    return oss.str();
}

std::string build_marker_key(const std::string & source, int pr_number,
                             std::optional<long long> trigger_comment_id)
{
    std::ostringstream oss;
    oss << "source=" << source
        << " pr=" << pr_number
        << " trigger=" << trigger_str(trigger_comment_id)
        << " -->";
    return oss.str();
}

bool body_has_marker_key(const std::string & body, const std::string & key)
{
    return body.find(key) != std::string::npos;
}

// --- LiveWatcherIo --------------------------------------------------------

LiveWatcherIo::LiveWatcherIo(GithubClient & gh, IReviewer & rv, StateStore & state)
    : gh_(gh), rv_(rv), state_(state) {}

std::vector<PrSummary> LiveWatcherIo::list_open_prs() { return gh_.list_open_prs(); }
std::vector<Review> LiveWatcherIo::list_reviews(int n) { return gh_.list_reviews(n); }
std::vector<IssueComment> LiveWatcherIo::list_pr_comments(int n) { return gh_.list_pr_comments(n); }
std::vector<IssueComment> LiveWatcherIo::list_issue_comments(const std::string & since)
{ return gh_.list_issue_comments(since); }
DiffResult LiveWatcherIo::stream_diff(int n) { return gh_.stream_diff(n); }
void LiveWatcherIo::post_comment(int n, const std::string & b) { gh_.post_comment(n, b); }
PrDetail LiveWatcherIo::get_issue_detail(int n) { return gh_.get_issue_detail(n); }
bool LiveWatcherIo::is_collaborator(const std::string & u) { return gh_.is_collaborator(u); }
std::string LiveWatcherIo::run_reviewer(const std::string & d) { return rv_.run(d); }
bool LiveWatcherIo::reviewed(int n) { return state_.reviewed(n); }
void LiveWatcherIo::mark_reviewed(int n) { state_.mark_reviewed(n); }
bool LiveWatcherIo::handled(std::int64_t id) { return state_.handled(id); }
void LiveWatcherIo::mark_handled(std::int64_t id) { state_.mark_handled(id); }
std::string LiveWatcherIo::cursor_updated_at() { return state_.cursor_updated_at(); }
bool LiveWatcherIo::is_at_or_before_cursor(const std::string & u, std::int64_t id)
{ return state_.is_at_or_before_cursor(u, id); }
void LiveWatcherIo::advance_cursor(const std::string & u, std::int64_t id)
{ state_.advance_cursor(u, id); }
void LiveWatcherIo::save_state() { state_.save(); }

// --- Watcher --------------------------------------------------------------

Watcher::Watcher(const Config & cfg, WatcherIo & io)
    : cfg_(cfg), io_(io) {}

void Watcher::tick()
{
    run_auto_path();
    run_ping_path();
}

void Watcher::run_auto_path()
{
    std::vector<PrSummary> prs;
    try
    {
        prs = io_.list_open_prs();
    }
    catch (const std::exception & e)
    {
        if (is_fatal_github(e)) throw;
        log_warn(std::string("list_open_prs failed: ") + e.what());
        return;
    }

    for (const auto & pr : prs)
    {
        if (io_.reviewed(pr.number)) continue;

        std::vector<Review> reviews;
        try
        {
            reviews = io_.list_reviews(pr.number);
        }
        catch (const std::exception & e)
        {
            if (is_fatal_github(e)) throw;
            log_warn("list_reviews(" + std::to_string(pr.number)
                     + ") failed: " + e.what());
            continue;
        }

        for (const auto & r : reviews)
        {
            if (r.state == "APPROVED")
            {
                const std::string marker_key = build_marker_key(
                    "auto", pr.number, std::nullopt);

                // Marker dedupe protects against a crash between
                // post_comment and mark_reviewed. We match the
                // version-agnostic key so a version bump between
                // those events still finds the marker.
                bool already_posted = false;
                try
                {
                    for (const auto & c : io_.list_pr_comments(pr.number))
                    {
                        if (eq_login(c.user.login, cfg_.bot_handle)
                            && body_has_marker_key(c.body, marker_key))
                        {
                            already_posted = true;
                            break;
                        }
                    }
                }
                catch (const std::exception & e)
                {
                    if (is_fatal_github(e)) throw;
                    log_warn("list_pr_comments(" + std::to_string(pr.number)
                             + ") failed: " + e.what());
                    // Bail on this PR; we'll retry next tick.
                    break;
                }

                if (!already_posted)
                {
                    try
                    {
                        dispatch_review(pr.number, "auto", std::nullopt);
                    }
                    catch (const std::exception & e)
                    {
                        if (is_fatal_github(e)) throw;
                        log_err("dispatch_review(" + std::to_string(pr.number)
                                + ") failed: " + e.what());
                        // Do NOT mark reviewed; retry next tick.
                        break;
                    }
                }
                else
                {
                    log_info("auto: marker already present for PR #"
                             + std::to_string(pr.number)
                             + " — skipping dispatch");
                }

                io_.mark_reviewed(pr.number);
                try { io_.save_state(); }
                catch (const std::exception & e)
                {
                    log_err(std::string("state save failed: ") + e.what());
                }
                break; // stop scanning reviews for this PR
            }
        }
    }
}

void Watcher::run_ping_path()
{
    std::vector<IssueComment> comments;
    try
    {
        comments = io_.list_issue_comments(io_.cursor_updated_at());
    }
    catch (const std::exception & e)
    {
        if (is_fatal_github(e)) throw;
        log_warn(std::string("list_issue_comments failed: ") + e.what());
        return;
    }

    // Defensive sort by (updated_at, id). GitHub returns them sorted by
    // updated ascending per our query string, but we re-sort here so
    // cursor tuple-compare semantics hold even if same-updated_at items
    // arrive in id order other than ascending.
    std::sort(comments.begin(), comments.end(),
        [](const IssueComment & a, const IssueComment & b) {
            if (a.updated_at != b.updated_at) return a.updated_at < b.updated_at;
            return a.id < b.id;
        });

    for (const auto & c : comments)
    {
        // Classify this comment. The lambda returns true iff the
        // decision is durable (mark_handled + advance_cursor); a return
        // of false means transient failure and we'll retry next tick.
        // A lambda is used so that early-out branches (resolved without
        // dispatch) don't accidentally `continue` past the durable-state
        // commit at the bottom of the loop body.
        auto classify = [&]() -> bool {
            // GitHub returns >= since on updated_at; tuple-filter to skip
            // anything we've already classified.
            if (io_.is_at_or_before_cursor(c.updated_at, c.id)) return true;
            if (io_.handled(c.id)) return true;
            if (eq_login(c.user.login, cfg_.bot_handle)) return true;
            if (!mention_matches(c.body, cfg_.bot_handle)) return true;

            const int issue_number = parse_issue_number_from_url(c.issue_url);
            if (issue_number <= 0)
            {
                log_warn("ping: could not extract issue number from URL: "
                         + c.issue_url);
                return true;
            }

            PrDetail detail;
            try
            {
                detail = io_.get_issue_detail(issue_number);
            }
            catch (const std::exception & e)
            {
                if (is_fatal_github(e)) throw;
                log_warn("ping: get_issue_detail("
                         + std::to_string(issue_number) + ") failed: "
                         + e.what());
                return false; // transient
            }

            if (!detail.is_pr || detail.state != "open") return true;

            const bool is_collab = io_.is_collaborator(c.user.login);
            // 403 from is_collaborator propagates as exception — fatal per
            // plan §6 (token scope problem). is_collaborator returns
            // false for 404 (not a collaborator).
            if (!is_collab) return true;

            const std::string marker_key = build_marker_key(
                "ping", detail.number, c.id);
            bool already_posted = false;
            try
            {
                for (const auto & pc : io_.list_pr_comments(detail.number))
                {
                    if (eq_login(pc.user.login, cfg_.bot_handle)
                        && body_has_marker_key(pc.body, marker_key))
                    {
                        already_posted = true;
                        break;
                    }
                }
            }
            catch (const std::exception & e)
            {
                if (is_fatal_github(e)) throw;
                log_warn("ping: list_pr_comments("
                         + std::to_string(detail.number) + ") failed: "
                         + e.what());
                return false; // transient
            }

            if (already_posted)
            {
                log_info("ping: marker already present for PR #"
                         + std::to_string(detail.number)
                         + " comment=" + std::to_string(c.id)
                         + " — skipping dispatch");
                return true;
            }

            try
            {
                dispatch_review(detail.number, "ping", c.id);
            }
            catch (const std::exception & e)
            {
                if (is_fatal_github(e)) throw;
                log_err("ping: dispatch_review("
                        + std::to_string(detail.number) + ") failed: "
                        + e.what());
                return false; // transient
            }
            return true;
        };

        const bool decided = classify();
        if (decided)
        {
            io_.mark_handled(c.id);
            io_.advance_cursor(c.updated_at, c.id);
            try { io_.save_state(); }
            catch (const std::exception & e)
            {
                log_err(std::string("state save failed: ") + e.what());
            }
        }
        else
        {
            // Transient failure on this comment. We must NOT process
            // later comments in this tick: doing so would advance the
            // cursor past this one and we'd never retry it.
            log_warn("ping: leaving comment " + std::to_string(c.id)
                     + " for retry on next tick");
            return;
        }
    }
}

void Watcher::dispatch_review(int pr_number, const std::string & source,
                              std::optional<long long> trigger_comment_id)
{
    const std::string marker = build_marker(source, pr_number, trigger_comment_id);

    DiffResult diff = io_.stream_diff(pr_number);
    if (diff.truncated)
    {
        const std::string body = marker + "\n\n"
            + "(diff exceeds MAX_DIFF_BYTES="
            + std::to_string(cfg_.max_diff_bytes) + " — skipped)\n";
        io_.post_comment(pr_number, body);
        log_info("dispatched " + source + " for PR #" + std::to_string(pr_number)
                 + ": diff truncated, posted notice");
        return;
    }

    log_info("running reviewer for PR #" + std::to_string(pr_number)
             + " (diff " + std::to_string(diff.body.size()) + " bytes)");
    const std::string review_output = io_.run_reviewer(diff.body);

    const std::string body = marker + "\n\n" + review_output;
    io_.post_comment(pr_number, body);
    log_info("posted " + source + " review for PR #"
             + std::to_string(pr_number));
}

} // namespace modmesh_bot
