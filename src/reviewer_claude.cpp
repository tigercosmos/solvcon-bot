#include "reviewer.hpp"

#include "subprocess.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace modmesh_bot
{

namespace
{

// Per-kind defaults. Empty REVIEWER_MODEL / REVIEWER_EFFORT in the
// config means "use these"; the operator can still override either by
// setting the matching env var to anything non-empty.
constexpr const char * kDefaultClaudeModel = "claude-opus-4-8";
constexpr const char * kDefaultClaudeEffort = "high";

class ClaudeReviewer final : public IReviewer
{
public:
    ClaudeReviewer(std::string model,
                   std::string effort,
                   std::string prompt,
                   std::size_t max_output_bytes,
                   int subprocess_timeout_sec,
                   std::vector<std::string> env_passthrough)
        : m_model(model.empty() ? std::string(kDefaultClaudeModel)
                                : std::move(model))
        , m_effort(effort.empty() ? std::string(kDefaultClaudeEffort)
                                  : std::move(effort))
        , m_prompt(prompt.empty() ? default_review_prompt() : std::move(prompt))
        , m_max_output_bytes(max_output_bytes)
        , m_subprocess_timeout_sec(subprocess_timeout_sec)
        , m_env_passthrough(std::move(env_passthrough))
    {
    }

    ReviewerKind kind() const override { return ReviewerKind::Claude; }

    // Test introspection: composed CLI invocation for the given diff.
    ReviewerInvocation build_invocation(const std::string & diff) const
    {
        ReviewerInvocation inv;
        inv.argv.push_back("claude");
        inv.argv.push_back("-p");
        if (!m_model.empty())
        {
            inv.argv.push_back("--model");
            inv.argv.push_back(m_model);
        }
        inv.stdin_input = assemble_review_stdin(m_prompt, diff);
        inv.env_passthrough = m_env_passthrough;
        // claude reads CLAUDE_EFFORT from its environment to bias the
        // depth of internal reasoning. Empty value = leave unset.
        if (!m_effort.empty())
        {
            inv.env_values.emplace_back("CLAUDE_EFFORT", m_effort);
        }
        return inv;
    }

    std::string run(const std::string & diff) override
    {
        const ReviewerInvocation inv = build_invocation(diff);
        RunResult r;
        try
        {
            r = run_subprocess(inv.argv,
                               inv.stdin_input,
                               m_max_output_bytes,
                               m_subprocess_timeout_sec,
                               inv.env_passthrough,
                               inv.env_values);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(
                std::string("claude reviewer setup failed: ") + e.what());
        }

        if (r.timed_out)
        {
            std::ostringstream oss;
            oss << "claude reviewer timed out after "
                << m_subprocess_timeout_sec << "s"
                << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
                << "):\n" << r.stderr_buf;
            throw ReviewerError(oss.str());
        }
        if (r.exit_status != 0)
        {
            std::ostringstream oss;
            oss << "claude reviewer exited " << r.exit_status
                << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
                << "):\n" << r.stderr_buf;
            throw ReviewerError(oss.str());
        }
        return maybe_append_truncation_note(
            std::move(r.stdout_buf), r.stdout_truncated);
    }

private:
    std::string m_model;
    std::string m_effort;
    std::string m_prompt;
    std::size_t m_max_output_bytes;
    int m_subprocess_timeout_sec;
    std::vector<std::string> m_env_passthrough;
};

} // namespace

std::unique_ptr<IReviewer> make_claude_reviewer(const Config & cfg)
{
    return std::make_unique<ClaudeReviewer>(
        cfg.reviewer_model,
        cfg.reviewer_effort,
        cfg.reviewer_prompt,
        cfg.max_output_bytes,
        cfg.subprocess_timeout_sec,
        cfg.reviewer_env_passthrough);
}

// Test-only — symbol kept narrow so tests can construct + introspect
// without spawning the CLI.
ReviewerInvocation claude_build_invocation_for_test(
    const Config & cfg, const std::string & diff)
{
    ClaudeReviewer r(cfg.reviewer_model,
                     cfg.reviewer_effort,
                     cfg.reviewer_prompt,
                     cfg.max_output_bytes,
                     cfg.subprocess_timeout_sec,
                     cfg.reviewer_env_passthrough);
    return r.build_invocation(diff);
}

} // namespace modmesh_bot
