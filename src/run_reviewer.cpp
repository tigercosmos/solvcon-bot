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
       << "  MAX_OUTPUT_BYTES       (default 60000)\n"
       << "  SUBPROCESS_TIMEOUT_SEC (default 300)\n"
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
        // to the bot.
        modmesh_bot::apply_reviewer_env(cfg);

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
              << " diff_bytes=" << diff.size() << std::endl;

    try
    {
        std::string out = rv->run(diff);
        std::cout << out;
        if (out.empty() || out.back() != '\n') std::cout << '\n';
        return 0;
    }
    catch (const std::exception & e)
    {
        std::cerr << "run-reviewer error: " << e.what() << std::endl;
        return 1;
    }
}
