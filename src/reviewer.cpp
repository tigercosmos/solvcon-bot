#include "reviewer.hpp"

#include "subprocess.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace modmesh_bot
{

// Forward declarations of factory helpers defined in the per-kind
// translation units. The public factory below dispatches to one.
std::unique_ptr<IReviewer> make_mock_reviewer(const Config & cfg);
std::unique_ptr<IReviewer> make_claude_reviewer(const Config & cfg);
std::unique_ptr<IReviewer> make_codex_reviewer(const Config & cfg);

std::unique_ptr<IReviewer> make_reviewer(const Config & cfg)
{
    switch (cfg.reviewer_kind)
    {
    case ReviewerKind::Mock:   return make_mock_reviewer(cfg);
    case ReviewerKind::Claude: return make_claude_reviewer(cfg);
    case ReviewerKind::Codex:  return make_codex_reviewer(cfg);
    }
    throw std::runtime_error("unhandled ReviewerKind in make_reviewer");
}

std::string default_review_prompt()
{
    return
        "You are reviewing a GitHub pull request. The full diff is shown\n"
        "below between BEGIN_DIFF and END_DIFF lines.\n"
        "\n"
        "Write a concise code review. Focus on:\n"
        "- correctness or security bugs\n"
        "- design or API issues that matter\n"
        "- missing tests for new behavior\n"
        "\n"
        "Skip nits unless they hide a real problem. If the diff is fine\n"
        "as-is, say so plainly in one sentence. Use Markdown. No emojis.";
}

std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff)
{
    std::ostringstream oss;
    oss << prompt << "\n\nBEGIN_DIFF\n" << diff;
    if (!diff.empty() && diff.back() != '\n') oss << '\n';
    oss << "END_DIFF\n";
    return oss.str();
}

// Reviewers cap their stdout at MAX_OUTPUT_BYTES. When that fires the
// posted comment would otherwise look like a complete review that
// happens to end mid-sentence. Append a trailing notice so the PR
// reader can tell the bot's output is truncated.
std::string maybe_append_truncation_note(std::string body, bool truncated)
{
    if (!truncated) return body;
    if (!body.empty() && body.back() != '\n') body += '\n';
    body += "\n_modmesh-bot: reviewer output truncated at MAX_OUTPUT_BYTES._\n";
    return body;
}

// --- Heartbeat -----------------------------------------------------------

struct Heartbeat::Impl
{
    int interval_sec;
    std::string label;
    std::chrono::steady_clock::time_point start;
    std::atomic<bool> done{false};
    std::mutex cv_mtx;
    std::condition_variable cv;
    std::thread thread;
};

Heartbeat::Heartbeat(int interval_sec, std::string label)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->interval_sec = interval_sec;
    m_impl->label = std::move(label);
    m_impl->start = std::chrono::steady_clock::now();
    if (interval_sec <= 0) return;

    m_impl->thread = std::thread([impl = m_impl.get()]() {
        std::unique_lock<std::mutex> lk(impl->cv_mtx);
        while (!impl->done.load())
        {
            if (impl->cv.wait_for(
                    lk, std::chrono::seconds(impl->interval_sec),
                    [impl]() { return impl->done.load(); }))
            {
                break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - impl->start).count();
            // Use ::write to skip stdio buffering — both binaries
            // want the heartbeat to flush immediately. For the bot
            // daemon this lands directly in whatever stderr is
            // configured to (systemd journal / logfile / TTY).
            std::string line = impl->label + ": still working... "
                + std::to_string(elapsed) + "s elapsed\n";
            const char * p = line.data();
            ssize_t left = static_cast<ssize_t>(line.size());
            while (left > 0)
            {
                ssize_t w = ::write(STDERR_FILENO, p, left);
                if (w <= 0) break;
                left -= w; p += w;
            }
        }
    });
}

Heartbeat::~Heartbeat()
{
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->cv_mtx);
        m_impl->done.store(true);
    }
    m_impl->cv.notify_all();
    if (m_impl->thread.joinable()) m_impl->thread.join();
}

