#pragma once

#include <cstddef>
#include <string>
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
// only PATH/HOME/LANG/TERM — no other variables are inherited.
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
    int timeout_seconds);

} // namespace modmesh_bot
