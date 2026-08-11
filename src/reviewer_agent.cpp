// AgentReviewer: every AI reviewer kind (claude / codex / cursor) is
// one class that shells out to `codexmon run`. codexmon supervises the
// agent CLI — it injects each agent's event-stream flags, parses the
// stream, emits heartbeats to stderr, enforces idle/tool/wall
// watchdogs, and captures the final message — so the bot only
// composes argv, feeds the diff on stdin, and reads the result back.
//
// Adding a new agent that codexmon supports is one row in
// traits_for() plus one ReviewerKind enum value.

#include "reviewer.hpp"

#include "subprocess.hpp"

#include <solvcon/serialization/SerializableItem.hpp>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace solvcon_bot
{

namespace
{

namespace detail = solvcon::detail;

// Per-agent knobs. `default_model` is what the bot passes as --model
// when REVIEWER_MODEL is empty; an empty default means "let the agent
// (or codexmon) pick".
struct AgentTraits
{
    const char * agent;         // codexmon --agent value
    const char * default_model;
};

AgentTraits traits_for(ReviewerKind k)
{
    switch (k)
    {
    case ReviewerKind::Claude: return {"claude", "claude-opus-4-8"};
    case ReviewerKind::Codex:  return {"codex", "gpt-5.5"};
    case ReviewerKind::Cursor: return {"cursor", ""}; // codexmon defaults composer
    case ReviewerKind::Mock:   break;
    }
    throw std::runtime_error("AgentReviewer: unsupported ReviewerKind");
}

// How long the bot waits for the codexmon process itself beyond the
// wall timeout it hands codexmon. codexmon kills a stuck agent at
// --wall-timeout and needs a moment to reap + report; only if codexmon
// itself wedges does the bot's outer kill fire.
constexpr int kCodexmonGraceSec = 60;

// Capture cap for codexmon's OWN stdout/stderr (the status JSON plus
// diagnostics), independent of MAX_OUTPUT_BYTES. The operator's cap
// governs the review body read from result_file; applying it to the
// status capture would let a small-but-valid MAX_OUTPUT_BYTES truncate
// the status JSON and fail an otherwise successful review (found by
// Codex review).
constexpr std::size_t kStatusCaptureBytes = 1 << 20;

// Read up to `cap` bytes of `path`. Returns false if the file cannot
// be opened; sets `truncated` when the file is larger than the cap.
bool read_file_capped(const std::string & path,
                      std::size_t cap,
                      std::string & out,
                      bool & truncated)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.clear();
    truncated = false;
    char buf[4096];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0)
    {
        const std::size_t n = static_cast<std::size_t>(ifs.gcount());
        const std::size_t room = cap > out.size() ? cap - out.size() : 0;
        out.append(buf, std::min(n, room));
        if (n > room)
        {
            truncated = true;
            break;
        }
    }
    return true;
}

std::string tail_of(const std::string & s, std::size_t n = 2000)
{
    if (s.size() <= n) return s;
    return "..." + s.substr(s.size() - n);
}

class AgentReviewer final : public IReviewer
{
public:
    explicit AgentReviewer(const Config & cfg)
        : m_kind(cfg.reviewer_kind)
        , m_traits(traits_for(cfg.reviewer_kind))
        , m_codexmon_bin(cfg.codexmon_bin)
        , m_model(cfg.reviewer_model.empty() ? m_traits.default_model
                                             : cfg.reviewer_model)
        , m_effort(cfg.reviewer_effort.empty() ? "high" : cfg.reviewer_effort)
        , m_prompt(compose_prompt_with_guide(
              cfg.reviewer_prompt.empty() ? default_review_prompt()
                                          : cfg.reviewer_prompt,
              cfg.reviewer_guide))
        , m_max_output_bytes(cfg.max_output_bytes)
        , m_wall_timeout_sec(cfg.subprocess_timeout_sec)
        , m_idle_timeout_sec(cfg.reviewer_idle_timeout_sec)
        , m_heartbeat_sec(cfg.reviewer_heartbeat_sec)
        , m_env_passthrough(cfg.reviewer_env_passthrough)
        , m_stream_io(cfg.reviewer_stream_io)
    {
    }

