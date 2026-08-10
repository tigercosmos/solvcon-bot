# solvcon-bot — Plan

A lightweight C++23 daemon that watches a GitHub repository's pull requests.
The bot runs an AI code review when either:

1. **Auto path:** the PR receives its first `APPROVED` review (once per PR), or
2. **Ping path:** a repo collaborator posts a comment that `@`-mentions the bot.

The review is produced by an AI CLI (`claude`, `codex`, …) run as a subprocess
on the PR diff, and the output is posted back as a PR comment.

---

## 1. Decisions (locked)

| | Decision | Detail |
|---|---|---|
| Q1 | HTTP library | **cpp-httplib**, pinned tag, vendored single header; HTTPS via OpenSSL 3 |
| Q2 | JSON library | **solvcon `serialization/`** via git submodule (pinned commit) |
| Reuse from solvcon | `base.hpp` + `serialization/SerializableItem.{hpp,cpp}` only | Adopt `SOLVCON_EXCEPT`, `is_specialization_of_v` |
| Q3 (auto) | Re-review policy | Auto-review **once per PR** on first `APPROVED` review |
| Q3a | Ping format | Word-boundary, case-insensitive `@<BOT_HANDLE>` in comment body (regex) |
| Q3b | Ping authorization | **Repo collaborators only** — verified via `GET /repos/{o}/{r}/collaborators/{user}` |
| Q3c | Ping pre-approval | Ping works at any time, independent of approval state |
| Q4 | Comment shape | Plain issue comment via `POST /issues/{n}/comments`, with a hidden HTML-comment marker |
| Q5 | Reviewer dispatch | `REVIEWER_KIND` enum (`mock`/`claude`/`codex`/`cursor`). `mock` is a local `/bin/cat` echo. Every AI kind is **one** `AgentReviewer` class that runs the agent CLI through **[codexmon](https://github.com/tigercosmos/codexmon)** (`codexmon run --json --stdin --agent <kind> -- <agent args>`); codexmon owns supervision (heartbeats, idle/tool/wall watchdogs, event-stream parsing, result capture). `execvp` directly — **no shell** in the production path. codexmon is installed pinned + checksum-verified via `scripts/install_codexmon.sh`. |
| Q6 | Poll interval | 30 s default |

**Total third-party C++ deps:** two (cpp-httplib + solvcon). **Runtime link
dep:** OpenSSL 3 (Homebrew `openssl@3` on macOS; distro `libssl-dev` on
Linux).

---

## 2. Scope

### In scope (v1)
- Single repository, configured at startup.
- Single instance only — guarded by `flock(2)` on the state file.
- Poll GitHub REST API every 30 s, **paginated** on every list endpoint via the `Link: rel="next"` header (`per_page=100`).
- **Auto path:** detect first `APPROVED` review on each open PR → run one review.
- **Ping path:** detect new collaborator comments containing `@<BOT_HANDLE>` on any open PR (not an Issue, not a closed PR) → run a review.
- Spawn the configured AI CLI — via the **codexmon** supervisor for all non-mock kinds — as a subprocess, with a **sanitized environment** (no `GITHUB_TOKEN`), feed it the PR diff on stdin, read the result back.
- Post the captured output as a PR comment, embedding a hidden marker so we can recognize our own comments and stay idempotent across crashes.
- Persist "already handled" state across restarts.

### Out of scope (v1)
- Webhook receiver.
- Multiple repositories.
- Re-review on push / new commits (force-push detection).
- Multi-instance coordination (we hard-fail if the state file is already locked).
- A web UI / dashboard.

---

## 3. Reuse of solvcon

| solvcon module | Verdict | Why |
|---|---|---|
| `base.hpp` | **REUSE** | Small, self-contained. `SOLVCON_EXCEPT`, `is_specialization_of_v`, stdlib includes. |
| `serialization/` | **REUSE** | `SerializableItem` + `JsonHelper<T>` + `JsonNode` parser. |
| `toggle/` | skip | Depends on `buffer/`; too heavy for env-var config. |
| `buffer/`, `mesh/`, `inout/`, `linalg/`, `math/`, `simd/`, `spacetime/`, `multidim/`, `onedim/`, `transform/`, `device/`, `grid.hpp`, `oasis/`, `pilot/`, `python/`, `universe/` | skip | Irrelevant. |

We **do not** rely on solvcon's internal CMake cache variables
(`SOLVCON_SERIALIZATION_SOURCES`); we list the two source files explicitly in
our own CMake (see §13).

---

## 4. Architecture

```
            ┌──────────────────────────────────────────────────┐
            │                     solvcon-bot                  │
            │                                                  │
   poll ───►│  PrWatcher                                       │
            │     ├─► auto path: list reviews (paginated)      │
            │     │       └─► APPROVED + state-gated           │
            │     │                                            │
            │     └─► ping path: list issue comments since cur │
            │             ├─► is_open_pr(n)?                   │
            │             ├─► author is collaborator?          │
            │             ├─► body matches @<BOT_HANDLE> rx?   │
            │             └─► state-gated by comment id        │
            │                                                  │
            │                       │ dispatch                 │
            │                       ▼                          │
            │  Reviewer ──► spawn codexmon (sanitized env) ───►│──► codexmon ──► claude / codex / cursor
            │                       │ (diff on stdin)          │
            │                       ▼                          │
            │  GithubClient (httplib::Client, timeouts) ──────►│──► api.github.com
            │     ├─► User-Agent: solvcon-bot/<ver>            │
            │     ├─► X-GitHub-Api-Version: 2022-11-28         │
            │     ├─► Authorization: Bearer …                  │
            │     ├─► Accept: application/vnd.github+json      │
            │     └─► pagination via Link: rel="next"          │
            │                       │                          │
            │                       ▼                          │
            │  solvcon::SerializableItem  (parse / emit)       │
            │                                                  │
            │  StateStore (flock'd)                            │
            │    • reviewed_prs : set<int>                     │
            │    • handled_comments : set<int64_t>             │
            │    • comment_cursor : (updated_at, id) HWM       │
            └──────────────────────────────────────────────────┘
```

---

## 5. Components & files

```
solvcon-bot/
├── CMakeLists.txt
├── plan.md
├── third_party/
│   ├── cpp-httplib/                vendored httplib.h @ pinned tag
│   └── solvcon/                    git submodule → solvcon/solvcon @ pinned sha
├── src/
│   ├── main.cpp
│   ├── config.hpp / .cpp           env-var driven config (incl. REVIEWER_KIND + per-kind knobs)
│   ├── subprocess.hpp / .cpp       fork/execve, nonblocking pipes, sanitized env, killpg
│   ├── github_types.hpp / .cpp     User, Review, PrSummary, PrDetail, IssueComment
│   ├── github_client.hpp / .cpp    httplib::Client wrapper: pagination, headers, timeouts
│   ├── state_store.hpp / .cpp      flock'd; reviewed-PR set + handled-comment set + cursor
│   ├── reviewer.hpp / .cpp         IReviewer interface + factory + MockReviewer
│   ├── reviewer_agent.cpp          AgentReviewer: claude/codex/cursor via `codexmon run`
│   ├── mention.hpp / .cpp          regex-based @-mention detector
│   └── watcher.hpp / .cpp          polling loop + auto/ping dispatch
└── tests/
    └── (TBD: state_store, github_types round-trip, subprocess timeout, mention regex)
```

---

## 6. GitHub API surface

All requests include:

| Header | Value |
|---|---|
| `Authorization` | `Bearer $GITHUB_TOKEN` |
| `Accept` | `application/vnd.github+json` (or `application/vnd.github.diff` for diffs) |
| `User-Agent` | `solvcon-bot/<version>` *(GitHub requires User-Agent)* |
| `X-GitHub-Api-Version` | `2022-11-28` |

| Purpose | Method | Endpoint | Notes |
|---|---|---|---|
| List open PRs | GET | `/repos/{o}/{r}/pulls?state=open&per_page=100` | paginate |
| List reviews on a PR | GET | `/repos/{o}/{r}/pulls/{n}/reviews?per_page=100` | paginate |
| List recent issue comments (repo-wide) | GET | `/repos/{o}/{r}/issues/comments?sort=updated&direction=asc&per_page=100&since={ISO8601}` | paginate; `since` filters by `updated_at` |
| Get issue/PR detail (is-it-a-PR + state) | GET | `/repos/{o}/{r}/issues/{n}` | `pull_request` field present ⇒ it's a PR; require `state == "open"` |
| Verify collaborator | GET | `/repos/{o}/{r}/collaborators/{user}` | 204 ⇒ yes, 404 ⇒ no, **403 ⇒ fatal auth error** |
| List PR comments (for idempotency marker check) | GET | `/repos/{o}/{r}/issues/{n}/comments?per_page=100` | paginate |
| Fetch PR diff | GET | `/repos/{o}/{r}/pulls/{n}` w/ `Accept: application/vnd.github.diff` | stream; abort once over `MAX_DIFF_BYTES` |
| Post comment | POST | `/repos/{o}/{r}/issues/{n}/comments` | body includes hidden marker |

**Pagination:** every list endpoint walks the `Link: rel="next"` header until
absent. cpp-httplib gives us response headers via `httplib::Result::headers`.

**Auth / token scopes:**
- Classic PAT: needs `repo` (private) or `public_repo` (public), **plus `read:org`** if the target is in an organization (collaborators endpoint requires it).
- Fine-grained: `pull-requests: write`, `contents: read`, `metadata: read`, **`members: read`** for org repos.
- A `403` from the collaborator endpoint is treated as a **hard auth error** (bot exits non-zero so the operator notices), not "not a collaborator".

---

## 7. Typed JSON wiring

```cpp
struct User : solvcon::SerializableItem {
    std::string login;
    /* to/from_json overrides */
};

struct Review : solvcon::SerializableItem {
    int64_t     id = 0;
    std::string state;          // "APPROVED", ...
    std::string submitted_at;
    User        user;
};

struct PrHead : solvcon::SerializableItem {
    std::string sha;
};

struct PrSummary : solvcon::SerializableItem {
    int         number = 0;
    PrHead      head;        // GitHub nests sha inside `head`
    std::string updated_at;
};

// /repos/{o}/{r}/issues/{n} — used to verify open-PR-ness
struct PrDetail : solvcon::SerializableItem {
    int         number = 0;
    std::string state;          // "open" / "closed"
    bool        is_pr = false;  // true iff "pull_request" key present
};

struct IssueComment : solvcon::SerializableItem {
    int64_t     id = 0;
    std::string body;
    std::string created_at;
    std::string updated_at;     // ← cursor key
    std::string issue_url;      // ".../issues/{n}"
    User        user;
};
```

Outgoing JSON (e.g. `{"body": "..."}`) emitted via
`solvcon::detail::JsonHelper::to_json_string`.

---

## 8. Configuration

| Var | Required | Default | Purpose |
|---|---|---|---|
| `GITHUB_TOKEN` | yes | — | API auth |
| `GITHUB_REPO` | yes | — | e.g. `tigercosmos/solvcon` |
| `BOT_HANDLE` | yes | — | bot's GitHub username, no `@` |
| `REVIEWER_KIND` | no | `mock` | One of `mock`/`claude`/`codex`/`cursor`. `mock` is local; all others run through codexmon. |
| `CODEXMON_BIN` | no | `codexmon` | Path to the codexmon executable (default PATH lookup). `scripts/install_codexmon.sh` installs the pinned release. |
| `REVIEWER_MODEL` | no | per-kind | Forwarded as `--model` to the AI CLI. Empty falls back to `claude-opus-4-8` (kind=claude), `gpt-5.5` (kind=codex), or codexmon's composer default (kind=cursor). |
| `REVIEWER_EFFORT` | no | `high` | For `claude`: exported as `CLAUDE_EFFORT` (codexmon passes env through). For `codex`: passed as `-c reasoning.effort=$value`. Ignored for `cursor`. |
| `REVIEWER_IDLE_TIMEOUT_SEC` | no | unset | Forwarded as `codexmon run --idle-timeout`. Unset keeps codexmon's 180 s default; `0` disables the idle watchdog. |
| `REVIEWER_PROMPT` | no | (built-in) | Literal prompt that replaces the built-in review preamble. Mutually exclusive with `REVIEWER_PROMPT_FILE`. |
| `REVIEWER_PROMPT_FILE` | no | (built-in) | Path whose contents replace the built-in preamble. |
| `REVIEWER_MOCK_EXIT_CODE` | no | `0` | Mock-only. Non-zero makes the mock exit with this code (forced-failure tests). |
| `REVIEWER_MOCK_OUTPUT` | no | — | Mock-only. If set, the mock prints this string instead of echoing the diff. |
| `POLL_INTERVAL_SEC` | no | `30` | polling cadence |
| `STATE_FILE` | no | `./solvcon-bot.state` | persistence + lock file |
| `MAX_DIFF_BYTES` | no | `200000` | abort diff download past this |
| `MAX_OUTPUT_BYTES` | no | `60000` | cap AI stdout (and stderr) capture |
| `SUBPROCESS_TIMEOUT_SEC` | no | `300` | AI CLI hard timeout |
| `HTTP_CONNECT_TIMEOUT_SEC` | no | `10` | cpp-httplib `set_connection_timeout` |
| `HTTP_READ_TIMEOUT_SEC` | no | `30` | cpp-httplib `set_read_timeout` |
| `HTTP_WRITE_TIMEOUT_SEC` | no | `30` | cpp-httplib `set_write_timeout` |
| `REVIEWER_ENV_PASSTHROUGH` | no | empty | Comma-separated list of env-var names to pass through from the bot to the reviewer subprocess on top of the defaults (`PATH`/`HOME`/`LANG`/`TERM`/`USER`/`LOGNAME`). Typically `ANTHROPIC_API_KEY` and/or `OPENAI_API_KEY`. |

The reviewer is selected by `REVIEWER_KIND` and constructed by the
factory in `src/reviewer.cpp`. `mock` is `MockReviewer`; every AI kind
is the single `AgentReviewer` (`src/reviewer_agent.cpp`), which
composes

```
codexmon run --json --stdin --agent <kind> --wall-timeout $SUBPROCESS_TIMEOUT_SEC \
    [--idle-timeout N] [--heartbeat N] -- <agent-native args>
```

and feeds prompt + diff on stdin. codexmon injects the agent's
event-stream flags, supervises the run (heartbeats to stderr,
idle/tool/wall watchdogs), and prints one status JSON on stdout when
done; the full review body is read from the `result_file` path it
reports. Exit codes: `0` completed, `124` stalled/wall-timeout, `130`
cancelled, anything else forwards the agent's own exit. The bot's
outer subprocess timeout is wall+60 s — a backstop that fires only if
codexmon itself wedges. At startup `AgentReviewer::preflight()` runs
`codexmon doctor --agent <kind> --json` so a missing codexmon or agent
CLI fails immediately with an install hint. Adding a new agent that
codexmon supports = one `ReviewerKind` value + one row in the agent
table. `execvp` directly — no `/bin/sh -c` in the production path; the
mock kind opts into `/bin/sh -c "exit N"` only when explicitly asked
to simulate a reviewer crash, and that command is fully self-contained
(the diff never appears in argv).

---

## 9. Control flow

```
startup:
    state.lock(STATE_FILE)              # flock(LOCK_EX | LOCK_NB); exit if held

loop forever:
    try:
        run_auto_path()
        run_ping_path()
        failures = 0
    except: failures++; sleep(backoff(failures))    # capped at 10 min
    sleep(POLL_INTERVAL_SEC)


def run_auto_path():
    for pr in github.list_open_prs():                       # paginated
        if state.reviewed(pr.number): continue
        for r in github.list_reviews(pr.number):            # paginated
            if r.state == "APPROVED":
                if not already_posted_marker(pr, "auto"):
                    dispatch_review(pr, source="auto")
                state.mark_reviewed(pr.number)              # written after dispatch
                break


def run_ping_path():
    comments = github.list_issue_comments(since=state.cursor_updated_at)  # paginated, sort=updated
    for c in comments:
        decided = False
        try:
            if state.handled(c.id):                          decided = True; continue
            if eq_login(c.user.login, BOT_HANDLE):           decided = True; continue   # ignore self
            if not mention.matches(c.body, BOT_HANDLE):      decided = True; continue
            pr = github.get_issue_detail(parse_n(c.issue_url))
            if not pr.is_pr or pr.state != "open":           decided = True; continue
            if not github.is_collaborator(c.user.login):     decided = True; continue   # 404 only; 403 = fatal
            if not already_posted_marker(pr.number, "ping", c.id):
                dispatch_review_for(pr.number, source="ping", trigger_comment_id=c.id)
            decided = True
        finally:
            if decided:
                state.mark_handled(c.id)
                state.advance_cursor(c.updated_at, c.id)     # advance only after durable decision


def dispatch_review(pr, source, trigger_comment_id=None):
    diff = github.stream_diff(pr.number, max_bytes=MAX_DIFF_BYTES)
    if diff.truncated:
        github.post_comment(pr.number,
            with_marker(source, pr, trigger_comment_id, "(diff exceeds MAX_DIFF_BYTES — skipped)"))
        return
    output = reviewer.run(diff.body)
    github.post_comment(pr.number, with_marker(source, pr, trigger_comment_id, format(output)))


def already_posted_marker(pr, source, trigger_comment_id=None):
    marker = build_marker(source, pr, trigger_comment_id)
    for c in github.list_pr_comments(pr.number):
        if eq_login(c.user.login, BOT_HANDLE) and marker in c.body:
            return True
    return False
```

**Cursor semantics.** `since` on `issues/comments` filters by `updated_at`,
and we sort `sort=updated&direction=asc`. The high-water mark we persist is
the tuple `(updated_at, id)` — we treat any comment with
`(updated_at, id) <= cursor` as already seen, breaking ties by id. Cursor
only advances after the comment is **durably** marked handled (or
deliberately skipped). A transient API failure on the collaborator check
leaves the comment for the next tick.

**Idempotency marker.** Every comment we post starts with an HTML comment:
```
<!-- solvcon-bot/<ver> source=auto|ping pr=<n> trigger=<comment_id or "first-approval"> -->
```
Before posting, we list the PR's comments and look for a matching marker
authored by `BOT_HANDLE`. This makes the post step idempotent even if the
process dies between `post_comment` and `state.mark_reviewed`.

**Mention matching.** `mention.matches(body, handle)` uses the regex
`(?i)(?<![A-Za-z0-9-])@<handle>(?![A-Za-z0-9-])` (GitHub usernames are
case-insensitive, `[A-Za-z0-9-]` charset, max 39 chars). Login comparisons
(`eq_login`) lowercase both sides before comparing.

---

## 10. Subprocess details

- `pipe2(O_NONBLOCK | O_CLOEXEC)` for stdin/stdout/stderr.
- `fork()` + `setpgid(0,0)` in the child, then `execve` (not `execvp`) with a sanitized environment containing only: `PATH`, `HOME`, `LANG`, `TERM`, `USER`, `LOGNAME` plus whatever names the operator lists in `REVIEWER_ENV_PASSTHROUGH` (typically `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` so the AI CLI can authenticate). **`GITHUB_TOKEN` is explicitly not passed unless the operator names it.** `USER`/`LOGNAME` are in defaults because macOS keychain APIs (used by `claude`) require them.
- Parent runs a single `poll(2)` loop: writes diff to child stdin (handles `EAGAIN`), drains stdout/stderr into capped buffers (`MAX_OUTPUT_BYTES` each — further bytes discarded with a "[truncated]" footer).
- Hard timeout (`SUBPROCESS_TIMEOUT_SEC`). On timeout: `killpg(pgid, SIGTERM)`, wait briefly, then `killpg(pgid, SIGKILL)`.
- Exit status non-zero ⇒ log stderr buffer, do **not** post a comment, do **not** mark state.
- The diff is untrusted input from PR authors; we never echo it into argv or env, only into the child's stdin. Any prompt-injection risk lives entirely inside the AI CLI's own context.

---

## 11. HTTP client details

- One `httplib::Client cli("https://api.github.com")` for the bot's lifetime.
- `cli.set_connection_timeout(HTTP_CONNECT_TIMEOUT_SEC)`, `set_read_timeout(...)`, `set_write_timeout(...)`.
- `cli.enable_server_certificate_verification(true)` (default).
- Default headers (`User-Agent`, `Accept`, `Authorization`, `X-GitHub-Api-Version`) are attached to every request via a small wrapper, not by the caller.
- `stream_diff(n)` uses `httplib::Client::Get(path, headers, content_receiver)` where the receiver returns `false` to abort once accumulated bytes exceed `MAX_DIFF_BYTES`; the result is `{body, truncated}`.
- 5xx and 429 are retried with exponential backoff honoring `Retry-After`. 4xx (other than 404 on collaborator check) is treated as fatal — the bot logs and exits non-zero.
- Pagination: `walk_pages(path)` consumes `Link: rel="next"` until exhausted, yielding parsed items.

---

## 12. Idempotency & concurrency

- **Single instance:** `flock(state_fd, LOCK_EX | LOCK_NB)` at startup. If held, exit immediately with a clear message.
- **Atomic state writes:** write to `STATE_FILE.tmp`, `fsync`, `rename(2)`.
- **Marker-based dedupe:** before posting, scan existing PR comments for our marker; skip post if present. This protects against crashes between `post_comment` and `state.mark_*`.

---

## 13. Build

- CMake ≥ 3.20, `cxx_std_23`, `-Wall -Wextra -Werror`.
- `find_package(OpenSSL 3 REQUIRED)`.
- **solvcon files listed explicitly** (no reliance on solvcon's cache vars):
  ```cmake
  set(SOLVCON_FILES
      third_party/solvcon/cpp/solvcon/serialization/SerializableItem.cpp
      third_party/solvcon/cpp/solvcon/serialization/SerializableItem.hpp)
  target_sources(solvcon-bot PRIVATE ${SOLVCON_FILES})
  target_include_directories(solvcon-bot SYSTEM PRIVATE
      third_party/solvcon/cpp
      third_party/cpp-httplib)
  target_compile_definitions(solvcon-bot PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(solvcon-bot PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  ```
  `SYSTEM` keeps third-party warnings out of our `-Werror` build.
- **cpp-httplib pinned:** vendored from a specific release tag (e.g. `v0.18.5`), checked in with a `THIRD_PARTY_VERSIONS` note. Not pulled from `master`.
- **solvcon pinned:** submodule pinned at a specific commit SHA; CI verifies the pinned SHA didn't drift.

**macOS / OpenSSL caveat.** macOS ships LibreSSL headers via the SDK but no
linkable OpenSSL. Operator must `brew install openssl@3` and either set
`OPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` before invoking CMake, or rely
on a `find_package` hint we add. cpp-httplib's OpenSSL backend uses standard
OpenSSL APIs — no Apple Security framework linkage needed at this version,
but the operator must point the build at the brew install.

Bootstrap for a fresh checkout (macOS shown):
```
brew install openssl@3
git submodule add https://github.com/solvcon/solvcon third_party/solvcon
git -C third_party/solvcon checkout <pinned-sha>
git submodule update --init --recursive
mkdir -p third_party/cpp-httplib
curl -L -o third_party/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h
OPENSSL_ROOT_DIR=$(brew --prefix openssl@3) cmake -S . -B build
cmake --build build
```

---

## 14. Milestones

1. **M1 — skeleton & config:** submodule + vendored pinned httplib, CMake builds an empty binary (macOS + Linux), `Config::from_env` (incl. `BOT_HANDLE`, `REVIEWER_KIND` enum + per-kind knobs), `StateStore` opens with `flock`, main loop logs each tick.
2. **M2 — github_types:** `User`, `Review`, `PrSummary`, `PrDetail`, `IssueComment` as `SerializableItem`s; round-trip unit tests against captured real-response JSON.
3. **M3 — GithubClient:** typed list endpoints with `walk_pages`, default headers, timeouts, `stream_diff` with abort, `get_issue_detail`, `is_collaborator` (404 vs 403 handling), `post_comment`. Smoke-test against a real test repo.
4. **M4 — StateStore:** reviewed-PR set + handled-comment set + `(updated_at, id)` cursor; `flock` + atomic rename; round-trip tests.
5. **M5 — Subprocess + Reviewer:** `pipe2` + `fork` + `execve` with sanitized env, single `poll` loop, output caps, `killpg` on timeout. Tests using `/bin/cat` and `/usr/bin/yes` for echo/timeout/deadlock cases.
6. **M6 — Mention matcher:** the regex, case-insensitive login eq. Tests cover `@bot`, `@bot-user-2`, `@botand`, `email@bot.com`, code-block contents.
7. **M7 — Watcher auto path:** approval detection → marker dedupe → dispatch → comment, end-to-end on throwaway PR.
8. **M8 — Watcher ping path:** mention detection → is-PR check → collaborator check → dispatch; end-to-end with collaborator, non-collaborator, and Issue (not PR).
9. **M9 — Polish:** structured logging, `SIGINT`/`SIGTERM` clean exit, exponential backoff, `Retry-After`, README.

---

## 15. What Codex's review changed

This plan was reviewed by `codex exec`. Substantive fixes incorporated:

1. **Pagination** added to every list endpoint (§6, §9).
2. **Cursor** moved from `created_at` → `updated_at`, sorted `updated`, tie-broken by id (§7, §9).
3. **Cursor advance** moved to after-durable-decision (§9).
4. **is_open_pr check** via `GET /issues/{n}` before dispatching ping (§6, §9).
5. **User-Agent** + `X-GitHub-Api-Version` headers required (§6).
6. **Collaborator token scopes** documented; 403 ≠ "not a collaborator" (§6).
7. **Idempotency marker** in comment bodies + `flock` on state file (§12).
8. **Sanitized subprocess env**, no `GITHUB_TOKEN` to child (§10).
9. **Reviewer dispatched by enum + class hierarchy** (`REVIEWER_KIND` selects `mock`/`claude`/`codex`, each with its own class; see §8); `execvp` directly, no shell in the production path.
10. **Subprocess I/O:** nonblocking pipes, single `poll` loop, output caps, `killpg` (§10).
11. **HTTP timeouts** + **streaming diff with abort** at `MAX_DIFF_BYTES` (§11).
12. **OpenSSL 3 on macOS** documented (brew + `OPENSSL_ROOT_DIR`) (§13).
13. **cpp-httplib pinned** to a release tag (§13).
14. **solvcon files listed explicitly**, includes marked `SYSTEM` (§13).
15. **Mention matching** via word-boundary regex, case-insensitive login eq (§9).

Ready to start M1.
