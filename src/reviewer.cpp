#include "reviewer.hpp"

#include "subprocess.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace modmesh_bot
{

// Defined in reviewer_agent.cpp: the codexmon-backed reviewer that
// serves every AI kind.
std::unique_ptr<IReviewer> make_agent_reviewer(const Config & cfg);

namespace
{

// -------------------------------------------------------------------
// MockReviewer
//
// Spawns /bin/cat so the e2e subprocess path is exercised without
// codexmon or an AI CLI. With REVIEWER_MOCK_OUTPUT set, prints that
// string instead of echoing the diff. With REVIEWER_MOCK_EXIT_CODE
// non-zero, the spawn fails with that code (forced-failure tests).
// -------------------------------------------------------------------

class MockReviewer final : public IReviewer
{
public:
    explicit MockReviewer(const Config & cfg)
        : m_exit_code(cfg.reviewer_mock_exit_code)
        , m_output(cfg.reviewer_mock_output)
        , m_max_output_bytes(cfg.max_output_bytes)
        , m_subprocess_timeout_sec(cfg.subprocess_timeout_sec)
        , m_env_passthrough(cfg.reviewer_env_passthrough)
        , m_stream_io(cfg.reviewer_stream_io)
    {
    }

    ReviewerKind kind() const override { return ReviewerKind::Mock; }

    std::string run(const std::string & diff) override
    {
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
};

} // namespace

std::unique_ptr<IReviewer> make_reviewer(const Config & cfg)
{
    if (cfg.reviewer_kind == ReviewerKind::Mock)
    {
        return std::make_unique<MockReviewer>(cfg);
    }
    return make_agent_reviewer(cfg);
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

// Review bodies are capped at MAX_OUTPUT_BYTES. When that fires the
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

} // namespace modmesh_bot
