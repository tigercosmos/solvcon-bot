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

class CodexReviewer final : public IReviewer
{
public:
    CodexReviewer(std::string model,
                  std::string effort,
                  std::string prompt,
                  std::size_t max_output_bytes,
                  int subprocess_timeout_sec,
                  std::vector<std::string> env_passthrough)
        : m_model(std::move(model))
        , m_effort(std::move(effort))
        , m_prompt(prompt.empty() ? default_review_prompt() : std::move(prompt))
        , m_max_output_bytes(max_output_bytes)
        , m_subprocess_timeout_sec(subprocess_timeout_sec)
        , m_env_passthrough(std::move(env_passthrough))
    {
    }

    ReviewerKind kind() const override { return ReviewerKind::Codex; }

    ReviewerInvocation build_invocation(const std::string & diff) const
    {
        ReviewerInvocation inv;
        inv.argv.push_back("codex");
        inv.argv.push_back("exec");
        if (!m_model.empty())
        {
            inv.argv.push_back("--model");
            inv.argv.push_back(m_model);
        }
        if (!m_effort.empty())
        {
            // codex's TOML-config override syntax: -c key=value.
            inv.argv.push_back("-c");
            inv.argv.push_back(std::string("reasoning.effort=") + m_effort);
        }
        inv.stdin_input = assemble_review_stdin(m_prompt, diff);
        inv.env_passthrough = m_env_passthrough;
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
                std::string("codex reviewer setup failed: ") + e.what());
        }

        if (r.timed_out)
        {
            std::ostringstream oss;
            oss << "codex reviewer timed out after "
                << m_subprocess_timeout_sec << "s"
                << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
                << "):\n" << r.stderr_buf;
            throw ReviewerError(oss.str());
        }
        if (r.exit_status != 0)
        {
            std::ostringstream oss;
            oss << "codex reviewer exited " << r.exit_status
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

std::unique_ptr<IReviewer> make_codex_reviewer(const Config & cfg)
{
    return std::make_unique<CodexReviewer>(
        cfg.reviewer_model,
        cfg.reviewer_effort,
        cfg.reviewer_prompt,
        cfg.max_output_bytes,
        cfg.subprocess_timeout_sec,
        cfg.reviewer_env_passthrough);
}

ReviewerInvocation codex_build_invocation_for_test(
    const Config & cfg, const std::string & diff)
{
    CodexReviewer r(cfg.reviewer_model,
                    cfg.reviewer_effort,
                    cfg.reviewer_prompt,
                    cfg.max_output_bytes,
                    cfg.subprocess_timeout_sec,
                    cfg.reviewer_env_passthrough);
    return r.build_invocation(diff);
}

} // namespace modmesh_bot
