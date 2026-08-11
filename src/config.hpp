#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace solvcon_bot
{

// Which reviewer the factory should instantiate. The strings on the
// right are what REVIEWER_KIND accepts (case-insensitive). Every kind
// except Mock is one AgentReviewer that shells out to `codexmon run
// --agent <kind>`; adding a kind means one enum value plus one row in
// the agent table in src/reviewer_agent.cpp.
enum class ReviewerKind
{
    Mock,   // "mock"   — spawns /bin/cat; knobs for forced failure / fixed output
    Claude, // "claude" — codexmon run --agent claude  (claude -p)
    Codex,  // "codex"  — codexmon run --agent codex   (codex exec)
    Cursor, // "cursor" — codexmon run --agent cursor  (cursor-agent -p)
};

struct Config
{
    std::string github_token;
    std::string github_owner;
    std::string github_repo;
    std::string bot_handle;

    // Reviewer selection. The factory in src/reviewer.cpp dispatches
    // on `reviewer_kind`. Per-kind knobs live in the same struct so
    // there's exactly one place to read them.
    ReviewerKind reviewer_kind = ReviewerKind::Mock;
    std::string reviewer_model;   // --model NAME (claude/codex; empty -> CLI default)
    std::string reviewer_effort;  // CLAUDE_EFFORT env (claude) / reasoning.effort (codex)
    std::string reviewer_prompt;  // override built-in review prompt; empty -> default
    // Operator-authored, repo-specific review conventions appended to
    // the prompt (style rules, known invariants, what to skip). Env:
    // REVIEWER_GUIDE (literal) / REVIEWER_GUIDE_FILE (path), mutually
    // exclusive. Empty -> no guide section.
    std::string reviewer_guide;
    // For Mock only:
    int reviewer_mock_exit_code = 0;     // non-zero -> mock fails with this code
    std::string reviewer_mock_output;    // if non-empty, mock prints this instead of echoing

    int poll_interval_sec = 30;
    std::string state_file = "./solvcon-bot.state";

    // Review-payload budget for the diff. A diff over this size is
    // trimmed per file (whole file sections dropped, listed in the
    // posted comment) rather than reviewed partially mid-hunk.
    std::size_t max_diff_bytes = 200000;
    // Hard cap on the diff download itself. Only a diff bigger than
    // this is skipped outright. Env MAX_DIFF_FETCH_BYTES; when unset,
    // defaults to 5x max_diff_bytes. Must be >= max_diff_bytes.
    std::size_t max_diff_fetch_bytes = 1000000;
    std::size_t max_output_bytes = 60000;

    // Changed-file context: the watcher fetches the head-side contents
    // of files touched by the diff and appends them (fenced) to the
    // review payload. A file over the per-file cap, a binary file, or
    // anything past the total/count caps is silently skipped — context
    // is best-effort enrichment, never a reason to fail a review.
    // MAX_CONTEXT_TOTAL_BYTES=0 disables the feature.
    std::size_t max_context_file_bytes = 65536;   // MAX_CONTEXT_FILE_BYTES
    std::size_t max_context_total_bytes = 262144; // MAX_CONTEXT_TOTAL_BYTES
    std::size_t max_context_files = 10;           // MAX_CONTEXT_FILES

    // Reviewer wall-clock limit. Env SUBPROCESS_TIMEOUT_SEC, accepted
    // range 1..86400 (1 day). The upper bound is deliberate:
    // src/reviewer_agent.cpp adds a grace period to this value in
    // plain `int`, which would be signed overflow (UB) for values
    // near INT_MAX.
    int subprocess_timeout_sec = 300;

    // Per-request HTTP timeouts. Env HTTP_{CONNECT,READ,WRITE}_TIMEOUT_SEC,
    // accepted range 1..3600 (1 hour) — a REST call still stuck after
    // an hour is wedged, so a larger value only delays the failure.
    int http_connect_timeout_sec = 10;
    int http_read_timeout_sec = 30;
    int http_write_timeout_sec = 30;

    // GitHub REST API base URL. Default points at the public API.
    // Override via GITHUB_API_BASE_URL. The bot still appends paths
    // starting with `/repos/...`, so the override is only useful for
    // transport tests against a local server (e.g.
    // http://127.0.0.1:PORT). Full GitHub Enterprise support (path
    // prefix /api/v3) is not implemented.
    std::string github_api_base_url = "https://api.github.com";

    // Additional env var NAMES to pass through from the bot's
    // environment to the reviewer subprocess. PATH, HOME, LANG, TERM,
    // USER, LOGNAME are always passed; this list adds credentials like
    // ANTHROPIC_API_KEY, OPENAI_API_KEY, or anything else the AI CLI
    // needs. Read from REVIEWER_ENV_PASSTHROUGH as a comma-separated
    // list. Defaults to empty.
    std::vector<std::string> reviewer_env_passthrough;

    // When true, the codexmon subprocess's stdout + stderr are
    // mirrored to the bot's stderr in real time (codexmon writes its
    // heartbeats and live agent progress to stderr). Set by
    // run-reviewer unconditionally; the bot daemon honours the
    // REVIEWER_STREAM_IO env var (default off — operators with
    // structured logs usually don't want raw AI output interleaved).
    bool reviewer_stream_io = false;

    // Forwarded to `codexmon run --heartbeat N` (cadence of codexmon's
    // stderr heartbeat lines). 0 = don't pass the flag; codexmon's own
    // default (10s) applies.
    int reviewer_heartbeat_sec = 0;

    // Forwarded to `codexmon run --idle-timeout N`: kill the agent
    // after N idle seconds when nothing is in flight. -1 = don't pass
    // the flag (codexmon's default, 180s, applies); 0 = disable the
    // idle watchdog. Env: REVIEWER_IDLE_TIMEOUT_SEC.
    int reviewer_idle_timeout_sec = -1;

    // Path to the codexmon executable. Default resolves via PATH.
    // Env: CODEXMON_BIN. Install with scripts/install_codexmon.sh.
    std::string codexmon_bin = "codexmon";

    static Config from_env();
};

// Case-insensitive parse of the REVIEWER_KIND env value. Throws
// std::runtime_error if the value is not one of mock/claude/codex.
ReviewerKind parse_reviewer_kind(const std::string & s);

const char * to_string(ReviewerKind k);

// Load just the reviewer-related fields (kind, model, effort, prompt,
// mock knobs, env passthrough, the subprocess output cap, timeout)
// from the bot's environment into `cfg`. Used by Config::from_env()
// and by the standalone `run-reviewer` tool. Hard-fails if the
// removed REVIEWER_ARGV is still set, and validates effort + the
// prompt-file size cap the same way production does.
void apply_reviewer_env(Config & cfg);

inline std::ostream & operator<<(std::ostream & os, ReviewerKind k)
{
    return os << to_string(k);
}

} // namespace solvcon_bot
