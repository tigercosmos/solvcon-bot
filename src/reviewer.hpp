#pragma once

#include "config.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace modmesh_bot
{

class ReviewerError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Abstract code-reviewer interface. Concrete subclasses live in
// src/reviewer*.cpp and are constructed via make_reviewer().
class IReviewer
{
public:
    virtual ~IReviewer() = default;

    // Returns the reviewer's stdout (the review body) for the given
    // diff. Throws ReviewerError on timeout, non-zero exit, or spawn
    // failure.
    virtual std::string run(const std::string & diff) = 0;

    // Returns the reviewer's kind for logging.
    virtual ReviewerKind kind() const = 0;
};

// Factory: build the right reviewer for `cfg.reviewer_kind`. Throws
// std::runtime_error if the config is malformed.
std::unique_ptr<IReviewer> make_reviewer(const Config & cfg);

// Composed argv + stdin + env that a subprocess-backed reviewer would
// pass to run_subprocess. Test-only introspection — exposed so that
// unit tests can assert the CLI invocation without actually spawning
// the AI CLI.
struct ReviewerInvocation
{
    std::vector<std::string> argv;
    std::string stdin_input;
    std::vector<std::string> env_passthrough;
    std::vector<std::pair<std::string, std::string>> env_values;
};

// Built-in default review-prompt preamble. Reviewers prepend this
// (or the operator's override) to the diff before piping into the AI
// CLI.
std::string default_review_prompt();

// Assemble the full stdin buffer (prompt + BEGIN_DIFF + diff + END_DIFF)
// that the AI reviewers ship to their child process. Lives in the
// header purely for testability.
std::string assemble_review_stdin(const std::string & prompt,
                                  const std::string & diff);

// Reviewer subprocesses cap their stdout at MAX_OUTPUT_BYTES. When
// that fires we append a notice to the returned body so the posted
// comment doesn't look like a complete review that ends mid-sentence.
// No-op when not truncated.
std::string maybe_append_truncation_note(std::string body, bool truncated);

} // namespace modmesh_bot
