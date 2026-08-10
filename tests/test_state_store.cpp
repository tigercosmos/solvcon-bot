// Tests for src/state_store.{hpp,cpp}.
// Uses a fresh /tmp directory per test process to isolate state files.

#include "state_store.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{

int g_failed = 0;
int g_passed = 0;

#define EXPECT(expr)                                                         \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #expr << "\n";                              \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

#define EXPECT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _a = (a);                                                       \
        auto _b = (b);                                                       \
        if (!(_a == _b))                                                     \
        {                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__              \
                      << ": " << #a << " == " << #b << "\n";                 \
            ++g_failed;                                                      \
        }                                                                    \
        else { ++g_passed; }                                                 \
    } while (0)

using solvcon_bot::StateStore;

std::filesystem::path g_tmp_dir;

std::string fresh_state_path(const char * name)
{
    auto p = g_tmp_dir / (std::string(name) + ".state");
    // Make sure we don't see leftovers from a previous failed run.
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove(p.string() + ".tmp", ec);
    std::filesystem::remove(p.string() + ".lock", ec);
    return p.string();
}

// --- save/load round-trip ------------------------------------------------

void test_round_trip_basic()
{
    const std::string path = fresh_state_path("basic");
    {
        StateStore s(path);
        s.mark_reviewed(7);
        s.mark_reviewed(42);
        s.mark_handled(1001);
        s.advance_cursor("2026-04-01T00:00:01Z", 1001);
        s.save();
    }
    {
        StateStore s(path);
        EXPECT(s.reviewed(7));
        EXPECT(s.reviewed(42));
        EXPECT(!s.reviewed(99));
        EXPECT(s.handled(1001));
        EXPECT(!s.handled(0));
        EXPECT_EQ(s.cursor_updated_at(), std::string("2026-04-01T00:00:01Z"));
        EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(1001));
    }
}

void test_no_save_yet_is_empty()
{
    const std::string path = fresh_state_path("empty");
    StateStore s(path);
    EXPECT(!s.reviewed(1));
    EXPECT(!s.handled(1));
    EXPECT_EQ(s.cursor_updated_at(), std::string());
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(0));
}

void test_reopen_without_save_drops_in_memory_changes()
{
    const std::string path = fresh_state_path("nosave");
    {
        StateStore s(path);
        s.mark_reviewed(5);
        // No save().
    }
    {
        StateStore s(path);
        EXPECT(!s.reviewed(5));
    }
}

// --- cursor advance semantics --------------------------------------------

void test_cursor_advance_monotonic()
{
    const std::string path = fresh_state_path("cursor");
    StateStore s(path);
    EXPECT_EQ(s.cursor_updated_at(), std::string());

    s.advance_cursor("2026-04-01T00:00:01Z", 100);
    EXPECT_EQ(s.cursor_updated_at(), std::string("2026-04-01T00:00:01Z"));
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(100));

    // Older timestamp: ignored.
    s.advance_cursor("2026-03-01T00:00:00Z", 9999);
    EXPECT_EQ(s.cursor_updated_at(), std::string("2026-04-01T00:00:01Z"));
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(100));

    // Same timestamp, larger id: advances.
    s.advance_cursor("2026-04-01T00:00:01Z", 200);
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(200));

    // Same timestamp, smaller id: ignored.
    s.advance_cursor("2026-04-01T00:00:01Z", 150);
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(200));

    // Strictly newer timestamp: replaces id too.
    s.advance_cursor("2026-04-02T00:00:00Z", 50);
    EXPECT_EQ(s.cursor_updated_at(), std::string("2026-04-02T00:00:00Z"));
    EXPECT_EQ(s.cursor_id(), static_cast<std::int64_t>(50));
}

