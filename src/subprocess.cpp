#include "subprocess.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

extern char ** environ;

namespace solvcon_bot
{

namespace
{

constexpr const char * kTruncatedFooter = "\n[truncated]\n";

void set_nonblock_cloexec(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0
        || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0
        || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
    {
        throw std::system_error(errno, std::generic_category(),
            "fcntl on pipe");
    }
}

void set_cloexec_only(int fd)
{
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
    {
        throw std::system_error(errno, std::generic_category(),
            "fcntl FD_CLOEXEC");
    }
}

// macOS does not have pipe2, so we use pipe + fcntl. parent_end_idx is
// which side of the pipe (0 or 1) the parent reads/writes — that side
// gets O_NONBLOCK so the parent's poll() loop never blocks. The
// child-side end stays in blocking mode so the child's stdio behaves
// normally; both ends get FD_CLOEXEC.
void make_pipe(int fds[2], int parent_end_idx)
{
    if (::pipe(fds) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    const int child_end_idx = 1 - parent_end_idx;
    try
    {
        set_nonblock_cloexec(fds[parent_end_idx]);
        set_cloexec_only(fds[child_end_idx]);
    }
    catch (...)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        throw;
    }
}

void close_if_open(int & fd)
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
}

// Append into `dst`, capped at `cap`. Sets truncated=true if any byte
// from `data` had to be dropped.
void append_capped(std::string & dst, bool & truncated,
                   const char * data, std::size_t len, std::size_t cap)
{
    if (truncated) return; // already capped; drop further bytes
    if (dst.size() + len > cap)
    {
        const std::size_t take = (cap > dst.size()) ? (cap - dst.size()) : 0;
        dst.append(data, take);
        truncated = true;
        dst.append(kTruncatedFooter);
    }
    else
    {
        dst.append(data, len);
    }
}

// Build a sanitized environ for the child: only PATH, HOME, LANG, TERM.
// Returns an array of "KEY=VAL" strings (storage owned by the returned
// vector) plus a nullptr-terminated argv-style pointer array.
struct ChildEnv
{
    std::vector<std::string> strings;
    std::vector<char *> ptrs;
};

ChildEnv build_sanitized_env(
    const std::vector<std::string> & extra_allowlist,
    const std::vector<std::pair<std::string, std::string>> & extra_values)
{
    ChildEnv e;
    // Default allowlist: process basics that every CLI expects + the
    // user-identity vars that macOS keychain APIs need (claude on
    // macOS, gh on Linux, etc.). Nothing here is a credential.
    const char * keep[] = {
        "PATH", "HOME", "LANG", "TERM", "USER", "LOGNAME"
    };

    // Build a "key -> entry index" map so an extra_value can override
    // a passthrough with the same key. We iterate the default keep
    // list first, then the passthrough list, then the explicit values.
    auto upsert = [&](const std::string & key, const std::string & val)
    {
        const std::string prefix = key + "=";
        for (auto & existing : e.strings)
        {
            // `>=` not `>`: an existing "KEY=" (empty value, e.g.
            // empty parent $LANG) IS a match for prefix "KEY=" and
            // must be overwritten, not duplicated.
            if (existing.size() >= prefix.size()
                && existing.compare(0, prefix.size(), prefix) == 0)
            {
                existing = key + "=" + val;
                return;
            }
        }
        e.strings.emplace_back(key + "=" + val);
    };

    for (const char * key : keep)
    {
        if (const char * v = std::getenv(key); v != nullptr)
        {
            upsert(key, v);
        }
    }
    for (const std::string & key : extra_allowlist)
    {
        if (key.empty()) continue;
        if (const char * v = std::getenv(key.c_str()); v != nullptr)
        {
            upsert(key, v);
        }
    }
    for (const auto & [key, val] : extra_values)
    {
        if (key.empty()) continue;
        upsert(key, val);
    }

    e.ptrs.reserve(e.strings.size() + 1);
    for (auto & s : e.strings) e.ptrs.push_back(s.data());
    e.ptrs.push_back(nullptr);
    return e;
}

std::vector<char *> build_argv_pointers(std::vector<std::string> & owned)
{
    std::vector<char *> ptrs;
    ptrs.reserve(owned.size() + 1);
    for (auto & s : owned) ptrs.push_back(s.data());
    ptrs.push_back(nullptr);
    return ptrs;
}

void killpg_terminate_then_kill(pid_t pgid, int wait_ms_before_kill)
{
    if (pgid <= 0) return;
    ::killpg(pgid, SIGTERM);
    // Unconditional grace period, then SIGKILL the whole group. Reaping
    // the direct child early is not enough — grandchildren or pg members
    // that ignore SIGTERM would otherwise survive.
    struct timespec ts{};
    ts.tv_sec = wait_ms_before_kill / 1000;
    ts.tv_nsec = (wait_ms_before_kill % 1000) * 1'000'000L;
    // main.cpp installs its SIGINT/SIGTERM handlers WITHOUT SA_RESTART,
    // so nanosleep can return early with EINTR. Sleep out the REMAINING
    // time instead, or a signal arriving here would cut the grace period
    // short and SIGKILL a child that was about to exit cleanly.
    struct timespec remaining{};
    while (::nanosleep(&ts, &remaining) != 0 && errno == EINTR)
    {
        ts = remaining;
    }
    ::killpg(pgid, SIGKILL);
}

} // namespace

