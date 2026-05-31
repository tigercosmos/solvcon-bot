#include "reviewer.hpp"

#include "claude_stream_parser.hpp"
#include "subprocess.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
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
                   std::vector<std::string> env_passthrough,
                   bool stream_io,
                   int heartbeat_sec)
        : m_model(model.empty() ? std::string(kDefaultClaudeModel)
                                : std::move(model))
        , m_effort(effort.empty() ? std::string(kDefaultClaudeEffort)
                                  : std::move(effort))
        , m_prompt(prompt.empty() ? default_review_prompt() : std::move(prompt))
        , m_max_output_bytes(max_output_bytes)
        , m_subprocess_timeout_sec(subprocess_timeout_sec)
        , m_env_passthrough(std::move(env_passthrough))
        , m_stream_io(stream_io)
        , m_heartbeat_sec(heartbeat_sec)
    {
    }

    ReviewerKind kind() const override { return ReviewerKind::Claude; }

    // Test introspection: composed CLI invocation for the given diff.
    ReviewerInvocation build_invocation(const std::string & diff) const
    {
        ReviewerInvocation inv;
        inv.argv.push_back("claude");
        inv.argv.push_back("-p");
        // We always ask for stream-json output, even when the operator
        // hasn't enabled REVIEWER_STREAM_IO. The plain `text` format
        // buffers the whole response until the end and yields no
        // progress signal at all; stream-json gives us one JSON event
        // per line and lets ClaudeStreamParser surface live activity
        // to stderr when the operator opted in. The bot daemon still
        // gets the same plain-markdown body via parser.final_text() —
        // the wire format change is internal.
        inv.argv.push_back("--output-format");
        inv.argv.push_back("stream-json");
        // --verbose is required by claude when --output-format is
        // stream-json (init/result events are gated behind it).
        inv.argv.push_back("--verbose");
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
        Heartbeat hb(m_heartbeat_sec, "modmesh-bot: claude reviewer");
        const ReviewerInvocation inv = build_invocation(diff);

        // The parser ALWAYS runs — claude now emits JSON events on
        // stdout, so this is how we extract the plain markdown body
        // we return to the caller. The progress sink is only installed
        // when the operator opted into streaming; with no sink the
        // parser is silent and the daemon's log stays clean.
        ClaudeStreamParser parser;
        if (m_stream_io)
        {
            parser.set_progress_sink([](std::string_view line)
            {
                // ::write skips stdio buffering so progress is visible
                // immediately; best-effort, ignore short writes.
                ssize_t left = static_cast<ssize_t>(line.size());
                const char * p = line.data();
                while (left > 0)
                {
                    ssize_t w = ::write(STDERR_FILENO, p, left);
                    if (w <= 0) break;
                    left -= w;
                    p += w;
                }
            });
        }
        StdoutChunkHandler chunk_handler =
            [&parser](std::string_view chunk) { parser.feed(chunk); };

        RunResult r;
        try
        {
            r = run_subprocess(inv.argv,
                               inv.stdin_input,
                               m_max_output_bytes,
                               m_subprocess_timeout_sec,
                               inv.env_passthrough,
                               inv.env_values,
                               m_stream_io,
                               chunk_handler);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(
                std::string("claude reviewer setup failed: ") + e.what());
        }
        // The child may close stdout without a final newline if it
        // crashes mid-event. flush() processes any trailing partial.
        parser.flush();

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
        if (parser.saw_error())
        {
            // Successful CLI invocation, but the model itself reported
            // failure (is_error=true in the result event). Surface the
            // captured body so the operator can see what went wrong.
            std::ostringstream oss;
            oss << "claude reviewer reported is_error=true; body:\n"
                << parser.final_text();
            throw ReviewerError(oss.str());
        }

        std::string body = parser.final_text();
        bool body_truncated = false;
        if (body.empty())
        {
            // Defensive fallback: parser couldn't extract a body (no
            // result event, no assistant text). Hand back the raw
            // stdout so the operator sees whatever claude actually
            // wrote, with the cap-truncation note if applicable.
            body = std::move(r.stdout_buf);
            body_truncated = r.stdout_truncated;
        }
        else if (body.size() > m_max_output_bytes)
        {
            // Enforce the cap on the EXTRACTED body, not the wrapping
            // JSON stream — that's what callers (GitHub PR comment)
            // care about.
            body.resize(m_max_output_bytes);
            body_truncated = true;
        }
        return maybe_append_truncation_note(std::move(body), body_truncated);
    }

private:
    std::string m_model;
    std::string m_effort;
    std::string m_prompt;
    std::size_t m_max_output_bytes;
    int m_subprocess_timeout_sec;
    std::vector<std::string> m_env_passthrough;
    bool m_stream_io;
    int m_heartbeat_sec;
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
        cfg.reviewer_env_passthrough,
        cfg.reviewer_stream_io,
        cfg.reviewer_heartbeat_sec);
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
                     cfg.reviewer_env_passthrough,
                     cfg.reviewer_stream_io,
                     cfg.reviewer_heartbeat_sec);
    return r.build_invocation(diff);
}

} // namespace modmesh_bot
