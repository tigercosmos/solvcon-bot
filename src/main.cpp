#include "config.hpp"
#include "github_client.hpp"
#include "github_types.hpp"
#include "log.hpp"
#include "reviewer.hpp"
#include "state_store.hpp"
#include "watcher.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace
{

volatile std::sig_atomic_t g_stop = 0;

// "YYYY-MM-DDTHH:MM:SSZ" in UTC, offset by `offset_sec` from now —
// the shape GitHub's `since` query parameter expects. Same gmtime_r +
// snprintf approach as log.cpp.
std::string now_iso8601_utc(long offset_sec = 0)
{
    const std::time_t tt = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() + std::chrono::seconds(offset_sec));
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return ts;
}

void on_signal(int)
{
    g_stop = 1;
}

void install_signal_handlers()
{
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // SIGPIPE is set to SIG_IGN inside subprocess.cpp the first time
    // we spawn a child; here we also install it pre-emptively so the
    // HTTP client doesn't kill us either.
    signal(SIGPIPE, SIG_IGN);
}

} // namespace

int main()
{
    try
    {
        auto cfg = solvcon_bot::Config::from_env();
        solvcon_bot::log_info("main",
            std::string("starting solvcon-bot ") + SOLVCON_BOT_VERSION
            + " repo=" + cfg.github_owner + "/" + cfg.github_repo
            + " bot=@" + cfg.bot_handle
            + " poll=" + std::to_string(cfg.poll_interval_sec) + "s");

        solvcon_bot::StateStore state(cfg.state_file);
        solvcon_bot::log_info("main", "state file locked: " + cfg.state_file);

        // First run: pin the comment cursor to now so we don't replay the
        // repo's entire comment history and answer year-old mentions.
        // Seeded 10 minutes in the past: GitHub stamps comments with ITS
        // clock, and a locally-seeded cursor ahead of that would filter
        // fresh mentions for as long as the skew lasts (the cursor only
        // moves forward). Ten minutes of replay is harmless — marker
        // dedupe suppresses any duplicate. Saved immediately — a crash
        // before the first successful tick would otherwise reopen the
        // full backlog.
        if (state.init_cursor_if_empty(now_iso8601_utc(/*offset_sec=*/-600)))
        {
            state.save();
            solvcon_bot::log_info("main",
                "first run: comment cursor seeded at "
                + state.cursor_updated_at()
                + " — comments older than that are ignored");
        }

        solvcon_bot::GithubClient gh(cfg);
        auto rv = solvcon_bot::make_reviewer(cfg);
        // Fail at startup — not on the first PR hours later — when the
        // reviewer's external dependency (codexmon + the agent CLI) is
        // missing or broken.
        rv->preflight();
        solvcon_bot::log_info("main",
            std::string("reviewer kind=") + solvcon_bot::to_string(cfg.reviewer_kind));
        solvcon_bot::LiveWatcherIo io(gh, *rv, state);
        solvcon_bot::Watcher watcher(cfg, io);

        install_signal_handlers();

        while (g_stop == 0)
        {
            try
            {
                watcher.tick();
            }
            catch (const solvcon_bot::GithubError & e)
            {
                // 401/403/422 from GitHub are not transient — keep
                // looping would just keep failing. Surface to operator.
                // A rate-limited 403 is the exception: GithubError marks
                // it transient and we keep polling. Mirrors watcher's
                // is_fatal_github().
                const bool fatal = e.status() == 401 || e.status() == 422
                    || (e.status() == 403 && !e.transient());
                if (fatal)
                {
                    solvcon_bot::log_error("main",
                        "fatal HTTP " + std::to_string(e.status()) + ": "
                        + e.what() + " — exiting non-zero so operator notices");
                    state.save();
                    return 1;
                }
                solvcon_bot::log_warn("main",
                    "tick GithubError " + std::to_string(e.status()) + ": "
                    + e.what());
            }
            catch (const std::exception & e)
            {
                solvcon_bot::log_warn("main",
                    std::string("tick error: ") + e.what());
            }
            // Sleep in short slices so signals interrupt within ~1s.
            for (int i = 0; i < cfg.poll_interval_sec && g_stop == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        solvcon_bot::log_info("main", "shutting down");
        state.save();
        return 0;
    }
    catch (const std::exception & e)
    {
        solvcon_bot::log_error("main", std::string("fatal: ") + e.what());
        return 1;
    }
}
