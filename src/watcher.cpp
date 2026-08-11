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

namespace solvcon_bot
{

namespace
{

inline void log_info(const std::string & m) { solvcon_bot::log_info("watcher", m); }
inline void log_warn(const std::string & m) { solvcon_bot::log_warn("watcher", m); }
inline void log_err (const std::string & m) { solvcon_bot::log_error("watcher", m); }

// 401/403/422 are not transient: continuing to retry would just keep
// failing and the operator would never notice. These get re-thrown out
// of every catch so they reach main() and exit the process non-zero.
//
// The exception is a rate-limited 403, which GithubError marks transient.
// That one clears on its own, so it must be handled like any other
// retryable failure — killing the bot over a rate limit is a bug.
// Must stay in sync with main()'s fatal check.
inline bool is_fatal_github(const std::exception & e)
{
    if (auto * ge = dynamic_cast<const GithubError *>(&e))
    {
        const int s = ge->status();
        if (s == 401 || s == 422) return true;
        return s == 403 && !ge->transient();
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
    oss << "<!-- solvcon-bot/" << SOLVCON_BOT_VERSION
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
PrInfo LiveWatcherIo::get_pr_info(int n) { return gh_.get_pr_info(n); }
FileContent LiveWatcherIo::get_file_at_ref(const std::string & path,
                                           const std::string & ref)
{ return gh_.get_file_at_ref(path, ref); }
bool LiveWatcherIo::is_collaborator(const std::string & u) { return gh_.is_collaborator(u); }
std::string LiveWatcherIo::run_reviewer(const ReviewRequest & r) { return rv_.run(r); }
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
            if (r.state != "APPROVED") continue;

            // On a public repo ANY account can submit an APPROVED review,
            // and dispatching costs a paid AI run. Gate on collaborator
            // status. We deliberately do NOT mark_reviewed on a skip: a
            // later approval from a real collaborator must still fire.
            // The same non-collaborator approval is re-checked each tick,
            // which is cheap because the reviews list is ETag-cached.
            bool approver_is_collaborator = false;
            try
            {
                approver_is_collaborator = io_.is_collaborator(r.user.login);
            }
            catch (const std::exception & e)
            {
                if (is_fatal_github(e)) throw;
                log_warn("auto: is_collaborator(" + r.user.login
                         + ") failed: " + e.what());
                // Bail on this PR; we'll retry next tick.
                break;
            }

            if (!approver_is_collaborator)
            {
                log_info("auto: ignoring APPROVED review from "
                         "non-collaborator " + r.user.login + " on PR #"
                         + std::to_string(pr.number));
                continue; // a later review in this list may still qualify
            }

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
        // Set once the comment is confirmed to @-mention us; gates
        // mark_handled below so the handled set stays small.
        bool mention_matched = false;

        auto classify = [&]() -> bool {
            // GitHub returns >= since on updated_at; tuple-filter to skip
            // anything we've already classified.
            if (io_.is_at_or_before_cursor(c.updated_at, c.id)) return true;
            if (io_.handled(c.id)) return true;
            if (eq_login(c.user.login, cfg_.bot_handle)) return true;
            if (!mention_matches(c.body, cfg_.bot_handle)) return true;
            mention_matched = true;

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
            // A scope-problem 403 propagates as an exception and is fatal
            // per plan §6; a rate-limited 403 is marked transient and is
            // caught by main(), which retries the tick. is_collaborator
            // returns false for 404 (not a collaborator).
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
            // Only mention comments enter the handled set. Recording every
            // comment the repo ever sees would grow the state file without
            // bound, and non-mentions are already covered by the cursor.
            // Consequence: a comment later edited to add a mention comes
            // back with a fresh updated_at and does trigger — intended.
            if (mention_matched) io_.mark_handled(c.id);
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
        // Too big even to download in full — per-file trimming needs
        // the whole diff, so all we can do is post the notice.
        const std::string body = marker + "\n\n"
            + "(diff exceeds MAX_DIFF_FETCH_BYTES="
            + std::to_string(cfg_.max_diff_fetch_bytes) + " — skipped)\n";
        io_.post_comment(pr_number, body);
        log_info("dispatched " + source + " for PR #" + std::to_string(pr_number)
                 + ": diff truncated, posted notice");
        return;
    }

    ReviewRequest request;
    request.diff = std::move(diff.body);

    // PR title/description tell the reviewer what the change claims to
    // do; the head sha keys the changed-file context fetch. Both are
    // enrichment — a transient metadata failure must not lose the
    // review, so fall back to a bare-diff request (fatal auth errors
    // still propagate and stop the bot).
    PrInfo info;
    bool have_info = false;
    try
    {
        info = io_.get_pr_info(pr_number);
        have_info = true;
    }
    catch (const std::exception & e)
    {
        if (is_fatal_github(e)) throw;
        log_warn("get_pr_info(" + std::to_string(pr_number) + ") failed: "
                 + e.what() + " — reviewing without PR metadata");
    }
    if (have_info)
    {
        request.pr_title = info.title;
        request.pr_body = info.body;
    }

    // Over-budget diffs are trimmed at file-section granularity instead
    // of skipped: the largest PRs are the ones a skipped review hurts
    // most. Omissions are disclosed both to the reviewer (inside the
    // fenced metadata) and to the PR readers (note under the review).
    std::string trim_note;
    if (request.diff.size() > cfg_.max_diff_bytes)
    {
        DiffTrimResult trimmed =
            trim_diff_to_budget(request.diff, cfg_.max_diff_bytes);
        if (trimmed.kept == 0)
        {
            const std::string body = marker + "\n\n"
                + "(diff exceeds MAX_DIFF_BYTES="
                + std::to_string(cfg_.max_diff_bytes)
                + " and no complete file section fits the budget"
                  " — skipped)\n";
            io_.post_comment(pr_number, body);
            log_info("dispatched " + source + " for PR #"
                     + std::to_string(pr_number)
                     + ": no file section fits MAX_DIFF_BYTES, posted notice");
            return;
        }
        trim_note = "\n\n_solvcon-bot: diff exceeded MAX_DIFF_BYTES="
            + std::to_string(cfg_.max_diff_bytes) + "; reviewed "
            + std::to_string(trimmed.kept) + " file section(s), omitted "
            + std::to_string(trimmed.omitted.size()) + "._\n";
        request.diff = std::move(trimmed.diff);
        request.omitted_files = std::move(trimmed.omitted);
        log_info("trimmed diff for PR #" + std::to_string(pr_number)
                 + ": kept " + std::to_string(trimmed.kept)
                 + " file section(s), omitted "
                 + std::to_string(request.omitted_files.size()));
    }

    if (have_info) collect_context_files(request, info.head_sha);

    log_info("running reviewer for PR #" + std::to_string(pr_number)
             + " (diff " + std::to_string(request.diff.size()) + " bytes, "
             + std::to_string(request.context_files.size())
             + " context file(s))");
    const std::string review_output = io_.run_reviewer(request);

    const std::string body = marker + "\n\n" + review_output + trim_note;
    io_.post_comment(pr_number, body);
    log_info("posted " + source + " review for PR #"
             + std::to_string(pr_number));
}

void Watcher::collect_context_files(ReviewRequest & request,
                                    const std::string & head_sha)
{
    if (cfg_.max_context_total_bytes == 0 || cfg_.max_context_files == 0
        || head_sha.empty())
    {
        return;
    }

    std::size_t total = 0;
    std::size_t attempts = 0;
    for (const std::string & path : changed_paths(request.diff))
    {
        // The count cap bounds API calls, so every attempt counts
        // against it whether or not it yields usable content.
        if (attempts >= cfg_.max_context_files) break;
        ++attempts;

        FileContent fc;
        try
        {
            fc = io_.get_file_at_ref(path, head_sha);
        }
        catch (const std::exception & e)
        {
            log_warn("context fetch failed for " + path + ": " + e.what()
                     + " — continuing without further context");
            break;
        }
        if (!fc.found || fc.truncated) continue; // absent or over per-file cap
        if (fc.body.find('\0') != std::string::npos) continue; // binary
        if (total + fc.body.size() > cfg_.max_context_total_bytes) break;
        total += fc.body.size();
        request.context_files.push_back({path, std::move(fc.body)});
    }
}

} // namespace solvcon_bot