// -------------------------------------------------------------------
// MockReviewer
//
// Spawns /bin/cat so the e2e subprocess path is exercised. With
// REVIEWER_MOCK_OUTPUT set, prints that string instead of echoing the
// diff. With REVIEWER_MOCK_EXIT_CODE non-zero, the spawn fails with
// that code (used by the e2e-failure scenario).
// -------------------------------------------------------------------

namespace
{

class MockReviewer final : public IReviewer
{
public:
    MockReviewer(int exit_code,
                 std::string output,
                 std::size_t max_output_bytes,
                 int subprocess_timeout_sec,
                 std::vector<std::string> env_passthrough,
                 bool stream_io,
                 int heartbeat_sec)
        : m_exit_code(exit_code)
        , m_output(std::move(output))
        , m_max_output_bytes(max_output_bytes)
        , m_subprocess_timeout_sec(subprocess_timeout_sec)
        , m_env_passthrough(std::move(env_passthrough))
        , m_stream_io(stream_io)
        , m_heartbeat_sec(heartbeat_sec)
    {
    }

    ReviewerKind kind() const override { return ReviewerKind::Mock; }

    std::string run(const std::string & diff) override
    {
        // Streaming the mock's stdout is the heartbeat for free, so
        // skip the timer thread when stream_io is on. Likewise mock
        // is fast — heartbeat almost never fires for it — but we
        // still respect the operator's config for symmetry.
        Heartbeat hb(m_stream_io ? 0 : m_heartbeat_sec,
                     "modmesh-bot: mock reviewer");
        // Build the argv + stdin we want the child to see. For the
        // happy path (exit_code == 0, no override output), we use
        // /bin/cat and pipe the diff through. For an override output,
        // we pipe the override text instead of the diff. For forced
        // failure, we use /bin/sh -c "..." that writes a deterministic
        // stderr line and exits with the configured code.
        std::vector<std::string> argv;
        std::string stdin_payload;

        if (m_exit_code != 0)
        {
            // Bake the exit code into the shell command. Avoids
            // passing through env (which we don't want anyway) and
            // makes the command self-contained.
            std::ostringstream cmd;
            cmd << "echo 'mock reviewer simulated failure' 1>&2; exit "
                << m_exit_code;
            argv = {"/bin/sh", "-c", cmd.str()};
        }
        else if (!m_output.empty())
        {
            // Pipe the configured override text through cat. cat is
            // POSIX and present on every supported runner.
            argv = {"/bin/cat"};
            stdin_payload = m_output;
        }
        else
        {
            argv = {"/bin/cat"};
            stdin_payload = diff;
        }

        RunResult r;
        try
        {
            r = run_subprocess(argv,
                               stdin_payload,
                               m_max_output_bytes,
                               m_subprocess_timeout_sec,
                               m_env_passthrough,
                               /*extra_env_values=*/{},
                               m_stream_io);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(
                std::string("mock reviewer setup failed: ") + e.what());
        }

        if (r.timed_out)
        {
            throw ReviewerError("mock reviewer timed out");
        }
        if (r.exit_status != 0)
        {
            std::ostringstream oss;
            oss << "mock reviewer exited " << r.exit_status
                << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
                << "):\n" << r.stderr_buf;
            throw ReviewerError(oss.str());
        }
        return maybe_append_truncation_note(
            std::move(r.stdout_buf), r.stdout_truncated);
    }

private:
    int m_exit_code;
    std::string m_output;
    std::size_t m_max_output_bytes;
    int m_subprocess_timeout_sec;
    std::vector<std::string> m_env_passthrough;
    bool m_stream_io;
    int m_heartbeat_sec;
};

} // namespace

std::unique_ptr<IReviewer> make_mock_reviewer(const Config & cfg)
{
    return std::make_unique<MockReviewer>(
        cfg.reviewer_mock_exit_code,
        cfg.reviewer_mock_output,
        cfg.max_output_bytes,
        cfg.subprocess_timeout_sec,
        cfg.reviewer_env_passthrough,
        cfg.reviewer_stream_io,
        cfg.reviewer_heartbeat_sec);
}

} // namespace modmesh_bot