RunResult run_subprocess(
    const std::vector<std::string> & argv,
    const std::string & stdin_input,
    std::size_t max_output_bytes,
    int timeout_seconds,
    const std::vector<std::string> & extra_env_allowlist,
    const std::vector<std::pair<std::string, std::string>> & extra_env_values,
    bool tee_child_io_to_stderr)
{
    if (argv.empty())
    {
        throw std::runtime_error("run_subprocess: argv is empty");
    }

    // Writing to a child that closed its stdin would otherwise raise
    // SIGPIPE and kill the bot. Installed once per process.
    static std::once_flag once;
    std::call_once(once, []() { ::signal(SIGPIPE, SIG_IGN); });

    // Build argv and the sanitized child environ in the parent, before
    // fork(). Doing it in the child would call non-async-signal-safe
    // routines (getenv, malloc, std::string ctors) between fork() and
    // execvp(), which is unsafe if the bot ever becomes multithreaded.
    ChildEnv env = build_sanitized_env(extra_env_allowlist, extra_env_values);
    std::vector<std::string> owned_argv = argv; // own the strings for execvp
    std::vector<char *> argv_ptrs = build_argv_pointers(owned_argv);

    int p_in[2]  = {-1, -1}; // parent writes p_in[1], child reads p_in[0]
    int p_out[2] = {-1, -1}; // child writes p_out[1], parent reads p_out[0]
    int p_err[2] = {-1, -1}; // child writes p_err[1], parent reads p_err[0]

    // For p_in: parent writes p_in[1] (non-block); child reads p_in[0].
    // For p_out: parent reads p_out[0] (non-block); child writes p_out[1].
    // For p_err: parent reads p_err[0] (non-block); child writes p_err[1].
    make_pipe(p_in, /*parent_end_idx=*/1);
    try { make_pipe(p_out, /*parent_end_idx=*/0); }
    catch (...) { ::close(p_in[0]); ::close(p_in[1]); throw; }
    try { make_pipe(p_err, /*parent_end_idx=*/0); }
    catch (...)
    {
        ::close(p_in[0]); ::close(p_in[1]);
        ::close(p_out[0]); ::close(p_out[1]);
        throw;
    }

    // We must clear FD_CLOEXEC on the child-side ends before exec so they
    // survive into the child as the standard streams.
    auto clear_cloexec = [](int fd) {
        int flags = ::fcntl(fd, F_GETFD, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    };

    pid_t pid = ::fork();
    if (pid < 0)
    {
        int err = errno;
        ::close(p_in[0]); ::close(p_in[1]);
        ::close(p_out[0]); ::close(p_out[1]);
        ::close(p_err[0]); ::close(p_err[1]);
        throw std::system_error(err, std::generic_category(), "fork");
    }

    if (pid == 0)
    {
        // CHILD
        // Become own process group leader so the parent can killpg us.
        ::setpgid(0, 0);

        // Wire up stdio.
        clear_cloexec(p_in[0]);
        clear_cloexec(p_out[1]);
        clear_cloexec(p_err[1]);
        if (::dup2(p_in[0], STDIN_FILENO) < 0
            || ::dup2(p_out[1], STDOUT_FILENO) < 0
            || ::dup2(p_err[1], STDERR_FILENO) < 0)
        {
            const char msg[] = "subprocess: dup2 failed\n";
            (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(127);
        }
        // Close pipe FDs in the child. (The CLOEXEC ones would close on
        // exec anyway, but we close eagerly so the child sees only stdio.)
        ::close(p_in[0]);  ::close(p_in[1]);
        ::close(p_out[0]); ::close(p_out[1]);
        ::close(p_err[0]); ::close(p_err[1]);

        // env.ptrs and argv_ptrs were built in the parent and survive
        // fork() into the child's copy-on-write memory. Only async-
        // signal-safe calls past this point.
        environ = env.ptrs.data();
        ::execvp(argv_ptrs[0], argv_ptrs.data());
        // If we got here, exec failed.
        const char msg[] = "subprocess: execvp failed\n";
        (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(127);
    }

    // PARENT
    // Make sure the child's process group is set up before any killpg.
    // setpgid in parent too is harmless and avoids races.
    ::setpgid(pid, pid);
    pid_t pgid = pid;

    // Close the child-side ends.
    ::close(p_in[0]);  p_in[0] = -1;
    ::close(p_out[1]); p_out[1] = -1;
    ::close(p_err[1]); p_err[1] = -1;

    int in_fd  = p_in[1];   // we WRITE to this
    int out_fd = p_out[0];  // we READ
    int err_fd = p_err[0];  // we READ

    RunResult result;
    std::size_t in_written = 0;
    bool stdin_done = stdin_input.empty();
    if (stdin_done) close_if_open(in_fd);

    bool out_eof = false;
    bool err_eof = false;

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(timeout_seconds);
    char buf[4096];

    while (!(out_eof && err_eof && stdin_done))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            result.timed_out = true;
            killpg_terminate_then_kill(pgid, /*wait_ms_before_kill=*/200);
            break;
        }
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        if (remaining_ms > 1000) remaining_ms = 1000; // tick at least 1Hz

        struct pollfd pfds[3];
        int nfds = 0;
        if (!stdin_done)
        {
            pfds[nfds].fd = in_fd;
            pfds[nfds].events = POLLOUT;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (!out_eof)
        {
            pfds[nfds].fd = out_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (!err_eof)
        {
            pfds[nfds].fd = err_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (nfds == 0) break;

        int rc = ::poll(pfds, nfds, static_cast<int>(remaining_ms));
        if (rc < 0)
        {
            if (errno == EINTR) continue;
            int err = errno;
            killpg_terminate_then_kill(pgid, 100);
            close_if_open(in_fd);
            close_if_open(out_fd);
            close_if_open(err_fd);
            throw std::system_error(err, std::generic_category(), "poll");
        }
        if (rc == 0) continue; // poll timeout slice; loop checks deadline

        for (int i = 0; i < nfds; ++i)
        {
            const int fd = pfds[i].fd;
            const short ev = pfds[i].revents;
            if (ev == 0) continue;

            if (fd == in_fd)
            {
                if (ev & (POLLOUT | POLLERR | POLLHUP))
                {
                    const std::size_t left = stdin_input.size() - in_written;
                    if (left == 0)
                    {
                        stdin_done = true;
                        close_if_open(in_fd);
                        continue;
                    }
                    ssize_t n = ::write(in_fd, stdin_input.data() + in_written, left);
                    if (n > 0)
                    {
                        in_written += static_cast<std::size_t>(n);
                        if (in_written == stdin_input.size())
                        {
                            stdin_done = true;
                            close_if_open(in_fd);
                        }
                    }
                    else if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        // EINTR: our signal handlers carry no SA_RESTART,
                        // so a signal can interrupt this write before any
                        // byte is transferred. Nothing was consumed, so
                        // just retry on the next poll pass rather than
                        // abandoning the child's stdin.
                        if (errno == EINTR) continue;
                        // EPIPE: child closed stdin. Stop writing.
                        stdin_done = true;
                        close_if_open(in_fd);
                    }
                }
            }
            else if (fd == out_fd || fd == err_fd)
            {
                const bool is_stdout = (fd == out_fd);
                std::string & dst = is_stdout ? result.stdout_buf : result.stderr_buf;
                bool & trunc = is_stdout ? result.stdout_truncated : result.stderr_truncated;
                bool & eof = is_stdout ? out_eof : err_eof;

                while (true)
                {
                    ssize_t n = ::read(fd, buf, sizeof(buf));
                    if (n > 0)
                    {
                        if (tee_child_io_to_stderr)
                        {
                            // Mirror to the parent's stderr in real
                            // time so a human-facing caller can watch
                            // the AI CLI's output land. We use ::write
                            // (not std::cerr) to skip stdio buffering;
                            // ignore short writes / errors — this is
                            // best-effort instrumentation. EINTR is the
                            // one error we do retry (handlers without
                            // SA_RESTART), so a stray SIGINT/SIGTERM does
                            // not silently swallow a chunk of the mirror.
                            ssize_t left = n;
                            const char * p = buf;
                            while (left > 0)
                            {
                                ssize_t w = ::write(STDERR_FILENO, p, left);
                                if (w < 0 && errno == EINTR) continue;
                                if (w <= 0) break;
                                left -= w;
                                p += w;
                            }
                        }
                        append_capped(dst, trunc, buf, static_cast<std::size_t>(n),
                                      max_output_bytes);
                        continue;
                    }
                    if (n == 0)
                    {
                        eof = true;
                        if (fd == out_fd) close_if_open(out_fd);
                        else              close_if_open(err_fd);
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    // Hard read error.
                    eof = true;
                    if (fd == out_fd) close_if_open(out_fd);
                    else              close_if_open(err_fd);
                    break;
                }
            }
        }
    }

    close_if_open(in_fd);
    close_if_open(out_fd);
    close_if_open(err_fd);

    int wait_status = 0;
    // If we already SIGKILL'd above, this still reaps without blocking.
    //
    // The loop is mandatory, not defensive: main.cpp installs its SIGINT
    // and SIGTERM handlers WITHOUT SA_RESTART, so a signal delivered
    // while we block here makes waitpid fail with EINTR. Returning on
    // that would leave the child UNREAPED and still RUNNING (no kill is
    // issued on this path) — we would strand a live reviewer CLI.
    while (::waitpid(pid, &wait_status, 0) < 0)
    {
        if (errno == EINTR) continue;
        // Any other errno should be impossible for a child we forked
        // ourselves and reap nowhere else (we never set SIGCHLD to
        // SIG_IGN). Be defensive anyway: never leave a running child
        // behind. Kill the whole group, then make one non-blocking
        // attempt to reap so we do not add a zombie either.
        killpg_terminate_then_kill(pgid, /*wait_ms_before_kill=*/100);
        int discard = 0;
        while (::waitpid(pid, &discard, WNOHANG) < 0 && errno == EINTR)
        {
            // retry only the interrupted call
        }
        result.exit_status = -1;
        return result;
    }

    if (WIFEXITED(wait_status))
    {
        result.exit_status = WEXITSTATUS(wait_status);
    }
    else if (WIFSIGNALED(wait_status))
    {
        result.exit_status = -1;
    }
    return result;
}

} // namespace solvcon_bot
