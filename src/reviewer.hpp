#pragma once

#include "config.hpp"

#include <stdexcept>
#include <string>

namespace modmesh_bot
{

class ReviewerError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Reviewer
{
public:
    explicit Reviewer(const Config & cfg) : cfg_(cfg) {}

    // Spawn the configured AI CLI (cfg.reviewer_argv) with a sanitized
    // environment, feed `diff` on stdin, and return its captured stdout.
    // Throws ReviewerError if the process times out, exits non-zero, or
    // fails to spawn; stderr is folded into the error message in those
    // cases.
    std::string run(const std::string & diff) const;

private:
    Config cfg_;
};

} // namespace modmesh_bot
