#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace modmesh_bot
{

struct RunResult
{
    // Process exit code. -1 if the process was killed by signal (after
    // timeout or signal from outside).
    int exit_status = -1;
    bool timed_out = false;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    std::string stdout_buf;
    std::string stderr_buf;
};

// Spawn argv as a child, feed `stdin_input` on its stdin, return its
// captured stdout/stderr (each capped to max_output_bytes) and exit
// status. On timeout the child's process group is signaled SIGTERM and
// then SIGKILL. The child runs with a sanitized environment containing
// PATH, HOME, LANG, TERM, USER, LOGNAME by default — every other
// parent var (including GITHUB_TOKEN) is dropped.
//
// `extra_env_allowlist` adds further variable NAMES whose values are
// passed through from the parent if set. The operator uses this to
// give the reviewer CLI its API credentials, e.g. ANTHROPIC_API_KEY
// or OPENAI_API_KEY.
//
// `extra_env_values` adds explicit KEY=VALUE pairs the caller wants
// the child to see, regardless of what's in the parent env. Use this
// for reviewer-side knobs like CLAUDE_EFFORT that are configured by
// the operator at startup rather than inherited. Values here OVERRIDE
// allowlist-passthrough entries with the same key.
//
// The diff itself is never put on argv or env.
//
// argv[0] must name the executable; passed to execvp so PATH resolution
// happens.
//
// Throws std::runtime_error only for set-up failures (pipe/fork) before
// the child is reachable. Otherwise it always returns a RunResult.
RunResult run_subprocess(
    const std::vector<std::string> & argv,
    const std::string & stdin_input,
    std::size_t max_output_bytes,
    int timeout_seconds,
    const std::vector<std::string> & extra_env_allowlist = {},
    const std::vector<std::pair<std::string, std::string>> & extra_env_values = {});

} // namespace modmesh_bot
