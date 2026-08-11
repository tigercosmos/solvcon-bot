#pragma once

#include "config.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace solvcon_bot
{

class ReviewerError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Abstract code-reviewer interface. Two concrete implementations:
// MockReviewer (src/reviewer.cpp, no AI, for tests/e2e) and
// AgentReviewer (src/reviewer_agent.cpp), which delegates every AI
// kind — claude, codex, cursor — to the `codexmon` wrapper. codexmon
// owns process supervision: heartbeats, stall detection, timeouts,
// event-stream parsing, and result capture.
class IReviewer
{
public:
    virtual ~IReviewer() = default;

    // Returns the review body for the given diff. Throws
    // ReviewerError on timeout, non-zero exit, or spawn failure.
    virtual std::string run(const std::string & diff) = 0;

    // Returns the reviewer's kind for logging.
    virtual ReviewerKind kind() const = 0;

    // Startup check that the reviewer's external dependencies are
    // usable (e.g. `codexmon doctor`). Throws ReviewerError with an
    // actionable message when they are not. Default: no-op.
    virtual void preflight() {}
};

// Factory: build the right reviewer for `cfg.reviewer_kind`. Throws
// std::runtime_error if the config is malformed.
std::unique_ptr<IReviewer> make_reviewer(const Config & cfg);

// Composed argv + stdin + env that a subprocess-backed reviewer would
// pass to run_subprocess. Test-only introspection — exposed so that
// unit tests can assert the CLI invocation without actually spawning
// codexmon.
struct ReviewerInvocation
{
    std::vector<std::string> argv;
    std::string stdin_input;
    std::vector<std::string> env_passthrough;
    std::vector<std::pair<std::string, std::string>> env_values;
};

// The subset of codexmon's `--json` status record the bot consumes.
// `codexmon run --json` prints one such object on stdout when the job
// finishes; the full review text lives in `result_file`
// (result_preview is capped at 600 chars by codexmon).
struct CodexmonStatus
{
    std::string state;          // "completed" / "failed" / "stalled" / "cancelled"
    std::string error;          // codexmon's failure reason; empty on success
    std::string result_file;    // path to the full final agent message
    std::string result_preview; // truncated copy, for error context
};

// Parse the status JSON printed by `codexmon run --json`. Throws
// std::runtime_error on malformed input.
CodexmonStatus parse_codexmon_status(const std::string & json);

// Built-in default review-prompt preamble. Reviewers prepend this
// (or the operator's override) to the diff before piping into the AI
// CLI.
std::string default_review_prompt();

// A fresh 32-lowercase-hex-char (128-bit) random token used to name the
// diff fences for a single review run. Drawn from std::random_device —
// never from the clock — because a PR author who can predict the nonce
// can forge the closing fence and smuggle instructions past it.
std::string generate_diff_fence_nonce();

// Assemble the full stdin buffer that the AI reviewers ship to their
// child process:
//
//   <prompt>
//
//   <one instruction line naming both fences>
//   BEGIN_DIFF_<nonce>
//   <diff>
//   END_DIFF_<nonce>
//
// The nonce-suffixed fences are unforgeable from inside the diff, and
// the instruction line carries the fence names to the model at runtime
// so the scheme also works with an operator-supplied prompt (which
// cannot know the nonce). Lives in the header purely for testability.
//
// The two-argument form draws a fresh nonce per call; the three-argument
// form takes an explicit nonce so tests can be byte-exact. Callers must
// not reuse a nonce across runs.
std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff);
std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff,
                                  const std::string & nonce);

// Review bodies are capped at MAX_OUTPUT_BYTES. When that fires we
// append a notice to the returned body so the posted comment doesn't
// look like a complete review that ends mid-sentence. No-op when not
// truncated.
std::string maybe_append_truncation_note(std::string body, bool truncated);

} // namespace solvcon_bot
