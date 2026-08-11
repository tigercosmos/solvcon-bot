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

// Post-change contents of one file touched by the diff, fetched from
// the PR head. Fenced into the review payload as extra context so the
// reviewer sees the whole file, not just +/-3 lines around each hunk.
struct ContextFile
{
    std::string path;
    std::string content;
};

// Everything the reviewer receives about one PR. Only `diff` is
// required; the rest enriches the payload when the watcher has it
// (run-reviewer and tests often ship a bare diff).
//
// All of it except the operator's prompt/guide is authored by the PR
// submitter and is therefore untrusted — assemble_review_stdin fences
// each piece accordingly.
struct ReviewRequest
{
    std::string diff;
    std::string pr_title;
    std::string pr_body;
    std::vector<ContextFile> context_files;
    // Human-readable "path (N bytes)" entries for diff file sections
    // dropped by trim_diff_to_budget. Shown to the reviewer (inside the
    // metadata fence — paths are attacker-chosen) so it knows coverage
    // is partial.
    std::vector<std::string> omitted_files;
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

    // Returns the review body for the given request. Throws
    // ReviewerError on timeout, non-zero exit, or spawn failure.
    virtual std::string run(const ReviewRequest & request) = 0;

    // Convenience: review a bare diff with no metadata or context
    // (run-reviewer tool, tests).
    std::string run(const std::string & diff)
    {
        ReviewRequest request;
        request.diff = diff;
        return run(request);
    }

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
//   <instruction paragraph naming every fence present>
//   BEGIN_PR_METADATA_<nonce>          (only when title/body/omissions exist)
//   PR title: ...
//   PR description:
//   ...
//   END_PR_METADATA_<nonce>
//   BEGIN_DIFF_<nonce>
//   <diff>
//   END_DIFF_<nonce>
//   BEGIN_FILE_<nonce> <path>          (one pair per context file)
//   <post-change file content>
//   END_FILE_<nonce>
//
// Every PR-author-controlled piece (metadata, diff, file contents)
// lives between nonce-suffixed fences that are unforgeable from inside
// the payload, and the instruction paragraph carries the fence names to
// the model at runtime so the scheme also works with an
// operator-supplied prompt (which cannot know the nonce). Lives in the
// header purely for testability.
//
// The nonce-less forms draw a fresh nonce per call; the explicit-nonce
// forms exist so tests can be byte-exact. Callers must not reuse a
// nonce across runs. The (prompt, diff) forms are shorthand for a
// request carrying only a diff.
std::string assemble_review_stdin(const std::string & prompt,
                                  const ReviewRequest & request);
std::string assemble_review_stdin(const std::string & prompt,
                                  const ReviewRequest & request,
                                  const std::string & nonce);
std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff);
std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff,
                                  const std::string & nonce);

// Prompt + operator guide -> effective prompt. The guide is trusted
// operator input, so it sits with the prompt, outside the fences. No-op
// when the guide is empty.
std::string compose_prompt_with_guide(const std::string & prompt,
                                      const std::string & guide);

// --- diff surgery ---------------------------------------------------------
//
// Pure helpers over a unified diff as served by GitHub's
// application/vnd.github.diff media type ("diff --git" section per
// file). No I/O; exposed for unit tests.

// One per-file section of the diff, byte-preserved.
struct DiffFileSection
{
    std::string path;  // new-side path; old-side path when deleted; ""
                       // when unparseable (quoted/binary oddities)
    bool deleted = false;
    std::string text;  // the full section, including its headers
};

struct DiffSplit
{
    std::string preamble; // bytes before the first "diff --git" (normally "")
    std::vector<DiffFileSection> files;
};

DiffSplit split_diff_by_file(const std::string & diff);

// Keep whole file sections, in order, while they fit `budget` bytes;
// drop (and record) sections that do not. A dropped section does not
// stop later, smaller sections from being kept.
struct DiffTrimResult
{
    std::string diff;                  // concatenated kept sections
    std::size_t kept = 0;              // number of kept file sections
    std::vector<std::string> omitted;  // "path (N bytes)" per dropped section
};

DiffTrimResult trim_diff_to_budget(const std::string & diff,
                                   std::size_t budget);

// Deduplicated new-side paths of files the diff touches, excluding
// deletions and sections whose path could not be parsed. These are the
// candidates for head-content context fetching.
std::vector<std::string> changed_paths(const std::string & diff);

// Review bodies are capped at MAX_OUTPUT_BYTES. When that fires we
// append a notice to the returned body so the posted comment doesn't
// look like a complete review that ends mid-sentence. No-op when not
// truncated.
std::string maybe_append_truncation_note(std::string body, bool truncated);

} // namespace solvcon_bot
