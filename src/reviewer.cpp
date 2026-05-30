#include "reviewer.hpp"

#include "subprocess.hpp"

#include <sstream>
#include <string>

namespace modmesh_bot
{

std::string Reviewer::run(const std::string & diff) const
{
    if (cfg_.reviewer_argv.empty())
    {
        throw ReviewerError("reviewer_argv is empty");
    }
    const std::string & exe = cfg_.reviewer_argv.front();

    RunResult r;
    try
    {
        r = run_subprocess(
            cfg_.reviewer_argv,
            diff,
            cfg_.max_output_bytes,
            cfg_.subprocess_timeout_sec,
            cfg_.reviewer_env_passthrough);
    }
    catch (const std::exception & e)
    {
        // Pipe/fork failure or other set-up error. Convert to the
        // wrapper's contract type so callers don't need to know
        // about std::system_error / std::runtime_error.
        throw ReviewerError(std::string("reviewer setup failed: ") + e.what());
    }

    if (r.timed_out)
    {
        std::ostringstream oss;
        oss << "reviewer timed out after " << cfg_.subprocess_timeout_sec
            << "s: " << exe
            << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
            << "):\n" << r.stderr_buf;
        throw ReviewerError(oss.str());
    }
    if (r.exit_status != 0)
    {
        std::ostringstream oss;
        oss << "reviewer exited " << r.exit_status << ": " << exe
            << "\nstderr (truncated=" << (r.stderr_truncated ? "yes" : "no")
            << "):\n" << r.stderr_buf;
        throw ReviewerError(oss.str());
    }
    return r.stdout_buf;
}

} // namespace modmesh_bot
