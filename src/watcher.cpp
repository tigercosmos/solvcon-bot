#include "watcher.hpp"

#include "github_client.hpp"
#include "github_types.hpp"
#include "mention.hpp"
#include "reviewer.hpp"
#include "state_store.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace modmesh_bot
{

namespace
{

void log_info(const std::string & msg)
{
    std::cerr << "[modmesh-bot] " << msg << std::endl;
}

void log_warn(const std::string & msg)
{
    std::cerr << "[modmesh-bot] warn: " << msg << std::endl;
}

void log_err(const std::string & msg)
{
    std::cerr << "[modmesh-bot] error: " << msg << std::endl;
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

LiveWatcherIo::LiveWatcherIo(GithubClient & gh, Reviewer & rv, StateStore & state)
    : gh_(gh), rv_(rv), state_(state) {}

std::vector<PrSummary> LiveWatcherIo::list_open_prs() { return gh_.list_open_prs(); }
std::vector<Review> LiveWatcherIo::list_reviews(int n) { return gh_.list_reviews(n); }
std::vector<IssueComment> LiveWatcherIo::list_pr_comments(int n) { return gh_.list_pr_comments(n); }
DiffResult LiveWatcherIo::stream_diff(int n) { return gh_.stream_diff(n); }
void LiveWatcherIo::post_comment(int n, const std::string & b) { gh_.post_comment(n, b); }
std::string LiveWatcherIo::run_reviewer(const std::string & d) { return rv_.run(d); }
bool LiveWatcherIo::reviewed(int n) { return state_.reviewed(n); }
void LiveWatcherIo::mark_reviewed(int n) { state_.mark_reviewed(n); }
void LiveWatcherIo::save_state() { state_.save(); }

// --- Watcher --------------------------------------------------------------

Watcher::Watcher(const Config & cfg, WatcherIo & io)
    : cfg_(cfg), io_(io) {}

void Watcher::tick()
{
    run_auto_path();
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
