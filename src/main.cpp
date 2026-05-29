#include "config.hpp"
#include "github_client.hpp"
#include "github_types.hpp"
#include "reviewer.hpp"
#include "state_store.hpp"
#include "watcher.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>

namespace
{

volatile std::sig_atomic_t g_stop = 0;

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

void log_info(const std::string & msg)
{
    std::cerr << "[modmesh-bot] " << msg << std::endl;
}

} // namespace

int main()
{
    try
    {
        auto cfg = modmesh_bot::Config::from_env();
        log_info("starting modmesh-bot " MODMESH_BOT_VERSION
                 " repo=" + cfg.github_owner + "/" + cfg.github_repo
                 + " bot=@" + cfg.bot_handle
                 + " poll=" + std::to_string(cfg.poll_interval_sec) + "s");

        modmesh_bot::StateStore state(cfg.state_file);
        log_info("state file locked: " + cfg.state_file);

        modmesh_bot::GithubClient gh(cfg);
        modmesh_bot::Reviewer rv(cfg);
        modmesh_bot::LiveWatcherIo io(gh, rv, state);
        modmesh_bot::Watcher watcher(cfg, io);

        install_signal_handlers();

        while (g_stop == 0)
        {
            try
            {
                watcher.tick();
            }
            catch (const modmesh_bot::GithubError & e)
            {
                // 401/403/422 from GitHub are not transient — keep
                // looping would just keep failing. Surface to operator.
                if (e.status() == 401 || e.status() == 403 || e.status() == 422)
                {
                    std::cerr << "[modmesh-bot] fatal HTTP " << e.status()
                              << ": " << e.what()
                              << " — exiting non-zero so operator notices"
                              << std::endl;
                    state.save();
                    return 1;
                }
                std::cerr << "[modmesh-bot] tick GithubError "
                          << e.status() << ": " << e.what() << std::endl;
            }
            catch (const std::exception & e)
            {
                std::cerr << "[modmesh-bot] tick error: " << e.what()
                          << std::endl;
            }
            // Sleep in short slices so signals interrupt within ~1s.
            for (int i = 0; i < cfg.poll_interval_sec && g_stop == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        log_info("shutting down");
        state.save();
        return 0;
    }
    catch (const std::exception & e)
    {
        std::cerr << "[modmesh-bot] fatal: " << e.what() << std::endl;
        return 1;
    }
}