void test_is_at_or_before_cursor()
{
    const std::string path = fresh_state_path("cursorcmp");
    StateStore s(path);
    s.advance_cursor("2026-04-01T00:00:00Z", 500);

    // Older timestamp: at or before, regardless of id.
    EXPECT(s.is_at_or_before_cursor("2026-03-31T23:59:59Z", 99999));
    // Newer timestamp: after.
    EXPECT(!s.is_at_or_before_cursor("2026-04-01T00:00:01Z", 0));
    // Same timestamp, smaller id: at or before.
    EXPECT(s.is_at_or_before_cursor("2026-04-01T00:00:00Z", 400));
    // Same timestamp, equal id: at or before.
    EXPECT(s.is_at_or_before_cursor("2026-04-01T00:00:00Z", 500));
    // Same timestamp, larger id: after.
    EXPECT(!s.is_at_or_before_cursor("2026-04-01T00:00:00Z", 501));
}

// --- second-instance flock -----------------------------------------------

// Run a child process that tries to construct a StateStore on `path` and
// exits 0 on success / 1 on the expected "already locked" failure. Returns
// the child's exit status.
int child_try_lock(const std::string & path)
{
    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); std::exit(2); }
    if (pid == 0)
    {
        try
        {
            StateStore s(path);
            (void)s;
            std::_Exit(0); // got the lock — unexpected
        }
        catch (...)
        {
            std::_Exit(1); // failed to lock — expected
        }
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { std::perror("waitpid"); return -1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

void test_second_instance_in_child_process_blocked()
{
    const std::string path = fresh_state_path("xproc");
    StateStore s1(path);
    EXPECT_EQ(child_try_lock(path), 1);
}

void test_lock_released_lets_child_acquire()
{
    const std::string path = fresh_state_path("xproc_free");
    {
        StateStore s(path);
        (void)s;
    }
    EXPECT_EQ(child_try_lock(path), 0);
}

// --- lock survives a save (regression for M1 codex finding) -------------

void test_lock_survives_save()
{
    const std::string path = fresh_state_path("saved");

    StateStore s1(path);
    s1.mark_reviewed(11);
    s1.save();

    // The save() rewrites the state file via rename. The lock is on a
    // separate .lock file and must still be held — a child process
    // attempting to lock must fail.
    EXPECT_EQ(child_try_lock(path), 1);
}

// --- atomic save -------------------------------------------------------

void test_no_tmp_left_after_save()
{
    const std::string path = fresh_state_path("atomic");
    {
        StateStore s(path);
        s.mark_reviewed(1);
        s.save();
    }
    EXPECT(std::filesystem::exists(path));
    EXPECT(!std::filesystem::exists(path + ".tmp"));
}

// Force the tmp-open step to fail by pre-occupying the .tmp path with a
// directory. The save() call must throw and the previously-committed state
// file must remain intact and unchanged.
void test_save_failure_preserves_existing_state()
{
    const std::string path = fresh_state_path("atomic_fail");
    {
        StateStore s(path);
        s.mark_reviewed(7);
        s.save();
    }
    const std::string original = [&] {
        std::ifstream ifs(path);
        return std::string(std::istreambuf_iterator<char>(ifs), {});
    }();

    // Block the tmp path with a directory.
    const std::string tmp_path = path + ".tmp";
    std::filesystem::create_directory(tmp_path);

    StateStore s(path);
    s.mark_reviewed(42); // would extend the saved set, if save succeeded
    bool threw = false;
    try { s.save(); }
    catch (...) { threw = true; }
    EXPECT(threw);

    const std::string after = [&] {
        std::ifstream ifs(path);
        return std::string(std::istreambuf_iterator<char>(ifs), {});
    }();
    EXPECT_EQ(after, original);

    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
}

} // namespace

int main()
{
    // Per-process tmp dir so concurrent test runs don't collide.
    g_tmp_dir = std::filesystem::temp_directory_path()
        / ("solvcon-bot-state-" + std::to_string(getpid()));
    std::filesystem::create_directories(g_tmp_dir);

    test_round_trip_basic();
    test_no_save_yet_is_empty();
    test_reopen_without_save_drops_in_memory_changes();
    test_cursor_advance_monotonic();
    test_is_at_or_before_cursor();
    test_second_instance_in_child_process_blocked();
    test_lock_released_lets_child_acquire();
    test_lock_survives_save();
    test_no_tmp_left_after_save();
    test_save_failure_preserves_existing_state();

    std::error_code ec;
    std::filesystem::remove_all(g_tmp_dir, ec);

    std::cerr << "state_store tests: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