    ReviewerKind kind() const override { return m_kind; }

    // Test introspection: composed codexmon invocation for the given
    // request. Monitor flags come before `--`; everything after it is
    // passed to the agent CLI verbatim.
    ReviewerInvocation build_invocation(const ReviewRequest & request) const
    {
        ReviewerInvocation inv;
        inv.argv = {m_codexmon_bin, "run", "--json", "--stdin"};
        inv.argv.push_back("--agent");
        inv.argv.push_back(m_traits.agent);
        inv.argv.push_back("--wall-timeout");
        inv.argv.push_back(std::to_string(m_wall_timeout_sec));
        if (m_idle_timeout_sec >= 0)
        {
            inv.argv.push_back("--idle-timeout");
            inv.argv.push_back(std::to_string(m_idle_timeout_sec));
        }
        if (m_heartbeat_sec > 0)
        {
            inv.argv.push_back("--heartbeat");
            inv.argv.push_back(std::to_string(m_heartbeat_sec));
        }
        inv.argv.push_back("--");
        append_agent_args(inv.argv);

        inv.stdin_input = assemble_review_stdin(m_prompt, request);
        inv.env_passthrough = m_env_passthrough;
        if (m_kind == ReviewerKind::Claude)
        {
            // claude reads CLAUDE_EFFORT from its environment; codexmon
            // passes its own environment through to the agent.
            inv.env_values.emplace_back("CLAUDE_EFFORT", m_effort);
        }
        return inv;
    }

    void preflight() override
    {
        std::vector<std::string> argv = {
            m_codexmon_bin, "doctor", "--agent", m_traits.agent, "--json"};
        RunResult r;
        try
        {
            r = run_subprocess(argv,
                               /*stdin_input=*/"",
                               /*max_output_bytes=*/65536,
                               /*timeout_seconds=*/60,
                               m_env_passthrough);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(install_hint(
                std::string("could not spawn codexmon: ") + e.what()));
        }
        if (r.timed_out)
        {
            throw ReviewerError(install_hint("codexmon doctor timed out"));
        }
        if (r.exit_status != 0)
        {
            throw ReviewerError(install_hint(
                "codexmon doctor exited " + std::to_string(r.exit_status)
                + "\nstdout:\n" + tail_of(r.stdout_buf)
                + "\nstderr:\n" + tail_of(r.stderr_buf)));
        }
    }

    std::string run(const ReviewRequest & request) override
    {
        const ReviewerInvocation inv = build_invocation(request);
        RunResult r;
        try
        {
            r = run_subprocess(inv.argv,
                               inv.stdin_input,
                               kStatusCaptureBytes,
                               m_wall_timeout_sec + kCodexmonGraceSec,
                               inv.env_passthrough,
                               inv.env_values,
                               m_stream_io);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(label() + " setup failed: " + e.what());
        }

        if (r.timed_out)
        {
            // codexmon itself failed to wrap up within its wall timeout
            // plus grace — its own watchdog should have fired first.
            throw ReviewerError(
                label() + ": codexmon did not return within "
                + std::to_string(m_wall_timeout_sec + kCodexmonGraceSec)
                + "s\nstderr:\n" + tail_of(r.stderr_buf));
        }

        CodexmonStatus st;
        try
        {
            st = parse_codexmon_status(r.stdout_buf);
        }
        catch (const std::exception & e)
        {
            throw ReviewerError(
                label() + ": cannot parse codexmon status (exit "
                + std::to_string(r.exit_status) + "): " + e.what()
                + "\nstdout:\n" + tail_of(r.stdout_buf)
                + "\nstderr:\n" + tail_of(r.stderr_buf));
        }

        if (r.exit_status != 0)
        {
            // 124 = stalled / wall timeout, 130 = cancelled, everything
            // else is the agent's own exit code forwarded by codexmon.
            std::ostringstream oss;
            oss << label() << " " << (st.state.empty() ? "failed" : st.state)
                << " (codexmon exit " << r.exit_status << ")";
            if (!st.error.empty()) oss << ": " << st.error;
            if (!st.result_preview.empty())
            {
                oss << "\nresult preview:\n" << st.result_preview;
            }
            oss << "\nstderr:\n" << tail_of(r.stderr_buf);
            throw ReviewerError(oss.str());
        }

        // Success: the full review body lives in result_file (the JSON
        // status carries only a 600-char preview).
        std::string body;
        bool truncated = false;
        if (st.result_file.empty()
            || !read_file_capped(st.result_file, m_max_output_bytes, body, truncated))
        {
            body = st.result_preview;
        }
        if (body.empty())
        {
            throw ReviewerError(
                label() + " completed but produced no result text"
                + " (result_file=" + st.result_file + ")");
        }
        return maybe_append_truncation_note(std::move(body), truncated);
    }

private:
    std::string label() const
    {
        return std::string(m_traits.agent) + " reviewer";
    }

