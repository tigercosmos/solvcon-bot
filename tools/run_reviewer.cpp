// Standalone driver for the reviewer subsystem. Useful for iterating
// on REVIEWER_PROMPT / REVIEWER_MODEL / REVIEWER_EFFORT without
// posting a real GitHub PR.
//
// Usage:
//   cat my.diff | REVIEWER_KIND=claude ./build/run-reviewer
//   ./build/run-reviewer path/to.diff           (read file instead of stdin)
//   git diff main...HEAD | ./build/run-reviewer (review whatever is on the branch)
//
// Output goes to stdout (the same string the bot would post as a PR
// comment, minus the marker prefix). Diagnostics + errors go to
// stderr. Exit 0 on success, 1 on reviewer error, 2 on usage / setup
// error.
//
// Note: this tool intentionally does NOT enforce MAX_DIFF_BYTES — the
// bot caps at 200 KB before posting because PR comments would be
// unreadable otherwise, but for local iteration you usually want to
// feed the AI CLI whatever you can dig up. If the AI CLI rejects the
// size, that's its problem to surface.

#include "config.hpp"
#include "reviewer.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace
{

std::string read_stream(std::istream & is)
{
    std::ostringstream ss;
    ss << is.rdbuf();
    return ss.str();
}

void print_usage(std::ostream & os)
{
    os << "Usage: run-reviewer [DIFF_FILE]\n"
       << "\n"
       << "If DIFF_FILE is omitted, the diff is read from stdin.\n"
       << "\n"
       << "Reviewer is selected by the same env vars as the bot:\n"
       << "  REVIEWER_KIND          mock | claude | codex  (default: mock)\n"
       << "  REVIEWER_MODEL         passed to --model (kind-specific default)\n"
       << "  REVIEWER_EFFORT        CLAUDE_EFFORT (claude) or reasoning.effort (codex)\n"
       << "  REVIEWER_PROMPT        literal prompt text\n"
       << "  REVIEWER_PROMPT_FILE   path whose contents replace the default prompt\n"
       << "  REVIEWER_MOCK_EXIT_CODE non-zero forces mock to fail (e2e helper)\n"
       << "  REVIEWER_MOCK_OUTPUT   if set, mock prints this instead of echoing\n"
       << "  REVIEWER_ENV_PASSTHROUGH comma-separated env-var names\n"
       << "  REVIEWER_STREAM_IO     1/true/yes/on -> mirror child stdout/stderr live\n"
       << "  REVIEWER_HEARTBEAT_SEC seconds between 'still working' lines (0 = off)\n"
       << "  MAX_OUTPUT_BYTES       (default 60000)\n"
       << "  SUBPROCESS_TIMEOUT_SEC (default 300)\n"
       << "\n"
       << "This tool's defaults: REVIEWER_STREAM_IO=on, REVIEWER_HEARTBEAT_SEC=10.\n"
       << "Override either by setting the env var explicitly.\n"
       << "\n"
       << "Exit codes: 0 success, 1 reviewer error, 2 usage / setup error.\n";
}

} // namespace

int main(int argc, char ** argv)
{
    // --help / -h
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            print_usage(std::cout);
            return 0;
        }
    }

    modmesh_bot::Config cfg;
    std::string diff;
    std::unique_ptr<modmesh_bot::IReviewer> rv;
    const bool from_file = (argc > 1);
    try
    {
        // Reuse the production env loader for the reviewer block so
        // numeric validation, prompt-file size cap, effort whitelist,
        // and the REVIEWER_ARGV migration-error all behave identically
        // to the bot. After it runs, any env var the operator set
        // takes effect; below we set tool-specific defaults only when
        // the env left the field at Config's compiled-in default.
        modmesh_bot::apply_reviewer_env(cfg);

        // run-reviewer is interactive — humans watching want both
        // streaming and a heartbeat. apply_reviewer_env may have
        // already set them via env; only fill in when unset.
        if (!std::getenv("REVIEWER_STREAM_IO"))
        {
            cfg.reviewer_stream_io = true;
        }
        if (cfg.reviewer_heartbeat_sec == 0 && !std::getenv("REVIEWER_HEARTBEAT_SEC"))
        {
            cfg.reviewer_heartbeat_sec = 10;
        }

        if (from_file)
        {
            std::ifstream ifs(argv[1]);
            if (!ifs)
            {
                std::cerr << "run-reviewer: cannot open " << argv[1] << std::endl;
                return 2;
            }
            diff = read_stream(ifs);
        }
        else
        {
            diff = read_stream(std::cin);
        }

        if (diff.empty())
        {
            if (from_file)
            {
                std::cerr << "run-reviewer: file '" << argv[1] << "' is empty"
                          << std::endl;
            }
            else
            {
                std::cerr << "run-reviewer: no diff input (stdin was empty)"
                          << std::endl;
            }
            return 2;
        }

        // make_reviewer can throw (currently only for unhandled enum
        // values, but make sure it's inside the setup-error path).
        rv = modmesh_bot::make_reviewer(cfg);
    }
    catch (const std::exception & e)
    {
        std::cerr << "run-reviewer setup error: " << e.what() << std::endl;
        return 2;
    }

    std::cerr << "run-reviewer: kind=" << to_string(cfg.reviewer_kind)
              << " diff_bytes=" << diff.size()
              << " timeout=" << cfg.subprocess_timeout_sec << "s"
              << " stream_io=" << (cfg.reviewer_stream_io ? "on" : "off")
              << " heartbeat_sec=" << cfg.reviewer_heartbeat_sec
              << std::endl;
    if (cfg.reviewer_stream_io
        && (cfg.reviewer_kind == modmesh_bot::ReviewerKind::Claude
            || cfg.reviewer_kind == modmesh_bot::ReviewerKind::Codex))
    {
        std::cerr << "run-reviewer: stream_io is on — child stdout/stderr "
                     "will be mirrored to this stderr as it arrives. Note "
                     "that `claude -p` and `codex exec` buffer their full "
                     "response until completion, so the visible signal "
                     "during the wait is usually the heartbeat below; the "
                     "review body lands all at once at the end and is also "
                     "re-printed on stdout."
                  << std::endl;
    }

    const auto start = std::chrono::steady_clock::now();
    int rc = 0;
    std::string out;
    std::string err_msg;
    try
    {
        // The Reviewer instance owns the heartbeat thread internally
        // (see Heartbeat in reviewer.hpp). No more user-space thread
        // management here.
        out = rv->run(diff);
    }
    catch (const std::exception & e)
    {
        err_msg = e.what();
        rc = 1;
    }
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    if (rc == 0)
    {
        std::cerr << "run-reviewer: done in " << total << "s, "
                  << out.size() << " bytes" << std::endl;
        if (cfg.reviewer_stream_io)
        {
            std::cerr << "run-reviewer: ---- captured review body on stdout ----"
                      << std::endl;
        }
        std::cout << out;
        if (out.empty() || out.back() != '\n') std::cout << '\n';
        return 0;
    }
    else
    {
        std::cerr << "run-reviewer: error after " << total << "s: "
                  << err_msg << std::endl;
        return rc;
    }
}