    std::string install_hint(const std::string & why) const
    {
        return label() + " preflight failed: " + why
            + "\nInstall codexmon with scripts/install_codexmon.sh"
              " (or `make codexmon`), ensure it is on PATH or set"
              " CODEXMON_BIN, and check `"
            + m_codexmon_bin + " doctor --agent "
            + m_traits.agent + "`.";
    }

    // The agent-native argv appended after codexmon's `--`. codexmon
    // injects each agent's stream/result flags on top of these.
    void append_agent_args(std::vector<std::string> & argv) const
    {
        switch (m_kind)
        {
        case ReviewerKind::Claude:
        case ReviewerKind::Cursor:
            argv.push_back("-p"); // print mode; prompt arrives on stdin
            break;
        case ReviewerKind::Codex:
            argv.push_back("exec");
            argv.push_back("-c");
            argv.push_back("reasoning.effort=" + m_effort);
            break;
        case ReviewerKind::Mock:
            break; // unreachable; traits_for() already threw
        }
        if (!m_model.empty())
        {
            argv.push_back("--model");
            argv.push_back(m_model);
        }
    }

    ReviewerKind m_kind;
    AgentTraits m_traits;
    std::string m_codexmon_bin;
    std::string m_model;
    std::string m_effort;
    std::string m_prompt;
    std::size_t m_max_output_bytes;
    int m_wall_timeout_sec;
    int m_idle_timeout_sec;
    int m_heartbeat_sec;
    std::vector<std::string> m_env_passthrough;
    bool m_stream_io;
};

} // namespace

CodexmonStatus parse_codexmon_status(const std::string & json)
{
    CodexmonStatus st;
    auto node = std::make_unique<detail::JsonNode>(detail::JsonType::Object, json);
    const auto & obj = std::get<detail::JsonMap>(node->value);
    auto get_str = [&obj](const char * key, std::string & dst)
    {
        if (auto it = obj.find(key); it != obj.end())
        {
            detail::JsonHelper::from_json_string(it->second, dst);
        }
    };
    get_str("state", st.state);
    get_str("error", st.error);
    get_str("result_file", st.result_file);
    get_str("result_preview", st.result_preview);
    return st;
}

std::unique_ptr<IReviewer> make_agent_reviewer(const Config & cfg)
{
    return std::make_unique<AgentReviewer>(cfg);
}

// Test-only — construct + introspect without spawning codexmon.
ReviewerInvocation agent_build_invocation_for_test(
    const Config & cfg, const ReviewRequest & request)
{
    return AgentReviewer(cfg).build_invocation(request);
}

ReviewerInvocation agent_build_invocation_for_test(
    const Config & cfg, const std::string & diff)
{
    ReviewRequest request;
    request.diff = diff;
    return AgentReviewer(cfg).build_invocation(request);
}

} // namespace solvcon_bot
